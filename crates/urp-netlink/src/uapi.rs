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

// --- wire frame constants (kernel/include/uapi/linux/urp.h) -----------------
// Mirrored here and pinned by the table-driven `wire_defines_match_kernel_header`
// parity test so a symbol used on the wire cannot silently drift from -- or go
// missing in -- the kernel header (design 40: the class of bug where
// URP_CONN_CAP_TSTAMP was referenced in C but never `#define`d).

pub const URP_FRAME_HEADER_SIZE: u32 = 20;
pub const URP_PONG_PAYLOAD_SIZE: u32 = 48;

/// DATA-frame flag byte [13] bits. SYN/FIN/RST occupy bits 0..2; the OWD
/// timestamp trailer takes the next free bit (design 40 §40.2 -- NOT BIT(1),
/// which the original spec mis-stated).
pub const URP_DATA_FLAG_SYN: u8 = 1 << 0;
pub const URP_DATA_FLAG_FIN: u8 = 1 << 1;
pub const URP_DATA_FLAG_RST: u8 = 1 << 2;
pub const URP_DATA_FLAG_TSTAMP: u8 = 1 << 3;
/// Length of the little-endian u64 `t_send_real` (ns) OWD trailer.
pub const URP_TSTAMP_TRAILER_LEN: u32 = 8;

/// CONTROL-frame flag bits. CREDIT_BYTES is bit 5 (bits 1..4 reserved by the
/// Rust protocol twin; see the kernel header comment).
pub const URP_CTRL_FLAG_CREDIT: u8 = 1 << 0;
pub const URP_CTRL_FLAG_CREDIT_BYTES: u8 = 1 << 5;
pub const URP_CREDIT_BYTES_PAYLOAD_SIZE: u32 = 8;

