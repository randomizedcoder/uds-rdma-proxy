//! High-level errors surfaced by the CLI. Errno values from the kernel
//! get translated into these so the user sees friendly messages.

use std::io;
use thiserror::Error;

#[derive(Debug, Error)]
pub enum UrpError {
    #[error("urp kernel module not loaded")]
    KernelModuleNotLoaded,

    #[error("endpoint not found")]
    EndpointNotFound,

    #[error("endpoint already exists")]
    EndpointExists,

    #[error("invalid argument: {0}")]
    InvalidArgument(String),

    #[error("requires root or CAP_NET_ADMIN")]
    PermissionDenied,

    #[error("netlink error: {0}")]
    Netlink(String),

    #[error(transparent)]
    Io(#[from] io::Error),
}

impl UrpError {
    /// Map a raw errno to a UrpError. `ctx` is one of "resolve", "new", "del",
    /// "set", "get", "monitor" -- it lets us decide whether ENOENT is "module
    /// not loaded" (during family resolution) or "endpoint not found"
    /// (everywhere else).
    pub fn from_errno(errno: i32, ctx: &str) -> Self {
        let extack = String::new();
        Self::from_errno_with_extack(errno, ctx, extack)
    }

    pub fn from_errno_with_extack(errno: i32, ctx: &str, extack: String) -> Self {
        match errno {
            libc::ENOENT if ctx == "resolve" => UrpError::KernelModuleNotLoaded,
            libc::ENOENT => UrpError::EndpointNotFound,
            libc::EEXIST => UrpError::EndpointExists,
            libc::EPERM | libc::EACCES => UrpError::PermissionDenied,
            libc::EINVAL => {
                if extack.is_empty() {
                    UrpError::InvalidArgument("kernel rejected request".into())
                } else {
                    UrpError::InvalidArgument(extack)
                }
            }
            other => UrpError::Io(io::Error::from_raw_os_error(other)),
        }
    }
}
