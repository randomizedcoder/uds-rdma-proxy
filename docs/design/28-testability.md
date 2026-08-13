# 28. Testability Review + Table-Driven-Test Refactoring Plan

Status: **review + plan (steps 1–2 implemented)** — 2026-08-09, updated
2026-08-11. Prompted by the observation that the two RX security bugs
(design 27 §27.8, fixed in the rx-hardening PR) both lived in code that
**no unit test could reach**. Since drafting: **E1 landed**
(`urp_classify_frame` in the dual-compile `kernel/urp_frame.c`, table-tested,
fuzzed by `.#fuzz-classify`) and **E2 landed** (`urp_stream_next_state` in
the dual-compile `kernel/urp_stream_sm.{h,c}`, with the Rust twin
`uds_rdma_protocol::stream`, table-tested, fuzzed by `.#fuzz-rx-seq`).
Steps 3–5 of §28.5 remain open.

## 28.1 The thesis

The bugs we keep finding cluster in the same place: pure decision logic that
is *entangled* with kernel infrastructure (`ib_wc`, DMA buffers, `rcu`,
`kthread`, `genl_info`) and therefore reachable only through a live VM. The
well-tested code (frame codec, credit, reorder, QP select) is exactly the
code that is already **pure and leaf-level**. The correlation is not a
coincidence — untestable code is where bugs hide.

So the work is twofold: (a) **extract the pure decision logic** out of the
entangled callbacks into small, side-effect-free functions, and (b)
**table-drive** the tests over them. Extraction is what unlocks coverage; the
table-driven style is what makes that coverage exhaustive and cheap to extend.

## 28.2 Where we are today

*(Counts below are as of drafting, 2026-08-09; after E1/E2 the suite is 34
KUnit cases — the additions cover the stream state machine and the RX
classifier — and the protocol crate gained the `stream` module twin.)*

**KUnit (`kernel/urp_test.c`, 30 cases, suite `"urp"`)** covers the pure
leaves only — frame/probe codec inlines (12), credit state (8, a 1:1 mirror
of the Rust credit tests), reorder rbtree (5), QP round-robin (3), buffer
free-list (3). Every case is **imperative/ad-hoc** (one scenario per
function); **none is table-driven**. Built via `CONFIG_KUNIT` into `urp.ko`
(`Kbuild:29`), run by scraping KTAP from dmesg inside the microVM boot
(`nix/test-kmod-k0.nix:177`) — there is no dedicated `kunit.py` runner or
`nix run .#kunit` target.

**Rust (~63 tests in the protocol crate, ~8 in the CLI)** — frame (24), mtu
(13), reorder (8), credit (8), probe (6), qp (4); CLI uapi/attr/format/main.
Also overwhelmingly one-assertion-per-fn. **Only three tests are genuinely
table-driven**, and the best of them is the reference to standardize on:
`crates/urp-cli/src/uapi.rs:261` iterates a `&[(&str, String)]` slice diffing
each Rust constant against the re-parsed UAPI header.

**Untested entirely** (no KUnit, no Rust twin, live-path only): `urp_recv_done`
frame dispatch/validation (`urp_rdma.c`), the SYN/FIN/RST **stream state
machine** (`urp_stream.c`), netlink attribute→config build (`urp_netlink.c`),
the TX pump, socket/endpoint lifecycle. **This is precisely where §27.8's two
bugs were.**

## 28.3 The extract points (highest leverage first)

### E1 — Frame classifier out of `urp_recv_done` (`urp_rdma.c:444`) — **DONE** (`kernel/urp_frame.c`)

Today one `void(ib_cq*, ib_wc*)` callback interleaves the whole
classify→validate→route decision tree with `wc` reads, DMA, credit/EWMA
mutation, rhashtable dispatch, blocking connect, and `kernel_sendmsg`. The
pure part is keyed only on `(byte_len, 20 header bytes)`:

- short-frame reject; oversized reject; **payload-exceeds-received** (the
  §27.8 disclosure guard); CONTROL/PROBE/DATA type routing; short-PROBE
  guard; PONG-vs-PING; legacy-vs-per-stream routing.

**Extract:** `enum urp_rx_action urp_classify_frame(u32 byte_len, const u8 *hdr,
struct urp_rx_decoded *out)` returning `{DROP_SHORT, DROP_OVERSIZE,
DROP_PAYLOAD_OVERRUN, DROP_SHORT_PROBE, CREDIT_GRANT, PROBE_PING, PROBE_PONG,
DELIVER_LEGACY, DELIVER_STREAM}` plus decoded `payload_len/stream_id/flags`.
`urp_recv_done` becomes "classify, then execute the action". A
`{byte_len, header} → action` case array table-tests the entire validation
surface — **including a regression row for each §27.8 bug** — and it has a
direct Rust twin in `frame::FrameHeader` to diff against.

### E2 — Stream state machine out of the handlers (`urp_stream.c`) — *the big one* — **DONE** (`kernel/urp_stream_sm.{h,c}` + Rust twin `stream.rs`)

