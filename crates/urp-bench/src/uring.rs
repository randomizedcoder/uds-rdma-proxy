//! io_uring backend (design 30 B4) — Rust twin of the uring event loop in
//! `tools/urp-bench.c`, on the tokio-rs `io-uring` crate (raw SQE/CQE,
//! no runtime — §30.10). Same modes, same user_data encoding, same
//! echo-in-place discipline, same skip/fail taxonomy as the C shell.

use std::os::fd::RawFd;

use io_uring::{cqueue, opcode, squeue, types, IoUring};

use crate::shell::{fail, now_ns, skip, EchoAction, Shell, RECV_BUF_SZ};
use urp_bench::config::Mode;
use urp_bench::deframe::Deframer;
use urp_bench::frame::{Hdr, FLAG_ECHO, HDR_SIZE};
use urp_bench::Error;

const UD_KIND_RECV: u64 = 1 << 56;
const UD_KIND_SEND: u64 = 2 << 56;
const NOTIF_ZC_COPIED: u32 = 1 << 31; // IORING_NOTIF_USAGE_ZC_COPIED

/// SOCK_STREAM invariant: exactly ONE recv SQE outstanding at a time —
/// concurrent recvs on a stream socket complete in arbitrary order and
/// would permute the byte stream. The pool only parks buffers whose
/// in-place echoes are still in flight.
struct RecvBuf {
    data: Vec<u8>,
    pending_echoes: u32,
    in_recv: bool,
}

#[derive(Clone, Copy)]
struct SendOp {
    ptr: *const u8,
    remaining: u32,
    recv_idx: i32,
    slot: i32,
    scratch: i32,
    fixed_idx: i32,
    zc_notifs: i32,
    in_use: bool,
}

impl SendOp {
    const FREE: SendOp = SendOp {
        ptr: std::ptr::null(),
        remaining: 0,
        recv_idx: -1,
        slot: -1,
        scratch: -1,
        fixed_idx: -1,
        zc_notifs: 0,
        in_use: false,
    };
}

struct Ring {
    ring: IoUring,
    use_fixed: bool,
    use_sendzc: bool,
}

impl Ring {
    fn push(&mut self, entry: &squeue::Entry, shell: &mut Shell) -> Result<(), Error> {
        // SAFETY: every buffer referenced by an entry stays alive until
        // its CQE is harvested — enforced by the SendOp/RecvBuf release
        // bookkeeping below (same protocol as the C shell).
        unsafe {
            if self.ring.submission().push(entry).is_err() {
                shell.syscalls += 1;
                let _ = self.ring.submit();
                if self.ring.submission().push(entry).is_err() {
                    return Err(Error::Full);
                }
            }
        }
        Ok(())
    }
}

struct Bufs {
    rbufs: Vec<RecvBuf>,
    send_slots: Vec<Vec<u8>>,
    slot_free: Vec<bool>,
    scratch: Vec<Vec<u8>>,
    scratch_busy: Vec<bool>,
    sends: Vec<SendOp>,
    /// No recv armed: every buffer had echoes in flight.
    recv_wanted: bool,
}

fn alloc_send_op(bufs: &Bufs) -> Option<usize> {
    bufs.sends.iter().position(|op| !op.in_use)
}

fn prep_send(
    ring: &mut Ring,
    shell: &mut Shell,
    bufs: &mut Bufs,
    op_idx: usize,
) -> Result<(), Error> {
    let op = bufs.sends[op_idx];
    let fd = types::Fixed(0);
    let entry = if ring.use_sendzc {
        bufs.sends[op_idx].zc_notifs += 1;
        shell.zc_sends += 1;
        opcode::SendZc::new(fd, op.ptr, op.remaining)
            .flags(libc::MSG_NOSIGNAL)
            .build()
    } else if ring.use_fixed && op.fixed_idx >= 0 {
        opcode::WriteFixed::new(fd, op.ptr, op.remaining, op.fixed_idx as u16)
            .offset(0)
            .build()
    } else {
        opcode::Send::new(fd, op.ptr, op.remaining)
            .flags(libc::MSG_NOSIGNAL)
            .build()
    };
    ring.push(&entry.user_data(UD_KIND_SEND | op_idx as u64), shell)
}

