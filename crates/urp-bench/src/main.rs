//! urp-bench (Rust) — CLI + sockets + blocking control backend; the
//! io_uring backend lives in `uring.rs`. Same CLI, same output grammar,
//! and byte-identical `BENCH_OK` lines as the C shell (design 30 §30.10):
//! the harness swaps languages without special-casing.

use std::os::fd::AsRawFd;
use std::os::unix::net::{UnixListener, UnixStream};

use urp_bench::config::{Config, Mode, Role, Verify};
use urp_bench::deframe::Deframer;
use urp_bench::frame::HDR_SIZE;
use urp_bench::report::Report;

// Bin-only modules: the lib stays io_uring/libc-free and miri-clean.
mod shell;
mod uring;

use shell::{fail, now_ns, EchoAction, Shell, RECV_BUF_SZ};

fn usage() -> ! {
    eprintln!(
        "usage: urp-bench (--listen PATH | --connect PATH) --id N\n\
         \x20 --mode {{blocking,uring-rw,uring-fixed,uring-bufring,\n\
         \x20         uring-sqpoll,uring-sendzc}}\n\
         \x20 --msg-size BYTES --batch N (--count N | --duration S)\n\
         \x20 [--verify {{none,header,full}}] [--defer-taskrun]\n\
         \x20 [--memcpy-baseline]"
    );
    std::process::exit(2);
}

fn memcpy_baseline(msg_size: u32) {
    let a = vec![0xa5u8; msg_size as usize];
    let mut b = vec![0u8; msg_size as usize];
    let start = now_ns();
    let mut iters = 0u64;
    let mut elapsed;
    loop {
        b.copy_from_slice(&a);
        iters += 1;
        elapsed = now_ns() - start;
        if elapsed >= 300_000_000 {
            break;
        }
    }
    std::hint::black_box(&b);
    let mbps = iters as f64 * msg_size as f64 / 1e6 / (elapsed as f64 / 1e9);
    println!("BENCH_MEMCPY msg_size={msg_size} mbps={mbps:.1}");
}

fn connect_retry(path: &str) -> Option<UnixStream> {
    for _ in 0..100 {
        if let Ok(s) = UnixStream::connect(path) {
            return Some(s);
        }
        std::thread::sleep(std::time::Duration::from_millis(50));
    }
    None
}

fn cpu_ns() -> u64 {
    // SAFETY: valid pointer; RUSAGE_SELF always succeeds.
    let mut ru: libc::rusage = unsafe { std::mem::zeroed() };
    unsafe { libc::getrusage(libc::RUSAGE_SELF, &mut ru) };
    let tv = |t: libc::timeval| t.tv_sec as u64 * 1_000_000 + t.tv_usec as u64;
    (tv(ru.ru_utime) + tv(ru.ru_stime)) * 1000
}

/* ---- blocking (control) backend --------------------------------------- */

struct Carry {
    buf: Vec<u8>,
}

impl Carry {
    /// Direct write first; the carry exists only for partial-write
    /// remainders (no steady-state extra memcpy — mirrors the C shell).
    fn queue(&mut self, shell: &mut Shell, fd: i32, data: &[u8]) -> Result<(), i32> {
        let mut off = 0usize;
        if self.buf.is_empty() {
            while off < data.len() {
                shell.syscalls += 1;
                let w = unsafe { libc::write(fd, data[off..].as_ptr().cast(), data.len() - off) };
                if w < 0 {
                    let e = std::io::Error::last_os_error();
                    if e.raw_os_error() == Some(libc::EAGAIN) {
                        break;
                    }
                    return Err(-e.raw_os_error().unwrap_or(1));
                }
                off += w as usize;
            }
            if off == data.len() {
                return Ok(());
            }
        }
        self.buf.extend_from_slice(&data[off..]);
        Ok(())
    }

    fn flush(&mut self, shell: &mut Shell, fd: i32) -> Result<(), i32> {
        while !self.buf.is_empty() {
            shell.syscalls += 1;
            let w = unsafe { libc::write(fd, self.buf.as_ptr().cast(), self.buf.len()) };
            if w < 0 {
                let e = std::io::Error::last_os_error();
                if e.raw_os_error() == Some(libc::EAGAIN) {
                    return Ok(());
                }
                return Err(-e.raw_os_error().unwrap_or(1));
            }
            self.buf.drain(..w as usize);
        }
        Ok(())
    }
}

