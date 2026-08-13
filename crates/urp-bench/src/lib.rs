//! urp-bench pure core — Rust twin of `tools/urp-bench-core.{c,h}`.
//!
//! Design: `docs/design/30-urp-bench-io-uring.md` §30.5/§30.10 (work item
//! B2). Module-per-concern, 1:1 with the C core; the shared hex test
//! vectors in `frame::tests` are duplicated verbatim from
//! `tools/urp-bench-test.c` as the cross-language oracle, and the
//! `bench_differential` fuzz target (B6) keeps decode behavior identical.
//!
//! Everything here is safe Rust with no dependencies: `cargo test -p
//! urp-bench` runs in the nix sandbox and `cargo miri test -p urp-bench`
//! in the devshell. The io_uring backend (B4) is deliberately not in this
//! crate yet.

pub mod batch;
pub mod config;
pub mod deframe;
pub mod frame;
pub mod report;
pub mod stats;
pub mod tracker;

/// Error codes, 1:1 with the C core's `BENCH_E*` constants.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    /// Buffer shorter than a header.
    Short,
    /// Bad magic.
    Magic,
    /// Unknown version.
    Version,
    /// Reserved flag bit set.
    Flags,
    /// `payload_len` above cap.
    Cap,
    /// Payload byte mismatch.
    Corrupt,
    /// Echo for a seq never sent.
    Unknown,
    /// Duplicate echo / double recycle.
    Dup,
    /// Tracker window / ring full.
    Full,
    /// Ring or sample set empty.
    Empty,
    /// Index out of range.
    Range,
    /// Invalid configuration.
    Invalid,
}

impl core::fmt::Display for Error {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        let s = match self {
            Error::Short => "short buffer",
            Error::Magic => "bad magic",
            Error::Version => "unknown version",
            Error::Flags => "reserved flag set",
            Error::Cap => "payload over cap",
            Error::Corrupt => "payload corrupt",
            Error::Unknown => "unknown seq",
            Error::Dup => "duplicate",
            Error::Full => "window full",
            Error::Empty => "empty",
            Error::Range => "out of range",
            Error::Invalid => "invalid config",
        };
        f.write_str(s)
    }
}

impl std::error::Error for Error {}
