//! C-ABI exports for the `ReorderBuffer`. See crate-level docs for ownership
//! rules. All functions are `unsafe` because the C caller is responsible for
//! upholding pointer validity / non-aliasing / lifetime.

use alloc::collections::VecDeque;
use alloc::vec::Vec;
use core::ffi::c_int;
use core::ptr;
use core::slice;

use uds_rdma_protocol::error::ProtocolError;
use uds_rdma_protocol::reorder::ReorderBuffer;

// errno values matching <linux/errno.h>. We can't depend on the kernel's
// uapi headers from Rust, so the constants are duplicated here (these match
// arch/x86/include/uapi/asm-generic/errno-base.h and asm-generic/errno.h).
const EINVAL: c_int = 22;
const ENOENT: c_int = 2;
const ENOMEM: c_int = 12;
const ENOBUFS: c_int = 105;
const EEXIST: c_int = 17;

/// Opaque handle. The C side declares `struct urp_rust_reorder;` and only
/// ever passes pointers around.
pub struct UrpRustReorder {
    inner: ReorderBuffer,
    /// Frames already drained by `insert()` but not yet handed out via
    /// `drain_next()`. We separate insert from drain at the FFI boundary
    /// because the C caller can't conveniently consume `Vec<(u64, Vec<u8>)>`.
    drain_queue: VecDeque<(u64, Vec<u8>)>,
}

fn protocol_err_to_errno(e: &ProtocolError) -> c_int {
    match e {
        ProtocolError::ReorderDuplicate { .. } => -EEXIST,
        ProtocolError::ReorderFull { .. } => -ENOBUFS,
        ProtocolError::CreditExhausted => -ENOBUFS,
        ProtocolError::BufferTooShort { .. }
        | ProtocolError::InvalidFrameType(_)
        | ProtocolError::InvalidFlags { .. } => -EINVAL,
    }
}

/// Allocate a new reorder buffer. Returns null on allocation failure.
#[no_mangle]
pub unsafe extern "C" fn urp_rust_reorder_new(
    initial_expected: u64,
    max_buffered: usize,
) -> *mut UrpRustReorder {
    let boxed = alloc::boxed::Box::new(UrpRustReorder {
        inner: ReorderBuffer::new(initial_expected, max_buffered),
        drain_queue: VecDeque::new(),
    });
    alloc::boxed::Box::into_raw(boxed)
}

/// Free a reorder buffer previously returned by `urp_rust_reorder_new`.
/// `rb` must not be used afterwards. Passing null is a no-op.
#[no_mangle]
pub unsafe extern "C" fn urp_rust_reorder_free(rb: *mut UrpRustReorder) {
    if rb.is_null() {
        return;
    }
    drop(alloc::boxed::Box::from_raw(rb));
}

/// Insert a frame.
///
/// On success returns 0; the frame may have been delivered immediately or
/// buffered for later. Drained frames (in-order ones now ready to deliver)
/// must be retrieved by repeatedly calling [`urp_rust_reorder_drain_next`]
/// until it returns `-ENOENT`.
///
/// Errors:
/// * `-EINVAL` if `rb` is null or `data` is null with non-zero `data_len`.
/// * `-EEXIST` if the sequence number was already delivered or already in
///   the buffer.
/// * `-ENOBUFS` if the buffer is at capacity.
/// * `-ENOMEM` if the internal copy allocation fails.
#[no_mangle]
pub unsafe extern "C" fn urp_rust_reorder_insert(
    rb: *mut UrpRustReorder,
    seq: u64,
    data: *const u8,
    data_len: usize,
) -> c_int {
    if rb.is_null() {
        return -EINVAL;
    }
    if data.is_null() && data_len != 0 {
        return -EINVAL;
    }
    let rb = &mut *rb;

    let buf: Vec<u8> = if data_len == 0 {
        Vec::new()
    } else {
        let src = slice::from_raw_parts(data, data_len);
        let mut v = Vec::new();
        if v.try_reserve_exact(data_len).is_err() {
            return -ENOMEM;
        }
        v.extend_from_slice(src);
        v
    };

    match rb.inner.insert(seq, buf) {
        Ok(drain_result) => {
            if rb
                .drain_queue
                .try_reserve(drain_result.delivered.len())
                .is_err()
            {
                return -ENOMEM;
            }
            for entry in drain_result.delivered {
                rb.drain_queue.push_back(entry);
            }
            0
        }
        Err(e) => protocol_err_to_errno(&e),
    }
}

/// Drain the next in-order frame.
///
/// Behaviour:
/// * If a frame is available and fits in `out_data` (`*inout_len` bytes):
///   writes the sequence number to `*out_seq`, copies the payload into
///   `out_data`, sets `*inout_len` to the actual payload length, returns 0.
/// * If a frame is available but `out_data` is too small: sets `*inout_len`
///   to the required size, leaves the frame on the queue, returns `-ENOBUFS`.
/// * If no frame is available: returns `-ENOENT`.
/// * On argument errors: returns `-EINVAL`.
///
/// `out_data` may be null only if `*inout_len` is 0 (used to peek the size).
#[no_mangle]
pub unsafe extern "C" fn urp_rust_reorder_drain_next(
    rb: *mut UrpRustReorder,
    out_seq: *mut u64,
    out_data: *mut u8,
    inout_len: *mut usize,
) -> c_int {
    if rb.is_null() || out_seq.is_null() || inout_len.is_null() {
        return -EINVAL;
    }
    let rb = &mut *rb;
    let cap = *inout_len;
    if out_data.is_null() && cap != 0 {
        return -EINVAL;
    }

    let front_len = match rb.drain_queue.front() {
        Some((_, payload)) => payload.len(),
        None => return -ENOENT,
    };
    if front_len > cap {
        *inout_len = front_len;
        return -ENOBUFS;
    }

    let (seq, payload) = rb
        .drain_queue
        .pop_front()
        .expect("front was Some immediately above");
    *out_seq = seq;
    *inout_len = payload.len();
    if !payload.is_empty() {
        ptr::copy_nonoverlapping(payload.as_ptr(), out_data, payload.len());
    }
    0
}

/// Returns the next expected sequence number for in-order delivery.
#[no_mangle]
pub unsafe extern "C" fn urp_rust_reorder_next_expected(
    rb: *const UrpRustReorder,
) -> u64 {
    if rb.is_null() {
        return 0;
    }
    (*rb).inner.next_expected()
}

/// Returns the number of frames currently buffered out-of-order (gap count).
#[no_mangle]
pub unsafe extern "C" fn urp_rust_reorder_gap_count(
    rb: *const UrpRustReorder,
) -> usize {
    if rb.is_null() {
        return 0;
    }
    (*rb).inner.gap_count()
}

/// Returns the number of frames waiting in the drain queue
/// (delivered-but-not-yet-popped).
#[no_mangle]
pub unsafe extern "C" fn urp_rust_reorder_drain_pending(
    rb: *const UrpRustReorder,
) -> usize {
    if rb.is_null() {
        return 0;
    }
    (*rb).drain_queue.len()
}