fn run_blocking(shell: &mut Shell, stream: &UnixStream) -> Result<u64, i32> {
    let fd = stream.as_raw_fd();
    stream.set_nonblocking(true).map_err(|_| -1)?;
    let msg_size = shell.cfg.msg_size as usize;
    let mut rbuf = vec![0u8; RECV_BUF_SZ];
    let mut slot = vec![0x5au8; msg_size];
    let mut deframer = Deframer::new(msg_size, shell.cfg.msg_size - HDR_SIZE as u32);
    let mut carry = Carry { buf: Vec::new() };
    let hard_deadline = shell.hard_deadline();
    // (hdr, in-place range or payload copy)
    type PendingEcho = (urp_bench::frame::Hdr, Option<(usize, usize)>, Vec<u8>);
    let mut echoes: Vec<PendingEcho> = Vec::new();

    while !shell.done_core() || !carry.buf.is_empty() {
        // top-up (None = slot backpressure after out-of-order echoes:
        // stop for this iteration, not an error)
        let (n, fin) = shell.plan();
        for _ in 0..n {
            match shell.next_original(&mut slot, false) {
                Some(_) => {}
                None => break,
            }
            let s = slot[..msg_size].to_vec(); // borrow untangling only
            carry.queue(shell, fd, &s)?;
        }
        if fin && shell.next_original(&mut slot, true).is_some() {
            let s = slot[..HDR_SIZE].to_vec();
            carry.queue(shell, fd, &s)?;
        }

        let mut pfd = libc::pollfd {
            fd,
            events: libc::POLLIN
                | if carry.buf.is_empty() {
                    0
                } else {
                    libc::POLLOUT
                },
            revents: 0,
        };
        shell.syscalls += 1;
        let pr = unsafe { libc::poll(&mut pfd, 1, 1000) };
        if pr < 0 {
            let e = std::io::Error::last_os_error();
            if e.raw_os_error() != Some(libc::EINTR) {
                return Err(-e.raw_os_error().unwrap_or(1));
            }
        }
        if pfd.revents & libc::POLLOUT != 0 {
            carry.flush(shell, fd)?;
        }
        if pfd.revents & (libc::POLLIN | libc::POLLHUP) != 0 {
            shell.syscalls += 1;
            let got = unsafe { libc::read(fd, rbuf.as_mut_ptr().cast(), RECV_BUF_SZ) };
            if got == 0 {
                shell.peer_closed = true;
                break;
            }
            if got < 0 {
                let e = std::io::Error::last_os_error();
                if e.raw_os_error() != Some(libc::EAGAIN) {
                    return Err(-e.raw_os_error().unwrap_or(1));
                }
            }
            if got > 0 {
                let chunk_ptr = rbuf.as_ptr() as usize;
                let chunk_len = got as usize;
                let chunk = &rbuf[..chunk_len];
                let ret = deframer.feed(chunk, &mut |hdr, payload| {
                    let off = (payload.as_ptr() as usize).checked_sub(chunk_ptr);
                    let chunk_off = match off {
                        Some(o) if o >= HDR_SIZE && o + payload.len() <= chunk_len => Some(o),
                        _ => None,
                    };
                    match shell.classify_msg(hdr, payload, chunk_off)? {
                        EchoAction::None => {}
                        EchoAction::InPlace { hdr_off, len } => {
                            echoes.push((*hdr, Some((hdr_off, len)), Vec::new()))
                        }
                        EchoAction::Rebuild => echoes.push((*hdr, None, payload.to_vec())),
                    }
                    Ok(())
                });
                if let Err(e) = ret {
                    return Err(-(e as i32));
                }
                for (hdr, in_place, payload) in echoes.drain(..) {
                    match in_place {
                        Some((hdr_off, len)) => {
                            rbuf[hdr_off + 5] |= urp_bench::frame::FLAG_ECHO;
                            let region = rbuf[hdr_off..hdr_off + len].to_vec();
                            carry.queue(shell, fd, &region)?;
                        }
                        None => {
                            let mut e = hdr;
                            e.flags |= urp_bench::frame::FLAG_ECHO;
                            let mut hb = [0u8; HDR_SIZE];
                            e.encode(&mut hb);
                            carry.queue(shell, fd, &hb)?;
                            if !payload.is_empty() {
                                carry.queue(shell, fd, &payload)?;
                            }
                        }
                    }
                }
            }
        }
        if now_ns() > hard_deadline {
            fail(&shell.cfg, "timeout", 0);
        }
    }
    Ok(deframer.msgs_reassembled)
}

