use crate::constants::{FRAME_HEADER_SIZE, ROCEV2_HEADER_OVERHEAD};

/// InfiniBand Path MTU values (discrete, not arbitrary).
///
/// These are the standard IB PMTU values that determine the maximum payload
/// in a single RDMA SEND/RECV operation.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum IbPmtu {
    Mtu256 = 256,
    Mtu512 = 512,
    Mtu1024 = 1024,
    Mtu2048 = 2048,
    Mtu4096 = 4096,
}

impl IbPmtu {
    /// The raw MTU value in bytes.
    #[inline]
    pub fn bytes(self) -> usize {
        self as usize
    }
}

/// Maximum frame payload that fits in a single RDMA message for the given PMTU.
///
/// Returns `pmtu - FRAME_HEADER_SIZE`.
#[inline]
pub fn max_payload_for_mtu(pmtu: IbPmtu) -> usize {
    pmtu.bytes() - FRAME_HEADER_SIZE
}

/// Total frame size (header + payload).
#[inline]
pub fn total_frame_size(payload_len: usize) -> usize {
    FRAME_HEADER_SIZE + payload_len
}

/// Whether a payload of the given length would exceed the PMTU.
#[inline]
pub fn would_fragment(payload_len: usize, pmtu: IbPmtu) -> bool {
    total_frame_size(payload_len) > pmtu.bytes()
}

/// Select the largest standard IB PMTU that fits within the Ethernet MTU
/// after subtracting RoCEv2 header overhead.
///
/// For example: Ethernet MTU 9000 - 44 overhead = 8956 available.
/// Largest IB PMTU that fits: 4096.
pub fn pmtu_for_ethernet_mtu(ethernet_mtu: usize) -> Option<IbPmtu> {
    let available = ethernet_mtu.saturating_sub(ROCEV2_HEADER_OVERHEAD);
    if available >= 4096 {
        Some(IbPmtu::Mtu4096)
    } else if available >= 2048 {
        Some(IbPmtu::Mtu2048)
    } else if available >= 1024 {
        Some(IbPmtu::Mtu1024)
    } else if available >= 512 {
        Some(IbPmtu::Mtu512)
    } else if available >= 256 {
        Some(IbPmtu::Mtu256)
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // -- max_payload_for_mtu --

    #[test]
    fn max_payload_mtu_256() {
        assert_eq!(max_payload_for_mtu(IbPmtu::Mtu256), 236);
    }

    #[test]
    fn max_payload_mtu_512() {
        assert_eq!(max_payload_for_mtu(IbPmtu::Mtu512), 492);
    }

    #[test]
    fn max_payload_mtu_1024() {
        assert_eq!(max_payload_for_mtu(IbPmtu::Mtu1024), 1004);
    }

    #[test]
    fn max_payload_mtu_2048() {
        assert_eq!(max_payload_for_mtu(IbPmtu::Mtu2048), 2028);
    }

    #[test]
    fn max_payload_mtu_4096() {
        assert_eq!(max_payload_for_mtu(IbPmtu::Mtu4096), 4076);
    }

    // -- pmtu_for_ethernet_mtu --

    #[test]
    fn ethernet_1500() {
        // 1500 - 44 = 1456 >= 1024
        assert_eq!(pmtu_for_ethernet_mtu(1500), Some(IbPmtu::Mtu1024));
    }

    #[test]
    fn ethernet_9000() {
        // 9000 - 44 = 8956 >= 4096
        assert_eq!(pmtu_for_ethernet_mtu(9000), Some(IbPmtu::Mtu4096));
    }

    #[test]
    fn ethernet_9216() {
        assert_eq!(pmtu_for_ethernet_mtu(9216), Some(IbPmtu::Mtu4096));
    }

    #[test]
    fn ethernet_300() {
        // 300 - 44 = 256 >= 256
        assert_eq!(pmtu_for_ethernet_mtu(300), Some(IbPmtu::Mtu256));
    }

    #[test]
    fn ethernet_too_small() {
        // 43 - 44 = 0 (saturating_sub)
        assert_eq!(pmtu_for_ethernet_mtu(43), None);
    }

    // -- would_fragment --

    #[test]
    fn no_fragmentation() {
        // 4076 payload + 20 header = 4096 == MTU
        assert!(!would_fragment(4076, IbPmtu::Mtu4096));
    }

    #[test]
    fn fragmentation() {
        // 4077 payload + 20 header = 4097 > 4096
        assert!(would_fragment(4077, IbPmtu::Mtu4096));
    }

    // -- total_frame_size --

    #[test]
    fn frame_size() {
        assert_eq!(total_frame_size(0), FRAME_HEADER_SIZE);
        assert_eq!(total_frame_size(100), FRAME_HEADER_SIZE + 100);
    }
}
