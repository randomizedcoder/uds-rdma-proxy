//! Tiny AF_NETLINK / NETLINK_GENERIC client.
//!
//! Only what the urp CLI needs: family resolution, request/reply for
//! single-message commands, dump for multipart, and an event subscriber.

use std::ffi::c_int;
use std::io;
use std::mem::{size_of, MaybeUninit};
use std::os::raw::c_void;

use crate::attr::{align4, payload_str, payload_u16, AttrBuf, AttrIter};
use crate::error::UrpError;
use crate::uapi::{
    CTRL_ATTR_FAMILY_ID, CTRL_ATTR_FAMILY_NAME, CTRL_ATTR_MCAST_GROUPS, CTRL_ATTR_MCAST_GRP_ID,
    CTRL_ATTR_MCAST_GRP_NAME, CTRL_CMD_GETFAMILY, GENL_ID_CTRL, NLMSG_DONE, NLMSG_ERROR, NLM_F_ACK,
    NLM_F_DUMP, NLM_F_REQUEST, URP_GENL_MCGRP_EVENTS, URP_GENL_NAME, URP_GENL_VERSION,
};

const NETLINK_GENERIC: c_int = 16;
const NLMSG_HDR_LEN: usize = 16;
const GENL_HDR_LEN: usize = 4;

#[allow(dead_code)]
#[repr(C)]
#[derive(Copy, Clone)]
struct Nlmsghdr {
    nlmsg_len: u32,
    nlmsg_type: u16,
    nlmsg_flags: u16,
    nlmsg_seq: u32,
    nlmsg_pid: u32,
}

#[allow(dead_code)]
#[repr(C)]
#[derive(Copy, Clone)]
struct Genlmsghdr {
    cmd: u8,
    version: u8,
    reserved: u16,
}

#[repr(C)]
#[derive(Copy, Clone, Default)]
struct SockaddrNl {
    nl_family: u16,
    nl_pad: u16,
    nl_pid: u32,
    nl_groups: u32,
}

pub struct UrpSocket {
    fd: c_int,
    seq: u32,
    pid: u32,
    family_id: u16,
    events_mcgrp_id: u32,
}

impl Drop for UrpSocket {
    fn drop(&mut self) {
        if self.fd >= 0 {
            unsafe { libc::close(self.fd) };
        }
    }
}

impl UrpSocket {
    pub fn connect() -> Result<Self, UrpError> {
        let fd = unsafe { libc::socket(libc::AF_NETLINK, libc::SOCK_RAW, NETLINK_GENERIC) };
        if fd < 0 {
            return Err(UrpError::Io(io::Error::last_os_error()));
        }

        let mut addr = SockaddrNl {
            nl_family: libc::AF_NETLINK as u16,
            ..Default::default()
        };
        let r = unsafe {
            libc::bind(
                fd,
                &addr as *const _ as *const libc::sockaddr,
                size_of::<SockaddrNl>() as u32,
            )
        };
        if r < 0 {
            let e = io::Error::last_os_error();
            unsafe { libc::close(fd) };
            return Err(UrpError::Io(e));
        }

        // Read back assigned pid.
        let mut len = size_of::<SockaddrNl>() as libc::socklen_t;
        let r = unsafe {
            libc::getsockname(
                fd,
                &mut addr as *mut _ as *mut libc::sockaddr,
                &mut len as *mut _,
            )
        };
        if r < 0 {
            let e = io::Error::last_os_error();
            unsafe { libc::close(fd) };
            return Err(UrpError::Io(e));
        }

        let mut s = Self {
            fd,
            seq: 1,
            pid: addr.nl_pid,
            family_id: 0,
            events_mcgrp_id: 0,
        };
        s.resolve_family()?;
        Ok(s)
    }

    fn resolve_family(&mut self) -> Result<(), UrpError> {
        let mut payload = AttrBuf::new();
        payload.put_string(CTRL_ATTR_FAMILY_NAME, URP_GENL_NAME);
        let reply = self
            .send_request_raw(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 1, &payload.into_bytes())
            .map_err(|e| match e {
                UrpError::Io(io_err) => {
                    UrpError::from_errno(io_err.raw_os_error().unwrap_or(libc::EIO), "resolve")
                }
                other => other,
            })?;

        for (t, p) in AttrIter::new(&reply) {
            if t == CTRL_ATTR_FAMILY_ID {
                if let Some(id) = payload_u16(p) {
                    self.family_id = id;
                }
            } else if t == CTRL_ATTR_MCAST_GROUPS {
                for (_idx, grp) in AttrIter::new(p) {
                    let mut name: Option<String> = None;
                    let mut id: Option<u32> = None;
                    for (gt, gp) in AttrIter::new(grp) {
                        if gt == CTRL_ATTR_MCAST_GRP_NAME {
                            name = payload_str(gp).map(String::from);
                        } else if gt == CTRL_ATTR_MCAST_GRP_ID {
                            id = crate::attr::payload_u32(gp);
                        }
                    }
                    if let (Some(n), Some(i)) = (name, id) {
                        if n == URP_GENL_MCGRP_EVENTS {
                            self.events_mcgrp_id = i;
                        }
                    }
                }
            }
        }

        if self.family_id == 0 {
            return Err(UrpError::KernelModuleNotLoaded);
        }
        Ok(())
    }

