# One-way zero-copy bulk-throughput runner (design 31 PR5b, `.#urp-fast-bw-matrix`),
# the fast-path twin of urp-bw-matrix.nix. Driven over SSH from `l` against the
# two hp boxes running the urp module.
#
#   nix run .#urp-fast-bw-matrix -- <acceptor-host> <initiator-host> <acceptor-ip> [port]
#   nix run .#urp-fast-bw-matrix -- hp1 hp3 10.10.2.1
#
# It stands up a DEDICATED `--kind fast` endpoint pair (it never touches the
# standing declarative endpoints), then for each buffer_size sweep point runs a
# one-way `urp-bench --mode uring-cmd --pattern stream` transfer: the SOURCE on
# the initiator, the SINK on the acceptor. The NIC DMAs straight into/out of the
# app's pinned pool -- zero software copy on either host. Goodput is scraped from
# the SINK's BENCH_OK (§34.4 sink-measured goodput); the BENCH_OK line also
# carries syscalls_per_msg and cpu_us_per_msg for the copy-vs-zero-copy delta
# against urp-bw-matrix's AF_UNIX curves (design 34 §34.5.1: copy best 52.7 MB/s
# @64KB, post-bound). The measured win is the whole point of this runner.
#
# Unlike the copy path there is NO lazy UDS backend-connect: a fast endpoint
# dials the RC eagerly at activate, and the two sides interop over RC's own
# reliability (RNR retries cover a source that briefly outruns the sink's armed
# recvs). Readiness is a non-destructive urp-fast-poc REGISTER probe on each side.
#
# Env overrides: URP_FAST_BUFSIZES, URP_FAST_MSG, URP_FAST_BUFCOUNT,
# URP_FAST_DUR, URP_FAST_BATCH, URP_FAST_LINE_MBPS.
#
# NOT in ci-local (needs real hardware) -- like urp-bw-matrix / urp-hw-matrix.
{ pkgs }:

let
  benchC = import ./urp-bench.nix { inherit pkgs; };
  pocC = import ./urp-fast-poc.nix { inherit pkgs; };
