//! Batch/window accounting (§30.6): how many new originals to queue this
//! iteration.

#[derive(Debug, Clone, Copy)]
pub struct Batch {
    /// Max outstanding originals (= the matrix `batch` dimension).
    pub window: u32,
}

impl Batch {
    /// Never exceeds the free window, never exceeds what remains.
    pub fn plan(&self, inflight: u32, remaining: u64) -> u32 {
        if inflight >= self.window {
            return 0;
        }
        let room = self.window - inflight;
        remaining.min(room as u64) as u32
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn plan_table() {
        // Mirror of C test_batch_plan.
        let cases: &[(&str, u32, u32, u64, u32)] = &[
            ("empty-window-free", 8, 0, 100, 8),
            ("partial-inflight", 8, 5, 100, 3),
            ("window-full", 8, 8, 100, 0),
            ("remaining-below-room", 8, 2, 3, 3),
            ("remaining-zero", 8, 0, 0, 0),
            ("remaining-one", 8, 7, 1, 1),
            ("window-one", 1, 0, 100, 1),
            ("window-one-busy", 1, 1, 100, 0),
            ("huge-remaining", 256, 0, u64::MAX, 256),
        ];
        for (name, window, inflight, remaining, want) in cases {
            let got = Batch { window: *window }.plan(*inflight, *remaining);
            assert_eq!(got, *want, "case {name}");
        }
    }
}
