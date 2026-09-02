//! `urp-exporter` -- a lightweight Prometheus exporter for the `urp` kernel
//! module (design 39). The binary (`src/main.rs`) is a thin shell over this
//! library so every piece -- arg parsing, the netlink scrape, the exposition
//! renderer, and the hand-rolled HTTP responder -- is unit-testable in-process
//! without a socket or the module loaded.
//!
//! Design constraint (design 39 §39.3): the mesh is per-node-CPU / copy-path
//! bound, so the exporter must steal negligible CPU. That is enforced here by
//! construction -- single thread, blocking, no async runtime, reused render
//! buffer (zero steady-state alloc on the hot path), and a min-interval scrape
//! cache -- and tested against (`render` micro-bench + the table tests).

pub mod config;
pub mod exporter;
pub mod http;
pub mod render;
pub mod scrape;

/// Synthetic-fleet generator for tests, benches, and the stress runner. Only
/// compiled under test or the `mock` feature so it never reaches production.
#[cfg(any(test, feature = "mock"))]
pub mod mock;

pub use config::Config;
pub use exporter::Exporter;

// Under `cargo test` a wrapping global allocator counts heap allocations so the
// render hot path can be asserted zero-alloc (design 39 §39.8a). It is
// `#[cfg(test)]` only -- production links the system allocator directly.
#[cfg(test)]
mod alloc_counter {
    use std::alloc::{GlobalAlloc, Layout, System};
    use std::sync::atomic::{AtomicU64, Ordering};

    /// Live count of `alloc` calls since process start.
    pub static ALLOCS: AtomicU64 = AtomicU64::new(0);

    pub struct Counting;

    // SAFETY: pure pass-through to the system allocator; the only added work is
    // a relaxed counter bump, which touches no allocator state.
    unsafe impl GlobalAlloc for Counting {
        unsafe fn alloc(&self, l: Layout) -> *mut u8 {
            ALLOCS.fetch_add(1, Ordering::Relaxed);
            System.alloc(l)
        }
        unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
            System.dealloc(p, l);
        }
        unsafe fn realloc(&self, p: *mut u8, l: Layout, n: usize) -> *mut u8 {
            ALLOCS.fetch_add(1, Ordering::Relaxed);
            System.realloc(p, l, n)
        }
    }
}

#[cfg(test)]
#[global_allocator]
static GLOBAL: alloc_counter::Counting = alloc_counter::Counting;

/// Snapshot the process-wide allocation counter (test-only). Used by the render
/// alloc bench to bracket a single `render_into_scratch` call.
#[cfg(test)]
pub fn alloc_count() -> u64 {
    use std::sync::atomic::Ordering;
    alloc_counter::ALLOCS.load(Ordering::Relaxed)
}
