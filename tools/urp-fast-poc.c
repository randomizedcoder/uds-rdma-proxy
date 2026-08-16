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
	const char *dev = (argc > 1) ? argv[1] : URP_CMD_DEVICE_PATH;
	uint32_t buf_size = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0) : 4096;
	uint32_t count = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 0) : 8;
	size_t pool_len = (size_t)buf_size * count;
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

	ret = io_uring_queue_init(8, &ring, 0);
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

	/* --- happy path: register the pool (this pins it, FOLL_LONGTERM) --- */
	reg.base = (uint64_t)(uintptr_t)pool;
	reg.len = pool_len;
	reg.buf_size = buf_size;
	reg.count = count;
	reg.flags = 0;
	reg.__resv = 0;
	memset(&reg_sqe, 0, sizeof(reg_sqe));
	reg_sqe.arg = (uint64_t)(uintptr_t)&reg;
	expect("REGISTER", do_cmd(&ring, fd, URP_CMD_REGISTER, &reg_sqe,
				  sizeof(reg_sqe)), 0);

	/* --- negative: double register is rejected --- */
	expect("REGISTER_DUP", do_cmd(&ring, fd, URP_CMD_REGISTER, &reg_sqe,
				      sizeof(reg_sqe)), -EEXIST);

	/* --- SEND path reaches the validator, then reports not-yet-implemented --- */
	memset(&data, 0, sizeof(data));
	data.buf_index = 0;
	data.len = 64;
	expect("SEND_ENOSYS", do_cmd(&ring, fd, URP_CMD_SEND, &data,
				     sizeof(data)), -ENOSYS);

	/* --- SEND with an out-of-range buffer index is rejected by validation --- */
	memset(&data, 0, sizeof(data));
	data.buf_index = count;		/* one past the end */
	data.len = 64;
	expect("SEND_ERANGE", do_cmd(&ring, fd, URP_CMD_SEND, &data,
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