fn prep_recv(
    ring: &mut Ring,
    shell: &mut Shell,
    bufs: &mut Bufs,
    rb_idx: usize,
) -> Result<(), Error> {
    let ptr = bufs.rbufs[rb_idx].data.as_mut_ptr();
    let fd = types::Fixed(0);
    let entry = if ring.use_fixed {
        opcode::ReadFixed::new(fd, ptr, RECV_BUF_SZ as u32, rb_idx as u16)
            .offset(0)
            .build()
    } else {
        opcode::Recv::new(fd, ptr, RECV_BUF_SZ as u32).build()
    };
    bufs.rbufs[rb_idx].in_recv = true;
    ring.push(&entry.user_data(UD_KIND_RECV | rb_idx as u64), shell)
}

/// Arm the single next recv: reuse the drained buffer if its echoes are
/// gone, else any echo-free buffer, else wait for release_send.
fn arm_next_recv(
    ring: &mut Ring,
    shell: &mut Shell,
    bufs: &mut Bufs,
    prefer: usize,
) -> Result<(), Error> {
    if bufs.rbufs[prefer].pending_echoes == 0 {
        return prep_recv(ring, shell, bufs, prefer);
    }
    if let Some(i) = bufs
        .rbufs
        .iter()
        .position(|rb| !rb.in_recv && rb.pending_echoes == 0)
    {
        return prep_recv(ring, shell, bufs, i);
    }
    bufs.recv_wanted = true;
    Ok(())
}

fn release_send(
    ring: &mut Ring,
    shell: &mut Shell,
    bufs: &mut Bufs,
    op_idx: usize,
) -> Result<(), Error> {
    let op = bufs.sends[op_idx];
    if op.recv_idx >= 0 {
        let rb_idx = op.recv_idx as usize;
        bufs.rbufs[rb_idx].pending_echoes -= 1;
        if bufs.rbufs[rb_idx].pending_echoes == 0 && bufs.recv_wanted {
            bufs.recv_wanted = false;
            prep_recv(ring, shell, bufs, rb_idx)?;
        }
    }
    if op.scratch >= 0 {
        bufs.scratch_busy[op.scratch as usize] = false;
    }
    if op.slot >= 0 {
        bufs.slot_free[op.slot as usize] = true;
    }
    bufs.sends[op_idx].in_use = false;
    Ok(())
}

/// `Ok(false)` = slot backpressure (the tracker slot for next_seq is
/// still occupied by next_seq − window after out-of-order echoes): stop
/// topping up this iteration — not an error.
fn queue_original(
    ring: &mut Ring,
    shell: &mut Shell,
    bufs: &mut Bufs,
    n_rbufs: usize,
    fin: bool,
) -> Result<bool, Error> {
    let op_idx = alloc_send_op(bufs).ok_or(Error::Full)?;
    let slot = bufs.slot_free.iter().position(|f| *f).ok_or(Error::Full)?;
    bufs.slot_free[slot] = false;

    let msg_size = shell.cfg.msg_size as usize;
    let h = {
        let buf = &mut bufs.send_slots[slot];
        shell.next_original(buf, fin)
    };
    let h = match h {
        Some(h) => h,
        None => {
            bufs.slot_free[slot] = true;
            return Ok(false);
        }
    };
    let total = if fin { HDR_SIZE } else { msg_size };
    let use_fixed = ring.use_fixed;
    bufs.sends[op_idx] = SendOp {
        ptr: bufs.send_slots[slot].as_ptr(),
        remaining: total as u32,
        recv_idx: -1,
        slot: slot as i32,
        scratch: -1,
        fixed_idx: if use_fixed {
            (n_rbufs + slot) as i32
        } else {
            -1
        },
        zc_notifs: 0,
        in_use: true,
    };
    let _ = h;
    prep_send(ring, shell, bufs, op_idx)?;
    Ok(true)
}

