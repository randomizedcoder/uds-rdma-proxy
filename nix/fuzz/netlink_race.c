// SPDX-License-Identifier: GPL-2.0
/*
 * netlink_race -- a concurrent (multi-threaded) racer for the "urp" generic-
 * netlink control plane (design 27 F2, S3 concurrency). The single-threaded
 * fuzzers (netlink_fuzz, netlink_cov_fuzz) cannot reach concurrency bugs; this
 * one targets them directly.
 *
 * Specific target: the endpoint lifecycle has NO kref/refcount (design 26).
 * urp_endpoint_lookup() returns a bare pointer, and the genl handlers do
 *   rcu_read_lock(); ep = lookup(name); rcu_read_unlock(); ... deref ep ...
 * (e.g. urp_set_endpoint_doit's mutex_lock(&ep->lock), the GET serializers),
 * while DEL_ENDPOINT frees ep via call_rcu(). If a lookup thread is scheduled
 * out between rcu_read_unlock() and its deref while a concurrent DEL runs and
 * a grace period elapses, ep is freed under it -- a use-after-free. Two
 * concurrent DELs on one name are the double-free variant.
 *
 * The racer just manufactures that pressure: N threads, each on its own
 * netlink socket, hammering a SMALL shared pool of endpoint names with
 * NEW/DEL/SET/GET so lookups, inserts, frees, and derefs collide on the same
 * objects. It is NOT its own oracle -- it runs in the KASAN sanitizer VM and
 * the harness scrapes dmesg (Phase 11b + per-phase scan_splat) for the report.
 *
 * Uses more threads than vCPUs so preemption between rcu_read_unlock() and the
 * deref is frequent. Deterministic-ish: each thread seeds from argv[3]+tid,
 * but races are inherently nondeterministic.
 *
 *   usage: netlink_race <seconds> <bind-ip> [seed] [nthreads]
 *
 * Self-contained: raw netlink + pthreads + libc.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>

#define URP_GENL_NAME		"urp"
#define URP_CMD_NEW_ENDPOINT	1
#define URP_CMD_DEL_ENDPOINT	2
#define URP_CMD_SET_ENDPOINT	3
#define URP_CMD_GET_ENDPOINT	4
#define URP_A_ENDPOINT		1

#define URP_EP_A_NAME		1
#define URP_EP_A_CONNECT_PATH	3
#define URP_EP_A_BIND_ADDR	6
#define URP_EP_A_NUM_QPS	7
#define URP_EP_A_BUFFER_COUNT	8
#define URP_EP_A_BUFFER_SIZE	9

#define NAME_POOL		4	/* small -> heavy collision on same ep */
#define RACE_BASE_PORT		4800

static int g_family;
static const char *g_bind_ip = "10.99.99.1";
static volatile int g_stop;

/* per-thread PRNG */
struct rng { uint64_t s; };
static uint64_t rng_next(struct rng *r)
{
	uint64_t x = r->s;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	r->s = x;
	return x;
}