/* ---- main -------------------------------------------------------------- */

fn main() {
    let mut cfg = Config {
        role: None,
        id: 0,
        mode: Mode::UringRw,
        verify: Verify::Header,
        msg_size: 4076,
        batch: 32,
        count: 0,
        duration_s: 0,
        defer_taskrun: false,
    };
    let mut path: Option<String> = None;
    let mut do_memcpy = false;

    let args: Vec<String> = std::env::args().collect();
    let mut i = 1;
    while i < args.len() {
        let need = |i: usize| args.get(i + 1).cloned().unwrap_or_else(|| usage());
        match args[i].as_str() {
            "--listen" => {
                cfg.role = Some(Role::Listen);
                path = Some(need(i));
                i += 1;
            }
            "--connect" => {
                cfg.role = Some(Role::Connect);
                path = Some(need(i));
                i += 1;
            }
            "--id" => {
                cfg.id = need(i).parse().unwrap_or_else(|_| usage());
                i += 1;
            }
            "--mode" => {
                cfg.mode = need(i).parse().unwrap_or_else(|_| usage());
                i += 1;
            }
            "--msg-size" => {
                cfg.msg_size = need(i).parse().unwrap_or_else(|_| usage());
                i += 1;
            }
            "--batch" => {
                cfg.batch = need(i).parse().unwrap_or_else(|_| usage());
                i += 1;
            }
            "--count" => {
                cfg.count = need(i).parse().unwrap_or_else(|_| usage());
                i += 1;
            }
            "--duration" => {
                cfg.duration_s = need(i).parse().unwrap_or_else(|_| usage());
                i += 1;
            }
            "--verify" => {
                cfg.verify = need(i).parse().unwrap_or_else(|_| usage());
                i += 1;
            }
            "--defer-taskrun" => cfg.defer_taskrun = true,
            "--memcpy-baseline" => do_memcpy = true,
            _ => usage(),
        }
        i += 1;
    }

    if do_memcpy {
        memcpy_baseline(cfg.msg_size);
        return;
    }
    let path = match (&path, cfg.validate()) {
        (Some(p), Ok(())) => p.clone(),
        _ => usage(),
    };
    let stream = match cfg.role {
        Some(Role::Listen) => {
            let _ = std::fs::remove_file(&path);
            let listener = match UnixListener::bind(&path) {
                Ok(l) => l,
                Err(_) => fail(&cfg, "socket", 0),
            };
            match listener.accept() {
                Ok((s, _)) => s,
                Err(_) => fail(&cfg, "socket", 0),
            }
        }
        _ => match connect_retry(&path) {
            Some(s) => s,
            None => fail(&cfg, "socket", 0),
        },
    };

    let mut shell = Shell::new(cfg.clone());
    let cpu0 = cpu_ns();
    let t0 = now_ns();
    let reassembled = match cfg.mode {
        Mode::Blocking => run_blocking(&mut shell, &stream),
        _ => uring::run(&mut shell, stream.as_raw_fd()),
    };
    let t1 = now_ns();
    let cpu1 = cpu_ns();

    let reassembled = match reassembled {
        Ok(v) => v,
        Err(e) => fail(&cfg, "run", e),
    };
    if shell.peer_closed && !(shell.own_fin_echoed && shell.peer_fin_seen) {
        fail(&cfg, "peer_closed_early", 0);
    }
    let rtt = match shell.stats.finalize() {
        Ok(r) => r,
        Err(_) if cfg.count > 0 => fail(&cfg, "no_samples", 0),
        Err(_) => urp_bench::stats::StatsResult {
            min_ns: 0,
            max_ns: 0,
            p50_ns: 0,
            p99_ns: 0,
            count: 0,
        },
    };

    let rep = Report {
        lang: "rust",
        cfg: &cfg,
        rtt,
        msgs: shell.sent_originals,
        bytes: shell.bytes_echoed_back,
        elapsed_ns: t1 - t0,
        syscalls: shell.syscalls,
        cpu_ns: cpu1 - cpu0,
        reassembled,
        msgs_rx_total: shell.msgs_rx_total,
    };
    println!("{}", rep.format());
    if cfg.mode == Mode::UringSendzc {
        println!(
            "BENCH_ZC sends={} copied={}",
            shell.zc_sends, shell.zc_copied
        );
    }
}