The SYN/FIN/RST transitions are embedded in handlers that each `mutex_lock`
and do socket/kthread/rhashtable side effects, reachable only via the live RX
path. But the transitions themselves are pure `(state, event) → next_state`:
SYN → ESTABLISHED (or create→SYN_RECEIVED); FIN: ESTABLISHED→CLOSE_WAIT,
FIN_WAIT→CLOSED; RST →CLOSED; tx_fin: ESTABLISHED→FIN_WAIT,
CLOSE_WAIT→CLOSED.

**Extract:** `enum urp_stream_state urp_stream_next_state(enum urp_stream_state
cur, enum urp_stream_event ev, u32 *action_mask)` where the action mask is
`{SHUTDOWN_WR, SHUTDOWN_RDWR, CREATE, DESTROY}`. Handlers keep the locking +
socket calls but delegate the decision. A `{state, event} → {next, actions}`
table then covers the **entire transition matrix** in KUnit — at drafting
time the state machine had *zero* direct tests (since fixed by this
extraction; the transition table is now KUnit-covered and fuzzed).

This is also the **highest-leverage shared-logic win**: confirmed, the stream
state machine exists **only in C** — the Rust protocol crate has frame/probe/
reorder/credit/mtu/qp modules but **no `stream` module**. Extracting the pure
transition function lets us add a Rust `stream` twin and diff it exactly the
way credit/reorder are already kept in lock-step — turning the single most
security-relevant, least-tested surface into a doubly-checked one.

### E3 — Netlink config-build (`urp_netlink.c:354`) — lower leverage

`urp_new_endpoint_doit` mixes `nla_*` extraction with defaults/validation into
a heap `cfg`. Range checks are already declarative in the policy table and
enforced by the genl core, so the residual pure part is thin. **Extract** a
`urp_build_config(const struct urp_endpoint_fields *in, struct urp_endpoint
*cfg)` taking already-extracted POD fields — testable without `genl_info` —
but do this last; the payoff is smaller.

## 28.4 Standardize the table-driven idiom

Introduce one case-array idiom on each side and convert incrementally:

- **KUnit (new idiom):**
  ```c
  static const struct { u32 byte_len; u8 hdr[20]; enum urp_rx_action want; }
  classify_cases[] = { … };
  /* one KUnit case iterates, KUNIT_EXPECT_EQ per row with a labelled msg */
  ```
- **Rust (reference already in-tree):** the `&[(input, expected)]` slice loop
  at `uapi.rs:261`. Fold the per-value `mtu.rs`/`frame.rs` tests into single
  table tests.

Convention: each row carries a short label used in the assert message so a
failing row is identifiable (as `uapi.rs` already does).

## 28.5 Phased plan

1. **E2 stream state machine** — **DONE**: `urp_stream_next_state` extracted
   into the dual-compile `kernel/urp_stream_sm.{h,c}`, `{state,event}` KUnit
   table added, Rust `stream` module twin added, and the whole pipeline is
   additionally fuzzed by `.#fuzz-rx-seq` (design 27 F1).
2. **E1 frame classifier** — **DONE**: `urp_classify_frame` extracted into
   the dual-compile `kernel/urp_frame.c`, table-tested with the §27.8 bugs
   as regression rows, and fuzzed by `.#fuzz-classify`.
3. **Convert existing tests to tables**: `mtu.rs` (13→1), the per-flag/per-
   type frame and credit/reorder cases, and the KUnit frame/probe cases —
   using the 28.4 idiom. Pure mechanical, no code under test changes.
4. **E3 netlink config-build** if still worth it after 1–3.
5. **A real KUnit runner**: a `nix run .#kunit` target (kunit.py or a minimal
   boot-and-scrape) so the suite runs as a first-class check, not only as a
   side effect of the pair-test boot; wire the fast table tests into per-push
   CI.

## 28.6 Payoff

- The two surfaces that produced §27.8 (frame validation, stream transitions)
  become exhaustively table-tested and, for the state machine, cross-checked
  against an independent Rust model.
- New wire-format or state-machine cases become one table row, not a new
  function — which is what makes the fuzzing corpus (design 27) and the
  differential harnesses cheap to keep growing.
- Extraction shrinks the entangled callbacks to "decide, then act", which is
  also easier to review and to reason about under the concurrency rules that
  §27.8 turned on.

## 28.7 Relation to other docs

- [27-fuzz-testing.md](27-fuzz-testing.md) — fuzzing needs exactly these pure
  extract points (F0 differential, F1 C-harness). E1/E2 are shared
  infrastructure: the classifier and the state machine are the first things
  the userspace libFuzzer harnesses would target.
- [26-upstream-readiness.md](26-upstream-readiness.md) — "decide, then act"
  extraction also addresses the reviewer-facing "one function does too much"
  smell noted there.