    #[allow(dead_code)]
    pub fn family_id(&self) -> u16 {
        self.family_id
    }

    fn next_seq(&mut self) -> u32 {
        let s = self.seq;
        self.seq = self.seq.wrapping_add(1);
        s
    }

    /// Build + send a single nlmsg, then read the kernel reply. Returns
    /// the inner attribute blob (i.e. payload after the genlmsghdr).
    /// Handles ACK / ERROR.
    fn send_request_raw(
        &mut self,
        family: u16,
        cmd: u8,
        version: u8,
        payload: &[u8],
    ) -> Result<Vec<u8>, UrpError> {
        let seq = self.next_seq();
        let total = NLMSG_HDR_LEN + GENL_HDR_LEN + payload.len();
        let mut buf = Vec::with_capacity(align4(total));
        buf.extend_from_slice(&(total as u32).to_ne_bytes());
        buf.extend_from_slice(&family.to_ne_bytes());
        buf.extend_from_slice(&(NLM_F_REQUEST | NLM_F_ACK).to_ne_bytes());
        buf.extend_from_slice(&seq.to_ne_bytes());
        buf.extend_from_slice(&self.pid.to_ne_bytes());
        buf.push(cmd);
        buf.push(version);
        buf.extend_from_slice(&0u16.to_ne_bytes());
        buf.extend_from_slice(payload);
        while buf.len() % 4 != 0 {
            buf.push(0);
        }

        self.send(&buf)?;
        self.recv_one(seq)
    }

    /// Public wrapper for the urp family.
    pub fn send_request(&mut self, cmd: u8, payload: &[u8]) -> Result<Vec<u8>, UrpError> {
        self.send_request_raw(self.family_id, cmd, URP_GENL_VERSION, payload)
    }

    /// Send a request marked NLM_F_DUMP and gather all multipart replies.
    /// Returns each reply's inner attribute blob (post-genlhdr).
    pub fn dump(&mut self, cmd: u8, payload: &[u8]) -> Result<Vec<Vec<u8>>, UrpError> {
        let seq = self.next_seq();
        let total = NLMSG_HDR_LEN + GENL_HDR_LEN + payload.len();
        let mut buf = Vec::with_capacity(align4(total));
        buf.extend_from_slice(&(total as u32).to_ne_bytes());
        buf.extend_from_slice(&self.family_id.to_ne_bytes());
        buf.extend_from_slice(&(NLM_F_REQUEST | NLM_F_ACK | NLM_F_DUMP).to_ne_bytes());
        buf.extend_from_slice(&seq.to_ne_bytes());
        buf.extend_from_slice(&self.pid.to_ne_bytes());
        buf.push(cmd);
        buf.push(URP_GENL_VERSION);
        buf.extend_from_slice(&0u16.to_ne_bytes());
        buf.extend_from_slice(payload);
        while buf.len() % 4 != 0 {
            buf.push(0);
        }

        self.send(&buf)?;

        let mut out = Vec::new();
        loop {
            let chunk = self.recv_raw()?;
            let mut pos = 0;
            while pos + NLMSG_HDR_LEN <= chunk.len() {
                let nlmsg_len =
                    u32::from_ne_bytes(chunk[pos..pos + 4].try_into().unwrap()) as usize;
                let nlmsg_type = u16::from_ne_bytes(chunk[pos + 4..pos + 6].try_into().unwrap());
                let _flags = u16::from_ne_bytes(chunk[pos + 6..pos + 8].try_into().unwrap());
                let _mseq = u32::from_ne_bytes(chunk[pos + 8..pos + 12].try_into().unwrap());

                if nlmsg_len < NLMSG_HDR_LEN || pos + nlmsg_len > chunk.len() {
                    return Err(UrpError::Netlink("truncated multipart".into()));
                }

                if nlmsg_type == NLMSG_DONE {
                    return Ok(out);
                }
                if nlmsg_type == NLMSG_ERROR {
                    let errno = -i32::from_ne_bytes(
                        chunk[pos + NLMSG_HDR_LEN..pos + NLMSG_HDR_LEN + 4]
                            .try_into()
                            .unwrap(),
                    );
                    if errno == 0 {
                        // ack inside dump -- ignore
                    } else {
                        let extack = parse_extack(&chunk[pos..pos + nlmsg_len]);
                        return Err(UrpError::from_errno_with_extack(errno, "dump", extack));
                    }
                } else {
                    // genl payload
                    let body_start = pos + NLMSG_HDR_LEN + GENL_HDR_LEN;
                    let body_end = pos + nlmsg_len;
                    if body_end > body_start {
                        out.push(chunk[body_start..body_end].to_vec());
                    }
                }
                pos += align4(nlmsg_len);
            }
        }
    }

