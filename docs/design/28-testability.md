# 28. Testability Review + Table-Driven-Test Refactoring Plan

Status: **review + plan** — 2026-08-09. Prompted by the observation that the
two RX security bugs (design 27 §27.8, fixed in the rx-hardening PR) both
lived in code that **no unit test could reach**.

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

### E1 — Frame classifier out of `urp_recv_done` (`urp_rdma.c:444`)

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

### E2 — Stream state machine out of the handlers (`urp_stream.c`) — *the big one*

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
table then covers the **entire transition matrix** in KUnit — the state
machine currently has *zero* direct tests (the header comment at
`urp_stream.c` even claims KUnit coverage that does not exist).

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

1. **E2 stream state machine** (biggest security + shared-logic win): extract
   `urp_stream_next_state`, add a `{state,event}` KUnit table, add a Rust
   `stream` module twin + its own table, wire a differential check like
   credit/reorder. Land the §27.8 RST transition as an explicit row.
2. **E1 frame classifier**: extract `urp_classify_frame`, table-test it with
   the two §27.8 bugs as regression rows, diff against `frame::FrameHeader`.
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
