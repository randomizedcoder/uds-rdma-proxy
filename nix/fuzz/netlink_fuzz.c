// SPDX-License-Identifier: GPL-2.0
/*
 * netlink_fuzz -- a live-kernel fuzzer for the "urp" generic-netlink family
 * (design 27 F2, S3). Runs INSIDE a VM whose kernel has the urp module
 * loaded and KASAN/KMEMLEAK/lockdep enabled, and hammers the genl control
 * plane (URP_CMD_{NEW,DEL,SET,GET}_ENDPOINT) with malformed nested-attribute
 * messages. Bugs surface as KASAN / lockdep / BUG / WARN reports in dmesg,
 * which the microVM harness's sanitizer phase already scrapes.
 *
 * This targets the real netlink handlers (urp_new_endpoint_doit,
 * urp_set_endpoint_doit, urp_parse_endpoint, the policy validation) where
 * the surface sweep flagged e.g. the SET num_qps out-of-bounds path -- the
 * integration code the userspace F1 harnesses can't reach.
 *
 * Deterministic: the PRNG is seeded from argv[2] (default 1) so a crashing
 * run is reproducible. usage: netlink_fuzz <seconds> [seed]
 *
 * Self-contained: raw netlink, libc only.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>

/* UAPI constants (mirror kernel/include/uapi/linux/urp.h). Kept local so
 * the fuzzer builds standalone in the VM rootfs.
 */
#define URP_GENL_NAME		"urp"
#define URP_CMD_NEW_ENDPOINT	1
#define URP_CMD_DEL_ENDPOINT	2
#define URP_CMD_SET_ENDPOINT	3
#define URP_CMD_GET_ENDPOINT	4
#define URP_A_ENDPOINT		1	/* top-level NLA_NESTED */
/* nested URP_ENDPOINT_A_* attribute types */
#define URP_EP_A_MAX		12

/* xorshift64 -- deterministic, no libc rand() state games. */
static uint64_t prng_state = 1;
static uint64_t xrand(void)
{
	uint64_t x = prng_state;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	prng_state = x;
	return x;
}
static uint32_t xrand_below(uint32_t n)
{
	return n ? (uint32_t)(xrand() % n) : 0;
}

