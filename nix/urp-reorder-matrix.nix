# Multi-QP reorder validation runner (status.md gap #1, `.#urp-reorder-matrix`),
# driven over SSH from `l` against the two hp boxes running `services.urp`.
#
#   nix run .#urp-reorder-matrix -- <acceptor-host> <initiator-host> <acceptor-ip> [port]
#   nix run .#urp-reorder-matrix -- hp1 hp3 10.10.2.1
#
# The per-stream reorder buffer (kernel/urp_reorder.c, wired at
# urp_rdma.c:urp_rx_deliver_stream) is exercised only when a single stream is
# striped across MULTIPLE QPs: the per-frame round-robin (urp_pump.c:338) puts
# adjacent seqs on independent RC QPs, and the one recv CQ then completes them
# in HW-post order, not send order -> seq != next_expected -> buffered. Every
# real-hardware run so far was single-QP, so this path had never fired on real
# RoCEv2. This runner closes that.
#
# For each (buffer_size, num_qps) sweep point it re-adds BOTH endpoints with
# those knobs (design 34 Option A), then runs a one-way `urp-bench --pattern
# stream --verify full` transfer (SOURCE on the initiator, SINK on the
# acceptor) and asserts, per cell, on the acceptor's `urp stats` deltas:
#
#   * num_qps > 1  => reorder-insertions > 0  (cross-QP skew engaged the buffer)
#   * num_qps == 1 => reorder-insertions == 0 (control: a single QP never skews)
#   * reorder-drops == 0                       (nothing lost/dropped)
#   * rx-frames advanced                       (the sink really received)
#   * sink BENCH_OK with verify=full           (bytes reassembled in order)
#
# Why BENCH_OK verify=full proves IN-ORDER delivery for a *stream* workload: urp
# slices the UDS byte stream into buffer_size urp frames that are NOT aligned to
# the bench frame boundaries, so a broken reorder (completion-order delivery)
# would scramble the reconstructed stream -> the sink's nested deframer sees bad
# magic / a payload mismatch -> not BENCH_OK. insertions>0 says the buffer
# engaged; BENCH_OK says it corrected the skew. (For an *echo* workload per-frame
# verify is order-tolerant, so this must be `--pattern stream`, and BUFSIZES must
# sweep so no cell aligns bench frames to urp frames.)
#
# Counters are netlink-only (`urp stats`), not in /proc/urp.
#
# Env overrides: URP_REORDER_BUFSIZES, URP_REORDER_NUMQPS, URP_REORDER_DUR,
# URP_REORDER_BUFCOUNT.
#
# NOT in ci-local (needs real hardware) — like urp-bw-matrix / urp-hw-matrix.
{ pkgs }:

let
  benchC = import ./urp-bench.nix { inherit pkgs; };
