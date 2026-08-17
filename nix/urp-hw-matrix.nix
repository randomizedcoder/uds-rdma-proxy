# Real-hardware client matrix (design 32, `.#urp-hw-matrix`), driven over SSH
# from the workstation `l` against two boxes running the `services.urp` module.
#
#   nix run .#urp-hw-matrix -- <acceptor-host> <initiator-host> <acceptor-ip>
#   URP_HW_MATRIX_QUICK=1 nix run .#urp-hw-matrix -- hp1 hp3 10.10.2.1
#
# For each cell (listener-lang x generator-lang x mode x msg-size x batch) it
# runs a `urp-bench{,-rs} --listen` on the ACCEPTOR (behind its endpoint's
# connectPath) and a matching `--connect` generator on the INITIATOR (into its
# endpoint's listenPath); data is tunnelled acceptor<->initiator over the
# standing RoCEv2 session. It scrapes BENCH_OK from both, then prints the four
# interop combos (C<->C, C<->Rust, Rust<->C, Rust<->Rust) as a delta table.
#
# Latency: BENCH_OK carries RTT p50/p99 (single-clock, always valid). A `pmc`
# PTP-offset check on the initiator bounds the hp1<->hp3 clock asymmetry, so
# RTT/2 is a defensible one-way estimate to within that offset. (A direct
# payload-timestamped one-way probe needs a urp-bench change — future work.)
#
# Requires: passwordless ssh from `l` to both hosts (root or a nix-trusted
# user), and `nix` on the hosts (NixOS). The bench binaries are copied to each
# host's store with `nix copy`, then invoked by absolute store path (both twins
# install a binary literally named `urp-bench`, so PATH is ambiguous).
{ pkgs }:

let
  benchC = import ./urp-bench.nix { inherit pkgs; };
  benchRs = import ./urp-bench-rs.nix { inherit pkgs; };
