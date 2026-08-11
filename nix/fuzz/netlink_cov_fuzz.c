// SPDX-License-Identifier: GPL-2.0
/*
 * netlink_cov_fuzz -- a KCOV coverage-guided fuzzer for the "urp" generic-
 * netlink family (design 27 F2, S3). This is the coverage-guided upgrade of
 * netlink_fuzz: instead of blind random messages, it uses the kernel's KCOV
 * facility (/sys/kernel/debug/kcov) to measure which kernel edges each message
 * exercises, and keeps in a corpus only the inputs that reach NEW coverage --
 * the same feedback loop syzkaller uses internally, minus syzkaller's VM
 * orchestration (which does not fit this expect/console microVM harness).
 *
 * KCOV is PER-TASK: it captures edges hit by THIS thread's syscalls. The genl
 * doit/dumpit handlers (urp_new_endpoint_doit, urp_parse_endpoint, the policy
 * validation, ...) run synchronously in the caller's sendmsg/recvmsg context,
 * so per-task KCOV captures them directly. (The RDMA RX path runs in the
 * acceptor's workqueue on a different task/VM, so it is NOT coverage-guidable
 * this way -- that is the wire fuzzer's domain and needs KCOV-remote.)
 *
 * Runs INSIDE a VM whose kernel has urp loaded and CONFIG_KCOV + KASAN. Bugs
 * still surface as KASAN/lockdep reports the harness sanitizer phase scrapes;
 * the added value here is DEPTH -- coverage feedback drives inputs past the
 * policy layer into the handler bodies that blind fuzzing rarely reaches.
 *
 * Falls back to blind mode (with a warning) if KCOV is unavailable, so the
 * binary still runs on a non-KCOV kernel.
 *
 * Deterministic: PRNG seeded from argv[2] (default 1).
 *   usage: netlink_cov_fuzz <seconds> [seed]
 *
 * Self-contained: raw netlink, libc only.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>

/* ---- UAPI constants (mirror kernel/include/uapi/linux/urp.h) ---- */
#define URP_GENL_NAME		"urp"
#define URP_CMD_NEW_ENDPOINT	1
#define URP_CMD_DEL_ENDPOINT	2
#define URP_CMD_SET_ENDPOINT	3
#define URP_CMD_GET_ENDPOINT	4
#define URP_A_ENDPOINT		1	/* top-level NLA_NESTED */

#define URP_EP_A_NAME		1	/* NUL_STRING max 15 */
#define URP_EP_A_LISTEN_PATH	2	/* NUL_STRING max 107 */
#define URP_EP_A_CONNECT_PATH	3	/* NUL_STRING max 107 */
#define URP_EP_A_RDMA_DEVICE	4	/* NUL_STRING */
#define URP_EP_A_PEER_ADDR	5	/* exact sizeof(sockaddr_in6) = 28 */
#define URP_EP_A_BIND_ADDR	6	/* exact 28 */
#define URP_EP_A_NUM_QPS	7	/* u32 range 1..32 */
#define URP_EP_A_BUFFER_COUNT	8	/* u32 min 16 */
#define URP_EP_A_BUFFER_SIZE	9	/* u32 range 20..65536 */
#define URP_EP_A_PASSWORD	10	/* NUL_STRING max 15 */
#define URP_EP_A_STATE		11	/* u8 0..STATE_MAX */
#define URP_EP_A_MAX		12

/* ---- KCOV (mirror include/uapi/linux/kcov.h) ---- */
#define KCOV_INIT_TRACE		_IOR('c', 1, unsigned long)
#define KCOV_ENABLE		_IO('c', 100)
#define KCOV_DISABLE		_IO('c', 101)
#define KCOV_TRACE_PC		0
#define KCOV_TRACE_CMP		1
/* Comparison-record type word (KCOV_TRACE_CMP): bit0 = arg2 is a compile-time
 * constant; bits1-2 = log2(operand size in bytes). Each record is 4 u64s
 * [type, arg1, arg2, pc]; cover[0] is the record count.
 */
#define KCOV_CMP_CONST		(1u << 0)
#define KCOV_CMP_SIZE(n)	((n) << 1)
#define KCOV_CMP_MASK		KCOV_CMP_SIZE(3u)
#define COVER_SIZE		(256u << 10)	/* max PCs/records per run */

/* Global kcov fd so we can toggle TRACE_PC <-> TRACE_CMP mid-run. */
static int kcov_fd = -1;

/* ---- deterministic PRNG ---- */
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

