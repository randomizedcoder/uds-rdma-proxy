# Direct-topology smoke runner (design 30 §30.14, `.#urp-bench-local`):
# C<->C, Rust<->Rust, and C<->Rust interop both ways over a plain UDS on
# this host — no urp.ko, no VM. Smoke cells run --verify full, so every
# payload byte is checked; the C<->Rust cells are the live differential
# test of the framing (§30.1).
#
# Runs via `nix run .#urp-bench-local` on a host with io_uring; a
# BENCH_SKIP from an unsupported mode fails the smoke (the smoke's modes
# are ancient; only sendzc is allowed to SKIP — that's its evidence).
{ pkgs }:

let
  benchC = import ./urp-bench.nix { inherit pkgs; };
  benchRs = import ./urp-bench-rs.nix { inherit pkgs; };
in
pkgs.writeShellApplication {
  name = "urp-bench-local";
  runtimeInputs = [ pkgs.coreutils pkgs.gnugrep ];
  text = ''
    C=${benchC}/bin/urp-bench
    RS=${benchRs}/bin/urp-bench
    DIR=$(mktemp -d /tmp/urp-bench.XXXXXX)
    SOCK="$DIR/b.sock"
    trap 'rm -rf "$DIR"' EXIT
    pass() { echo "PASS: $*"; }
    err()  { echo "FAIL: $*" >&2; exit 1; }

    # run <listener-bin> <connector-bin> <label> <args...>
    run_cell() {
      local lbin="$1" cbin="$2" label="$3"; shift 3
      rm -f "$SOCK"
      "$lbin" --listen "$SOCK" --id 2 "$@" > "$DIR/l.out" 2>&1 &
      local lpid=$!
      sleep 0.3
      if ! "$cbin" --connect "$SOCK" --id 1 "$@" > "$DIR/c.out" 2>&1; then
        cat "$DIR/c.out" "$DIR/l.out"
        err "$label: connector failed"
      fi
      wait "$lpid" || { cat "$DIR/c.out" "$DIR/l.out"; err "$label: listener failed"; }
      grep -h 'BENCH_' "$DIR/c.out" "$DIR/l.out"
      grep -q 'BENCH_OK .*verify=full' "$DIR/c.out" || err "$label: no BENCH_OK (connector)"
      grep -q 'BENCH_OK .*verify=full' "$DIR/l.out" || err "$label: no BENCH_OK (listener)"
      pass "$label"
    }

    COMMON=(--msg-size 4076 --batch 8 --count 2000 --verify full)
    for m in blocking uring-rw uring-fixed uring-bufring; do
      run_cell "$C"  "$C"  "c<->c $m"       --mode "$m" "''${COMMON[@]}"
      run_cell "$RS" "$RS" "rust<->rust $m" --mode "$m" "''${COMMON[@]}"
    done
    # Interop both ways — the live C-vs-Rust differential (§30.14).
    run_cell "$C"  "$RS" "c-listen/rust-connect uring-rw"       --mode uring-rw      "''${COMMON[@]}"
    run_cell "$RS" "$C"  "rust-listen/c-connect uring-fixed"    --mode uring-fixed   "''${COMMON[@]}"
    run_cell "$C"  "$RS" "c-listen/rust-connect uring-bufring"  --mode uring-bufring "''${COMMON[@]}"

    # One-way stream pattern (design 34 §34.4): connect=source, listen=sink,
    # blocking backend. Same four lang combos, so the C<->Rust cells double as
    # the differential for the stream path.
    STREAM=(--mode blocking --pattern stream --msg-size 4076 --batch 8 --count 5000 --verify full)
    run_cell "$C"  "$C"  "c<->c stream"              "''${STREAM[@]}"
    run_cell "$RS" "$RS" "rust<->rust stream"        "''${STREAM[@]}"
    run_cell "$C"  "$RS" "c-sink/rust-source stream" "''${STREAM[@]}"
    run_cell "$RS" "$C"  "rust-sink/c-source stream" "''${STREAM[@]}"

    # sendzc evidence probe: SKIP with the eopnotsupp evidence line is the
    # expected outcome on AF_UNIX; a BENCH_FAIL is not.
    rm -f "$SOCK"
    "$C" --listen "$SOCK" --id 2 --mode uring-sendzc "''${COMMON[@]}" > "$DIR/l.out" 2>&1 &
    zpid=$!
    sleep 0.3
    "$C" --connect "$SOCK" --id 1 --mode uring-sendzc "''${COMMON[@]}" > "$DIR/c.out" 2>&1 || true
    wait "$zpid" || true
    grep -h 'BENCH_' "$DIR/c.out" "$DIR/l.out"
    grep -q 'BENCH_FAIL' "$DIR/c.out" "$DIR/l.out" && err "sendzc probe hard-failed"
    pass "sendzc evidence probe"

    echo "URP_BENCH_LOCAL_OK"
  '';
}
