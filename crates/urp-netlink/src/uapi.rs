//! Mirror of `kernel/include/uapi/linux/urp.h`.
//!
//! The kernel header is the source of truth. The constants below are
//! hard-coded for fast compilation; `tests::uapi_constants_match_kernel_header`
//! re-parses the header and verifies they agree (skipped when the header
//! file isn't present, so the test still runs from a published tarball).

#![allow(dead_code)]

pub const URP_GENL_NAME: &str = "urp";
pub const URP_GENL_VERSION: u8 = 1;
pub const URP_GENL_MCGRP_EVENTS: &str = "events";

pub const URP_NAME_MAX: usize = 16;
pub const URP_PATH_MAX_LEN: usize = 108;
pub const URP_DEVICE_MAX: usize = 64;
pub const URP_PASSWORD_MAX: usize = 16;

pub const URP_NUM_QPS_MAX: u32 = 32;
pub const URP_NUM_QPS_MIN: u32 = 1;
pub const URP_BUFFER_COUNT_MIN: u32 = 16;
// = URP_FRAME_HEADER_SIZE (20) + URP_PONG_PAYLOAD_SIZE (48). A recv buffer must
// hold the largest frame urp emits (a PONG, 68 bytes); a smaller one overflows
// on the first PONG and crash-loops the QP. Kept in step with the kernel's
// uapi/linux/urp.h URP_BUFFER_SIZE_MIN (an expression there, so it is excluded
// from the uapi_constants_match_kernel_header token-parity test below).
pub const URP_BUFFER_SIZE_MIN: u32 = 68;
// = 1 MiB. A software/allocation ceiling, not a wire limit (payload_length is a
// u32 and the NIC segments large RC messages itself). Large slots are high-order
// compound pages, so pair with a small buffer_count. Kept in step with the
// kernel's uapi/linux/urp.h URP_BUFFER_SIZE_MAX (checked by the token-parity
// test below). See design 37.
pub const URP_BUFFER_SIZE_MAX: u32 = 1048576;

pub const URP_DEFAULT_PORT: u16 = 4791;

/// Generic netlink control family id (well-known).
pub const GENL_ID_CTRL: u16 = 16;

// --- enum urp_cmd ---
#[repr(u8)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum UrpCmd {
    Unspec = 0,
    NewEndpoint = 1,
    DelEndpoint = 2,
    SetEndpoint = 3,
    GetEndpoint = 4,
}

// --- enum urp_attr ---
#[repr(u16)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum UrpAttr {
    Unspec = 0,
    Endpoint = 1,
}

// --- enum urp_endpoint_attr ---
#[repr(u16)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum UrpEndpointAttr {
    Unspec = 0,
    Name = 1,
    ListenPath = 2,
    ConnectPath = 3,
    RdmaDevice = 4,
    PeerAddr = 5,
    BindAddr = 6,
    NumQps = 7,
    BufferCount = 8,
    BufferSize = 9,
    Password = 10,
    State = 11,
    Qps = 12,
    Streams = 13,
    Stats = 14,
    Mode = 15,
    Kind = 16,
    Interarrival = 17,
    Owd = 18,
}

// --- enum urp_hist_attr (design 40 §40.1) ---
#[repr(u16)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum UrpHistAttr {
    Unspec = 0,
    Stride = 1,
    Buckets = 2,
    SumNs = 3,
    Count = 4,
}

// --- enum urp_owd_attr (design 40 §40.2), nested in URP_ENDPOINT_A_OWD ---
#[repr(u16)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum UrpOwdAttr {
    Unspec = 0,
    Hist = 1,          // NLA_NESTED -- one urp_hist_attr set
    ClockOffsetNs = 2, // NLA_U64 -- PTP servo offset, 0 = unknown
    Anomalies = 3,     // NLA_U64 -- skew-rejected sample count
}

/// Number of histogram buckets (14 finite le edges + +Inf), mirroring
/// `URP_HIST_NBUCKETS` in `kernel/urp_hist.h`.
pub const URP_HIST_NBUCKETS: usize = 15;