#define NLMSG_TAIL(nmsg) \
	((struct nlattr *)(((char *)(nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

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

/* Build a 28-byte sockaddr_in6 that urp accepts as a bind/peer addr: the
 * module reads sin6_family + sin6_port + the v4 address. We encode an IPv4
 * address into the trailing 4 bytes of the in6 addr (v4-mapped tail), which is
 * what urp_endpoint's IPv4-only decode picks up, and a per-name port.
 */
static void fill_bind_addr(uint8_t out[28], uint16_t port)
{
	struct in_addr v4;

	memset(out, 0, 28);
	out[0] = 10;			/* sin6_family = AF_INET6 (LE u16 low) */
	out[2] = (uint8_t)(port >> 8);	/* sin6_port, network order */
	out[3] = (uint8_t)(port & 0xFF);
	inet_pton(AF_INET, g_bind_ip, &v4);
	/* v4-mapped: ::ffff:a.b.c.d -> bytes 8..23; put ffff at 18-19, v4 at 20-23 */
	out[8 + 10] = 0xFF;
	out[8 + 11] = 0xFF;
	memcpy(&out[8 + 12], &v4, 4);
}

static void send_op(int fd, struct rng *r)
{
	char buf[1024];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct genlmsghdr *ghdr;
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	struct nlattr *nest;
	unsigned idx = (unsigned)(rng_next(r) % NAME_POOL);
	char name[16];
	uint8_t op = (uint8_t)(rng_next(r) % 10);
	uint8_t cmd;

	snprintf(name, sizeof(name), "r%u", idx);

	memset(buf, 0, sizeof(buf));
	nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	nlh->nlmsg_type = g_family;
	nlh->nlmsg_flags = NLM_F_REQUEST;
	nlh->nlmsg_seq = (uint32_t)rng_next(r);
	ghdr = (struct genlmsghdr *)NLMSG_DATA(nlh);

	/* Weight: NEW 30%, DEL 30%, SET 20%, GET 20% -> heavy insert/free churn. */
	if (op < 3)
		cmd = URP_CMD_NEW_ENDPOINT;
	else if (op < 6)
		cmd = URP_CMD_DEL_ENDPOINT;
	else if (op < 8)
		cmd = URP_CMD_SET_ENDPOINT;
	else
		cmd = URP_CMD_GET_ENDPOINT;
	ghdr->cmd = cmd;

	nest = NLMSG_TAIL(nlh);
	nest->nla_type = URP_A_ENDPOINT | NLA_F_NESTED;
	nest->nla_len = NLA_HDRLEN;
	nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + NLA_HDRLEN;

	put_attr(nlh, URP_EP_A_NAME, name, (uint16_t)(strlen(name) + 1));
	if (cmd == URP_CMD_NEW_ENDPOINT) {
		uint8_t addr[28];
		char cpath[32];
		uint32_t nq = 1, bc = 16, bs = 4096;

		snprintf(cpath, sizeof(cpath), "/tmp/race-%u.sock", idx);
		fill_bind_addr(addr, (uint16_t)(RACE_BASE_PORT + idx));
		put_attr(nlh, URP_EP_A_CONNECT_PATH, cpath,
			 (uint16_t)(strlen(cpath) + 1));
		put_attr(nlh, URP_EP_A_BIND_ADDR, addr, 28);
		put_attr(nlh, URP_EP_A_NUM_QPS, &nq, 4);
		put_attr(nlh, URP_EP_A_BUFFER_COUNT, &bc, 4);
		put_attr(nlh, URP_EP_A_BUFFER_SIZE, &bs, 4);
	} else if (cmd == URP_CMD_SET_ENDPOINT) {
		/* touch a mutable field so SET takes ep->lock (the deref site) */
		uint32_t bc = 16;

		put_attr(nlh, URP_EP_A_BUFFER_COUNT, &bc, 4);
	}
	nest->nla_len = (uint16_t)((char *)NLMSG_TAIL(nlh) - (char *)nest);

	(void)sendto(fd, nlh, nlh->nlmsg_len, 0,
		     (struct sockaddr *)&sa, sizeof(sa));
	{
		char rbuf[2048];

		while (recv(fd, rbuf, sizeof(rbuf), MSG_DONTWAIT) > 0)
			;
	}
}

struct worker { pthread_t th; uint64_t seed; unsigned long ops; };

static int open_nl(void)
{
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };

	if (fd < 0)
		return -1;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void *worker_fn(void *arg)
{
	struct worker *w = arg;
	struct rng r = { .s = w->seed ? w->seed : 1 };
	int fd = open_nl();

	if (fd < 0)
		return NULL;
	while (!g_stop) {
		send_op(fd, &r);
		w->ops++;
	}
	close(fd);
	return NULL;
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

int main(int argc, char **argv)
{
	long seconds = argc > 1 ? atol(argv[1]) : 20;
	uint64_t seed = argc > 3 ? strtoull(argv[3], NULL, 0) : 1;
	int nthreads = argc > 4 ? atoi(argv[4]) : 8;
	struct worker *ws;
	unsigned long total = 0;
	int fd, i;
	time_t start;

	if (argc > 2)
		g_bind_ip = argv[2];
	if (nthreads < 2)
		nthreads = 2;
	if (nthreads > 64)
		nthreads = 64;

	fd = open_nl();
	if (fd < 0) {
		perror("socket");
		return 2;
	}
	g_family = resolve_family(fd);
	close(fd);
	if (g_family < 0) {
		fprintf(stderr, "RACE: urp family not found (module loaded?)\n");
		return 3;
	}
	printf("RACE: urp family id=%d, %d threads, %ld s, bind=%s, seed=%llu\n",
	       g_family, nthreads, seconds, g_bind_ip,
	       (unsigned long long)seed);
	fflush(stdout);

	ws = calloc(nthreads, sizeof(*ws));
	if (!ws) {
		perror("calloc");
		return 2;
	}
	for (i = 0; i < nthreads; i++) {
		ws[i].seed = seed + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		pthread_create(&ws[i].th, NULL, worker_fn, &ws[i]);
	}

	start = time(NULL);
	while (time(NULL) - start < seconds)
		usleep(100000);
	g_stop = 1;

	for (i = 0; i < nthreads; i++) {
		pthread_join(ws[i].th, NULL);
		total += ws[i].ops;
	}

	/* Best-effort cleanup: delete the race endpoints we may have left. */
	fd = open_nl();
	if (fd >= 0) {
		struct rng r = { .s = 1 };

		for (i = 0; i < NAME_POOL * 2; i++)
			send_op(fd, &r);	/* mixed ops incl. DEL */
		close(fd);
	}

	printf("RACE_DONE ops=%lu threads=%d\n", total, nthreads);
	fflush(stdout);
	free(ws);
	return 0;
}
