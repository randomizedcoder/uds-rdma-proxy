//! RTT statistics (§30.8): fixed-size sample array, sort + index
//! percentiles — the house idiom from `tools/urp-test-client.c` latency
//! mode.

use crate::Error;

pub struct Stats {
    samples: Vec<u64>,
    cap: usize,
    /// Samples beyond `cap` (saturating).
    pub dropped: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct StatsResult {
    pub min_ns: u64,
    pub max_ns: u64,
    pub p50_ns: u64,
    pub p99_ns: u64,
    pub count: usize,
}

impl Stats {
    pub fn new(cap: usize) -> Self {
        Stats {
            samples: Vec::with_capacity(cap),
            cap,
            dropped: 0,
        }
    }

    pub fn add(&mut self, rtt_ns: u64) {
        if self.samples.len() < self.cap {
            self.samples.push(rtt_ns);
        } else {
            self.dropped += 1;
        }
    }

    pub fn count(&self) -> usize {
        self.samples.len()
    }

    /// Sorts in place. `Err(Empty)` if no samples.
    pub fn finalize(&mut self) -> Result<StatsResult, Error> {
        if self.samples.is_empty() {
            return Err(Error::Empty);
        }
        self.samples.sort_unstable();
        let n = self.samples.len();
        Ok(StatsResult {
            min_ns: self.samples[0],
            max_ns: self.samples[n - 1],
            p50_ns: self.samples[n / 2],
            p99_ns: self.samples[(n * 99) / 100],
            count: n,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn known_distribution() {
        // 1..=100 reversed (finalize must sort) -> exact percentiles,
        // mirror of C test_stats.
        let mut s = Stats::new(100);
        for i in (1..=100u64).rev() {
            s.add(i * 1000);
        }
        let r = s.finalize().unwrap();
        assert_eq!(r.min_ns, 1000);
        assert_eq!(r.max_ns, 100_000);
        assert_eq!(r.p50_ns, 51_000);
        assert_eq!(r.p99_ns, 100_000);
    }

    #[test]
    fn single_sample() {
        let mut s = Stats::new(100);
        s.add(42);
        let r = s.finalize().unwrap();
        assert_eq!((r.min_ns, r.max_ns, r.p50_ns, r.p99_ns), (42, 42, 42, 42));
    }

    #[test]
    fn saturation() {
        let mut s = Stats::new(2);
        s.add(1);
        s.add(2);
        s.add(3);
        assert_eq!(s.count(), 2);
        assert_eq!(s.dropped, 1);
    }

    #[test]
    fn empty() {
        let mut s = Stats::new(100);
        assert_eq!(s.finalize(), Err(Error::Empty));
    }
}