in
pkgs.writeShellApplication {
  name = "urp-reorder-matrix";
  runtimeInputs = [ pkgs.coreutils pkgs.gnugrep pkgs.gawk pkgs.openssh pkgs.nix ];
  text = ''
    if [ "$#" -lt 3 ]; then
      echo "usage: urp-reorder-matrix <acceptor-host> <initiator-host> <acceptor-ip> [port]" >&2
      exit 2
    fi
    ACC="$1"; INIT="$2"; ACC_IP="$3"; PORT="''${4:-4791}"

    BENCH=${benchC}/bin/urp-bench

    # Standing services.urp convention on hp1/hp3 ([[hp-urp-deploy-recipe]]).
    ACC_EP=pair_acceptor;  ACC_SOCK=/run/urp-echo.sock  # acceptor connectPath: sink --listen here
    INIT_EP=pair_initiator; INIT_SOCK=/run/urp.sock     # initiator listenPath: source --connect here

    # Small -> large so no cell aligns bench (24B hdr + payload) to urp frames.
    # 68 = URP_BUFFER_SIZE_MIN (URP_FRAME_HEADER_SIZE 20 + URP_PONG_PAYLOAD_SIZE
    # 48): the smallest buffer that can still receive a QP-health PONG. Anything
    # below it is rejected at endpoint-create (a smaller recv buffer overflows on
    # the first PONG and crash-loops the QP), so 68 is the true small-frame floor.
    BUFSIZES="''${URP_REORDER_BUFSIZES:-68 4096 65516}"
    # 1 = control (must NOT reorder); 4/8 = the cross-QP skew cases.
    NUMQPS="''${URP_REORDER_NUMQPS:-1 4 8}"
    BUFCOUNT="''${URP_REORDER_BUFCOUNT:-1024}"
    DUR="''${URP_REORDER_DUR:-5}"

    ssh_o() { ssh -o BatchMode=yes -o ConnectTimeout=10 -l root "$@"; }

    echo "=== urp-reorder-matrix: acc=$ACC init=$INIT acc_ip=$ACC_IP:$PORT ==="

    # --- Preflight + stage the bench closure on both hosts -------------------
    for h in "$ACC" "$INIT"; do
      ssh_o "$h" 'lsmod | grep -q "^urp "' || { echo "FAIL: urp.ko not loaded on $h" >&2; exit 1; }
    done
    echo "  copying bench closure to hosts..."
    nix copy --no-check-sigs --to "ssh://root@$ACC"  "${benchC}"
    nix copy --no-check-sigs --to "ssh://root@$INIT" "${benchC}"

    # Restore the declarative endpoints on exit (stop -> remove -> reset -> start:
    # the unit's ExecStart is `urp add`, which fails if the swept EP still exists).
    restore() {
      echo "--- restoring declarative endpoints ---"
      ssh_o "$ACC"  "systemctl stop urp-endpoint-$ACC_EP 2>/dev/null; urp remove $ACC_EP 2>/dev/null; \
        systemctl reset-failed urp-endpoint-$ACC_EP 2>/dev/null; systemctl start urp-endpoint-$ACC_EP" >/dev/null 2>&1 || true
      ssh_o "$INIT" "systemctl stop urp-endpoint-$INIT_EP 2>/dev/null; urp remove $INIT_EP 2>/dev/null; \
        systemctl reset-failed urp-endpoint-$INIT_EP 2>/dev/null; systemctl start urp-endpoint-$INIT_EP" >/dev/null 2>&1 || true
    }

    # Re-add both endpoints with a given buffer_size / num_qps (Option A).
    reconfigure() {
      local bs="$1" q="$2"
      ssh_o "$ACC" "systemctl stop urp-endpoint-$ACC_EP 2>/dev/null; urp remove $ACC_EP 2>/dev/null; \
        urp add $ACC_EP --connect-path $ACC_SOCK --bind $ACC_IP:$PORT \
          --num-qps $q --buffer-count $BUFCOUNT --buffer-size $bs" >/dev/null 2>&1 || return 1
      ssh_o "$INIT" "systemctl stop urp-endpoint-$INIT_EP 2>/dev/null; urp remove $INIT_EP 2>/dev/null; \
        urp add $INIT_EP --listen-path $INIT_SOCK --peer $ACC_IP:$PORT \
          --num-qps $q --buffer-count $BUFCOUNT --buffer-size $bs" >/dev/null 2>&1 || return 1
      return 0
    }

    # Snapshot an endpoint's full counter set via the netlink CLI (dash-keyed,
    # colon-separated, one field per indented line). /proc/urp lacks the reorder
    # counters, so `urp stats` is the only source.
    snap() { ssh_o "$1" "urp stats $2 2>/dev/null" 2>/dev/null || true; }
    statf() { echo "$1" | awk -v k="$2:" '$1==k{print $2}'; }
    delta() { local a b; a=$(statf "$1" "$3"); b=$(statf "$2" "$3"); \
      if [ -n "$a" ] && [ -n "$b" ]; then echo $((b - a)); else echo "?"; fi; }

    RESULTS=$(mktemp /tmp/urp-reorder.XXXXXX)
    trap 'rm -f "$RESULTS"; restore' EXIT

    kill_sinks() { ssh_o "$ACC" "pkill -f 'urp-bench.*--listen' 2>/dev/null; true" >/dev/null 2>&1 || true; }
    # The acceptor connects its connect-path UDS lazily on a stream's FIRST frame
    # and does NOT retry -- the sink MUST be bound before the source sends, or
    # every frame is dropped. Confirm the socket node is in /proc/net/unix.
    sink_bound() { ssh_o "$ACC" "grep -q '$(basename "$ACC_SOCK")' /proc/net/unix 2>/dev/null"; }

    fails=0

    # --- Sweep ---------------------------------------------------------------
    for bs in $BUFSIZES; do
      for q in $NUMQPS; do
        SINK_UNIT="urpbench-reorder-sink-$bs-$q"
        echo "--- buffer_size=$bs num_qps=$q msg_size=$bs verify=full ---"
        if ! reconfigure "$bs" "$q"; then
          echo "WARN: reconfigure failed at bs=$bs q=$q" >&2; fails=$((fails + 1)); continue
        fi
        sleep 2  # let rdma_listen come up on the acceptor

        # Sink FIRST (verify=full: it is the receiver that checks byte order),
        # confirm bound before the source runs.
        kill_sinks
        ssh_o "$ACC" "systemctl stop $SINK_UNIT 2>/dev/null; systemctl reset-failed $SINK_UNIT 2>/dev/null; \
          rm -f $ACC_SOCK; systemd-run --unit=$SINK_UNIT --collect $BENCH --listen $ACC_SOCK \
            --id 2 --mode blocking --pattern stream --msg-size $bs --batch 16 --duration $((DUR+25)) --verify full" >/dev/null 2>&1 || true
        bound=""
        for _ in 1 2 3 4 5 6 7 8 9 10; do
          if sink_bound; then bound=yes; break; fi
          sleep 0.5
        done
        if [ -z "$bound" ]; then
          echo "WARN: sink did not bind $ACC_SOCK at bs=$bs q=$q -- skipping" >&2
          kill_sinks; fails=$((fails + 1)); continue
        fi
        sink_inv=$(ssh_o "$ACC" "systemctl show -p InvocationID --value $SINK_UNIT 2>/dev/null" 2>/dev/null || true)

        # Measured run: source blasts for DUR (verify=full so it fills the
        # deterministic payload the sink checks). Counters are cumulative, so
        # snapshot the acceptor (sink = where the reorder buffer lives) around it.
        acc_before=$(snap "$ACC" "$ACC_EP")
        ssh_o "$INIT" "$BENCH --connect $INIT_SOCK --id 1 --mode blocking --pattern stream \
          --msg-size $bs --batch 16 --duration $DUR --verify full" >/dev/null 2>&1 || true
        acc_after=$(snap "$ACC" "$ACC_EP")

        # Poll for the sink's BENCH_OK (verify=full passed) after the source FIN
        # propagates through urp to the sink's UDS.
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
        kill_sinks

        ins=$(delta "$acc_before" "$acc_after" reorder-insertions)
        drops=$(delta "$acc_before" "$acc_after" reorder-drops)
        rxd=$(delta "$acc_before" "$acc_after" rx-frames)

        # --- Per-cell verdict --------------------------------------------------
        verdict=PASS
        if [ -z "$sink" ]; then
          verdict="FAIL(no BENCH_OK: verify=full failed or sink silent)"
        elif [ "$drops" != "0" ]; then
          verdict="FAIL(reorder_drops=$drops)"
        elif [ "$rxd" = "0" ] || [ "$rxd" = "?" ]; then
          verdict="FAIL(no delivery: rx_frames=$rxd)"
        elif [ "$q" -gt 1 ]; then
          if [ "$ins" = "0" ] || [ "$ins" = "?" ]; then
            verdict="FAIL(insertions=$ins at qps=$q; raise URP_REORDER_NUMQPS or lower URP_REORDER_BUFSIZES)"
          fi
        else
          if [ "$ins" != "0" ]; then
            verdict="FAIL(single-QP reordered: insertions=$ins)"
          fi
        fi
        [ "$verdict" != "PASS" ] && fails=$((fails + 1))

        echo "buffer_size=$bs num_qps=$q reorder_insertions=''${ins} reorder_drops=''${drops} rx_delivered=''${rxd} verdict=$verdict" | tee -a "$RESULTS"
      done
    done

    # --- Report --------------------------------------------------------------
    echo ""
    echo "=== multi-QP reorder validation (sink = $ACC $ACC_EP) ==="
    awk '/verdict=/ {
      bs=""; q=""; ins=""; dr=""; rx=""; v=""
      for (i=1;i<=NF;i++){ n=index($i,"="); if(!n)continue; k=substr($i,1,n-1); val=substr($i,n+1)
        if(k=="buffer_size")bs=val; else if(k=="num_qps")q=val; else if(k=="reorder_insertions")ins=val
        else if(k=="reorder_drops")dr=val; else if(k=="rx_delivered")rx=val; else if(k=="verdict")v=substr($0,index($0,"verdict=")+8) }
      printf "  bufsz=%-6s qps=%-2s insertions=%-8s drops=%-4s rx=%-9s %s\n", bs,q,ins,dr,rx,v
    }' "$RESULTS"
    echo ""
    cells=$(grep -c 'verdict=' "$RESULTS" || echo 0)
    echo "URP_REORDER_MATRIX_DONE cells=$cells fails=$fails"
    if [ "$fails" -ne 0 ]; then
      echo "URP_REORDER_RESULT=RED"
      exit 1
    fi
    echo "URP_REORDER_RESULT=GREEN"
  '';
}
