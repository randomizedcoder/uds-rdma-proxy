//! Kernel runtime shims required when the crate is linked into a Linux
//! kernel module. These items wire `alloc`'s `GlobalAlloc` to kernel-supplied
//! `urp_kalloc` / `urp_kfree` and provide the `#[panic_handler]` /
//! `#[alloc_error_handler]` that the kernel environment lacks.

use core::alloc::{GlobalAlloc, Layout};
use core::panic::PanicInfo;
use core::ptr;

extern "C" {
    /// Kernel-supplied: must allocate `size` bytes aligned to `align`. May
    /// sleep (the kernel module side calls this only from sleepable context).
    /// Must return a non-null pointer on success or null on failure.
    fn urp_kalloc(size: usize, align: usize) -> *mut u8;

    /// Kernel-supplied: free a previously allocated region.
    fn urp_kfree(ptr: *mut u8, size: usize, align: usize);

    /// Kernel-supplied: terminal abort (calls `BUG()` or equivalent). Must
    /// not return.
    fn urp_panic_abort() -> !;
}

struct KernelAllocator;

unsafe impl GlobalAlloc for KernelAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        urp_kalloc(layout.size(), layout.align())
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        urp_kfree(ptr, layout.size(), layout.align());
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        let p = self.alloc(layout);
        if !p.is_null() {
            ptr::write_bytes(p, 0, layout.size());
        }
        p
    }
}

#[global_allocator]
static GLOBAL: KernelAllocator = KernelAllocator;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    // Information is intentionally discarded -- formatting through core::fmt
    // pulls in a lot of code we don't want in a kernel module. The kernel
    // side surfaces the abort via dmesg.
    //
    // Note: we intentionally do NOT register a `#[alloc_error_handler]`
    // because that attribute is still unstable on the Rust release we ship
    // with. The default behavior in `alloc` (since Rust 1.68) is to invoke
    // `panic!`, which routes through this handler -- giving us the same
    // abort path with no nightly feature dependency.
    unsafe { urp_panic_abort() }
}