/// Connection capability bits, advertised in the CM private_data trailer and
/// negotiated by BOTH peers before the feature activates (design 35 §35.3).
pub const URP_CONN_CAP_WINDOW_BYTES: u8 = 1 << 0;
pub const URP_CONN_CAP_TSTAMP: u8 = 1 << 1;

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

    /// Evaluate a scalar `#define NAME VALUE` from kernel C header text, or
    /// `None` if `name` is not `#define`d. Supports the value forms the header
    /// actually uses: a decimal or `0x` hex literal (optional `U`/`u` suffix),
    /// and a single `(1 << N)` bit-shift. A trailing `/* ... */` or `// ...`
    /// comment is ignored. Deliberately fail-closed: an absent symbol, or a
    /// value form it cannot evaluate, returns `None` (never a silent 0) so the
    /// parity test below cannot pass by accident.
    fn eval_define(src: &str, name: &str) -> Option<i64> {
        let needle = format!("#define {}", name);
        // Match the whole token: "#define NAME" must be followed by whitespace,
        // so `URP_CONN_CAP` does not match `URP_CONN_CAP_WINDOW_BYTES`.
        let line = src.lines().find(|l| {
            let t = l.trim_start();
            t.starts_with(&needle)
                && t[needle.len()..]
                    .chars()
                    .next()
                    .is_some_and(|c| c.is_whitespace())
        })?;
        // Value = text after the name, minus any trailing comment.
        let after = line.trim_start()[needle.len()..].trim();
        let val = after
            .split("/*")
            .next()
            .unwrap_or("")
            .split("//")
            .next()
            .unwrap_or("")
            .trim()
            .trim_matches(|c| c == '(' || c == ')')
            .trim();
        let parse_int = |s: &str| -> Option<i64> {
            // Order matters: strip a `U`/`u` integer suffix first, then any stray
            // parens (e.g. the `3)` left of an `(1 << 3)U` rhs), then re-trim.
            let s = s
                .trim()
                .trim_matches(|c| c == '(' || c == ')')
                .trim_end_matches(['U', 'u'])
                .trim_matches(|c| c == '(' || c == ')')
                .trim();
            if let Some(hex) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
                i64::from_str_radix(hex, 16).ok()
            } else {
                s.parse::<i64>().ok()
            }
        };
        if let Some((lhs, rhs)) = val.split_once("<<") {
            Some(parse_int(lhs)? << parse_int(rhs)?)
        } else {
            parse_int(val)
        }
    }

    #[derive(Debug, Clone, Copy, PartialEq)]
    enum Outcome {
        /// The symbol must be `#define`d and evaluate to `value`.
        Present,
        /// The symbol must NOT be `#define`d (proves the checker fails closed).
        Absent,
    }

    /// A wire-constant parity row: the kernel `#define` symbol, the value the
    /// Rust mirror expects, a human description, and whether the symbol is
    /// expected present or absent.
    struct Case {
        name: &'static str,
        value: i64,
        desc: &'static str,
        outcome: Outcome,
    }

    // design 40: guard the wire/negotiation `#define` family against the exact
    // class of bug where `URP_CONN_CAP_TSTAMP` was referenced in C but never
    // `#define`d in the header (undetected because its only user, urp_test.c,
    // isn't compiled by `.#urp-ko`). This table re-parses the header for every
    // symbol the Rust mirror carries and asserts value AND presence -- so a
    // missing or drifted define fails here, in a tier that always compiles.
    #[test]
    fn wire_defines_match_kernel_header() {
        let candidates = [
            "../../kernel/include/uapi/linux/urp.h",
            "../../../kernel/include/uapi/linux/urp.h",
            "kernel/include/uapi/linux/urp.h",
        ];
        let path = candidates.iter().find(|c| Path::new(c).exists());
        let path = match path {
            Some(p) => *p,
            None => {
                eprintln!("skipping wire_defines_match_kernel_header: header not found");
                return;
            }
        };
        let txt = std::fs::read_to_string(path).expect("read header");

        let cases: &[Case] = &[
            // --- positive: each wire constant the Rust mirror carries is
            //     #define'd with the matching value. ---
            Case {
                name: "URP_FRAME_HEADER_SIZE",
                value: URP_FRAME_HEADER_SIZE as i64,
                desc: "20-byte LE frame header",
                outcome: Outcome::Present,
            },
            Case {
                name: "URP_PONG_PAYLOAD_SIZE",
                value: URP_PONG_PAYLOAD_SIZE as i64,
                desc: "PONG probe payload (drives URP_BUFFER_SIZE_MIN)",
                outcome: Outcome::Present,
            },
            Case {
                name: "URP_DATA_FLAG_SYN",
                value: URP_DATA_FLAG_SYN as i64,
                desc: "DATA flag bit 0",
                outcome: Outcome::Present,
            },
            Case {
                name: "URP_DATA_FLAG_FIN",
                value: URP_DATA_FLAG_FIN as i64,
                desc: "DATA flag bit 1",
                outcome: Outcome::Present,
            },
            Case {
                name: "URP_DATA_FLAG_RST",
                value: URP_DATA_FLAG_RST as i64,
                desc: "DATA flag bit 2",
                outcome: Outcome::Present,
            },
            Case {
                name: "URP_CTRL_FLAG_CREDIT",
                value: URP_CTRL_FLAG_CREDIT as i64,
                desc: "CONTROL frame-credit grant flag",
                outcome: Outcome::Present,
            },
            Case {
                name: "URP_CTRL_FLAG_CREDIT_BYTES",
                value: URP_CTRL_FLAG_CREDIT_BYTES as i64,
                desc: "CONTROL byte-credit grant flag (bit 5)",
                outcome: Outcome::Present,
            },
            Case {
                name: "URP_CREDIT_BYTES_PAYLOAD_SIZE",
                value: URP_CREDIT_BYTES_PAYLOAD_SIZE as i64,
                desc: "u64 cumulative byte-credit payload",
                outcome: Outcome::Present,
            },
            Case {
                name: "URP_CONN_CAP_WINDOW_BYTES",
                value: URP_CONN_CAP_WINDOW_BYTES as i64,
                desc: "byte-windowing capability bit",
                outcome: Outcome::Present,
            },
            // --- positive: the design-40 additions that regressed. This row is
            //     the direct regression guard for the latent PR-B1 bug. ---
            Case {
                name: "URP_CONN_CAP_TSTAMP",
                value: URP_CONN_CAP_TSTAMP as i64,
                desc: "OWD timestamp capability bit (design 40 PR-B)",
                outcome: Outcome::Present,
            },
            Case {
                name: "URP_TSTAMP_TRAILER_LEN",
                value: URP_TSTAMP_TRAILER_LEN as i64,
                desc: "8-byte LE t_send_real OWD trailer",
                outcome: Outcome::Present,
            },
            // --- boundary: the OWD flag must be exactly BIT(3). Bits 0..2 are
            //     SYN/FIN/RST; the original spec mis-stated it as BIT(1). If the
            //     header ever moves it back onto an occupied bit, this fails. ---
            Case {
                name: "URP_DATA_FLAG_TSTAMP",
                value: 1 << 3,
                desc: "OWD flag = next free DATA bit (BIT(3), not BIT(1))",
                outcome: Outcome::Present,
            },
            // --- negative: a symbol that is NOT in the header must evaluate to
            //     None. This proves eval_define fails closed -- the property the
            //     ORIGINAL parity test lacked, which let the missing
            //     URP_CONN_CAP_TSTAMP define ship. If this row ever "passes" by
            //     finding a value, the checker is broken and every Present row
            //     above is worthless. ---
            Case {
                name: "URP_CONN_CAP_NONEXISTENT_XYZZY",
                value: 0,
                desc: "bogus symbol -- checker must report Absent",
                outcome: Outcome::Absent,
            },
            // --- corner: a prefix of a real symbol must not match the longer
            //     one. `URP_CONN_CAP` is not itself defined; the whole-token
            //     match must return None rather than latching onto
            //     URP_CONN_CAP_WINDOW_BYTES. ---
            Case {
                name: "URP_CONN_CAP",
                value: 0,
                desc: "prefix of a real define -- must not partial-match",
                outcome: Outcome::Absent,
            },
        ];

        for (i, c) in cases.iter().enumerate() {
            let got = eval_define(&txt, c.name);
            match c.outcome {
                Outcome::Present => assert_eq!(
                    got,
                    Some(c.value),
                    "case {i} ({}): {} -- expected #define = {}, header gave {:?}",
                    c.name,
                    c.desc,
                    c.value,
                    got
                ),
                Outcome::Absent => assert_eq!(
                    got, None,
                    "case {i} ({}): {} -- expected symbol absent, header gave {:?}",
                    c.name, c.desc, got
                ),
            }
        }

        // --- corner: the flag/cap bits must be pairwise distinct. A value-only
        //     table would still pass if, say, TSTAMP were redefined onto RST's
        //     bit (each still equals "its own" define); this pins the structural
        //     no-collision invariant the bit layout depends on. The mirror
        //     constants are themselves header-pinned by the Present rows above,
        //     so this is transitively a header invariant. ---
        let data_flags = [
            URP_DATA_FLAG_SYN,
            URP_DATA_FLAG_FIN,
            URP_DATA_FLAG_RST,
            URP_DATA_FLAG_TSTAMP,
        ];
        for (a, fa) in data_flags.iter().enumerate() {
            for fb in &data_flags[a + 1..] {
                assert_eq!(fa & fb, 0, "DATA flag bits collide: {fa:#x} & {fb:#x}");
            }
        }
        assert_eq!(
            URP_CONN_CAP_WINDOW_BYTES & URP_CONN_CAP_TSTAMP,
            0,
            "CONN cap bits collide"
        );
    }

    // Unit-cover the eval_define helper's expression forms directly, so a
    // failure in the parity test above can be localized to data vs. parser.
    #[test]
    fn eval_define_truth_table() {
        let src = "\
#define A_DEC        42
#define A_HEX        0x2a
#define A_SHIFT      (1 << 5)
#define A_SHIFT_U    (1 << 3)U
#define A_SUFFIX     16U
#define A_PREFIX     7
#define A_PREFIX_X   9
";
        // Each case: (symbol, expected, description).
        let cases: &[(&str, Option<i64>, &str)] = &[
            // positive: plain decimal
            ("A_DEC", Some(42), "decimal literal"),
            // positive: hex literal
            ("A_HEX", Some(0x2a), "hex literal equals its decimal"),
            // positive: parenthesized bit-shift
            ("A_SHIFT", Some(32), "(1 << 5) evaluates"),
            // boundary: shift with a U suffix on the rhs
            ("A_SHIFT_U", Some(8), "(1 << 3)U suffix tolerated"),
            // boundary: integer with U suffix
            ("A_SUFFIX", Some(16), "trailing U stripped"),
            // negative: absent symbol -> None (fail-closed)
            ("A_MISSING", None, "absent symbol is None, never 0"),
            // corner: a prefix must not match the longer symbol name
            ("A_PREFIX", Some(7), "exact token, not the A_PREFIX_X line"),
        ];
        for (i, (name, want, desc)) in cases.iter().enumerate() {
            assert_eq!(eval_define(src, name), *want, "case {i} ({name}): {desc}");
        }
    }
}
