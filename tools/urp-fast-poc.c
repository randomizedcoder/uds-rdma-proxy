// SPDX-License-Identifier: GPL-2.0
/*
 * urp-fast proof-of-concept driver (design 31, PR1).
 *
 * Exercises the /dev/urp uring_cmd interface end to end against a live
 * urp.ko: it mmaps an application buffer pool, then drives REGISTER /
 * UNREGISTER (and the not-yet-implemented SEND path) over
 * IORING_OP_URING_CMD, asserting both the happy path (the pool actually
 * pins) and the negative cases that prove the app->kernel trust boundary
 * (design 31 section 31.10) rejects bad input.
 *
 * This is NOT a sandboxed nix check: io_uring + a loaded kernel module can't
 * run in the build sandbox (design 30 section 30.14). It runs as a `nix run`
 * app and inside the microVM pair test, where urp.ko is loaded for real.
 *
 * Output is machine-greppable: one URP_FAST_* line per step, and a final
 * URP_FAST_POC_OK / URP_FAST_POC_FAIL verdict.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include <liburing.h>

#include "include/uapi/linux/urp_cmd.h"

static int failures;

/* Submit one uring_cmd on @fd and return the CQE result (negative errno). */
static int do_cmd(struct io_uring *ring, int fd, uint32_t cmd_op,
		  const void *inline_cmd, size_t inline_len)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	struct io_uring_cqe *cqe;
	int ret, res;

	if (!sqe) {
		fprintf(stderr, "no sqe\n");
		return -ENOSPC;
	}
	io_uring_prep_uring_cmd(sqe, (int)cmd_op, fd);
	/* The 16-byte inline SQE cmd area carries the per-op arguments. */
	memset(sqe->cmd, 0, 16);
	if (inline_cmd)
		memcpy(sqe->cmd, inline_cmd, inline_len);

	ret = io_uring_submit(ring);
	if (ret < 0) {
		fprintf(stderr, "submit: %s\n", strerror(-ret));
		return ret;
	}
	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "wait_cqe: %s\n", strerror(-ret));
		return ret;
	}
	res = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	return res;
}

/* Submit one uring_cmd without reaping its CQE (for the async RECV smoke: the
 * op stays in flight until ring teardown cancels it). Returns the submit result.
 */
static int arm_cmd(struct io_uring *ring, int fd, uint32_t cmd_op,
		   const void *inline_cmd, size_t inline_len)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	if (!sqe)
		return -ENOSPC;
	io_uring_prep_uring_cmd(sqe, (int)cmd_op, fd);
	memset(sqe->cmd, 0, 16);
	if (inline_cmd)
		memcpy(sqe->cmd, inline_cmd, inline_len);
	return io_uring_submit(ring);
}

/*
 * RECV smoke (design 31 PR4): REGISTER a pool against a *connected fast*
 * endpoint, arm several zero-copy RECV buffers directly on its QP, and exit
 * WITHOUT reaping -- no peer sends a frame, so the recvs stay in flight. Ring
 * teardown must then cancel them and the fd close must drain the RQ before
 * unpinning (else a KASAN UAF or a hang). This exercises the whole RECV arming +
 * teardown-quiesce path live; the delivered-bytes round trip is the pair test's
 * fast<->fast phase. Returns 0 on success (prints URP_FAST_POC_OK), 1 otherwise.
 */
static int run_recv_smoke(struct io_uring *ring, int fd, void *pool,
			  size_t pool_len, uint32_t buf_size, uint32_t count,
			  const char *endpoint)
{
	struct urp_cmd_reg reg;
	struct urp_cmd_reg_sqe reg_sqe;
	struct urp_cmd_data data;
	uint32_t armed = 0, want = count < 4 ? count : 4;
	uint32_t i;
	int res;

	memset(&reg, 0, sizeof(reg));
	reg.base = (uint64_t)(uintptr_t)pool;
	reg.len = pool_len;
	reg.buf_size = buf_size;
	reg.count = count;
	strncpy(reg.endpoint, endpoint, URP_CMD_NAME_MAX - 1);
	memset(&reg_sqe, 0, sizeof(reg_sqe));
	reg_sqe.arg = (uint64_t)(uintptr_t)&reg;
	res = do_cmd(ring, fd, URP_CMD_REGISTER, &reg_sqe, sizeof(reg_sqe));
	if (res != 0) {
		printf("URP_FAST_RECV_REGISTER FAIL res=%d\n", res);
		return 1;
	}
	printf("URP_FAST_RECV_REGISTER ok res=0\n");

	for (i = 0; i < want; i++) {
		memset(&data, 0, sizeof(data));
		data.buf_index = i;
		data.len = buf_size - URP_CMD_HEADER_RESV;
		res = arm_cmd(ring, fd, URP_CMD_RECV, &data, sizeof(data));
		if (res < 0) {
			printf("URP_FAST_RECV_ARM FAIL idx=%u res=%d\n", i, res);
			return 1;
		}
		armed++;
	}
	printf("URP_FAST_RECV_ARMED n=%u (no peer; left in flight)\n", armed);

	/*
	 * RECV submits punt to io-wq (the handler may sleep), so give them a
	 * moment to actually reach ib_post_recv and claim their buffers before we
	 * tear down -- that way teardown genuinely exercises the cancel + RQ-drain
	 * path against armed WRs rather than racing un-started submissions.
	 */
	usleep(200 * 1000);

	/* Fall through to teardown: queue_exit cancels the armed recvs, the fd
	 * close drains the RQ and unpins. A clean exit + KASAN silence is the pass.
	 */
	printf("URP_FAST_POC_OK\n");
	return 0;
}

