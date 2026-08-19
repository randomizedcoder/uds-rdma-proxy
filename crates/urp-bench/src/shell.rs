//! Shared shell state for the two backends (blocking + io_uring): the
//! Rust twin of the run bookkeeping in `tools/urp-bench.c`. The pure core
//! stays in the sibling modules; this module owns progress/counters and
//! the message-classification step both event loops share.

use urp_bench::config::{Config, Pattern, Role, Verify};
use urp_bench::frame::{self, Hdr, HDR_SIZE};
use urp_bench::stats::Stats;
use urp_bench::tracker::Tracker;
use urp_bench::Error;

pub const RECV_BUF_SZ: usize = 65536;
pub const SAMPLE_CAP: usize = 200_000;
pub const FIN_TIMEOUT_S: u64 = 10;

pub fn now_ns() -> u64 {
    let mut ts = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    // SAFETY: valid pointer to a timespec; CLOCK_MONOTONIC always exists.
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
    ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64
}

pub fn skip(cfg: &Config, reason: &str) -> ! {
    println!(
        "BENCH_SKIP lang=rust mode={} msg_size={} batch={} reason={reason}",
        cfg.mode.as_str(),
        cfg.msg_size,
        cfg.batch
    );
    std::process::exit(0);
}

pub fn fail(cfg: &Config, reason: &str, err: i32) -> ! {
    println!(
        "BENCH_FAIL lang=rust mode={} msg_size={} batch={} reason={reason} err={err}",
        cfg.mode.as_str(),
        cfg.msg_size,
        cfg.batch
    );
    std::process::exit(1);
}

/// What `classify_msg` asks the backend to do with a peer original.
pub enum EchoAction {
    /// Message sits intact in the recv chunk: flip the ECHO flag byte at
    /// `hdr_off + 5` and send `[hdr_off, hdr_off + len)` from the SAME
    /// buffer (§30.5 echo-in-place).
    InPlace { hdr_off: usize, len: usize },
    /// Message was reassembled: backend re-encodes header + payload copy.
    Rebuild,
    /// Not an original (it was an echo of ours) — nothing to send.
    None,
}

pub struct Shell {
    pub cfg: Config,
    pub tracker: Tracker,
    pub stats: Stats,

    pub next_seq: u32,
    pub sent_originals: u64,
    pub goal: u64,
    pub deadline_ns: u64,
    pub own_fin_sent: bool,
    pub own_fin_seq: u32,
    pub own_fin_echoed: bool,
    pub peer_fin_seen: bool,
    pub peer_closed: bool,

    pub syscalls: u64,
    pub bytes_echoed_back: u64,
    pub msgs_rx_total: u64,
    pub zc_copied: u64,
    pub zc_sends: u64,

    /// --pattern stream (§34.4). `do_generate`: this side sources traffic
    /// (echo, or the connect side of a stream). `do_echo`: reflect peer
    /// originals (echo pattern only).
    pub do_generate: bool,
    pub do_echo: bool,
    pub tx_wire_bytes: u64, // stream source: wire bytes handed to the socket
    pub rx_data_bytes: u64, // stream sink: wire bytes of data delivered
    pub rx_data_msgs: u64,  // stream sink: data frames delivered
    pub t_first_rx_ns: u64, // stream sink: first data frame (goodput window)
    pub t_stream_end_ns: u64, // stream sink: peer FIN seen
}

impl Shell {
    pub fn new(cfg: Config) -> Self {
        let goal = if cfg.count != 0 { cfg.count } else { u64::MAX };
        let deadline_ns = if cfg.duration_s != 0 {
            now_ns() + cfg.duration_s as u64 * 1_000_000_000
        } else {
            0
        };
        let batch = cfg.batch;
        // Role split (§34.4): echo sides both source+reflect; a stream
        // source (connect) only sources, a stream sink (listen) only drains.
        let do_generate =
            cfg.pattern == Pattern::Echo || cfg.role == Some(Role::Connect);
        let do_echo = cfg.pattern == Pattern::Echo;
        Shell {
            cfg,
            tracker: Tracker::new(batch),
            stats: Stats::new(SAMPLE_CAP),
            next_seq: 0,
            sent_originals: 0,
            goal,
            deadline_ns,
            own_fin_sent: false,
            own_fin_seq: 0,
            own_fin_echoed: false,
            peer_fin_seen: false,
            peer_closed: false,
            syscalls: 0,
            bytes_echoed_back: 0,
            msgs_rx_total: 0,
            zc_copied: 0,
            zc_sends: 0,
            do_generate,
            do_echo,
            tx_wire_bytes: 0,
            rx_data_bytes: 0,
            rx_data_msgs: 0,
            t_first_rx_ns: 0,
            t_stream_end_ns: 0,
        }
    }

    pub fn hard_deadline(&self) -> u64 {
        let secs = FIN_TIMEOUT_S
            + if self.deadline_ns != 0 {
                self.cfg.duration_s as u64
            } else {
                self.cfg.count / 1000 + 30
            };
        now_ns() + secs * 1_000_000_000
    }