#define NLMSG_TAIL(nmsg) \
	((struct nlattr *)(((char *)(nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

/* Append one attribute (type/len/payload) to the message, no bounds fuss:
 * buffers are oversized and lengths bounded below.
 */
static void put_attr(struct nlmsghdr *nlh, uint16_t type,
		     const void *data, uint16_t len)
{
	struct nlattr *nla = NLMSG_TAIL(nlh);
	uint16_t alen = NLA_HDRLEN + len;

	nla->nla_type = type;
	nla->nla_len = alen;
	if (len)
		memcpy((char *)nla + NLA_HDRLEN, data, len);
	nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + NLA_ALIGN(alen);
}

static int resolve_family(int fd)
{
	char buf[1024];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct genlmsghdr *ghdr;
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	ssize_t n;

	memset(buf, 0, sizeof(buf));
	nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	nlh->nlmsg_type = GENL_ID_CTRL;
	nlh->nlmsg_flags = NLM_F_REQUEST;
	nlh->nlmsg_seq = 1;
	ghdr = (struct genlmsghdr *)NLMSG_DATA(nlh);
	ghdr->cmd = CTRL_CMD_GETFAMILY;
	put_attr(nlh, CTRL_ATTR_FAMILY_NAME, URP_GENL_NAME,
		 (uint16_t)(strlen(URP_GENL_NAME) + 1));

	if (sendto(fd, nlh, nlh->nlmsg_len, 0,
		   (struct sockaddr *)&sa, sizeof(sa)) < 0)
		return -1;

	n = recv(fd, buf, sizeof(buf), 0);
	if (n < (ssize_t)NLMSG_HDRLEN || nlh->nlmsg_type == NLMSG_ERROR)
		return -1;

	/* Walk the response attrs for CTRL_ATTR_FAMILY_ID (u16). */
	ghdr = (struct genlmsghdr *)NLMSG_DATA(nlh);
	{
		struct nlattr *nla = (struct nlattr *)((char *)ghdr + GENL_HDRLEN);
		int rem = nlh->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);

		while (rem >= (int)NLA_HDRLEN && nla->nla_len >= NLA_HDRLEN &&
		       nla->nla_len <= rem) {
			if (nla->nla_type == CTRL_ATTR_FAMILY_ID)
				return *(uint16_t *)((char *)nla + NLA_HDRLEN);
			rem -= NLA_ALIGN(nla->nla_len);
			nla = (struct nlattr *)((char *)nla + NLA_ALIGN(nla->nla_len));
		}
	}
	return -1;
}

/* Build + send one fuzzed URP command. */
static void fuzz_one(int fd, int family)
{
	char buf[2048];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct genlmsghdr *ghdr;
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	struct nlattr *nest;
	uint8_t cmds[] = { URP_CMD_NEW_ENDPOINT, URP_CMD_DEL_ENDPOINT,
			   URP_CMD_SET_ENDPOINT, URP_CMD_GET_ENDPOINT };
	int nattrs, i;

	memset(buf, 0, sizeof(buf));
	nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	nlh->nlmsg_type = family;
	nlh->nlmsg_flags = NLM_F_REQUEST | (xrand_below(4) == 0 ? NLM_F_DUMP : 0);
	nlh->nlmsg_seq = (uint32_t)xrand();
	ghdr = (struct genlmsghdr *)NLMSG_DATA(nlh);
	ghdr->cmd = cmds[xrand_below(4)];

	/* Open a nested URP_A_ENDPOINT with a random pile of attributes. */
	nest = NLMSG_TAIL(nlh);
	nest->nla_type = URP_A_ENDPOINT | NLA_F_NESTED;
	nest->nla_len = NLA_HDRLEN;
	nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + NLA_HDRLEN;

	nattrs = 1 + xrand_below(6);
	for (i = 0; i < nattrs; i++) {
		uint8_t payload[64];
		/* Random type incl. 0 and > MAX; random length incl. 0 and
		 * mismatched (e.g. 4-byte value on a string attr, or a huge
		 * length on a fixed one). This is what exercises the policy.
		 */
		uint16_t type = xrand_below(URP_EP_A_MAX + 3);
		uint16_t len = (uint16_t)xrand_below(48);
		uint32_t k;

		for (k = 0; k < len && k < sizeof(payload); k++)
			payload[k] = (uint8_t)xrand();
		if (len > sizeof(payload))
			len = sizeof(payload);
		put_attr(nlh, type, payload, len);
	}
	/* Close the nest. */
	nest->nla_len = (uint16_t)((char *)NLMSG_TAIL(nlh) - (char *)nest);

	/* Occasionally truncate the whole message to test short-message
	 * handling in the parser.
	 */
	if (xrand_below(8) == 0 && nlh->nlmsg_len > GENL_HDRLEN + 8)
		nlh->nlmsg_len -= xrand_below(8);

	(void)sendto(fd, nlh, nlh->nlmsg_len, 0,
		     (struct sockaddr *)&sa, sizeof(sa));

	/* Drain any reply/ACK so the socket buffer doesn't fill. */
	{
		char rbuf[4096];

		(void)recv(fd, rbuf, sizeof(rbuf), MSG_DONTWAIT);
	}
}

int main(int argc, char **argv)
{
	int fd, family;
	long seconds = argc > 1 ? atol(argv[1]) : 20;
	unsigned long iters = 0;
	time_t start;

	if (argc > 2)
		prng_state = strtoull(argv[2], NULL, 0);
	if (prng_state == 0)
		prng_state = 1;

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
	if (fd < 0) {
		perror("socket");
		return 2;
	}
	{
		struct sockaddr_nl sa = { .nl_family = AF_NETLINK };

		if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
			perror("bind");
			return 2;
		}
	}

	family = resolve_family(fd);
	if (family < 0) {
		fprintf(stderr, "NETLINK_FUZZ: urp family not found (module loaded?)\n");
		return 3;
	}
	printf("NETLINK_FUZZ: urp family id=%d, fuzzing %lds seed=%llu\n",
	       family, seconds, (unsigned long long)prng_state);

	start = time(NULL);
	while (time(NULL) - start < seconds) {
		int batch;

		for (batch = 0; batch < 2000; batch++)
			fuzz_one(fd, family);
		iters += 2000;
	}
	printf("NETLINK_FUZZ_DONE iters=%lu\n", iters);
	close(fd);
	return 0;
}
