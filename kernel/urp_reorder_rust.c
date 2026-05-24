// SPDX-License-Identifier: GPL-2.0
/*
 * UDS-RDMA Proxy (urp) -- reorder buffer (Rust FFI backend)
 *
 * Phase 3a Step 5b. Opt-in Rust backend for the reorder buffer.
 * Selected via CONFIG_URP_REORDER_RUST=y; in that build the linker
 * pulls in liburp_protocol_ffi.a (built from
 * crates/uds-rdma-protocol-ffi). This .c file is the thin
 * cast-and-forward shim that satisfies the urp_reorder.h interface
 * defined by the C rbtree backend, so the rest of the data path
 * doesn't change between the two builds.
 *
 * urp_reorder.h is intentionally backend-agnostic; the build picks
 * either urp_reorder.c (default) or urp_reorder_rust.c (opt-in) via
 * Kbuild's $(CONFIG_URP_REORDER_RUST). The urp_reorder struct
 * pointer is opaque to callers, so we can flip the underlying
 * representation without source changes elsewhere.
 *
 * Kernel-supplied symbols the Rust staticlib calls back into:
 *   urp_kalloc / urp_kfree -- GFP_KERNEL-equivalent allocator
 *   urp_panic_abort        -- terminal BUG path
 * are defined here so they live in the same translation unit as the
 * urp_reorder.h facade.
 */

#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "urp_reorder.h"
#include "include/urp_ffi.h"

/* ---- urp_reorder.h facade routed through urp_rust_reorder_* exports ---- */

struct urp_reorder *urp_reorder_alloc(u64 initial_expected, u32 max_buffered)
{
	if (max_buffered == 0)
		return NULL;
	return (struct urp_reorder *)urp_rust_reorder_new(initial_expected,
							  (size_t)max_buffered);
}

void urp_reorder_free(struct urp_reorder *rb)
{
	urp_rust_reorder_free((struct urp_rust_reorder *)rb);
}

int urp_reorder_insert(struct urp_reorder *rb, u64 seq,
		       const u8 *data, size_t data_len)
{
	return urp_rust_reorder_insert((struct urp_rust_reorder *)rb,
				       seq, data, data_len);
}

int urp_reorder_drain_next(struct urp_reorder *rb, u64 *out_seq,
			   u8 *out_data, size_t *inout_len)
{
	return urp_rust_reorder_drain_next((struct urp_rust_reorder *)rb,
					   out_seq, out_data, inout_len);
}

u64 urp_reorder_next_expected(const struct urp_reorder *rb)
{
	return urp_rust_reorder_next_expected((const struct urp_rust_reorder *)rb);
}

size_t urp_reorder_gap_count(const struct urp_reorder *rb)
{
	return urp_rust_reorder_gap_count((const struct urp_rust_reorder *)rb);
}

size_t urp_reorder_drain_pending(const struct urp_reorder *rb)
{
	return urp_rust_reorder_drain_pending((const struct urp_rust_reorder *)rb);
}

/* ---- Allocator callbacks for the Rust staticlib ---- */

/*
 * The Rust side wants size + alignment hints. The kernel slab
 * allocator doesn't take alignment as a parameter; for the sizes the
 * reorder buffer uses (Vec<u8> payloads + Box<UrpRustReorder>), kmalloc
 * already meets ARCH_KMALLOC_MINALIGN which covers any alignment Rust
 * would request for the structs in question. If a caller requests a
 * stronger alignment we kmalloc and verify post-hoc, falling back to
 * NULL so the Rust side returns ENOMEM cleanly.
 */
void *urp_kalloc(size_t size, size_t align)
{
	void *p = kmalloc(size, GFP_KERNEL);

	if (p && align > ARCH_KMALLOC_MINALIGN &&
	    ((uintptr_t)p & (align - 1))) {
		kfree(p);
		return NULL;
	}
	return p;
}

void urp_kfree(void *ptr, size_t size, size_t align)
{
	/* size + align are informational for the Rust side; the kernel
	 * slab allocator tracks the real allocation itself. */
	(void)size;
	(void)align;
	kfree(ptr);
}

void urp_panic_abort(void)
{
	pr_err("urp: Rust panic_abort callback invoked\n");
	BUG();
}