in
pkgs.writeShellApplication {
  name = "urp-fast-bw-matrix";
  runtimeInputs = [ pkgs.coreutils pkgs.gnugrep pkgs.gawk pkgs.openssh pkgs.nix ];
  text = ''
    if [ "$#" -lt 3 ]; then
      echo "usage: urp-fast-bw-matrix <acceptor-host> <initiator-host> <acceptor-ip> [port]" >&2
      exit 2
    fi
    ACC="$1"; INIT="$2"; ACC_IP="$3"; PORT="''${4:-4796}"

    BENCH=${benchC}/bin/urp-bench
    POC=${pocC}/bin/urp-fast-poc

    # Dedicated fast endpoints (never the declarative pair_*). The UDS paths are
    # pure role markers -- a fast endpoint derives its role from the path attr
    # (listen-path => initiator, connect-path => acceptor) but binds no socket.
    ACC_EP=fast_bench_acc;  ACC_SOCK=/run/urp-fast-bench-acc.sock
    INIT_EP=fast_bench_init; INIT_SOCK=/run/urp-fast-bench-init.sock

    BUFSIZES="''${URP_FAST_BUFSIZES:-4096 16384 65516}"
    BUFCOUNT="''${URP_FAST_BUFCOUNT:-1024}"
    DUR="''${URP_FAST_DUR:-5}"
    BATCH="''${URP_FAST_BATCH:-16}"
    MSG="''${URP_FAST_MSG:-}"   # empty => msg_size follows buffer_size

    ssh_o() { ssh -o BatchMode=yes -o ConnectTimeout=10 -l root "$@"; }

    echo "=== urp-fast-bw-matrix: acc=$ACC init=$INIT acc_ip=$ACC_IP:$PORT ==="

    # --- Preflight + stage the bench + poc closures on both hosts ------------
    for h in "$ACC" "$INIT"; do
      ssh_o "$h" 'lsmod | grep -q "^urp "' || { echo "FAIL: urp.ko not loaded on $h" >&2; exit 1; }
    done
    echo "  copying bench + poc closures to hosts..."
    nix copy --no-check-sigs --to "ssh://root@$ACC"  "${benchC}" "${pocC}"
    nix copy --no-check-sigs --to "ssh://root@$INIT" "${benchC}" "${pocC}"

    # Remove the dedicated fast endpoints on exit (the declarative endpoints are
    # never touched, so there is nothing to restart).
    cleanup() {
      ssh_o "$ACC"  "urp remove $ACC_EP 2>/dev/null; true"  >/dev/null 2>&1 || true
      ssh_o "$INIT" "urp remove $INIT_EP 2>/dev/null; true" >/dev/null 2>&1 || true
    }
    trap cleanup EXIT

    # Add both fast endpoints fresh with the swept buffer_size. --buffer-size /
    # --buffer-count are the (suppressed) pump knobs; the bench's own pinned pool
    # geometry is derived from --msg-size, independent of these.
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

    # Non-destructive REGISTER-readiness probe: urp-fast-poc default mode does
    # REGISTER + validation edges + UNREGISTER and arms no recv WR, so it never
    # drains the QP. Both sides must pass before the measured run.
    ready() {
      local ok2 ok1
      ssh_o "$INIT" "$POC /dev/urp $INIT_EP 4096 8 2>&1 | grep -q URP_FAST_POC_OK" && ok2=1 || ok2=0
      ssh_o "$ACC"  "$POC /dev/urp $ACC_EP  4096 8 2>&1 | grep -q URP_FAST_POC_OK" && ok1=1 || ok1=0
      [ "$ok2" = 1 ] && [ "$ok1" = 1 ]
    }

    # Reap a sink by its transient unit (NOT `pkill -f 'urp-bench...'` -- that
    # pattern also matches the remote shell's own argv and kills the ssh session;
    # the copy hw-matrix learned this the hard way). --collect GCs the unit; the
    # per-iteration stop below clears any leftover from a crashed prior sweep.
    stop_sink() { ssh_o "$ACC" "systemctl stop $1 2>/dev/null; systemctl reset-failed $1 2>/dev/null; true" >/dev/null 2>&1 || true; }

    RESULTS=$(mktemp /tmp/urp-fast-bw.XXXXXX)
    trap 'rm -f "$RESULTS"; cleanup' EXIT

    # --- Sweep ---------------------------------------------------------------
    for bs in $BUFSIZES; do
      local_msg="''${MSG:-$bs}"
      SINK_UNIT="urpfastsink-$bs"
      echo "--- buffer_size=$bs msg_size=$local_msg ---"
      if ! reconfigure "$bs"; then
        echo "WARN: reconfigure failed at bs=$bs" >&2; continue
      fi
      # Wait for the RC session to establish + both pools to REGISTER-map.
      rdy=""
      for _ in $(seq 1 20); do
        if ready; then rdy=yes; break; fi
        sleep 0.5
      done
      if [ -z "$rdy" ]; then
        echo "WARN: fast pair not REGISTER-ready at bs=$bs -- skipping" >&2; continue
      fi

      # Sink FIRST (arms recvs); a source that briefly outruns it just RNR-retries.
      stop_sink "$SINK_UNIT"
      ssh_o "$ACC" "systemctl stop $SINK_UNIT 2>/dev/null; systemctl reset-failed $SINK_UNIT 2>/dev/null; \
        systemd-run --unit=$SINK_UNIT --collect $BENCH --listen $ACC_SOCK \
          --id 2 --mode uring-cmd --fast-endpoint $ACC_EP --pattern stream \
          --msg-size $local_msg --batch $BATCH --duration $((DUR+25)) --verify none" >/dev/null 2>&1 || true
      sleep 2
      sink_inv=$(ssh_o "$ACC" "systemctl show -p InvocationID --value $SINK_UNIT 2>/dev/null" 2>/dev/null || true)

      # Measured source: blast for DUR, then a bench FIN closes the sink's window.
      ssh_o "$INIT" "$BENCH --connect $INIT_SOCK --id 1 --mode uring-cmd --fast-endpoint $INIT_EP \
        --pattern stream --msg-size $local_msg --batch $BATCH --duration $DUR --verify none" >/dev/null 2>&1 || true

      # Poll for the sink's BENCH_OK (lands after the source's FIN propagates).
      sink=""
      for _ in $(seq 1 15); do
        if [ -n "$sink_inv" ]; then
          sink=$(ssh_o "$ACC" "journalctl _SYSTEMD_INVOCATION_ID=$sink_inv --no-pager -o cat 2>/dev/null | grep -h '^BENCH_OK' | tail -1" 2>/dev/null || true)
        else
          sink=$(ssh_o "$ACC" "journalctl -u $SINK_UNIT --no-pager -o cat 2>/dev/null | grep -h '^BENCH_OK' | tail -1" 2>/dev/null || true)
        fi
        [ -n "$sink" ] && break
        sleep 1
      done
      stop_sink "$SINK_UNIT"
      if [ -n "$sink" ]; then
        echo "buffer_size=$bs msg_size=$local_msg $sink" | tee -a "$RESULTS"
      else
        echo "WARN: no sink BENCH_OK at bs=$bs" >&2
      fi
    done

    # --- Report --------------------------------------------------------------
    LINE_MBPS="''${URP_FAST_LINE_MBPS:-25000}"
    echo ""
    echo "=== zero-copy bulk-throughput (sink-measured goodput) — line=$LINE_MBPS Mb/s ==="
    echo "    (compare vs urp-bw-matrix AF_UNIX goodput at the same msg_size)"
    awk -v line="$LINE_MBPS" '
      /BENCH_OK/ {
        bs=""; msz=""; mbps=""; mps=""; spm=""; cpm=""
        for (i=1;i<=NF;i++){ split($i,kv,"="); k=kv[1]; v=kv[2]
          if(k=="buffer_size")bs=v; if(k=="msg_size"&&msz=="")msz=v
          if(k=="mbps")mbps=v; if(k=="msgs_per_s")mps=v
          if(k=="syscalls_per_msg")spm=v; if(k=="cpu_us_per_msg")cpm=v }
        mbit = mbps*8
        pcln = (line>0) ? sprintf("%.2f%%", 100*mbit/line) : "-"
        printf "  bufsz=%-6s msg=%-6s goodput=%8s MB/s =%9.1f Mb/s  %%line=%-7s msgs/s=%-9s syscalls/msg=%-6s cpu_us/msg=%-6s\n",\
          bs,msz,mbps,mbit,pcln,mps,spm,cpm
      }' "$RESULTS"
    echo ""
    echo "URP_FAST_BW_MATRIX_DONE points=$(grep -c BENCH_OK "$RESULTS" || echo 0)"
  '';
}