/// Number of OWD histogram buckets (13 finite le edges + +Inf), mirroring
/// `URP_OWD_NBUCKETS` in `kernel/urp_hist.h`. The OWD nest reuses the
/// `urp_hist_attr` codec but carries only 14 bucket counts.
pub const URP_OWD_NBUCKETS: usize = 14;

/// enum urp_ep_mode -- endpoint operating mode (URP_ENDPOINT_A_MODE payload).
pub const URP_EP_MODE_MULTISTREAM: u8 = 0;
pub const URP_EP_MODE_K0: u8 = 1;

/// enum urp_ep_kind -- endpoint data path (URP_ENDPOINT_A_KIND payload).
/// uds = the copy path (AF_UNIX pump, default); fast = the zero-copy path
/// driven by the app over /dev/urp (design 31, requires CONFIG_URP_FAST).
pub const URP_EP_KIND_UDS: u8 = 0;
pub const URP_EP_KIND_FAST: u8 = 1;

// --- enum urp_qp_attr ---
#[repr(u16)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum UrpQpAttr {
    Unspec = 0,
    Index = 1,
    State = 2,
    RttNs = 3,
    TxBytes = 4,
    RxBytes = 5,
    TxFrames = 6,
    RxFrames = 7,
}

// --- enum urp_stream_attr ---
#[repr(u16)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum UrpStreamAttr {
    Unspec = 0,
    Id = 1,
    State = 2,
    TxBytes = 3,
    RxBytes = 4,
    ReorderDepth = 5,
    CreditsLocal = 6,
    CreditsRemote = 7,
}

// --- enum urp_stats_attr ---
#[repr(u16)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum UrpStatsAttr {
    Unspec = 0,
    ActiveStreams = 1,
    TxBytes = 2,
    RxBytes = 3,
    TxFrames = 4,
    RxFrames = 5,
    CreditStalls = 6,
    ReorderInsertions = 7,
    ReorderDrops = 8,
    BufferAllocFails = 9,
    AuthFailures = 10,
}

// --- enum urp_endpoint_state ---
#[repr(u8)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum UrpEndpointState {
    Creating = 0,
    Active = 1,
    Draining = 2,
    Stopped = 3,
}

impl UrpEndpointState {
    pub fn from_u8(v: u8) -> Option<Self> {
        match v {
            0 => Some(Self::Creating),
            1 => Some(Self::Active),
            2 => Some(Self::Draining),
            3 => Some(Self::Stopped),
            _ => None,
        }
    }
    pub fn as_str(&self) -> &'static str {
        match self {
            Self::Creating => "creating",
            Self::Active => "active",
            Self::Draining => "draining",
            Self::Stopped => "stopped",
        }
    }
    /// Inverse of [`as_str`]; maps a decoded state string (as carried on
    /// [`crate::format::Endpoint::state`]) back to the enum. Unknown -> None.
    pub fn from_str(s: &str) -> Option<Self> {
        match s {
            "creating" => Some(Self::Creating),
            "active" => Some(Self::Active),
            "draining" => Some(Self::Draining),
            "stopped" => Some(Self::Stopped),
            _ => None,
        }
    }
}

// --- enum urp_qp_state ---
#[repr(u8)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum UrpQpState {
    Qualifying = 0,
    Active = 1,
    Draining = 2,
    Removed = 3,
}

impl UrpQpState {
    pub fn from_u8(v: u8) -> Option<Self> {
        match v {
            0 => Some(Self::Qualifying),
            1 => Some(Self::Active),
            2 => Some(Self::Draining),
            3 => Some(Self::Removed),
            _ => None,
        }
    }
    pub fn as_str(&self) -> &'static str {
        match self {
            Self::Qualifying => "qualifying",
            Self::Active => "active",
            Self::Draining => "draining",
            Self::Removed => "removed",
        }
    }
}

// --- enum urp_stream_state ---
#[repr(u8)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum UrpStreamState {
    SynSent = 0,
    SynReceived = 1,
    Established = 2,
    FinWait = 3,
    CloseWait = 4,
    Closed = 5,
}

