use crate::error::ProtocolError;

/// Per-QP credit-based flow control state.
///
/// Sender tracks `send_credits` (decremented on each send, replenished by
/// grants from the receiver). Receiver tracks `credits_to_grant` (incremented
/// when a receive buffer is re-posted, sent when above threshold).
///
/// See design doc Section 5.3 for the full state machine.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CreditState {
    send_credits: u16,
    credits_to_grant: u16,
    threshold: u16,
    initial_credits: u16,
}

impl CreditState {
    /// Create a new credit state with `initial_credits` send credits.
    /// The grant threshold is set to `initial_credits / 4`.
    pub fn new(initial_credits: u16) -> Self {
        Self {
            send_credits: initial_credits,
            credits_to_grant: 0,
            threshold: initial_credits / 4,
            initial_credits,
        }
    }

    /// Whether the sender has credits remaining to post a send.
    #[inline]
    pub fn can_send(&self) -> bool {
        self.send_credits > 0
    }

    /// Consume one send credit. Returns `CreditExhausted` if none remain.
    pub fn consume(&mut self) -> Result<(), ProtocolError> {
        if self.send_credits == 0 {
            return Err(ProtocolError::CreditExhausted);
        }
        self.send_credits -= 1;
        Ok(())
    }

    /// Grant `n` credits (received from the peer).
    pub fn grant(&mut self, n: u16) {
        self.send_credits = self.send_credits.saturating_add(n);
    }

    /// Record that a receive buffer has been consumed and re-posted.
    /// Call this on the receiver side after processing a received frame.
    pub fn record_recv(&mut self) {
        self.credits_to_grant = self.credits_to_grant.saturating_add(1);
    }

    /// Number of credits accumulated to grant to the peer.
    #[inline]
    pub fn pending_grants(&self) -> u16 {
        self.credits_to_grant
    }

    /// Whether accumulated grants have reached the threshold.
    #[inline]
    pub fn should_grant(&self) -> bool {
        self.threshold == 0 || self.credits_to_grant >= self.threshold
    }

    /// Take all pending grants (to piggyback on a frame). Resets to 0.
    pub fn take_grants(&mut self) -> u16 {
        let grants = self.credits_to_grant;
        self.credits_to_grant = 0;
        grants
    }

    /// Current number of send credits.
    #[inline]
    pub fn send_credits(&self) -> u16 {
        self.send_credits
    }

    /// The initial credit count this state was created with.
    #[inline]
    pub fn initial_credits(&self) -> u16 {
        self.initial_credits
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn initial_state() {
        let cs = CreditState::new(128);
        assert_eq!(cs.send_credits(), 128);
        assert!(cs.can_send());
        assert_eq!(cs.pending_grants(), 0);
        assert_eq!(cs.initial_credits(), 128);
        // threshold = 128 / 4 = 32
        assert!(!cs.should_grant());
    }

    #[test]
    fn consume_all() {
        let mut cs = CreditState::new(128);
        for i in 0..128 {
            assert!(cs.can_send(), "should be able to send at iteration {i}");
            cs.consume().unwrap();
        }
        assert!(!cs.can_send());
        assert_eq!(cs.send_credits(), 0);
    }

    #[test]
    fn consume_below_zero() {
        let mut cs = CreditState::new(1);
        cs.consume().unwrap();
        assert_eq!(cs.consume(), Err(ProtocolError::CreditExhausted));
    }

    #[test]
    fn grant_restores() {
        let mut cs = CreditState::new(10);
        for _ in 0..10 {
            cs.consume().unwrap();
        }
        assert!(!cs.can_send());
        cs.grant(5);
        assert!(cs.can_send());
        assert_eq!(cs.send_credits(), 5);
    }

    #[test]
    fn record_recv_and_threshold() {
        let mut cs = CreditState::new(128);
        // threshold = 32
        for _ in 0..31 {
            cs.record_recv();
        }
        assert!(!cs.should_grant());
        assert_eq!(cs.pending_grants(), 31);

        cs.record_recv(); // 32nd
        assert!(cs.should_grant());
        assert_eq!(cs.pending_grants(), 32);
    }

    #[test]
    fn take_grants_resets() {
        let mut cs = CreditState::new(128);
        for _ in 0..40 {
            cs.record_recv();
        }
        assert_eq!(cs.take_grants(), 40);
        assert_eq!(cs.pending_grants(), 0);
        assert!(!cs.should_grant());
    }

    #[test]
    fn edge_initial_credits_one() {
        let mut cs = CreditState::new(1);
        // threshold = 1 / 4 = 0, so should_grant is always true
        assert!(cs.should_grant());
        cs.record_recv();
        assert!(cs.should_grant());
        cs.consume().unwrap();
        assert!(!cs.can_send());
    }

    #[test]
    fn edge_initial_credits_zero() {
        let cs = CreditState::new(0);
        assert!(!cs.can_send());
        assert!(cs.should_grant()); // threshold = 0
        assert_eq!(cs.send_credits(), 0);
    }
}