    fn send(&self, buf: &[u8]) -> Result<(), UrpError> {
        let mut dst = SockaddrNl {
            nl_family: libc::AF_NETLINK as u16,
            ..Default::default()
        };
        let r = unsafe {
            libc::sendto(
                self.fd,
                buf.as_ptr() as *const c_void,
                buf.len(),
                0,
                &mut dst as *mut _ as *mut libc::sockaddr,
                size_of::<SockaddrNl>() as u32,
            )
        };
        if r < 0 {
            return Err(UrpError::Io(io::Error::last_os_error()));
        }
        Ok(())
    }

    fn recv_raw(&self) -> Result<Vec<u8>, UrpError> {
        let mut buf = vec![MaybeUninit::<u8>::uninit(); 32 * 1024];
        let r = unsafe { libc::recv(self.fd, buf.as_mut_ptr() as *mut c_void, buf.len(), 0) };
        if r < 0 {
            return Err(UrpError::Io(io::Error::last_os_error()));
        }
        let n = r as usize;
        let buf: Vec<u8> = buf[..n]
            .iter()
            .map(|b| unsafe { b.assume_init() })
            .collect();
        Ok(buf)
    }

    /// Receive a single reply matching @want_seq. Returns the inner
    /// attribute blob (post-genlhdr).
    fn recv_one(&self, want_seq: u32) -> Result<Vec<u8>, UrpError> {
        loop {
            let chunk = self.recv_raw()?;
            if chunk.len() < NLMSG_HDR_LEN {
                return Err(UrpError::Netlink("short reply".into()));
            }
            let nlmsg_len = u32::from_ne_bytes(chunk[0..4].try_into().unwrap()) as usize;
            let nlmsg_type = u16::from_ne_bytes(chunk[4..6].try_into().unwrap());
            let _flags = u16::from_ne_bytes(chunk[6..8].try_into().unwrap());
            let mseq = u32::from_ne_bytes(chunk[8..12].try_into().unwrap());

            if mseq != want_seq && want_seq != 0 {
                continue;
            }

            if nlmsg_type == NLMSG_ERROR {
                let errno = -i32::from_ne_bytes(
                    chunk[NLMSG_HDR_LEN..NLMSG_HDR_LEN + 4].try_into().unwrap(),
                );
                if errno == 0 {
                    return Ok(Vec::new()); // pure ACK
                }
                let extack = parse_extack(&chunk[..nlmsg_len.min(chunk.len())]);
                return Err(UrpError::from_errno_with_extack(errno, "req", extack));
            }

            // genl reply.
            let body_start = NLMSG_HDR_LEN + GENL_HDR_LEN;
            let body_end = nlmsg_len.min(chunk.len());
            if body_end < body_start {
                return Ok(Vec::new());
            }
            return Ok(chunk[body_start..body_end].to_vec());
        }
    }

    /// Build a full nlmsg (nlmsghdr + genlmsghdr + payload, 4-aligned) with the
    /// given seq and flags for the urp family. Shared by the io_uring batch path.
    #[cfg(feature = "io-uring")]
    fn build_urp_nlmsg(&self, cmd: u8, seq: u32, flags: u16, payload: &[u8]) -> Vec<u8> {
        let total = NLMSG_HDR_LEN + GENL_HDR_LEN + payload.len();
        let mut buf = Vec::with_capacity(align4(total));
        buf.extend_from_slice(&(total as u32).to_ne_bytes());
        buf.extend_from_slice(&self.family_id.to_ne_bytes());
        buf.extend_from_slice(&flags.to_ne_bytes());
        buf.extend_from_slice(&seq.to_ne_bytes());
        buf.extend_from_slice(&self.pid.to_ne_bytes());
        buf.push(cmd);
        buf.push(URP_GENL_VERSION);
        buf.extend_from_slice(&0u16.to_ne_bytes());
        buf.extend_from_slice(payload);
        while buf.len() % 4 != 0 {
            buf.push(0);
        }
        buf
    }

