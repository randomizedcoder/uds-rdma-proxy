# 27. Comprehensive Fuzz-Testing Plan

Status: **plan** — drafted 2026-08-09, after the kthread-pinning fix (PR #4).
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
| S1 | **RDMA wire input**: frame header (stream_id/seq/type/flags/credits/payload_len), SYN/FIN/RST state machine, reorder, credit frames, PING/PONG | remote peer — assume compromised | reach port 4791 (+PSK if set) | **none** in kernel C; Rust crate has 4 cargo-fuzz targets |
| S2 | **CM `private_data`** (PSK blob, 0–255 B) | remote peer, **pre-auth** | reach port 4791 | none |
| S3 | **Generic netlink** NEW/DEL/SET/GET_ENDPOINT nested attrs, dumpit, concurrency | local; GET is **unprivileged**, mutate needs CAP_NET_ADMIN | local | none (hand tests only) |
| S4 | **UDS data plane + lifecycle**: connect storms, half-close orderings, add/remove churn under load | any local user with socket perms | UDS path perms | soak (fixed patterns) |
| S5 | **Rust FFI** (opt-in reorder backend) | internal (driven by S1) | — | cargo-fuzz ×4 + miri, Rust side only; **default C rbtree backend unfuzzed** |

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
The remaining targets below (codec truncations, reorder, credit, PSK) reuse
the same shim + `nix/fuzz/` scaffolding.

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

1. **syzkaller on the genl family** (S3): author syscall descriptions
   (`urp.txt`) for the `"urp"` family — `URP_CMD_{NEW,DEL,SET,GET}_ENDPOINT`
   with the nested `URP_A_ENDPOINT` attr grammar (name/paths/device strings,
   `sockaddr_in6` binary attrs, num_qps/buffer_* u32s, PSK). Reuse the
   existing microVM sanitizer kernel (KASAN+KMEMLEAK+lockdep already wired in
   `nix/microvms/mkVm.nix`) but add **KCOV** for coverage feedback. syzkaller
   drives malformed nesting, truncated/oversized/duplicate attrs, and
   concurrent NEW/SET/DEL/GET on one name (the DEL-after-RCU-drop race, the
   SET-`num_qps`-OOB path). Oracle: any KASAN/KMEMLEAK/lockdep report or leak.
2. **Hostile-peer wire fuzzer** (S1/S2): a userspace RDMA client on the peer
   rxe device that speaks raw urp frames into a live acceptor endpoint in the
   VM. Mutates every header field, flag combination (incl. SYN|RST, SYN|FIN),
   payload_len vs actual bytes, unknown stream_ids (stream-exhaustion),
   PROBE payloads shorter than 32 B, and CM `private_data` of every length.
   Structured generator seeded from `arbitrary`; runs against the KASAN VM.
   This is the track that would have caught both §27.8 bugs.

## 27.7 Track F3 — lifecycle/churn + automation

1. **Lifecycle fuzz** (S4): randomized connect storms, half-close orderings,
   payload sizes, and `urp add`/`drain`/`remove` churn *under* live traffic —
   an extension of the soak harness (`nix/soak-1h.nix`) with randomized rather
   than fixed patterns; run under the sanitizer kernel.
2. **Nix targets**: `nix run .#fuzz-<name>` for every target (F0 Rust, F1 C,
   F2 syz/wire), plus `.#fuzz-all-smoke` (time-boxed, all targets).
3. **CI wiring** (matches repo philosophy):
   - **per-push** (`ci.yml`): `fuzz-all-smoke` — each F0/F1 target 30–60 s
     from the committed corpus. Deterministic, cheap; a new crash fails CI.
   - **nightly** (`nightly.yml`, self-hosted): long F0/F1 runs, the F2 syz +
     wire fuzzer for N hours in the KASAN VM, corpus minimization.
4. **Corpus + crashes**: commit a seed corpus per target under
   `fuzz/corpus/<t>/` (currently empty); persist/minimize the grown corpus as
   a nightly artifact; check crash reproducers into
   `fuzz/regressions/<t>/` and replay them in the per-push smoke run so fixed
   bugs stay fixed. Coverage reported per nightly (llvm-cov for F0/F1, KCOV
   summary for F2).

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