in
pkgs.writeShellApplication {
  name = "urp-hw-matrix";
  runtimeInputs = [ pkgs.coreutils pkgs.gnugrep pkgs.gawk pkgs.openssh pkgs.nix ];
  text = ''
    if [ "$#" -lt 3 ]; then
      echo "usage: urp-hw-matrix <acceptor-host> <initiator-host> <acceptor-ip>" >&2
      echo "  (acceptor runs the bench listener; initiator runs the generator)" >&2
      exit 2
    fi
    ACC="$1"; INIT="$2"; ACC_IP="$3"

    # Store paths of the two bench twins (same path on l and on the hosts).
    BENCH_C=${benchC}/bin/urp-bench
    BENCH_RS=${benchRs}/bin/urp-bench

    # Endpoint UDS paths — must match the hp `services.urp` declaration.
    ACC_SOCK=/run/urp-echo.sock     # acceptor endpoint connectPath (bench --listen here)
    INIT_SOCK=/run/urp.sock         # initiator endpoint listenPath  (bench --connect here)

    DIR=$(mktemp -d /tmp/urp-hw-matrix.XXXXXX)
    RESULTS="$DIR/results.txt"
    : > "$RESULTS"
    trap 'rm -rf "$DIR"' EXIT

    # C twin has uring-sendzc; Rust twin does not (it emits BENCH_SKIP).
    MODES="blocking uring-rw uring-fixed uring-bufring"
    SIZES="24 1024 4076 65516"
    BATCHES="1 16"
    DUR=3
    if [ -n "''${URP_HW_MATRIX_QUICK:-}" ]; then
      MODES="blocking uring-rw"
      SIZES="1024 65516"
      BATCHES="16"
      DUR=2
    fi
    # urp-bench is a symmetric peer echo: run_done needs own_fin_echoed &&
    # peer_fin_seen. If both ends use the same --duration the listener (started
    # ~1 s earlier) FINs and exits before it can echo the generator's final FIN,
    # so the generator waits and reports a false timeout. Give the listener a few
    # extra seconds so it outlives the generator's FIN; the matrix verdict scrapes
    # the generator's BENCH_OK, so the listener over-running is harmless.
    LDUR=$((DUR + 3))

    # Connect as root: the bench listener binds the acceptor's root-owned
    # backend socket (/run/urp-echo.sock) and the generator connects to the
    # root-owned initiator listen socket (/run/urp.sock); both, plus the
    # transient systemd unit, need root. root is also a trusted nix user, so
    # the closure copies below need no signatures.
    ssh_o() { ssh -o BatchMode=yes -o ConnectTimeout=10 -l root "$@"; }

    echo "=== urp-hw-matrix: acceptor=$ACC initiator=$INIT acc_ip=$ACC_IP ==="

    # --- Preflight: endpoints up + copy bench closures to both hosts ---------
    echo "--- preflight ---"
    for h in "$ACC" "$INIT"; do
      if ! ssh_o "$h" 'lsmod | grep -q "^urp "'; then
        echo "FAIL: urp.ko not loaded on $h" >&2; exit 1
      fi
    done
    ssh_o "$ACC"  "urp show" | grep -q pair_acceptor  || { echo "FAIL: no pair_acceptor on $ACC" >&2; exit 1; }
    ssh_o "$INIT" "urp show" | grep -q pair_initiator || { echo "FAIL: no pair_initiator on $INIT" >&2; exit 1; }
    echo "  endpoints present on both hosts"

    echo "  copying bench closures to hosts (nix copy)..."
    nix copy --no-check-sigs --to "ssh://root@$ACC"  "${benchC}" "${benchRs}"
    nix copy --no-check-sigs --to "ssh://root@$INIT" "${benchC}" "${benchRs}"
    echo "  bench binaries staged"

    # --- PTP health (bounds one-way estimate) --------------------------------
    echo "--- PTP sync health (pmc on $INIT) ---"
    PTP_OFFSET_NS="unknown"
    if pmc_out=$(ssh_o "$INIT" "pmc -u -b 0 'GET CURRENT_DATA_SET' 2>/dev/null"); then
      PTP_OFFSET_NS=$(echo "$pmc_out" | awk '/offsetFromMaster/ {print $2; exit}')
      [ -z "$PTP_OFFSET_NS" ] && PTP_OFFSET_NS="unknown"
    fi
    echo "  offsetFromMaster=$PTP_OFFSET_NS ns (RTT/2 is a valid one-way estimate to within this)"

    bin_for() { if [ "$1" = c ]; then echo "$BENCH_C"; else echo "$BENCH_RS"; fi; }

    # --- Matrix --------------------------------------------------------------
    for llang in c rust; do        # listener language (on acceptor)
      for glang in c rust; do      # generator language (on initiator)
        LBIN=$(bin_for "$llang"); GBIN=$(bin_for "$glang")
        for mode in $MODES; do
          for sz in $SIZES; do
            for batch in $BATCHES; do
              cell="$llang<->$glang/$mode/$sz/$batch"
              ok=0
              for _ in 1 2 3; do
                # Start the listener on the acceptor's connectPath backend as a
                # transient systemd unit. Two subtleties, both of which silently
                # failed every cell before:
                #   * Do NOT `pkill -f 'urp-bench --listen'` here: the remote
                #     shell's own argv contains that exact string (from the
                #     `$LBIN --listen ...` below), so pkill kills its own shell
                #     before systemd-run runs. `systemctl stop urpbench-l` is the
                #     correct, targeted way to reap the prior listener.
                #   * systemd-run detaches into its own scope so the listener
                #     outlives the ssh session (a bare `nohup ... &` child gets
                #     reaped when the ssh session scope is torn down at logout).
                # --collect GCs the unit on exit; journalctl carries its BENCH_OK.
                ssh_o "$ACC" "systemctl stop urpbench-l 2>/dev/null; \
                  systemctl reset-failed urpbench-l 2>/dev/null; rm -f $ACC_SOCK; \
                  systemd-run --unit=urpbench-l --collect $LBIN --listen $ACC_SOCK \
                    --id 2 --mode $mode --msg-size $sz --batch $batch \
                    --duration $LDUR --verify full; sleep 1" >/dev/null 2>&1 || true
                # run the generator on the initiator's listenPath
                if gout=$(ssh_o "$INIT" "$GBIN --connect $INIT_SOCK --id 1 --mode $mode \
                    --msg-size $sz --batch $batch --duration $DUR --verify full 2>&1"); then
                  line=$(echo "$gout" | grep -h '^BENCH_' | head -1)
                  if echo "$line" | grep -q '^BENCH_OK'; then
                    echo "llisten=$llang gen=$glang $line" | tee -a "$RESULTS"
                    ok=1; break
                  elif echo "$line" | grep -q '^BENCH_SKIP'; then
                    echo "llisten=$llang gen=$glang $line" | tee -a "$RESULTS"
                    ok=1; break
                  fi
                fi
                sleep 1   # let the RC connection settle, retry
              done
              [ "$ok" -eq 0 ] && echo "WARN: cell $cell failed after 3 attempts" >&2
            done
          done
        done
      done
    done
    ssh_o "$ACC" "systemctl stop urpbench-l 2>/dev/null; \
      systemctl reset-failed urpbench-l 2>/dev/null; rm -f $ACC_SOCK" \
      >/dev/null 2>&1 || true

    # --- Interop delta table -------------------------------------------------
    echo ""
    echo "=== interop matrix (msgs_per_s per combo; c<->c RTT p50/p99 us) ==="
    # Result lines are prefixed "llisten=<l> gen=<g> BENCH_OK ..." (see the
    # tee above), so match BENCH_OK anywhere on the line, not anchored at ^.
    awk '
      /BENCH_OK/ {
        ll=""; gl=""; mode=""; sz=""; b=""; mps=""; p50=""; p99=""
        for (i = 1; i <= NF; i++) {
          split($i, kv, "=")
          if (kv[1] == "llisten") ll = kv[2]
          if (kv[1] == "gen") gl = kv[2]
          if (kv[1] == "mode") mode = kv[2]
          if (kv[1] == "msg_size") sz = kv[2]
          if (kv[1] == "batch") b = kv[2]
          if (kv[1] == "msgs_per_s") mps = kv[2]
          if (kv[1] == "p50_us") p50 = kv[2]
          if (kv[1] == "p99_us") p99 = kv[2]
        }
        key = mode ":" sz ":" b
        combo = ll "-" gl
        v[combo, key] = mps
        if (combo == "c-c") { l50[key]=p50; l99[key]=p99 }
        keys[key] = 1
      }
      END {
        printf "  %-14s %7s %5s | %10s %10s %10s %10s | %9s %9s\n", \
          "mode","msg","batch","c<->c","c<->rust","rust<->c","rust<->rust","cc_p50us","cc_p99us"
        for (k in keys) {
          split(k, p, ":")
          printf "  %-14s %7s %5s | %10s %10s %10s %10s | %9s %9s\n", \
            p[1], p[2], p[3], \
            (("c-c",k) in v ? v["c-c",k] : "-"), \
            (("c-rust",k) in v ? v["c-rust",k] : "-"), \
            (("rust-c",k) in v ? v["rust-c",k] : "-"), \
            (("rust-rust",k) in v ? v["rust-rust",k] : "-"), \
            (k in l50 ? l50[k] : "-"), (k in l99 ? l99[k] : "-")
        }
      }
    ' "$RESULTS" | (read -r h; echo "$h"; sort -k1,1 -k2,2n -k3,3n)

    oks=$(grep -c '^llisten.*BENCH_OK' "$RESULTS" || true)
    skips=$(grep -c '^llisten.*BENCH_SKIP' "$RESULTS" || true)
    echo ""
    echo "URP_HW_MATRIX_DONE cells_ok=$oks skips=$skips ptp_offset_ns=$PTP_OFFSET_NS"
  '';
}
