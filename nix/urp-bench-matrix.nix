# Full direct-topology matrix sweep (design 30 §30.7/§30.14,
# `.#urp-bench-matrix`): msg_size x batch x mode for BOTH languages over
# a plain UDS on this host, plus the BENCH_MEMCPY copy-cost yardstick.
# Emits every BENCH_* line, then a per-cell C-vs-Rust delta table whose
# expected value is ~0 (§30.1 — a sustained gap is a bug in the slower
# implementation).
#
#   nix run .#urp-bench-matrix                    # full (~30-45 min)
#   URP_BENCH_MATRIX_QUICK=1 nix run .#urp-bench-matrix   # smoke subset
{ pkgs }:

let
  benchC = import ./urp-bench.nix { inherit pkgs; };
  benchRs = import ./urp-bench-rs.nix { inherit pkgs; };
in
pkgs.writeShellApplication {
  name = "urp-bench-matrix";
  runtimeInputs = [ pkgs.coreutils pkgs.gnugrep pkgs.gawk ];
  text = ''
    BIN_C=${benchC}/bin/urp-bench
    BIN_RS=${benchRs}/bin/urp-bench
    DIR=$(mktemp -d /tmp/urp-bench.XXXXXX)
    SOCK="$DIR/b.sock"
    RESULTS="$DIR/results.txt"
    : > "$RESULTS"
    trap 'rm -rf "$DIR"' EXIT

    MODES="blocking uring-rw uring-fixed uring-bufring uring-sqpoll uring-sendzc"
    SIZES="24 64 256 1024 4076 4096 16384 65516 131072"
    BATCHES="1 4 16 64 256"
    DUR=2
    if [ -n "''${URP_BENCH_MATRIX_QUICK:-}" ]; then
      MODES="blocking uring-rw uring-fixed uring-bufring"
      SIZES="24 4076 65516"
      BATCHES="1 32"
      DUR=1
    fi

    for lang in c rust; do
      if [ "$lang" = c ]; then BIN=$BIN_C; else BIN=$BIN_RS; fi
      for sz in $SIZES; do
        "$BIN" --memcpy-baseline --msg-size "$sz" | sed "s/^/lang=$lang /" | tee -a "$RESULTS"
      done
      for mode in $MODES; do
        for sz in $SIZES; do
          for batch in $BATCHES; do
            rm -f "$SOCK"
            "$BIN" --listen "$SOCK" --id 2 --mode "$mode" --msg-size "$sz" \
              --batch "$batch" --duration "$DUR" --verify header \
              > "$DIR/l.out" 2>&1 &
            lpid=$!
            sleep 0.2
            if "$BIN" --connect "$SOCK" --id 1 --mode "$mode" --msg-size "$sz" \
                 --batch "$batch" --duration "$DUR" --verify header \
                 > "$DIR/c.out" 2>&1; then
              wait "$lpid" || true
              grep -h '^BENCH_' "$DIR/c.out" | tee -a "$RESULTS"
            else
              wait "$lpid" || true
              grep -h '^BENCH_' "$DIR/c.out" "$DIR/l.out" | tee -a "$RESULTS"
              echo "WARN: cell $lang/$mode/$sz/$batch failed" >&2
            fi
          done
        done
      done
    done

    echo ""
    echo "=== C-vs-Rust delta (msgs_per_s, (rust-c)/c %; expect ~0) ==="
    awk '
      /^BENCH_OK/ {
        lang=""; mode=""; sz=""; b=""; mps=""
        for (i = 1; i <= NF; i++) {
          split($i, kv, "=")
          if (kv[1] == "lang") lang = kv[2]
          if (kv[1] == "mode") mode = kv[2]
          if (kv[1] == "msg_size") sz = kv[2]
          if (kv[1] == "batch") b = kv[2]
          if (kv[1] == "msgs_per_s") mps = kv[2]
        }
        key = mode ":" sz ":" b
        v[lang, key] = mps
        keys[key] = 1
      }
      END {
        printf "  %-14s %8s %6s %12s %12s %8s\n", "mode", "msg", "batch", "c", "rust", "delta%"
        for (k in keys) {
          split(k, p, ":")
          c = v["c", k]; r = v["rust", k]
          if (c > 0 && r > 0)
            printf "  %-14s %8s %6s %12.0f %12.0f %+8.1f\n", p[1], p[2], p[3], c, r, 100 * (r - c) / c
        }
      }
    ' "$RESULTS" | sort -k1,1 -k2,2n -k3,3n
    echo "URP_BENCH_MATRIX_DONE cells=$(grep -c '^BENCH_OK' "$RESULTS" || true) skips=$(grep -c '^BENCH_SKIP' "$RESULTS" || true)"
  '';
}