impl UrpStreamState {
    pub fn from_u8(v: u8) -> Option<Self> {
        match v {
            0 => Some(Self::SynSent),
            1 => Some(Self::SynReceived),
            2 => Some(Self::Established),
            3 => Some(Self::FinWait),
            4 => Some(Self::CloseWait),
            5 => Some(Self::Closed),
            _ => None,
        }
    }
    pub fn as_str(&self) -> &'static str {
        match self {
            Self::SynSent => "syn-sent",
            Self::SynReceived => "syn-received",
            Self::Established => "established",
            Self::FinWait => "fin-wait",
            Self::CloseWait => "close-wait",
            Self::Closed => "closed",
        }
    }
}

// --- nl/genl wire protocol constants (from <linux/netlink.h>, <linux/genetlink.h>) ---
pub const NLMSG_NOOP: u16 = 0x1;
pub const NLMSG_ERROR: u16 = 0x2;
pub const NLMSG_DONE: u16 = 0x3;

pub const NLM_F_REQUEST: u16 = 0x01;
pub const NLM_F_MULTI: u16 = 0x02;
pub const NLM_F_ACK: u16 = 0x04;
pub const NLM_F_DUMP: u16 = 0x300; // ROOT|MATCH

pub const NLA_F_NESTED: u16 = 0x8000;

// genl ctrl
pub const CTRL_CMD_GETFAMILY: u8 = 3;
pub const CTRL_ATTR_FAMILY_ID: u16 = 1;
pub const CTRL_ATTR_FAMILY_NAME: u16 = 2;
pub const CTRL_ATTR_MCAST_GROUPS: u16 = 7;
pub const CTRL_ATTR_MCAST_GRP_NAME: u16 = 1;
pub const CTRL_ATTR_MCAST_GRP_ID: u16 = 2;

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::Path;

    #[test]
    fn uapi_constants_match_kernel_header() {
        // Try a few common locations; this test file lives at
        // crates/urp-cli/src/uapi.rs, so the kernel header is two levels up.
        let candidates = [
            "../../kernel/include/uapi/linux/urp.h",
            "../../../kernel/include/uapi/linux/urp.h",
            "kernel/include/uapi/linux/urp.h",
        ];
        let mut found = None;
        for c in &candidates {
            if Path::new(c).exists() {
                found = Some(*c);
                break;
            }
        }
        let path = match found {
            Some(p) => p,
            None => {
                eprintln!("skipping uapi_constants_match_kernel_header: header not found");
                return;
            }
        };
        let txt = std::fs::read_to_string(path).expect("read header");

        let pairs: &[(&str, String)] = &[
            ("URP_NAME_MAX", URP_NAME_MAX.to_string()),
            ("URP_PATH_MAX_LEN", URP_PATH_MAX_LEN.to_string()),
            ("URP_DEVICE_MAX", URP_DEVICE_MAX.to_string()),
            ("URP_PASSWORD_MAX", URP_PASSWORD_MAX.to_string()),
            ("URP_NUM_QPS_MAX", URP_NUM_QPS_MAX.to_string()),
            ("URP_NUM_QPS_MIN", URP_NUM_QPS_MIN.to_string()),
            ("URP_BUFFER_COUNT_MIN", URP_BUFFER_COUNT_MIN.to_string()),
            ("URP_BUFFER_SIZE_MAX", URP_BUFFER_SIZE_MAX.to_string()),
            ("URP_DEFAULT_PORT", URP_DEFAULT_PORT.to_string()),
        ];

        for (name, expected) in pairs {
            let needle = format!("#define {}", name);
            let line = txt
                .lines()
                .find(|l| l.trim_start().starts_with(&needle))
                .unwrap_or_else(|| panic!("kernel header missing #define {}", name));
            // Tokens: "#define", NAME, VALUE, [optional comment...]
            let mut parts = line.split_whitespace();
            let _def = parts.next();
            let _name = parts.next();
            let val = parts.next().unwrap_or("").trim_end_matches('U');
            assert_eq!(
                val, expected,
                "kernel #define {} = {} but rust constant = {}",
                name, val, expected
            );
        }

        // Spot-check that the family name string is right.
        assert!(txt.contains("\"urp\""));
        assert!(txt.contains("\"events\""));
    }
}