/* ---- coverage bitmap: hash observed PCs into a large bit array ---- */
#define COV_BITS	(1u << 22)		/* 4M buckets */
#define COV_MASK	(COV_BITS - 1)
static uint8_t *cov_seen;			/* COV_BITS/8 bytes */
static unsigned long cov_total;			/* distinct buckets set */

static int cov_note(uint64_t pc)
{
	uint32_t h = (uint32_t)((pc * 0x9E3779B97F4A7C15ull) >> 40) & COV_MASK;
	uint8_t bit = 1u << (h & 7);
	uint32_t idx = h >> 3;

	if (cov_seen[idx] & bit)
		return 0;
	cov_seen[idx] |= bit;
	cov_total++;
	return 1;
}

/* ---- comparison-operand dictionary (harvested via KCOV_TRACE_CMP) ---- */
#define DICT_MAX	512
static uint64_t dict[DICT_MAX];
static int dict_len;

static void dict_add(uint64_t v)
{
	int i;

	/* 0/1 are already well covered by the seeds + edge-value mutation; and
	 * likely-pointer values (very large) are noise, not magic constants.
	 */
	if (v <= 1 || v >= 0xffff000000000000ull)
		return;
	for (i = 0; i < dict_len; i++)
		if (dict[i] == v)
			return;
	if (dict_len < DICT_MAX)
		dict[dict_len++] = v;
}

/* ---- input model: one genl message ---- */
#define MAX_ATTRS	10
#define ATTR_DATA_MAX	64
struct attr {
	uint16_t type;
	uint16_t len;
	uint8_t  data[ATTR_DATA_MAX];
};
struct input {
	uint8_t  cmd;
	uint8_t  dump;
	uint8_t  nattrs;
	struct attr attrs[MAX_ATTRS];
};

/* corpus of coverage-increasing inputs */
#define CORPUS_MAX	4096
static struct input *corpus;
static int corpus_len;

/* ---- netlink message assembly ---- */
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

/* Serialize one input into a genl message, send it, and drain any reply.
 * (Shared by the PC-coverage exec and the CMP-operand harvest.)
 */
static void send_one(int fd, int family, const struct input *in)
{
	char buf[4096];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct genlmsghdr *ghdr;
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	struct nlattr *nest;
	int i;

	memset(buf, 0, sizeof(buf));
	nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	nlh->nlmsg_type = family;
	nlh->nlmsg_flags = NLM_F_REQUEST | (in->dump ? NLM_F_DUMP : 0);
	nlh->nlmsg_seq = (uint32_t)xrand();
	ghdr = (struct genlmsghdr *)NLMSG_DATA(nlh);
	ghdr->cmd = in->cmd;

	nest = NLMSG_TAIL(nlh);
	nest->nla_type = URP_A_ENDPOINT | NLA_F_NESTED;
	nest->nla_len = NLA_HDRLEN;
	nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + NLA_HDRLEN;
	for (i = 0; i < in->nattrs && i < MAX_ATTRS; i++) {
		uint16_t l = in->attrs[i].len;

		if (l > ATTR_DATA_MAX)
			l = ATTR_DATA_MAX;
		/* Guard against overrunning buf on pathological inputs. */
		if (nlh->nlmsg_len + NLA_HDRLEN + NLA_ALIGN(l) > sizeof(buf) - 64)
			break;
		put_attr(nlh, in->attrs[i].type, in->attrs[i].data, l);
	}
	nest->nla_len = (uint16_t)((char *)NLMSG_TAIL(nlh) - (char *)nest);

	(void)sendto(fd, nlh, nlh->nlmsg_len, 0,
		     (struct sockaddr *)&sa, sizeof(sa));
	{
		char rbuf[8192];

		while (recv(fd, rbuf, sizeof(rbuf), MSG_DONTWAIT) > 0)
			;	/* drain doit ACK / dumpit stream */
	}
}

/* Run one input under KCOV_TRACE_PC. Returns how many NEW coverage buckets it
 * hit (0 if KCOV is off).
 */
static int exec_input(int fd, int family, unsigned long *cover,
		      const struct input *in)
{
	int newcov = 0;
	unsigned long n, k;

	if (cover)
		__atomic_store_n(&cover[0], 0, __ATOMIC_RELAXED);

	send_one(fd, family, in);

	if (cover) {
		n = __atomic_load_n(&cover[0], __ATOMIC_RELAXED);
		if (n > COVER_SIZE)
			n = COVER_SIZE;
		for (k = 0; k < n; k++)
			newcov += cov_note(cover[k + 1]);
	}
	return newcov;
}

