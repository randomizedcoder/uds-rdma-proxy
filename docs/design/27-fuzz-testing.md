# 27. Comprehensive Fuzz-Testing Plan

Status: **implemented (living document)** — drafted 2026-08-09 as a plan
(after the kthread-pinning fix, PR #4); tracks F0–F3 were built out and
merged 2026-08-10/11 (PRs #12–#21). Per-section **IMPLEMENTED** markers
record what landed; §27.7 lists the few remaining optional items
(lifecycle/churn fuzz, corpus artifacts, coverage reporting).
Scope choices (owner-approved): syzkaller **and** userspace harness
extraction for the kernel C; a Rust-vs-C **differential** fuzzer; wired as
**Nix targets + nightly CI**.

## 27.1 Why, and the threat model

`urp.ko` parses **attacker-controllable bytes in kernel space**. For a kernel
module, review + example tests are not enough — every parsing surface needs
coverage-guided adversarial input. This planning sweep already found two real,
remotely-triggerable bugs by inspection (§27.8); fuzzing is how we find the
rest and keep them found. Surfaces, ordered by hostility:

| # | surface | who controls it | privilege | today |
|---|---|---|---|---|
| S1 | **RDMA wire input**: frame header (stream_id/seq/type/flags/credits/payload_len), SYN/FIN/RST state machine, reorder, credit frames, PING/PONG | remote peer — assume compromised | reach port 4791 (+PSK if set) | **none** in kernel C at drafting time; Rust crate has 5 cargo-fuzz targets |
| S2 | **CM `private_data`** (PSK blob, 0–255 B) | remote peer, **pre-auth** | reach port 4791 | none |
| S3 | **Generic netlink** NEW/DEL/SET/GET_ENDPOINT nested attrs, dumpit, concurrency | local; GET is **unprivileged**, mutate needs CAP_NET_ADMIN | local | none (hand tests only) |
| S4 | **UDS data plane + lifecycle**: connect storms, half-close orderings, add/remove churn under load | any local user with socket perms | UDS path perms | soak (fixed patterns) |
| S5 | **Rust FFI** (opt-in reorder backend) | internal (driven by S1) | — | cargo-fuzz ×5 + miri, Rust side only; default C rbtree backend was unfuzzed until `.#fuzz-reorder` (§27.5) |

Key asymmetry: the *optional* Rust reorder backend is fuzzed; the *default* C
one is not — and S1/S2 are reachable by a remote attacker before any
credential beyond the optional PSK.

## 27.2 Concrete entry points (fuzz targets, with file:line)

**S1 wire — `urp_recv_done` (`kernel/urp_rdma.c:444`)** decodes the 20-byte
header from `buf->data` (`:477`) and dispatches:
- DATA (`:561`): `stream_id`/`flags` → `urp_stream_rx_dispatch`
  (`kernel/urp_stream.c:406`) → SYN `urp_stream_rx_syn` (`:239`) /
  RST `urp_stream_rx_rst` (`:340`) / FIN `urp_stream_rx_fin` (`:315`).
- CONTROL/CREDIT (`:488`): `urp_credit_grant` with a peer-chosen u16.
- PROBE (`:506`): PING → `urp_emit_pong_on` (`kernel/urp_pump.c:463`),
  PONG → EWMA update from peer-chosen `t_send_mono`.
- C frame decoders: `kernel/urp.h:497–618` (`urp_frame_decode_*`,
  `urp_ping_decode_*`, `urp_pong_*`) — unconditional loads, **no length
  guard, no `frame_type` validation, no flags validation**.

**S2 PSK — `urp_cm_accept_one` (`kernel/urp_rdma.c:692`)**, check at `:705`
(`peer_priv_len < sizeof(auth_priv) || memcmp(...)`), pre-auth from
`RDMA_CM_EVENT_CONNECT_REQUEST` (`:842`).

**S3 netlink — `kernel/urp_netlink.c`**: policy `:39–63`; `urp_parse_endpoint`
`:291`; NEW `urp_new_endpoint_doit` `:354` (peer_addr/bind_addr `memcpy` of
`sockaddr_in6` `:393–399`, paths/name/device strscpy, num_qps/buffer_* gets);
SET `:480` (mutates live `ep->num_qps`/`buffer_count` under `ep->lock`);
DEL `:443`; GET doit `:537` / dumpit `:611` (**unprivileged**).

**S5 FFI — `crates/uds-rdma-protocol-ffi/src/ffi.rs`**: `urp_rust_reorder_*`
(`:51/70/96/159/201/214/228`), pointer/length contracts.

**Rust parsers — `crates/uds-rdma-protocol/src/`**: `frame.rs` decode `:45`
(fuzzed), `probe.rs` PING/PONG `:54/101` (**unfuzzed**), `reorder.rs` `:51`
(fuzzed weakly), `credit.rs` `:37` (fuzzed weakly), `mtu.rs`/`qp.rs`
div-by-zero on `num_qps==0` (**unfuzzed**).

## 27.3 Program overview — four tracks

```
F0  Parser + differential fuzz (hermetic; per-push CI-able)
F1  Userspace C-harness extraction (libFuzzer+ASAN on the real C code)
F2  syzkaller on netlink + hostile-peer wire fuzzer (KCOV/KASAN VM)
F3  Lifecycle/churn fuzz + nightly CI wiring, corpus + crash management
```

Each track lands something runnable via `nix run`. F0/F1 are hermetic and
fast; F2 needs a VM; F3 is integration + automation.

## 27.4 Track F0 — parser & differential fuzzing (hermetic, Rust)

Goal: every Rust decode path fuzzed, and both reorder/credit backends checked
against each other for wire-format divergence.

1. **New Rust targets** (extend `fuzz/`): `probe_decode` (PING 32 B / PONG
   48 B, `probe.rs`), `frame_validate_flags` (exercise `frame.rs:70`, which
   nothing calls today), `mtu_qp` (div-by-zero hunt on `num_qps==0`,
   full-range `payload_len` for the `total_frame_size` unchecked add).
2. **Strengthen existing targets**: `reorder_buffer` currently truncates seq
   to u16 and uses 1-byte payloads — switch to `arbitrary` with full u64 seq
   (reach the `next_expected += 1` overflow near `u64::MAX`), variable/zero
   payloads, and `max_buffered ∈ {0, …}`. `credit_state_machine`: let grants
   reach `u16::MAX` (exercise the `saturating_add` ceiling) and
   `initial < 4` (the `threshold==0 ⇒ always-grant` amplification lever).
3. **Differential C-vs-Rust** (the highest-value quick win — see F1 for the
   C harness it needs): feed one `arbitrary` op-sequence to *both* reorder
   backends and *both* credit backends; assert identical delivery order,
   return/errno mapping, gap/pending counts. Known seed divergences to encode
   as first cases: `max_buffered==0` (C returns NULL, Rust accepts);
   alloc-failure mid-drain (C partial-drain state, Rust can't reach it);
   credit errno (`-EAGAIN` C vs `CreditExhausted`→`-ENOBUFS` Rust).

Oracles: no panic; monotone in-order delivery; `pending ≤ max_buffered`;
byte-total bounded; Rust and C agree bit-for-bit on observable behaviour.

## 27.5 Track F1 — userspace C-harness extraction (libFuzzer + ASAN)

Goal: fuzz the **real C code** (not a port) in userspace, coverage-guided,
under ASAN/UBSan — no VM, fast iteration, runs in CI.

**Status:** the RX frame classifier harness is **implemented** —
`nix run .#fuzz-classify` compiles the real `kernel/urp_frame.c`
(`urp_classify_frame`, extracted with the codec into `urp_frame.h` per E1)
against `nix/fuzz/urp_fuzz_shim.h` under `-fsanitize=fuzzer,address,undefined`.
First run: 55M execs, full branch coverage, no ASAN/UBSan report, and the
in-harness §27.8 invariant (never route an overrun frame) held throughout.

The **stateful RX sequence harness** is also **implemented** —
`nix run .#fuzz-rx-seq` compiles the real `kernel/urp_frame.c` +
`kernel/urp_stream_sm.c` (the pure state machine `urp_stream_next_state`,
extracted per E2 into its own dual-compile unit so it fuzzes standalone) and
drives a *sequence* of frames through classify -> flag/event dispatch ->
state machine with an in-memory per-stream table. This is the coverage-guided,
hermetic counterpart to the live wire fuzzer (F2): it reaches the *composition*
(SYN->RST->DATA on a destroyed stream, id reuse, SYN|RST in one frame,
interleaved half-closes) that single-function harnesses miss. Oracles:
ASAN/UBSan + decision invariants (next_state never yields an out-of-range
state; a rejected event never changes state; the §27.8 overrun guard). First
run: **6.0M execs in 91 s, no ASAN/UBSan/invariant abort**, corpus still growing.

The **default C reorder backend** is now fuzzed too — **`nix run .#fuzz-reorder`**
compiles the real `kernel/urp_reorder.c` against the kernel's OWN userspace
rbtree (`tools/lib/rbtree.c`), extracted at build time from the nixpkgs-pinned
kernel source (narHash-secured, not vendored); the libc allocators satisfy the
slab (with `__GFP_ZERO` honoured so `kzalloc` zeroes). Until this, only the
*optional* Rust backend was fuzzed (cargo-fuzz `reorder_ops`) — the default C
rbtree backend that actually runs was not. The oracle is a spec-model
differential against the documented `urp_reorder.h` contract (the same contract
the Rust `ReorderBuffer` mirrors): in-order contiguous delivery, byte-exact
payloads, `gap_count ≤ max_buffered`, + ASAN/UBSan on the rbtree usage and the
`kmalloc(sizeof+len)` add. First run: **3.7M execs, clean** (the one abort was
the U64_MAX saturation terminal corner — a harness-model boundary the Rust
harness also relaxes per PR #11, not a backend bug).

The remaining targets below (codec truncations, credit, PSK) reuse the same
shim + `nix/fuzz/` scaffolding.

The pure, allocator-and-verbs-free C units compile standalone if we stub a
handful of kernel symbols (`kmalloc`→`malloc`, `kfree`→`free`,
`put/get_unaligned_le*`, `pr_*`→noop, an rbtree shim or bundle
`lib/rbtree.c`). Targets:

1. **Frame codec** (`kernel/urp.h` inline decoders): feed 0–4096-byte buffers
   + truncations at every field boundary; assert no OOB read (ASAN), and that
   the C decoder's accept/reject set is characterised (it currently accepts
   `frame_type 3..255` — encode that as a known divergence vs Rust).
2. **C reorder backend** (`kernel/urp_reorder.c`): the differential peer for
   F0.3; ASAN catches the `kmalloc(sizeof(*node)+data_len)` unchecked-add and
   the partial-drain leak path.
3. **C credit backend** (`kernel/urp_credit.c`): differential peer; trivial.
4. **PSK compare** (`urp_cm_accept_one`'s length+memcmp logic, lifted): all
   lengths 0..255, NULL data, off-by-one, right-len/wrong-bytes. Also the
   place to assert we move to a **constant-time** compare (§27.8).

Build: a `nix/analysis/`-style `nix/fuzz/` dir, one `writeShellApplication`
or `stdenv` derivation per target wrapping `clang -fsanitize=fuzzer,address`.
`extraction/` holds the kernel-symbol shim header shared by all C targets.

## 27.6 Track F2 — syzkaller (netlink) + hostile-peer wire fuzzer (VM)

Goal: attacker-grade coverage of S1/S2/S3 through the **real module** in a
KCOV+KASAN guest — the only way to reach the RCU/locking/verbs integration
paths F0/F1 can't.

1. **Coverage-guided netlink fuzzer** (S3) — **IMPLEMENTED**
   (`nix/fuzz/netlink_cov_fuzz.c`, added **KCOV** to the sanitizer kernel in
   `nix/microvms/mkVm.nix`): rather than pull in syzkaller's whole VM-management
   stack (which does not fit this expect/console microVM harness), this is an
   in-house coverage-guided engine using syzkaller's own feedback mechanism —
   `/sys/kernel/debug/kcov` per-task PC coverage. The genl doit/dumpit handlers
   run synchronously in the caller's sendmsg/recvmsg context, so per-task KCOV
   captures their edges directly. It seeds a corpus with well-formed
   NEW/GET/SET/DEL messages (so mutation explores outward from real handler
   depth, not from noise), mutates cmd/attr-type/attr-len/payload/dump and
   attr add/remove/duplicate, executes each under KCOV, and keeps in the corpus
   only inputs that hit new (hashed) edges. Reports `execs/corpus/edges`. Wired
   as **Phase 10c2** of the sanitizer pair test (falls back to blind mode on a
   non-KCOV kernel). **On its first real run it found the `SET num_qps` OOB
   teardown bug (§27.8 #3)** — the payoff coverage feedback buys over blind
   mutation. Oracle: KASAN/KMEMLEAK/lockdep, now also scanned inline per fuzz
   phase (`scan_splat`).

   **KCOV_TRACE_CMP operand feedback** — **IMPLEMENTED.** The fuzzer
   periodically re-runs a corpus input under `KCOV_TRACE_CMP` (toggling the
   shared kcov fd's mode), reads the comparison records, and harvests the
   compile-time constants the kernel checked the input against (`KCOV_CMP_CONST`
   arg2 — magic numbers, enum values, exact-length gates) into a dictionary. A
   new mutation injects those constants into attr payloads (u32/u64), so the
   fuzzer can satisfy value-gated branches it would never guess. Measured
   effect: the dictionary saturates (`dict=512`) and edge coverage rose from
   ~3300 to ~3540 in the same 25 s window, KASAN/KMEMLEAK clean.

   Still open: if deeper coverage warrants, real syzkaller with authored
   `urp.txt` descriptions (this in-house engine is the pragmatic substitute).

1b. **Concurrent netlink racer** (S3 concurrency) — **IMPLEMENTED**
   (`nix/fuzz/netlink_race.c`, Phase 10c3): the coverage-guided fuzzer is
   single-threaded, so it cannot reach concurrency bugs. This spawns N threads
   (default 8, oversubscribed vs the 2-vCPU guest so preemption is frequent),
   each on its own netlink socket, hammering a small shared name pool (`r0..r3`)
   with NEW/DEL/SET/GET so lookups, inserts, `call_rcu` frees, and derefs
   collide on the same endpoint objects. Target: the **deref-after-rcu-unlock
   UAF** — endpoints have no kref (design 26), and the SET/GET handlers do
   `rcu_read_lock(); ep = lookup(); rcu_read_unlock(); ... deref ep` while DEL
   frees via `call_rcu`; also the double-DEL free. Oracle: KASAN + `scan_splat`.
   Status: runs ~18k concurrent ops with real endpoint bind/listen churn under
   KASAN, **clean in a 25 s window** — which does *not* disprove the hazard (the
   grace-period alignment is narrow; syzkaller finds these over hours). The
   endpoint-kref refactor (design 26) is the definitive fix; the racer is the
   regression guard and the nightly-long-run tool that would catch a live UAF.
2. **Hostile-peer wire fuzzer** (S1/S2) — **IMPLEMENTED** (`nix/fuzz/wire_fuzz.c`):
   a standalone librdmacm/libibverbs RC peer that completes the CM handshake
   into a live acceptor endpoint and injects malformed frames into the RX path
   (`urp_recv_done`). It mutates every header field, flag combination (incl.
   `SYN|RST` and `SYN|FIN` in one frame), the `payload_len` header field
   *independently* of the actual wire byte_len (so the classifier's
   overrun/oversize guards fire), runt frames < 20 B, unknown frame types
   0x03..0xFF (the DATA fall-through), and short PROBE frames. Its highest-value
   mode is **scripted stream sequences** on a small stream_id space (SYN→RST,
   SYN+RST-in-one-frame, RST-without-SYN, double-RST, SYN→data→RST, id reuse):
   a SYN on a fresh id makes the acceptor open a backend UDS + spawn a pump
   kthread, and the following RST drives `urp_stream_destroy` → `kthread_stop`
   → `sock_release` → `call_rcu` — the RST-under-RCU / kthread-lifecycle path
   the cooperative pair test structurally cannot reach. Deterministic (xorshift
   seeded from argv) and self-healing (reconnects if the RC QP errors).

   Wired as **Phase 10d** of the microVM pair test (`nix/microvms/lib.nix`):
   it stands up a *dedicated* `fuzz_acceptor` (port 4792, own socat backend) —
   the live `pair_acceptor`'s QP slots are already full with the real initiator,
   so a second peer is `rdma_reject`ed (`urp_rdma.c`: `qp_index >= num_qps`) —
   runs `wire_fuzz` from the peer VM against it, then tears the endpoint down
   (also exercising teardown right after hostile traffic). Oracle: the existing
   Phase 11b KASAN/KMEMLEAK/lockdep dmesg scan. The S2 PSK pre-auth surface is
   reachable by pointing it at a password-protected endpoint (the connect-time
   `private_data` hook is in place). Throughput is acceptor-bound (RNR backoff +
   a real stream/kthread/socat-fork per SYN), so raw frame counts are modest by
   design; coverage comes from state diversity, not volume. This is the track
   that would have caught both §27.8 bugs.

   Still open (the coverage-guided upgrade): add **KCOV** to the sanitizer
   kernel and drive `wire_fuzz`'s generator from `arbitrary` under coverage
   feedback (and/or point real syzkaller at S3) rather than blind mutation.

## 27.7 Track F3 — lifecycle/churn + automation

1. **Lifecycle fuzz** (S4): randomized connect storms, half-close orderings,
   payload sizes, and `urp add`/`drain`/`remove` churn *under* live traffic —
   an extension of the soak harness (`nix/soak-1h.nix`) with randomized rather
   than fixed patterns; run under the sanitizer kernel.
2. **Nix targets**: `nix run .#fuzz-<name>` for every target (F0 Rust, F1 C,
   F2 syz/wire). (A combined `.#fuzz-all-smoke` wrapper was considered and
   dropped — CI invokes the individual targets directly.)
3. **CI wiring** (matches repo philosophy) — **IMPLEMENTED**:
   - **per-push** (`ci.yml`, `fuzz-smoke` job, hosted runner): the hermetic F1
     harnesses `fuzz-classify` + `fuzz-rx-seq` for 45 s each under libFuzzer +
     ASAN/UBSan. Deterministic, cheap, no KVM; a new crash exits non-zero and
     fails the gate. `fuzz-reorder` is nightly-only (it pulls the pinned
     `kernel.src` for the rbtree, a ~140 MB fetch not worth every push).
   - **nightly** (`nightly.yml`, `fuzz-long` job, hosted): `fuzz-classify` +
     `fuzz-rx-seq` + `fuzz-reorder` for 10 min each (matrix), crash artifacts
     uploaded. The F2 live-module fuzzers (netlink blind/coverage-guided,
     racer, hostile wire) already run nightly **inside** the
     `microvm-sanitizers` pair test under KASAN/KMEMLEAK on the `[self-hosted,
     kvm]` runner — no separate job needed.
4. **Corpus + crashes** — **convention wired**: crash reproducers go under
   `fuzz/regressions/<target>/` and are replayed first by both the per-push and
   nightly jobs, so fixed bugs stay fixed (see `fuzz/regressions/README.md`).
   Still open: committing/minimising grown seed corpora as a nightly artifact,
   and coverage reporting (llvm-cov for F1, KCOV summary for F2).

The one F3 item still open is the **lifecycle/churn fuzz** (item 1 above): a
randomized-pattern extension of `nix/soak-1h.nix` run under the sanitizer
kernel.

## 27.8 Seed findings — bugs this sweep already found (fuzz oracles + fixes)

These are confirmed by code inspection during planning; they double as the
first regression cases and as motivation for the tracks above. **They are
real security bugs and should be fixed on their own branch, not left for the
fuzzer to re-derive.**

1. **RX info-leak — stale DMA-pool memory to the local app (S1, remote, high).**
   `wc->byte_len` is read *nowhere* in the module (`grep byte_len kernel/` →
   0). `urp_recv_done` (`urp_rdma.c:477`) takes `payload_len` from the
   attacker-controlled header, bounds it only against `URP_MAX_PAYLOAD`
   (`:478`), then `kernel_sendmsg`s that many bytes from the DMA buffer to the
   local UDS app (`:601–605`). A peer sending a 20-byte frame declaring
   `payload_length = 4076` leaks ~4 KiB of stale kernel pool memory into the
   local application. **Fix**: clamp/validate `payload_len` against
   `wc->byte_len - URP_FRAME_HEADER_SIZE`; drop frames whose declared length
   exceeds what actually arrived. Fuzzed by F1.1 + F2.2.
2. **Sleep inside RCU read-side on the RST path (S1, remote, high).**
   `urp_rdma.c:571` holds `rcu_read_lock()` across `urp_stream_rx_dispatch`;
   the RST branch (`urp_stream.c:422`) calls `urp_stream_rx_rst` →
   `urp_stream_destroy` → `urp_stream_pump_stop` → **`kthread_stop()`**, which
   sleeps — illegal in an RCU read-side critical section (lockdep
   `rcu_sleep_check`, potential deadlock). The SYN branch similarly reaches
   `kzalloc(GFP_KERNEL)` + `mutex_lock`. Not caught by the pair test because a
   cooperative socat peer closes with FIN, never RST — a hostile peer sends
   RST. **Fix**: defer the destroy/create out of the RCU section (collect
   under RCU, act after `rcu_read_unlock`), mirroring the existing
   backend-connect deferral at `:588`. Fuzzed by F2.2.

Both are the archetype of what a hostile-peer wire fuzzer (F2.2) finds and a
cooperative integration test cannot.

3. **`SET_ENDPOINT num_qps`/`buffer_count` OOB teardown (S3, local CAP_NET_ADMIN,
   high) — FOUND LIVE by the coverage-guided netlink fuzzer, now fixed.** Not a
   planning-time inspection finding: the KCOV-guided `netlink_cov_fuzz` (F2, §27.6)
   hit it on essentially its first run, driving `NEW_ENDPOINT(num_qps=1)` →
   `SET_ENDPOINT(num_qps=8)` → `DEL_ENDPOINT`. `ep->qps` is
   `kcalloc(ep->num_qps, ...)` at activation (`urp_qp.c:26`), but
   `urp_set_endpoint_doit` (`urp_netlink.c`) overwrote `ep->num_qps` live with no
   realloc. Teardown then walks `for (i = 0; i < ep->num_qps; i++) ep->qps[i]`
   (`urp_rdma_cleanup`, three loops) past the allocation → KASAN slab-out-of-bounds
   reading `.established`/`.cm_id`, then a **general-protection fault in
   `rdma_disconnect()`** on the garbage pointer (a smaller `num_qps` silently leaks
   the QPs above the new count). **Fix**: `num_qps`/`buffer_count` are activation-time
   sizing params — reject changing them on an active endpoint (`ep->qps != NULL`)
   with `-EBUSY`; callers remove and re-add. This is exactly the "SET-`num_qps`-OOB
   path" §27.2 flagged, now confirmed exploitable-to-crash and closed. The
   coverage-guided fuzzer's seed sequence is the regression case.

The KASAN report reached the console during the fuzz phase but the run's later
Phase 11b dmesg scan came back "clean" (the crash wedged the VM). Fixed too: each
fuzz phase now scans its own captured output for splats (`scan_splat`,
`nix/microvms/lib.nix`) and fails the run inline.

## 27.9 Sequencing

F0 (Rust, days) → F1 (C extraction + differential, the security core) →
seed-fix branch for §27.8 with F1/F2 regression cases → F2 (syz + wire, the
deepest coverage) → F3 (automation). F0/F1 gate per-push once green; F2/F3
run nightly.

## 27.10 Relation to other docs

- [26-upstream-readiness.md](26-upstream-readiness.md) — static analysis;
  fuzzing is the dynamic counterpart. The kref/netns/lock-scope follow-ups
  there overlap the concurrency oracles here.
- [12-testing.md](12-testing.md) — the deterministic test matrix fuzzing
  augments.
- `docs/DESIGN.md` §17 (security tiers) — the PSK constant-time-compare and
  the amplification levers (§27.8, and the `urp_send_event`-per-auth-failure
  path) belong in the security hardening backlog.