#[allow(clippy::too_many_arguments)] // mirrors the C shell's signature 1:1
fn queue_echo(
    ring: &mut Ring,
    shell: &mut Shell,
    bufs: &mut Bufs,
    n_rbufs: usize,
    rb_idx: usize,
    action: &EchoAction,
    hdr: &Hdr,
    payload: &[u8],
) -> Result<(), Error> {
    let op_idx = alloc_send_op(bufs).ok_or(Error::Full)?;
    let batch = shell.cfg.batch as usize;

    match *action {
        EchoAction::InPlace { hdr_off, len } => {
            bufs.rbufs[rb_idx].data[hdr_off + 5] |= FLAG_ECHO;
            bufs.rbufs[rb_idx].pending_echoes += 1;
            bufs.sends[op_idx] = SendOp {
                ptr: bufs.rbufs[rb_idx].data[hdr_off..].as_ptr(),
                remaining: len as u32,
                recv_idx: rb_idx as i32,
                slot: -1,
                scratch: -1,
                fixed_idx: rb_idx as i32,
                zc_notifs: 0,
                in_use: true,
            };
        }
        EchoAction::Rebuild => {
            let s = bufs
                .scratch_busy
                .iter()
                .position(|b| !*b)
                .ok_or(Error::Full)?;
            bufs.scratch_busy[s] = true;
            let mut e = *hdr;
            e.flags |= FLAG_ECHO;
            let mut hdr_bytes = [0u8; HDR_SIZE];
            e.encode(&mut hdr_bytes);
            bufs.scratch[s][..HDR_SIZE].copy_from_slice(&hdr_bytes);
            bufs.scratch[s][HDR_SIZE..HDR_SIZE + payload.len()].copy_from_slice(payload);
            let use_fixed = ring.use_fixed;
            bufs.sends[op_idx] = SendOp {
                ptr: bufs.scratch[s].as_ptr(),
                remaining: (HDR_SIZE + payload.len()) as u32,
                recv_idx: -1,
                slot: -1,
                scratch: s as i32,
                fixed_idx: if use_fixed {
                    (n_rbufs + batch + s) as i32
                } else {
                    -1
                },
                zc_notifs: 0,
                in_use: true,
            };
        }
        EchoAction::None => {
            bufs.sends[op_idx].in_use = false;
            return Ok(());
        }
    }
    prep_send(ring, shell, bufs, op_idx)
}