- [29-code-review-refactor-plan.md](29-code-review-refactor-plan.md) — its two
  functional gaps (inert reorder buffer, ignored `buffer_count`) are the
  motivating cases for §28.8 below: both are *fully unit-tested and fuzzed*
  and *dead anyway*.

## 28.8 The integration / liveness layer (added 2026-08-11)

§28.1–28.7 are about **unit** coverage: extract pure logic, table-drive it.
That program worked — the frame classifier and stream state machine are now
exhaustively tested and differentially checked against Rust twins. But a
later review (design 29) found the reorder buffer **inert**: it has KUnit
cases, a spec-model differential, `fuzz-reorder`, and the Rust `reorder_ops`
twin — all green — and `urp_recv_done` never calls it. `stats.reorder_insertions`
is exported over netlink, printed by `urp stats`, and **no test asserts it is
ever non-zero**.

The lesson: **unit tests and fuzzers are structurally blind to dead code.**
They invoke the function under test directly, so "does the wired-up system
actually *reach* this?" is off their map. A component can be perfectly
tested and perfectly unreachable at the same time. Catching that needs a
different layer.

### 28.8.1 The liveness oracle

> For every configurable feature and every stat counter, there must be at
> least one integration scenario that **moves** it. A counter that no
> scenario can move is a dead-feature smell — either the feature is unwired
> or the test that should exercise it is missing.

This is cheap: the counters already exist and are already read back over
netlink. The oracle is one assertion per counter, in the VM pair test where
the real data path runs. It is the check that would have failed on day one
for the reorder buffer.

Two assertion primitives (implemented in `nix/microvms/lib.nix`, Phase 10b):

- `assert_moved <endpoint> <counter>` — hard-fail if, after a scenario that
  *must* have driven the counter, it is missing or still zero. Used for the
  traffic counters (`tx-frames`/`rx-frames`/`tx-bytes`/`rx-bytes`) after the
  echo round-trip + 12-stream burst, on **both** endpoints. This catches a
  silent data-path break directly.
- `expect_pending <endpoint> <counter> <why>` — a counter we *expect* inert
  today because of a documented gap. Never fails; logs its value on every
  run so the gap stays visible, and prints "promote this to assert_moved"
  the moment it goes non-zero. `reorder-insertions` is the first user
  (design 29 Gap 1). This is the honest XFAIL: green today, self-announcing
  the day the feature is wired.

### 28.8.2 Counter → scenario coverage map

| Counter | Scenario that should move it | Status |
|---|---|---|
| `tx-frames` / `rx-frames` / `tx-bytes` / `rx-bytes` | echo round-trip + 12-stream burst (Phase 10) | **asserted** (Phase 10b) |
| `reorder-insertions` / `reorder-drops` | crafted out-of-order peer (`urp-test-client … reorder`) | **LIVE** — design 29 Gap 1 wired the reorder RX path; Phase 10e asserts `reorder-insertions` moved, all frames delivered, zero drops |
| `auth-failures` | a connect with a mismatched PSK against a password-protected endpoint | **planned** — §28.8.3 |
| `credit-stalls` | sustained load that outruns credit replenishment | **planned** — surfaces under the soak/load scenario, not the light echo |
| `active-streams` | the 12-stream burst (transiently) | observable in diag; assert deferred (races reap) |
| `buffer-alloc-fails` | pool exhaustion under load | planned (load scenario) |
| effective `buffer_count` / `buffer_size` | add an endpoint with an explicit `--buffer-count`/`--buffer-size` and read it back | **LIVE** — design 29 Gap 2 wired both through the data path; `GET` returns the *effective* geometry and Phase 10f asserts `urp show` reports it and that payloads > the old 4076 ceiling transit byte-exact |

### 28.8.3 Roadmap for the pending scenarios

1. **Reorder liveness** (unblocks the design-29 Gap-1 fix): rxe delivers
   in-order *per QP*, so reordering only appears with `num_qps > 1` **and**
   genuine cross-QP arrival skew. Force it deterministically with a small
   crafted peer that sends frames with out-of-order sequence numbers (more
   reliable for a regression than stochastic `tc netem reorder`, design 12
   §12.7). Then flip `reorder-insertions` from `expect_pending` to
   `assert_moved`, and assert the *delivered* byte stream is still in-order
   and byte-exact.
2. **PSK mismatch phase**: stand up a second endpoint pair with mismatched
   `--password`, attempt a connect, and hard-assert (a) the connection is
   rejected and (b) `auth-failures` moved. A failure to reject is a real
   security regression, so this one asserts hard once added.
3. **Load/credit phase**: fold the randomized-churn soak (design 27 F3, the
   one open item) in under the sanitizer kernel and assert `credit-stalls`
   and `buffer-alloc-fails` move under pressure.

### 28.8.4 Why this belongs with the table-driven unit work

Same thesis, one level up: the unit layer makes each *decision* exhaustively
checkable; the liveness layer makes each *feature's participation in the
running system* checkable. Both are needed — the reorder buffer proves that
100% unit coverage of a component says nothing about whether the product
uses it.
