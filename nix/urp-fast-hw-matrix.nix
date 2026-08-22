# Zero-copy round-trip latency runner (design 31 PR5b, `.#urp-fast-hw-matrix`),
# the fast-path twin of urp-hw-matrix.nix. Driven over SSH from `l` against the
# two hp boxes running the urp module.
#
#   nix run .#urp-fast-hw-matrix -- <acceptor-host> <initiator-host> <acceptor-ip> [port]
#   nix run .#urp-fast-hw-matrix -- hp1 hp3 10.10.2.1
#
# For each message size it stands up a DEDICATED `--kind fast` endpoint pair (it
# never touches the standing declarative endpoints) and runs a fast echo:
#   * REFLECTOR on the acceptor (`--listen`, in-place zero-copy echo: flip the
#     BENCH_FLAG_ECHO byte and re-SEND the same pinned buffer), started first.
#   * PINGER on the initiator (`--connect`, generates + measures RTT), scraped
#     for BENCH_OK p50_us/p99_us.
# Unlike the copy hw-matrix (a *symmetric* peer echo), the fast echo is an
# *asymmetric* ping-pong, so there is no FIN-echo overrun to compensate for and
# no C<->Rust interop grid (the Rust fast backend is PR6). The pinger's RTT
# meaning is identical to the copy path's, so this drops straight into the
# design-34 copy-vs-zero-copy latency comparison.
#
# Latency is single-clock (RTT measured entirely on the pinger), so it is always
# valid; a pmc PTP-offset check bounds the hp1<->hp3 clock asymmetry only for the
# RTT/2 one-way estimate, exactly as in urp-hw-matrix.
#
# Env overrides: URP_FAST_SIZES, URP_FAST_BATCH, URP_FAST_DUR, URP_FAST_BUFCOUNT.
#
# NOT in ci-local (needs real hardware) -- like urp-bw-matrix / urp-hw-matrix.
{ pkgs }:

let
  benchC = import ./urp-bench.nix { inherit pkgs; };
  pocC = import ./urp-fast-poc.nix { inherit pkgs; };