/* Re-run one input under KCOV_TRACE_CMP and harvest the compile-time
 * comparison constants the kernel checked the input against (magic numbers,
 * enum values, exact-length gates) into the dictionary. The mutator then
 * injects these so the fuzzer can satisfy value-gated branches it would never
 * guess. No-op without KCOV. Toggles the shared fd's mode and restores PC.
 */
static void harvest_cmp(int fd, int family, unsigned long *cover,
			const struct input *in)
{
	unsigned long nrec, i;

	if (!cover || kcov_fd < 0)
		return;
	if (ioctl(kcov_fd, KCOV_DISABLE, 0))
		return;
	if (ioctl(kcov_fd, KCOV_ENABLE, KCOV_TRACE_CMP)) {
		/* Best effort: restore PC mode and give up on this harvest. */
		(void)ioctl(kcov_fd, KCOV_ENABLE, KCOV_TRACE_PC);
		return;
	}

	__atomic_store_n(&cover[0], 0, __ATOMIC_RELAXED);
	send_one(fd, family, in);
	nrec = __atomic_load_n(&cover[0], __ATOMIC_RELAXED);
	/* Each record is 4 u64 words [type, arg1, arg2, pc]. */
	if (nrec > (COVER_SIZE - 1) / 4)
		nrec = (COVER_SIZE - 1) / 4;
	for (i = 0; i < nrec; i++) {
		uint64_t type = cover[1 + i * 4];

		/* arg2 is the compile-time constant when KCOV_CMP_CONST is set --
		 * the clean "dictionary" subset (arg1 is often a runtime pointer).
		 */
		if (type & KCOV_CMP_CONST)
			dict_add(cover[1 + i * 4 + 2]);
	}

	(void)ioctl(kcov_fd, KCOV_DISABLE, 0);
	(void)ioctl(kcov_fd, KCOV_ENABLE, KCOV_TRACE_PC);
}

/* ---- seed corpus: well-formed-ish inputs that reach deep into the handlers
 * so mutation explores outward from real coverage rather than from noise.
 */
static void seed_attr_str(struct attr *a, uint16_t type, const char *s)
{
	size_t l = strlen(s) + 1;

	if (l > ATTR_DATA_MAX)
		l = ATTR_DATA_MAX;
	a->type = type;
	a->len = (uint16_t)l;
	memcpy(a->data, s, l);
}
static void seed_attr_u32(struct attr *a, uint16_t type, uint32_t v)
{
	a->type = type;
	a->len = 4;
	memcpy(a->data, &v, 4);
}
static void seed_attr_sockaddr_in6(struct attr *a, uint16_t type)
{
	/* 28-byte sockaddr_in6: family=AF_INET6(10), port, flowinfo, addr16, scope */
	a->type = type;
	a->len = 28;
	memset(a->data, 0, 28);
	a->data[0] = 10;		/* sin6_family = AF_INET6 (little-endian u16) */
	a->data[2] = 0x12;		/* sin6_port high */
	a->data[3] = 0x67;		/* -> 4711-ish */
}