/* Assert a step's result, log a greppable line, and track failures. */
static void expect(const char *label, int got, int want)
{
	if (got == want) {
		printf("URP_FAST_%s ok res=%d\n", label, got);
	} else {
		printf("URP_FAST_%s FAIL res=%d want=%d\n", label, got, want);
		failures++;
	}
}

int main(int argc, char **argv)
{
	const char *dev;
	const char *endpoint;
	uint32_t buf_size, count;
	size_t pool_len;

	if (argc < 3) {
		fprintf(stderr,
			"usage: %s <dev> <endpoint> [buf_size] [count]\n"
			"  <endpoint> is a connected urp endpoint (has a PD) to\n"
			"  DMA-map the pool against.\n",
			argv[0]);
		return 2;
	}
	const char *mode;

	dev = argv[1];
	endpoint = argv[2];
	buf_size = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 0) : 4096;
	count = (argc > 4) ? (uint32_t)strtoul(argv[4], NULL, 0) : 8;
	/* Optional mode: "recv-smoke" arms zero-copy RECVs against a connected fast
	 * endpoint and exits without reaping (tests cancel + drain teardown). The
	 * default (no mode) runs the REGISTER/UNREGISTER + validation-edge suite. */
	mode = (argc > 5) ? argv[5] : "";
	pool_len = (size_t)buf_size * count;
	struct io_uring ring;
	struct urp_cmd_reg reg;
	struct urp_cmd_reg_sqe reg_sqe;
	struct urp_cmd_data data;
	void *pool;
	int fd, ret;

	fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		return 2;
	}

	/*
	 * The zero-copy RECV completion is a 32-byte CQE (payload len in res,
	 * buf_index|stream_id in res2), so a recv client MUST set up a CQE32 ring
	 * (design 31 open question Q3) -- and the kernel completes even the cancel
	 * of an armed recv as a CQE32. The default REGISTER/SEND validation flow
	 * uses 16-byte CQEs. Pick the ring width from the mode.
	 */
	ret = io_uring_queue_init(8, &ring,
				  strcmp(mode, "recv-smoke") == 0 ?
					  IORING_SETUP_CQE32 : 0);
	if (ret < 0) {
		fprintf(stderr, "queue_init: %s\n", strerror(-ret));
		close(fd);
		return 2;
	}

	pool = mmap(NULL, pool_len, PROT_READ | PROT_WRITE,
		    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (pool == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		io_uring_queue_exit(&ring);
		close(fd);
		return 2;
	}
	printf("URP_FAST_POOL base=%p len=%zu buf_size=%u count=%u\n",
	       pool, pool_len, buf_size, count);

	if (strcmp(mode, "recv-smoke") == 0) {
		int rc = run_recv_smoke(&ring, fd, pool, pool_len, buf_size,
					count, endpoint);

		munmap(pool, pool_len);
		io_uring_queue_exit(&ring);	/* cancels the in-flight recvs */
		close(fd);			/* release drains the RQ + unpins */
		return rc;
	}

	/* --- negative: an unknown endpoint is rejected before any pin sticks --- */
	memset(&reg, 0, sizeof(reg));
	reg.base = (uint64_t)(uintptr_t)pool;
	reg.len = pool_len;
	reg.buf_size = buf_size;
	reg.count = count;
	strncpy(reg.endpoint, "no_such_ep", URP_CMD_NAME_MAX - 1);
	memset(&reg_sqe, 0, sizeof(reg_sqe));
	reg_sqe.arg = (uint64_t)(uintptr_t)&reg;
	expect("REGISTER_BADEP", do_cmd(&ring, fd, URP_CMD_REGISTER, &reg_sqe,
					sizeof(reg_sqe)), -ENODEV);

	/* --- happy path: register + pin (FOLL_LONGTERM) + DMA-map the pool --- */
	memset(&reg, 0, sizeof(reg));
	reg.base = (uint64_t)(uintptr_t)pool;
	reg.len = pool_len;
	reg.buf_size = buf_size;
	reg.count = count;
	strncpy(reg.endpoint, endpoint, URP_CMD_NAME_MAX - 1);
	reg_sqe.arg = (uint64_t)(uintptr_t)&reg;
	expect("REGISTER", do_cmd(&ring, fd, URP_CMD_REGISTER, &reg_sqe,
				  sizeof(reg_sqe)), 0);

	/* --- negative: double register is rejected --- */
	expect("REGISTER_DUP", do_cmd(&ring, fd, URP_CMD_REGISTER, &reg_sqe,
				      sizeof(reg_sqe)), -EEXIST);

	/*
	 * --- SEND validation (design 31 PR3b) ---
	 *
	 * The SEND data path is live now: a valid SEND posts a zero-copy frame on
	 * the endpoint's QP and completes asynchronously. This standalone PoC has
	 * no peer to ACK an RC send, so it exercises only the synchronous
	 * validation edges here; the full fast->uds / fast->fast round trip that
	 * asserts delivered bytes is the VM pair-test (Phase 10i) with a live
	 * acceptor. A stream_id of 0 is reserved (streams are app-assigned,
	 * non-zero) and an out-of-range buffer index is rejected before any post.
	 */
	memset(&data, 0, sizeof(data));
	data.buf_index = 0;
	data.len = 64;
	data.stream_id = 0;
	expect("SEND_STREAM0", do_cmd(&ring, fd, URP_CMD_SEND, &data,
				      sizeof(data)), -EINVAL);

	memset(&data, 0, sizeof(data));
	data.buf_index = count;		/* one past the end */
	data.len = 64;
	data.stream_id = 1;
	expect("SEND_ERANGE", do_cmd(&ring, fd, URP_CMD_SEND, &data,
				     sizeof(data)), -ERANGE);

	/*
	 * --- RECV validation (design 31 PR4) ---
	 *
	 * The RECV data path arms a donated buffer as a zero-copy RDMA landing
	 * slot on the endpoint QP and completes asynchronously (CQE32: payload len
	 * in res, buf_index|stream_id in res2) when the NIC fills it. This
	 * standalone PoC has no peer to send an inbound frame, so it exercises only
	 * the synchronous validation edges; the full uds->fast / fast->fast round
	 * trip that asserts delivered bytes is the VM pair-test (Phase 10i). A
	 * zero-length donation and an out-of-range buffer index are rejected before
	 * any recv WR is posted.
	 */
	memset(&data, 0, sizeof(data));
	data.buf_index = 0;
	data.len = 0;			/* zero-length donation is meaningless */
	expect("RECV_ZEROLEN", do_cmd(&ring, fd, URP_CMD_RECV, &data,
				      sizeof(data)), -EINVAL);

	memset(&data, 0, sizeof(data));
	data.buf_index = count;		/* one past the end */
	data.len = 64;
	expect("RECV_ERANGE", do_cmd(&ring, fd, URP_CMD_RECV, &data,
				     sizeof(data)), -ERANGE);

	/* --- unregister, then a second unregister has nothing to release --- */
	expect("UNREGISTER", do_cmd(&ring, fd, URP_CMD_UNREGISTER, NULL, 0), 0);
	expect("UNREGISTER_AGAIN",
	       do_cmd(&ring, fd, URP_CMD_UNREGISTER, NULL, 0), -ENXIO);

	/* --- negative: a misaligned base is rejected before any pin --- */
	reg.base = (uint64_t)(uintptr_t)pool + 1;	/* not page aligned */
	reg_sqe.arg = (uint64_t)(uintptr_t)&reg;
	expect("REGISTER_MISALIGN",
	       do_cmd(&ring, fd, URP_CMD_REGISTER, &reg_sqe, sizeof(reg_sqe)),
	       -EINVAL);

	munmap(pool, pool_len);
	io_uring_queue_exit(&ring);
	close(fd);

	if (failures) {
		printf("URP_FAST_POC_FAIL failures=%d\n", failures);
		return 1;
	}
	printf("URP_FAST_POC_OK\n");
	return 0;
}