/// Returns the deframer's reassembled-message count for the report.
pub fn run(shell: &mut Shell, fd: RawFd) -> Result<u64, i32> {
    let cfg = shell.cfg.clone();
    let entries = std::cmp::max(64, 4 * cfg.batch);

    let mut builder = IoUring::builder();
    if cfg.mode == Mode::UringSqpoll {
        builder.setup_sqpoll(2000);
    }
    if cfg.defer_taskrun {
        builder
            .setup_single_issuer()
            .setup_defer_taskrun()
            .setup_coop_taskrun();
    }
    let ring = match builder.build(entries) {
        Ok(r) => r,
        Err(e) => match e.raw_os_error() {
            Some(libc::EPERM) if cfg.mode == Mode::UringSqpoll => skip(&cfg, "sqpoll_eperm"),
            Some(libc::EINVAL) if cfg.defer_taskrun => skip(&cfg, "no_defer_taskrun"),
            _ => skip(&cfg, "no_io_uring"),
        },
    };

    let mut ring = Ring {
        ring,
        use_fixed: false,
        use_sendzc: cfg.mode == Mode::UringSendzc,
    };

    if ring.use_sendzc {
        let mut probe = io_uring::Probe::new();
        if ring.ring.submitter().register_probe(&mut probe).is_err()
            || !probe.is_supported(opcode::SendZc::CODE)
        {
            skip(&cfg, "no_sendzc");
        }
    }

    if ring.ring.submitter().register_files(&[fd]).is_err() {
        fail(&cfg, "register_files", 0);
    }

    let n_rbufs = (2 * cfg.batch).clamp(4, 64) as usize;
    let n_scratch = (cfg.batch + 4).min(64) as usize;
    let msg_size = cfg.msg_size as usize;
    let mut bufs = Bufs {
        rbufs: (0..n_rbufs)
            .map(|_| RecvBuf {
                data: vec![0; RECV_BUF_SZ],
                pending_echoes: 0,
                in_recv: false,
            })
            .collect(),
        send_slots: (0..cfg.batch).map(|_| vec![0x5a; msg_size]).collect(),
        slot_free: vec![true; cfg.batch as usize],
        scratch: (0..n_scratch).map(|_| vec![0; msg_size]).collect(),
        scratch_busy: vec![false; n_scratch],
        sends: vec![SendOp::FREE; (2 * cfg.batch) as usize + n_rbufs + n_scratch + 8],
        recv_wanted: false,
    };

    if cfg.mode == Mode::UringFixed {
        let mut iovecs: Vec<libc::iovec> = Vec::new();
        for rb in &mut bufs.rbufs {
            iovecs.push(libc::iovec {
                iov_base: rb.data.as_mut_ptr().cast(),
                iov_len: RECV_BUF_SZ,
            });
        }
        for s in &mut bufs.send_slots {
            iovecs.push(libc::iovec {
                iov_base: s.as_mut_ptr().cast(),
                iov_len: msg_size,
            });
        }
        for s in &mut bufs.scratch {
            iovecs.push(libc::iovec {
                iov_base: s.as_mut_ptr().cast(),
                iov_len: msg_size,
            });
        }
        // SAFETY: all iovecs point at Vecs that live in `bufs` for the
        // whole run; unregistered implicitly on ring drop.
        if unsafe { ring.ring.submitter().register_buffers(&iovecs) }.is_err() {
            skip(&cfg, "no_fixed_buffers");
        }
        ring.use_fixed = true;
    }

    let mut deframer = Deframer::new(msg_size, cfg.msg_size - HDR_SIZE as u32);
    // ONE outstanding recv (stream ordering — see RecvBuf).
    if prep_recv(&mut ring, shell, &mut bufs, 0).is_err() {
        fail(&cfg, "prep_recv", 0);
    }

    let hard_deadline = shell.hard_deadline();
    // Deferred per-CQE work; drained after the completion pass.
    let mut echoes: Vec<(usize, EchoAction, Hdr, Vec<u8>)> = Vec::new();

    'outer: while !(shell.done_core() && bufs.sends.iter().all(|op| !op.in_use)) {
        // top-up
        let (mut n, fin) = shell.plan();
        while n > 0 {
            match queue_original(&mut ring, shell, &mut bufs, n_rbufs, false) {
                Err(_) => return Err(-(Error::Full as i32)),
                Ok(false) => break, // slot backpressure: retry next loop
                Ok(true) => {}
            }
            n -= 1;
        }
        if fin && queue_original(&mut ring, shell, &mut bufs, n_rbufs, true).is_err() {
            return Err(-(Error::Full as i32));
        }

        shell.syscalls += 1;
        if let Err(e) = ring.ring.submit_and_wait(1) {
            if e.raw_os_error() != Some(libc::EINTR) {
                return Err(e.raw_os_error().unwrap_or(-1));
            }
        }

        // completion pass — collect, then act (borrow discipline)
        let mut cqes: Vec<(u64, i32, u32)> = Vec::new();
        for cqe in ring.ring.completion() {
            cqes.push((cqe.user_data(), cqe.result(), cqe.flags()));
        }
        for (ud, res, flags) in cqes {
            if ud & UD_KIND_RECV != 0 {
                let rb_idx = (ud & 0xffff_ffff) as usize;
                if res == 0 || res == -libc::ECONNRESET {
                    shell.peer_closed = true;
                    continue;
                }
                if res < 0 {
                    return Err(res);
                }
                bufs.rbufs[rb_idx].in_recv = false;
                let n = res as usize;
                // feed: deframer and shell are disjoint borrows; the
                // chunk is copied out of the recv buf borrow via split.
                let (chunk_ptr, chunk_len) = (bufs.rbufs[rb_idx].data.as_ptr(), n);
                let chunk = unsafe { std::slice::from_raw_parts(chunk_ptr, chunk_len) };
                let sh: &mut Shell = shell;
                let ret = deframer.feed(chunk, &mut |hdr, payload| {
                    let off = (payload.as_ptr() as usize).checked_sub(chunk_ptr as usize);
                    let chunk_off = match off {
                        Some(o) if o >= HDR_SIZE && o + payload.len() <= chunk_len => Some(o),
                        _ => None,
                    };
                    let action = sh.classify_msg(hdr, payload, chunk_off)?;
                    match action {
                        EchoAction::None => {}
                        EchoAction::InPlace { .. } => {
                            echoes.push((rb_idx, action, *hdr, Vec::new()))
                        }
                        EchoAction::Rebuild => {
                            echoes.push((rb_idx, action, *hdr, payload.to_vec()))
                        }
                    }
                    Ok(())
                });
                if let Err(e) = ret {
                    return Err(-(e as i32));
                }
                for (rbi, action, hdr, payload) in echoes.drain(..) {
                    if queue_echo(
                        &mut ring, shell, &mut bufs, n_rbufs, rbi, &action, &hdr, &payload,
                    )
                    .is_err()
                    {
                        return Err(-(Error::Full as i32));
                    }
                }
                // §30.5 invariant: re-arm before new writes queue.
                if arm_next_recv(&mut ring, shell, &mut bufs, rb_idx).is_err() {
                    return Err(-(Error::Full as i32));
                }
            } else {
                let op_idx = (ud & 0xffff_ffff) as usize;
                if ring.use_sendzc && cqueue::notif(flags) {
                    if res as u32 & NOTIF_ZC_COPIED != 0 {
                        shell.zc_copied += 1;
                    }
                    bufs.sends[op_idx].zc_notifs -= 1;
                    if bufs.sends[op_idx].remaining == 0 && bufs.sends[op_idx].zc_notifs == 0 {
                        release_send(&mut ring, shell, &mut bufs, op_idx)
                            .map_err(|e| -(e as i32))?;
                    }
                    continue;
                }
                if res < 0 {
                    if ring.use_sendzc && res == -libc::EOPNOTSUPP {
                        println!(
                            "BENCH_ZC sends={} copied={} result=eopnotsupp",
                            shell.zc_sends, shell.zc_copied
                        );
                        skip(&cfg, "sendzc_eopnotsupp");
                    }
                    return Err(res);
                }
                let op = &mut bufs.sends[op_idx];
                if (res as u32) < op.remaining {
                    op.ptr = unsafe { op.ptr.add(res as usize) };
                    op.remaining -= res as u32;
                    prep_send(&mut ring, shell, &mut bufs, op_idx).map_err(|e| -(e as i32))?;
                    continue;
                }
                op.remaining = 0;
                if ring.use_sendzc && op.zc_notifs > 0 {
                    continue;
                }
                release_send(&mut ring, shell, &mut bufs, op_idx).map_err(|e| -(e as i32))?;
            }
            if shell.peer_closed {
                break 'outer;
            }
        }
        if now_ns() > hard_deadline {
            fail(&cfg, "timeout", 0);
        }
    }
    Ok(deframer.msgs_reassembled)
}