static void build_seeds(void)
{
	struct input *in;

	/* Seed 1: full valid NEW_ENDPOINT (acceptor form). */
	in = &corpus[corpus_len++];
	memset(in, 0, sizeof(*in));
	in->cmd = URP_CMD_NEW_ENDPOINT;
	seed_attr_str(&in->attrs[0], URP_EP_A_NAME, "fuzzcov");
	seed_attr_str(&in->attrs[1], URP_EP_A_CONNECT_PATH, "/tmp/fuzzcov.sock");
	seed_attr_sockaddr_in6(&in->attrs[2], URP_EP_A_BIND_ADDR);
	seed_attr_u32(&in->attrs[3], URP_EP_A_NUM_QPS, 1);
	seed_attr_u32(&in->attrs[4], URP_EP_A_BUFFER_COUNT, 16);
	seed_attr_u32(&in->attrs[5], URP_EP_A_BUFFER_SIZE, 4096);
	in->nattrs = 6;

	/* Seed 2: NEW_ENDPOINT initiator form (listen_path + peer_addr). */
	in = &corpus[corpus_len++];
	memset(in, 0, sizeof(*in));
	in->cmd = URP_CMD_NEW_ENDPOINT;
	seed_attr_str(&in->attrs[0], URP_EP_A_NAME, "fuzzcov2");
	seed_attr_str(&in->attrs[1], URP_EP_A_LISTEN_PATH, "/tmp/fuzzcov2.sock");
	seed_attr_sockaddr_in6(&in->attrs[2], URP_EP_A_PEER_ADDR);
	seed_attr_u32(&in->attrs[3], URP_EP_A_NUM_QPS, 4);
	in->nattrs = 4;

	/* Seed 3: GET_ENDPOINT by name. */
	in = &corpus[corpus_len++];
	memset(in, 0, sizeof(*in));
	in->cmd = URP_CMD_GET_ENDPOINT;
	seed_attr_str(&in->attrs[0], URP_EP_A_NAME, "fuzzcov");
	in->nattrs = 1;

	/* Seed 4: GET_ENDPOINT dump. */
	in = &corpus[corpus_len++];
	memset(in, 0, sizeof(*in));
	in->cmd = URP_CMD_GET_ENDPOINT;
	in->dump = 1;
	in->nattrs = 0;

	/* Seed 5: SET_ENDPOINT name + num_qps + buffer_count. */
	in = &corpus[corpus_len++];
	memset(in, 0, sizeof(*in));
	in->cmd = URP_CMD_SET_ENDPOINT;
	seed_attr_str(&in->attrs[0], URP_EP_A_NAME, "fuzzcov");
	seed_attr_u32(&in->attrs[1], URP_EP_A_NUM_QPS, 8);
	seed_attr_u32(&in->attrs[2], URP_EP_A_BUFFER_COUNT, 32);
	in->nattrs = 3;

	/* Seed 6: DEL_ENDPOINT by name. */
	in = &corpus[corpus_len++];
	memset(in, 0, sizeof(*in));
	in->cmd = URP_CMD_DEL_ENDPOINT;
	seed_attr_str(&in->attrs[0], URP_EP_A_NAME, "fuzzcov");
	in->nattrs = 1;
}