    /// Grow SO_RCVBUF so a batch of replies (all queued before we drain them --
    /// a genl `doit` reply is generated synchronously in `send()`) does not
    /// overflow and get dropped. Best-effort; ignored on failure.
    #[cfg(feature = "io-uring")]
    fn bump_rcvbuf(&self, bytes: c_int) {
        unsafe {
            libc::setsockopt(
                self.fd,
                libc::SOL_SOCKET,
                libc::SO_RCVBUF,
                &bytes as *const _ as *const c_void,
                size_of::<c_int>() as u32,
            );
        }
    }

    /// Fan out `payloads.len()` single-reply genl requests (`cmd`, no ACK)
    /// through ONE io_uring submit/reap instead of `N` serial round-trips
    /// (design 39 §39.4). Returns, in input order, `Some(body)` (the reply's
    /// attribute blob, post-genlhdr) or `None` if that request came back as an
    /// `NLMSG_ERROR` (e.g. the endpoint raced away). Errors on ring/socket
    /// failure so the caller can fall back to the blocking path.
    ///
    /// Two phases avoid any completion-model hang: (1) submit all `N` sends and
    /// confirm they succeeded -- each success synchronously queues exactly one
    /// reply datagram; (2) submit `N` recvs (+ a timeout guard) and drain the
    /// `N` datagrams, mapping each back to its request by `nlmsg_seq`.
    #[cfg(feature = "io-uring")]
    pub fn send_batch_uring(
        &mut self,
        cmd: u8,
        payloads: &[Vec<u8>],
    ) -> Result<Vec<Option<Vec<u8>>>, UrpError> {
        use io_uring::{opcode, types, IoUring};

        let n = payloads.len();
        if n == 0 {
            return Ok(Vec::new());
        }

        // Build requests; seqs are consecutive from `base`, so seq->index is a
        // subtraction (no map allocation).
        let base = self.seq;
        let mut reqs: Vec<Vec<u8>> = Vec::with_capacity(n);
        for payload in payloads {
            let seq = self.next_seq();
            reqs.push(self.build_urp_nlmsg(cmd, seq, NLM_F_REQUEST, payload));
        }

        // All N replies are queued before we drain them; make room.
        self.bump_rcvbuf(16 << 20);

        let entries = ((2 * n) as u32).next_power_of_two().max(8);
        let mut ring = IoUring::new(entries).map_err(UrpError::Io)?;
        let fd = types::Fd(self.fd);

        // --- Phase 1: submit all sends, confirm each succeeded. ---
        {
            let mut sq = ring.submission();
            for (i, req) in reqs.iter().enumerate() {
                let e = opcode::Send::new(fd, req.as_ptr(), req.len() as u32)
                    .build()
                    .user_data(i as u64);
                // SAFETY: req buffers live in `reqs` until the fn returns.
                unsafe {
                    sq.push(&e)
                        .map_err(|_| UrpError::Netlink("uring sq full (send)".into()))?;
                }
            }
        }
        ring.submit_and_wait(n).map_err(UrpError::Io)?;
        {
            let mut cq = ring.completion();
            cq.sync();
            let mut got = 0;
            for cqe in &mut cq {
                got += 1;
                if cqe.result() < 0 {
                    return Err(UrpError::Netlink(format!(
                        "uring send failed: {}",
                        io::Error::from_raw_os_error(-cqe.result())
                    )));
                }
            }
            if got != n {
                return Err(UrpError::Netlink("uring: short send completion".into()));
            }
        }

        // --- Phase 2: submit N recvs + a timeout guard, drain N datagrams. ---
        const RECV_SZ: usize = 32 * 1024;
        let mut rbuf = vec![0u8; n * RECV_SZ];
        let ts = types::Timespec::new().sec(5);
        const TIMEOUT_UD: u64 = u64::MAX;
        {
            let mut sq = ring.submission();
            for i in 0..n {
                // SAFETY: distinct RECV_SZ slice per recv, all within `rbuf`.
                let ptr = unsafe { rbuf.as_mut_ptr().add(i * RECV_SZ) };
                let e = opcode::Recv::new(fd, ptr, RECV_SZ as u32)
                    .build()
                    .user_data(i as u64);
                unsafe {
                    sq.push(&e)
                        .map_err(|_| UrpError::Netlink("uring sq full (recv)".into()))?;
                }
                let _ = i;
            }
            let t = opcode::Timeout::new(&ts).build().user_data(TIMEOUT_UD);
            // SAFETY: `ts` lives until the fn returns (past the reap loop).
            unsafe {
                sq.push(&t)
                    .map_err(|_| UrpError::Netlink("uring sq full (timeout)".into()))?;
            }
        }

        let mut recv_len: Vec<Option<usize>> = vec![None; n];
        let mut done = 0usize;
        let mut timed_out = false;
        ring.submit()?;
        while done < n && !timed_out {
            ring.submit_and_wait(1).map_err(UrpError::Io)?;
            let mut cq = ring.completion();
            cq.sync();
            for cqe in &mut cq {
                if cqe.user_data() == TIMEOUT_UD {
                    timed_out = true;
                    continue;
                }
                let idx = cqe.user_data() as usize;
                if cqe.result() > 0 && idx < n {
                    recv_len[idx] = Some(cqe.result() as usize);
                }
                done += 1;
            }
        }
        if done < n {
            return Err(UrpError::Netlink("uring recv timeout".into()));
        }

        // Decode each datagram, mapping seq -> input index.
        let mut out: Vec<Option<Vec<u8>>> = vec![None; n];
        for (ri, len) in recv_len.iter().enumerate() {
            let Some(len) = *len else { continue };
            let msg = &rbuf[ri * RECV_SZ..ri * RECV_SZ + len];
            if msg.len() < NLMSG_HDR_LEN {
                continue;
            }
            let nlmsg_len = u32::from_ne_bytes(msg[0..4].try_into().unwrap()) as usize;
            let nlmsg_type = u16::from_ne_bytes(msg[4..6].try_into().unwrap());
            let mseq = u32::from_ne_bytes(msg[8..12].try_into().unwrap());
            let idx = mseq.wrapping_sub(base) as usize;
            if idx >= n {
                continue; // stale/foreign datagram
            }
            if nlmsg_type == NLMSG_ERROR {
                out[idx] = None; // request errored (endpoint raced away)
                continue;
            }
            let body_start = NLMSG_HDR_LEN + GENL_HDR_LEN;
            let body_end = nlmsg_len.min(msg.len());
            if body_end > body_start {
                out[idx] = Some(msg[body_start..body_end].to_vec());
            }
        }
        Ok(out)
    }