in
pkgs.writeShellApplication {
  name = "urp-fast-hw-matrix";
  runtimeInputs = [ pkgs.coreutils pkgs.gnugrep pkgs.gawk pkgs.openssh pkgs.nix ];
  text = ''
    if [ "$#" -lt 3 ]; then
      echo "usage: urp-fast-hw-matrix <acceptor-host> <initiator-host> <acceptor-ip> [port]" >&2
      echo "  (acceptor runs the reflector; initiator runs the pinger)" >&2
      exit 2
    fi
    ACC="$1"; INIT="$2"; ACC_IP="$3"; PORT="''${4:-4797}"

    BENCH=${benchC}/bin/urp-bench
    POC=${pocC}/bin/urp-fast-poc

    # Dedicated fast endpoints (never the declarative pair_*); UDS paths are pure
    # role markers (connect-path => acceptor, listen-path => initiator).
    ACC_EP=fast_bench_acc;  ACC_SOCK=/run/urp-fast-bench-acc.sock
    INIT_EP=fast_bench_init; INIT_SOCK=/run/urp-fast-bench-init.sock

    SIZES="''${URP_FAST_SIZES:-24 1024 4076 65516}"
    BATCH="''${URP_FAST_BATCH:-1}"   # batch=1 => strict ping-pong (true RTT)
    DUR="''${URP_FAST_DUR:-3}"
    BUFCOUNT="''${URP_FAST_BUFCOUNT:-1024}"

    ssh_o() { ssh -o BatchMode=yes -o ConnectTimeout=10 -l root "$@"; }

    echo "=== urp-fast-hw-matrix: acc=$ACC init=$INIT acc_ip=$ACC_IP:$PORT batch=$BATCH ==="

    # --- Preflight + stage the bench + poc closures --------------------------
    for h in "$ACC" "$INIT"; do
      ssh_o "$h" 'lsmod | grep -q "^urp "' || { echo "FAIL: urp.ko not loaded on $h" >&2; exit 1; }
    done
    echo "  copying bench + poc closures to hosts..."
    nix copy --no-check-sigs --to "ssh://root@$ACC"  "${benchC}" "${pocC}"
    nix copy --no-check-sigs --to "ssh://root@$INIT" "${benchC}" "${pocC}"

    cleanup() {
      ssh_o "$ACC" "systemctl stop urpfastrefl 2>/dev/null; systemctl reset-failed urpfastrefl 2>/dev/null; \
        urp remove $ACC_EP 2>/dev/null; true" >/dev/null 2>&1 || true
      ssh_o "$INIT" "urp remove $INIT_EP 2>/dev/null; true" >/dev/null 2>&1 || true
    }
    trap cleanup EXIT

    # --- PTP health (bounds the RTT/2 one-way estimate) ----------------------
    PTP_OFFSET_NS="unknown"
    if pmc_out=$(ssh_o "$INIT" "pmc -u -b 0 'GET CURRENT_DATA_SET' 2>/dev/null"); then
      PTP_OFFSET_NS=$(echo "$pmc_out" | awk '/offsetFromMaster/ {print $2; exit}')
      [ -z "$PTP_OFFSET_NS" ] && PTP_OFFSET_NS="unknown"
    fi
    echo "  ptp offsetFromMaster=$PTP_OFFSET_NS ns (bounds RTT/2 one-way estimate)"

    reconfigure() {
      local bs="$1"
      ssh_o "$ACC" "urp remove $ACC_EP 2>/dev/null; \
        urp add $ACC_EP --kind fast --connect-path $ACC_SOCK --bind $ACC_IP:$PORT \
          --buffer-count $BUFCOUNT --buffer-size $bs" >/dev/null 2>&1 || return 1
      ssh_o "$INIT" "urp remove $INIT_EP 2>/dev/null; \
        urp add $INIT_EP --kind fast --listen-path $INIT_SOCK --peer $ACC_IP:$PORT \
          --buffer-count $BUFCOUNT --buffer-size $bs" >/dev/null 2>&1 || return 1
      return 0
    }

    ready() {
      local ok2 ok1
      ssh_o "$INIT" "$POC /dev/urp $INIT_EP 4096 8 2>&1 | grep -q URP_FAST_POC_OK" && ok2=1 || ok2=0
      ssh_o "$ACC"  "$POC /dev/urp $ACC_EP  4096 8 2>&1 | grep -q URP_FAST_POC_OK" && ok1=1 || ok1=0
      [ "$ok2" = 1 ] && [ "$ok1" = 1 ]
    }

    RESULTS=$(mktemp /tmp/urp-fast-hw.XXXXXX)
    trap 'rm -f "$RESULTS"; cleanup' EXIT

    # --- Sweep ---------------------------------------------------------------
    for sz in $SIZES; do
      # The endpoint's suppressed pump buffer_size must still admit the frame;
      # size it to the payload like the bw runner (bench pool geometry is its own).
      bs=$sz; [ "$bs" -lt 4096 ] && bs=4096
      echo "--- msg_size=$sz (batch=$BATCH) ---"
      if ! reconfigure "$bs"; then echo "WARN: reconfigure failed at sz=$sz" >&2; continue; fi
      rdy=""
      for _ in $(seq 1 20); do if ready; then rdy=yes; break; fi; sleep 0.5; done
      if [ -z "$rdy" ]; then echo "WARN: fast pair not REGISTER-ready at sz=$sz -- skipping" >&2; continue; fi

      # Reflector FIRST, given extra runtime so it outlives the pinger's window.
      ssh_o "$ACC" "systemctl stop urpfastrefl 2>/dev/null; systemctl reset-failed urpfastrefl 2>/dev/null; \
        systemd-run --unit=urpfastrefl --collect $BENCH --listen $ACC_SOCK \
          --id 2 --mode uring-cmd --fast-endpoint $ACC_EP --pattern echo \
          --msg-size $sz --batch $BATCH --duration $((DUR+25)) --verify none" >/dev/null 2>&1 || true
      sleep 2

      # Pinger: measures RTT locally, single-clock. Scrape its own BENCH_OK.
      pout=$(ssh_o "$INIT" "$BENCH --connect $INIT_SOCK --id 1 --mode uring-cmd --fast-endpoint $INIT_EP \
        --pattern echo --msg-size $sz --batch $BATCH --duration $DUR --verify none 2>&1" || true)
      ssh_o "$ACC" "systemctl stop urpfastrefl 2>/dev/null; systemctl reset-failed urpfastrefl 2>/dev/null" >/dev/null 2>&1 || true

      line=$(echo "$pout" | grep -h '^BENCH_OK' | head -1)
      if [ -n "$line" ]; then
        echo "$line" | tee -a "$RESULTS"
      else
        echo "WARN: no pinger BENCH_OK at sz=$sz" >&2
        echo "$pout" | grep -h '^BENCH_' | head -1 >&2 || true
      fi
    done

    # --- Report --------------------------------------------------------------
    echo ""
    echo "=== zero-copy round-trip latency (pinger single-clock) ==="
    echo "    (compare vs urp-hw-matrix c<->c RTT p50/p99 at the same msg_size)"
    awk '
      /BENCH_OK/ {
        sz=""; b=""; p50=""; p99=""; mps=""; spm=""
        for (i=1;i<=NF;i++){ split($i,kv,"="); k=kv[1]; v=kv[2]
          if(k=="msg_size")sz=v; if(k=="batch")b=v
          if(k=="p50_us")p50=v; if(k=="p99_us")p99=v
          if(k=="msgs_per_s")mps=v; if(k=="syscalls_per_msg")spm=v }
        printf "  msg=%-6s batch=%-3s p50_us=%-9s p99_us=%-9s msgs/s=%-9s syscalls/msg=%-6s\n",\
          sz,b,p50,p99,mps,spm
      }' "$RESULTS" | (read -r h; echo "$h"; sort -k1,1)
    echo ""
    echo "URP_FAST_HW_MATRIX_DONE points=$(grep -c BENCH_OK "$RESULTS" || echo 0) ptp_offset_ns=$PTP_OFFSET_NS"
  '';
}
