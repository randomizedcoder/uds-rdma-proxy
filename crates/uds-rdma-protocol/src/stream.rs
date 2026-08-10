//! Stream SYN/FIN/RST state machine (design 28 E2).
//!
//! Pure Rust twin of the kernel C state machine in `kernel/urp_stream.c`
//! (`urp_stream_next_state`). The two must stay in lock-step: the C KUnit
//! table in `kernel/urp_test.c` (`test_stream_next_state`) and the Rust
//! table test below enumerate the identical 6-state x 5-event matrix, so a
//! divergence in either implementation fails a test on that side.
//!
//! State values match the UAPI `enum urp_stream_state`; action bit values
//! match the `URP_STREAM_ACT_*` defines in `kernel/urp.h`.

/// Stream lifecycle state. Discriminants match `enum urp_stream_state` in
/// `kernel/include/uapi/linux/urp.h`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum StreamState {
    SynSent = 0,
    SynReceived = 1,
    Established = 2,
    FinWait = 3,
    CloseWait = 4,
    Closed = 5,
}

/// Internal (non-wire) state-machine events. Mirror
/// `enum urp_stream_event` in `kernel/urp.h`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StreamEvent {
    RxSyn,
    RxFin,
    RxRst,
    TxFin,
    TxRst,
}

/// Side effects the caller must apply after a transition. Bit values match
/// the `URP_STREAM_ACT_*` defines in `kernel/urp.h`.
pub const ACT_SHUTDOWN_WR: u8 = 1 << 0;
pub const ACT_SHUTDOWN_RDWR: u8 = 1 << 1;
pub const ACT_DESTROY: u8 = 1 << 2;

/// Result of a transition: the next state, the side-effect bitmask, and
/// whether the event was accepted (an `RxSyn` on a closing/closed stream is
/// rejected -- the C side maps `accepted == false` to `-EEXIST`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Transition {
    pub next: StreamState,
    pub actions: u8,
    pub accepted: bool,
}

/// Pure `(state, event) -> transition`. Behaviourally identical to the C
/// `urp_stream_next_state()`.
///
/// Note: the `RxSyn` that *creates* an inbound stream is a distinct event
/// (create -> `SynReceived`), owned by the C handler; this models `RxSyn`
/// on an already-known stream.
pub fn next_state(cur: StreamState, ev: StreamEvent) -> Transition {
    use StreamEvent::*;
    use StreamState::*;

    let mut t = Transition {
        next: cur,
        actions: 0,
        accepted: true,
    };

    match ev {
        RxSyn => match cur {
            SynSent | SynReceived | Established => t.next = Established,
            _ => t.accepted = false, // -EEXIST: SYN on closing/closed
        },
        RxFin => {
            // Peer half-close: always SHUT_WR; advance EST / FIN_WAIT.
            t.actions = ACT_SHUTDOWN_WR;
            match cur {
                Established => t.next = CloseWait,
                FinWait => t.next = Closed,
                _ => {}
            }
        }
        RxRst => {
            t.next = Closed;
            t.actions = ACT_SHUTDOWN_RDWR | ACT_DESTROY;
        }
        TxFin => match cur {
            Established => t.next = FinWait,
            CloseWait => t.next = Closed,
            _ => {}
        },
        TxRst => {
            t.next = Closed;
            t.actions = ACT_SHUTDOWN_RDWR;
        }
    }
    t
}

#[cfg(test)]
mod tests {
    use super::StreamEvent::*;
    use super::StreamState::*;
    use super::*;

    const WR: u8 = ACT_SHUTDOWN_WR;
    const RDWR: u8 = ACT_SHUTDOWN_RDWR;
    const RDWR_DESTROY: u8 = ACT_SHUTDOWN_RDWR | ACT_DESTROY;

    /// The full 6-state x 5-event matrix. Row-for-row identical to the
    /// KUnit `test_stream_next_state` table in `kernel/urp_test.c` -- keep
    /// the two in sync. Columns: (state, event) -> (next, actions, accepted).
    #[rustfmt::skip]
    #[test]
    fn transition_matrix() {
        let cases: &[(StreamState, StreamEvent, StreamState, u8, bool)] = &[
            // RxSyn: idempotent handshake advance, else reject
            (SynSent,     RxSyn, Established, 0, true),
            (SynReceived, RxSyn, Established, 0, true),
            (Established,  RxSyn, Established, 0, true),
            (FinWait,     RxSyn, FinWait,     0, false),
            (CloseWait,   RxSyn, CloseWait,   0, false),
            (Closed,      RxSyn, Closed,      0, false),
            // RxFin: always SHUT_WR; advance EST/FIN_WAIT
            (SynSent,     RxFin, SynSent,     WR, true),
            (SynReceived, RxFin, SynReceived, WR, true),
            (Established,  RxFin, CloseWait,   WR, true),
            (FinWait,     RxFin, Closed,      WR, true),
            (CloseWait,   RxFin, CloseWait,   WR, true),
            (Closed,      RxFin, Closed,      WR, true),
            // RxRst: any -> Closed, RDWR + DESTROY
            (SynSent,     RxRst, Closed, RDWR_DESTROY, true),
            (SynReceived, RxRst, Closed, RDWR_DESTROY, true),
            (Established,  RxRst, Closed, RDWR_DESTROY, true),
            (FinWait,     RxRst, Closed, RDWR_DESTROY, true),
            (CloseWait,   RxRst, Closed, RDWR_DESTROY, true),
            (Closed,      RxRst, Closed, RDWR_DESTROY, true),
            // TxFin: advance EST->FIN_WAIT, CLOSE_WAIT->CLOSED
            (SynSent,     TxFin, SynSent,     0, true),
            (SynReceived, TxFin, SynReceived, 0, true),
            (Established,  TxFin, FinWait,     0, true),
            (FinWait,     TxFin, FinWait,     0, true),
            (CloseWait,   TxFin, Closed,      0, true),
            (Closed,      TxFin, Closed,      0, true),
            // TxRst: any -> Closed, RDWR only (no destroy)
            (SynSent,     TxRst, Closed, RDWR, true),
            (SynReceived, TxRst, Closed, RDWR, true),
            (Established,  TxRst, Closed, RDWR, true),
            (FinWait,     TxRst, Closed, RDWR, true),
            (CloseWait,   TxRst, Closed, RDWR, true),
            (Closed,      TxRst, Closed, RDWR, true),
        ];

        // Full matrix must be enumerated (6 states x 5 events).
        assert_eq!(cases.len(), 30, "transition matrix must be complete");

        for (cur, ev, next, actions, accepted) in cases {
            let t = next_state(*cur, *ev);
            assert_eq!(t.next, *next, "{cur:?}/{ev:?}: next");
            assert_eq!(t.actions, *actions, "{cur:?}/{ev:?}: actions");
            assert_eq!(t.accepted, *accepted, "{cur:?}/{ev:?}: accepted");
        }
    }
}