    pub fn subscribe_events(&mut self) -> Result<(), UrpError> {
        if self.events_mcgrp_id == 0 {
            return Err(UrpError::Netlink(
                "events multicast group not advertised by kernel".into(),
            ));
        }
        const NETLINK_ADD_MEMBERSHIP: c_int = 1;
        let id: u32 = self.events_mcgrp_id;
        let r = unsafe {
            libc::setsockopt(
                self.fd,
                libc::SOL_NETLINK,
                NETLINK_ADD_MEMBERSHIP,
                &id as *const _ as *const c_void,
                size_of::<u32>() as u32,
            )
        };
        if r < 0 {
            return Err(UrpError::Io(io::Error::last_os_error()));
        }
        Ok(())
    }

    /// Block waiting for a multicast event; returns the inner attribute blob.
    pub fn recv_event(&self) -> Result<Vec<u8>, UrpError> {
        let chunk = self.recv_raw()?;
        if chunk.len() < NLMSG_HDR_LEN + GENL_HDR_LEN {
            return Err(UrpError::Netlink("short event".into()));
        }
        let nlmsg_len = u32::from_ne_bytes(chunk[0..4].try_into().unwrap()) as usize;
        let body_start = NLMSG_HDR_LEN + GENL_HDR_LEN;
        let body_end = nlmsg_len.min(chunk.len());
        Ok(chunk[body_start..body_end].to_vec())
    }
}

/// Parse an NLMSGERR extack (NLA in the trailer past `struct nlmsgerr`).
/// Best-effort -- returns "" if absent.
fn parse_extack(msg: &[u8]) -> String {
    // Layout: nlmsghdr(16) + nlmsgerr { i32 error; nlmsghdr orig(16); }
    // Then optional nlattrs. Type 1 = NLMSGERR_ATTR_MSG (string).
    const NLMSGERR_ATTR_MSG: u16 = 1;
    let off = NLMSG_HDR_LEN + 4 + NLMSG_HDR_LEN;
    if msg.len() < off {
        return String::new();
    }
    for (t, p) in AttrIter::new(&msg[off..]) {
        if t == NLMSGERR_ATTR_MSG {
            if let Some(s) = payload_str(p) {
                return s.to_string();
            }
        }
    }
    String::new()
}