    /// Classify one deframed message; updates counters/tracker and tells
    /// the backend what echo (if any) to emit. `chunk_off` is the byte
    /// offset of `payload` within the current recv chunk, or `None` when
    /// the payload came from the assembly buffer.
    pub fn classify_msg(
        &mut self,
        hdr: &Hdr,
        payload: &[u8],
        chunk_off: Option<usize>,
    ) -> Result<EchoAction, Error> {
        self.msgs_rx_total += 1;

        if hdr.flags & frame::FLAG_ECHO != 0 {
            let rtt = self.tracker.echo(hdr.seq, now_ns())?;
            self.stats.add(rtt);
            self.bytes_echoed_back += (HDR_SIZE as u32 + hdr.payload_len) as u64;
            if self.cfg.verify == Verify::Full {
                frame::verify_payload(payload, hdr.origin_id, hdr.seq)?;
            }
            if hdr.flags & frame::FLAG_FIN != 0 && self.own_fin_sent && hdr.seq == self.own_fin_seq
            {
                self.own_fin_echoed = true;
            }
            return Ok(EchoAction::None);
        }

        // a peer original
        if self.cfg.verify == Verify::Full {
            frame::verify_payload(payload, hdr.origin_id, hdr.seq)?;
        }

        if !self.do_echo {
            // stream sink: count delivered bytes, never echo. Goodput is
            // measured from the first data frame to the peer's FIN.
            if hdr.flags & frame::FLAG_FIN != 0 {
                self.peer_fin_seen = true;
                self.t_stream_end_ns = now_ns();
            } else {
                if self.t_first_rx_ns == 0 {
                    self.t_first_rx_ns = now_ns();
                }
                self.rx_data_bytes += (HDR_SIZE as u32 + hdr.payload_len) as u64;
                self.rx_data_msgs += 1;
            }
            return Ok(EchoAction::None);
        }

        // echo pattern: reflect it back
        if hdr.flags & frame::FLAG_FIN != 0 {
            self.peer_fin_seen = true;
        }
        Ok(match chunk_off {
            Some(off) if hdr.payload_len > 0 => EchoAction::InPlace {
                hdr_off: off - HDR_SIZE,
                len: HDR_SIZE + hdr.payload_len as usize,
            },
            _ => EchoAction::Rebuild,
        })
    }

    /// Build the next original header into `buf[..HDR_SIZE]`; fills the
    /// payload only under --verify full (§30.5 — fill cost must not
    /// contaminate perf cells). Returns the header, or None if the
    /// tracker window slot is unexpectedly busy.
    pub fn next_original(&mut self, buf: &mut [u8], fin: bool) -> Option<Hdr> {
        let t = now_ns();
        let payload_len = if fin {
            0
        } else {
            self.cfg.msg_size - HDR_SIZE as u32
        };
        let h = Hdr::new(
            if fin { frame::FLAG_FIN } else { 0 },
            self.cfg.id,
            payload_len,
            self.next_seq,
            t,
        );
        let mut hdr_bytes = [0u8; HDR_SIZE];
        h.encode(&mut hdr_bytes);
        buf[..HDR_SIZE].copy_from_slice(&hdr_bytes);
        if self.cfg.verify == Verify::Full && payload_len > 0 {
            frame::fill_payload(
                &mut buf[HDR_SIZE..HDR_SIZE + payload_len as usize],
                h.origin_id,
                h.seq,
            );
        }
        // In stream mode there are no echoes to clear the RTT window, so the
        // tracker is not the pacing gate (socket/carry backpressure is).
        if self.cfg.pattern == Pattern::Echo {
            self.tracker.sent(h.seq, t).ok()?;
        }
        if fin {
            self.own_fin_sent = true;
            self.own_fin_seq = h.seq;
        }
        self.next_seq = self.next_seq.wrapping_add(1);
        self.sent_originals += 1;
        self.tx_wire_bytes += (HDR_SIZE as u32 + payload_len) as u64;
        Some(h)
    }

    /// How many originals to queue now (0 when FIN already sent); also
    /// reports whether the FIN should be appended.
    pub fn plan(&self) -> (u32, bool) {
        if self.own_fin_sent {
            return (0, false);
        }
        let remaining = if self.deadline_ns != 0 && now_ns() >= self.deadline_ns {
            0
        } else {
            self.goal.saturating_sub(self.sent_originals)
        };
        let b = urp_bench::batch::Batch {
            window: self.cfg.batch,
        };
        let n = b.plan(self.tracker.inflight_count, remaining);
        let fin = remaining == 0 && self.tracker.inflight_count < self.cfg.batch;
        (n, fin)
    }

    pub fn done_core(&self) -> bool {
        if self.cfg.pattern == Pattern::Stream {
            if self.do_generate {
                // source: FIN sent (the caller's loop also waits for the
                // carry to drain before exiting).
                return self.own_fin_sent;
            }
            // sink: drain until the source's FIN (or it closes).
            return self.peer_fin_seen || self.peer_closed;
        }
        self.peer_closed
            || (self.own_fin_echoed && self.peer_fin_seen && self.tracker.inflight_count == 0)
    }
}
