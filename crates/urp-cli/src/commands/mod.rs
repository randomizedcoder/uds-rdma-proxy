pub mod add;
pub mod drain;
pub mod monitor;
pub mod remove;
pub mod set;
pub mod show;
pub mod stats;

use urp_netlink::uapi::{URP_NUM_QPS_MAX, URP_NUM_QPS_MIN};

/// clap value parser enforcing the kernel's num_qps policy range.
/// Shared by `add --num-qps` and `set --num-qps`.
pub(crate) fn num_qps_parser() -> clap::builder::RangedU64ValueParser<u32> {
    clap::builder::RangedU64ValueParser::new()
        .range((URP_NUM_QPS_MIN as u64)..=(URP_NUM_QPS_MAX as u64))
}