/* ---- mutation ---- */
static void mutate(struct input *in)
{
	int rounds = 1 + xrand_below(4);

	while (rounds--) {
		switch (xrand_below(10)) {
		case 9:	/* inject a KCOV_TRACE_CMP-harvested magic constant */
			if (in->nattrs && dict_len) {
				int i = xrand_below(in->nattrs);
				uint64_t v = dict[xrand_below(dict_len)];

				/* Write it at a size the policy is likely to read
				 * (u32 for the range-checked attrs, u64 otherwise).
				 */
				if (xrand_below(2)) {
					uint32_t v32 = (uint32_t)v;

					in->attrs[i].len = 4;
					memcpy(in->attrs[i].data, &v32, 4);
				} else {
					in->attrs[i].len = 8;
					memcpy(in->attrs[i].data, &v, 8);
				}
			}
			break;
		case 0:	/* flip command (incl. out-of-range) */
			in->cmd = (uint8_t)xrand_below(7);
			break;
		case 1:	/* toggle dump */
			in->dump ^= 1;
			break;
		case 2:	/* change an attr type (incl. beyond MAX) */
			if (in->nattrs) {
				int i = xrand_below(in->nattrs);

				in->attrs[i].type =
					(uint16_t)xrand_below(URP_EP_A_MAX + 4);
			}
			break;
		case 3:	/* change an attr length (mismatched) */
			if (in->nattrs) {
				int i = xrand_below(in->nattrs);

				in->attrs[i].len =
					(uint16_t)xrand_below(ATTR_DATA_MAX + 1);
			}
			break;
		case 4:	/* flip bytes in an attr payload */
			if (in->nattrs) {
				int i = xrand_below(in->nattrs);
				int b = xrand_below(ATTR_DATA_MAX);

				in->attrs[i].data[b] ^= (uint8_t)xrand();
			}
			break;
		case 5:	/* append a random attr */
			if (in->nattrs < MAX_ATTRS) {
				struct attr *a = &in->attrs[in->nattrs++];
				int b;

				a->type = (uint16_t)xrand_below(URP_EP_A_MAX + 4);
				a->len = (uint16_t)xrand_below(ATTR_DATA_MAX + 1);
				for (b = 0; b < ATTR_DATA_MAX; b++)
					a->data[b] = (uint8_t)xrand();
			}
			break;
		case 6:	/* remove an attr */
			if (in->nattrs) {
				int i = xrand_below(in->nattrs);

				in->attrs[i] = in->attrs[--in->nattrs];
			}
			break;
		case 7:	/* duplicate an attr (same type twice) */
			if (in->nattrs && in->nattrs < MAX_ATTRS) {
				int i = xrand_below(in->nattrs);

				in->attrs[in->nattrs++] = in->attrs[i];
			}
			break;
		default:/* set an attr to a canonical u32 edge value */
			if (in->nattrs) {
				int i = xrand_below(in->nattrs);
				static const uint32_t edges[] = {
					0, 1, 15, 16, 17, 32, 33,
					19, 20, 21, 65535, 65536, 0xFFFFFFFFu
				};
				uint32_t v = edges[xrand_below(13)];

				in->attrs[i].len = 4;
				memcpy(in->attrs[i].data, &v, 4);
			}
			break;
		}
	}
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

/* Set up KCOV on this thread. Returns the mmap'd cover buffer, or NULL (blind
 * mode) if KCOV is unavailable. Leaks the fd/mapping intentionally for the
 * process lifetime.
 */
static unsigned long *kcov_setup(void)
{
	int fd;
	unsigned long *cover;

	fd = open("/sys/kernel/debug/kcov", O_RDWR);
	if (fd < 0)
		return NULL;
	if (ioctl(fd, KCOV_INIT_TRACE, (unsigned long)COVER_SIZE)) {
		close(fd);
		return NULL;
	}
	cover = mmap(NULL, COVER_SIZE * sizeof(unsigned long),
		     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (cover == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	if (ioctl(fd, KCOV_ENABLE, KCOV_TRACE_PC)) {
		munmap(cover, COVER_SIZE * sizeof(unsigned long));
		close(fd);
		return NULL;
	}
	kcov_fd = fd;	/* keep for TRACE_PC <-> TRACE_CMP toggling */
	return cover;
}

int main(int argc, char **argv)
{
	int fd, family;
	long seconds = argc > 1 ? atol(argv[1]) : 20;
	unsigned long execs = 0;
	unsigned long *cover;
	unsigned long long seed_used;
	time_t start;
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };

	if (argc > 2)
		prng_state = strtoull(argv[2], NULL, 0);
	if (prng_state == 0)
		prng_state = 1;
	/* Capture the input seed before seed-priming advances the PRNG, so the
	 * banner reports the value you pass to reproduce a run (not the state
	 * after priming).
	 */
	seed_used = (unsigned long long)prng_state;

	cov_seen = calloc(COV_BITS / 8, 1);
	corpus = calloc(CORPUS_MAX, sizeof(struct input));
	if (!cov_seen || !corpus) {
		perror("calloc");
		return 2;
	}

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
	if (fd < 0) {
		perror("socket");
		return 2;
	}
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("bind");
		return 2;
	}

	family = resolve_family(fd);
	if (family < 0) {
		fprintf(stderr, "COV_FUZZ: urp family not found (module loaded?)\n");
		return 3;
	}

	cover = kcov_setup();
	if (!cover)
		fprintf(stderr, "COV_FUZZ: KCOV unavailable -- blind mode "
			"(need CONFIG_KCOV + debugfs + root)\n");

	build_seeds();
	/* Prime coverage with the seeds so the corpus starts from real depth. */
	{
		int i;

		for (i = 0; i < corpus_len; i++)
			(void)exec_input(fd, family, cover, &corpus[i]);
	}
	printf("COV_FUZZ: urp family id=%d, kcov=%s, seeds=%d, fuzzing %lds seed=%llu\n",
	       family, cover ? "on" : "off", corpus_len, seconds, seed_used);
	fflush(stdout);

	start = time(NULL);
	while (time(NULL) - start < seconds) {
		int batch;

		for (batch = 0; batch < 1000; batch++) {
			struct input cand = corpus[xrand_below(corpus_len)];
			int gained;

			mutate(&cand);
			gained = exec_input(fd, family, cover, &cand);
			execs++;
			/* Coverage-increasing input -> add to corpus, and harvest
			 * its comparison constants (that is where new value gates
			 * tend to appear).
			 */
			if (gained > 0 && corpus_len < CORPUS_MAX) {
				corpus[corpus_len++] = cand;
				harvest_cmp(fd, family, cover, &cand);
			} else if ((execs & 0x1ff) == 0) {
				/* Periodic harvest from a random corpus entry so
				 * the dictionary keeps growing on plateaus.
				 */
				harvest_cmp(fd, family, cover,
					    &corpus[xrand_below(corpus_len)]);
			}
		}
	}

	printf("COV_FUZZ_DONE execs=%lu corpus=%d edges=%lu dict=%d\n",
	       execs, corpus_len, cov_total, dict_len);
	fflush(stdout);
	close(fd);
	return 0;
}
