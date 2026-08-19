# One-way bulk-throughput runner (design 34 §34.5, `.#urp-bw-matrix`), driven
# over SSH from `l` against the two hp boxes running `services.urp`.
#
#   nix run .#urp-bw-matrix -- <acceptor-host> <initiator-host> <acceptor-ip> [port]
#   nix run .#urp-bw-matrix -- hp1 hp3 10.10.2.1
#
# For each (buffer_size, num_qps) sweep point it re-adds BOTH endpoints with
# those kernel knobs (design 34 Option A), then runs a one-way `urp-bench
# --pattern stream` transfer: the SOURCE on the initiator (--connect the
# initiator's listenPath), the SINK on the acceptor (--listen the acceptor's
# connectPath). Goodput is scraped from the SINK's BENCH_OK line (§34.4 — the
# sink-measured number is the true delivered goodput). Per-run deltas of the
# `urp stats` counters (credit-stalls on the source, reorder-drops on the sink,
# tx-frames -> achieved frame rate) gate a "clean" run and flag copy-vs-post
# boundedness. /proc/urp exposes only byte/frame totals, so the netlink CLI is
# the counter source.
#
# Optional baselines (BW_BASELINES=1): iperf2 (TCP) and ib_write_bw (raw RDMA)
# over the same link, so urp goodput is reported as a % of each.
#
# Env overrides: URP_BW_BUFSIZES, URP_BW_NUMQPS, URP_BW_DUR, URP_BW_MSG,
# URP_BW_BUFCOUNT, BW_BASELINES.
#
# NOT in ci-local (needs real hardware) — like urp-hw-matrix.
{ pkgs }:

let
  benchC = import ./urp-bench.nix { inherit pkgs; };
  # iperf2 is not installed on the hp boxes; stage it by store path (same as
  # the bench closure) so the TCP baseline needs no host install. ib_write_bw
  # (the upstream `perftest` project) is not packaged in nixpkgs, so the RDMA
  # baseline falls back to whatever `ib_write_bw` is on the host PATH and skips
  # cleanly if absent.
  iperf2 = pkgs.iperf2;      # provides `iperf` (TCP, jumbo MTU)
in
pkgs.writeShellApplication {
  name = "urp-bw-matrix";
  runtimeInputs = [ pkgs.coreutils pkgs.gnugrep pkgs.gawk pkgs.openssh pkgs.nix ];
  text = ''
    if [ "$#" -lt 3 ]; then
      echo "usage: urp-bw-matrix <acceptor-host> <initiator-host> <acceptor-ip> [port]" >&2
      exit 2
    fi
    ACC="$1"; INIT="$2"; ACC_IP="$3"; PORT="''${4:-4791}"

    BENCH=${benchC}/bin/urp-bench

    # Standing services.urp convention on hp1/hp3 ([[hp-urp-deploy-recipe]]).
    ACC_EP=pair_acceptor;  ACC_SOCK=/run/urp-echo.sock  # acceptor connectPath: sink --listen here
    INIT_EP=pair_initiator; INIT_SOCK=/run/urp.sock     # initiator listenPath: source --connect here

    BUFSIZES="''${URP_BW_BUFSIZES:-4096 16384 65516}"
    NUMQPS="''${URP_BW_NUMQPS:-1}"
    BUFCOUNT="''${URP_BW_BUFCOUNT:-1024}"
    DUR="''${URP_BW_DUR:-5}"
    MSG="''${URP_BW_MSG:-}"   # empty => msg_size follows buffer_size

    ssh_o() { ssh -o BatchMode=yes -o ConnectTimeout=10 -l root "$@"; }

    echo "=== urp-bw-matrix: acc=$ACC init=$INIT acc_ip=$ACC_IP:$PORT ==="

    # --- Preflight + stage the bench closure on both hosts -------------------
    for h in "$ACC" "$INIT"; do
      ssh_o "$h" 'lsmod | grep -q "^urp "' || { echo "FAIL: urp.ko not loaded on $h" >&2; exit 1; }
    done
    echo "  copying bench closure to hosts..."
    nix copy --no-check-sigs --to "ssh://root@$ACC"  "${benchC}"
    nix copy --no-check-sigs --to "ssh://root@$INIT" "${benchC}"

    # Restore the declarative endpoints on exit, whatever happens. The unit's
    # ExecStart is `urp add`, so the swept endpoint must be removed first or the
    # restart fails with "endpoint already exists" (status 4) and leaves the
    # unit failed. Stop → remove → reset-failed → start.
    restore() {
      echo "--- restoring declarative endpoints ---"
      ssh_o "$ACC"  "systemctl stop urp-endpoint-$ACC_EP 2>/dev/null; urp remove $ACC_EP 2>/dev/null; \
        systemctl reset-failed urp-endpoint-$ACC_EP 2>/dev/null; systemctl start urp-endpoint-$ACC_EP" >/dev/null 2>&1 || true
      ssh_o "$INIT" "systemctl stop urp-endpoint-$INIT_EP 2>/dev/null; urp remove $INIT_EP 2>/dev/null; \
        systemctl reset-failed urp-endpoint-$INIT_EP 2>/dev/null; systemctl start urp-endpoint-$INIT_EP" >/dev/null 2>&1 || true
    }
    trap restore EXIT

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

    # Snapshot an endpoint's full counter set via the netlink CLI. /proc/urp
    # only exposes tx/rx bytes+frames; credit-stalls and reorder-drops live in
    # `urp stats` (dash-keyed, colon-separated, one field per indented line).
    snap() { ssh_o "$1" "urp stats $2 2>/dev/null" 2>/dev/null || true; }
    # statf <snapshot-text> <dash-key>  ->  numeric value ("" if absent).
    statf() { echo "$1" | awk -v k="$2:" '$1==k{print $2}'; }
    # Non-negative delta between two snapshots for a field ("?" if unreadable).
    delta() { local a b; a=$(statf "$1" "$3"); b=$(statf "$2" "$3"); \
      if [ -n "$a" ] && [ -n "$b" ]; then echo $((b - a)); else echo "?"; fi; }


    RESULTS=$(mktemp /tmp/urp-bw.XXXXXX)
    trap 'rm -f "$RESULTS"; restore' EXIT

    # Clear any bench sink still alive on the acceptor (its backstop duration
    # can outlast the inter-point spacing). The acceptor only runs sinks, so
    # this can't hit a source.
    kill_sinks() { ssh_o "$ACC" "pkill -f 'urp-bench.*--listen' 2>/dev/null; true" >/dev/null 2>&1 || true; }
    # The acceptor connects its connect-path UDS lazily on a stream's FIRST
    # frame and does NOT retry (urp_rdma.c:658 / urp_socket.c:166) -- so the
    # sink MUST be listening before the source sends, or every frame is dropped
    # as buffer_alloc_fails and never delivered. Confirm the socket node is
    # actually bound (in /proc/net/unix) before running the source.
    sink_bound() { ssh_o "$ACC" "grep -q '$(basename "$ACC_SOCK")' /proc/net/unix 2>/dev/null"; }

    # --- Sweep ---------------------------------------------------------------
    for bs in $BUFSIZES; do
      for q in $NUMQPS; do
        local_msg="''${MSG:-$bs}"
        # Unique sink unit per sweep point: journalctl -u <this> then yields
        # exactly this run's BENCH_OK (a fixed name let `tail -1` grab a stale
        # prior-point line when a run produced no fresh BENCH_OK).
        SINK_UNIT="urpbench-sink-$bs-$q"
        echo "--- buffer_size=$bs num_qps=$q msg_size=$local_msg ---"
        # reconfigure re-adds BOTH endpoints fresh (no stale streams) with the
        # swept knobs. The initiator dials the RC LAZILY on the first local UDS
        # client (design 33), so there is no connected:yes to gate on here --
        # the measured source's connect triggers the dial; ~tens of ms of
        # bring-up is negligible inside a multi-second window.
        if ! reconfigure "$bs" "$q"; then
          echo "WARN: reconfigure failed at bs=$bs q=$q" >&2; continue
        fi
        sleep 2  # let rdma_listen come up on the acceptor

        # Start the sink FIRST and confirm it is bound before any source runs
        # (the no-retry lazy backend-connect ordering requirement above).
        kill_sinks
        ssh_o "$ACC" "systemctl stop $SINK_UNIT 2>/dev/null; systemctl reset-failed $SINK_UNIT 2>/dev/null; \
          rm -f $ACC_SOCK; systemd-run --unit=$SINK_UNIT --collect $BENCH --listen $ACC_SOCK \
            --id 2 --mode blocking --pattern stream --msg-size $local_msg --batch 16 --duration $((DUR+25)) --verify none" >/dev/null 2>&1 || true
        bound=""
        for _ in 1 2 3 4 5 6 7 8 9 10; do
          if sink_bound; then bound=yes; break; fi
          sleep 0.5
        done
        if [ -z "$bound" ]; then
          echo "WARN: sink did not bind $ACC_SOCK at bs=$bs q=$q -- skipping" >&2; kill_sinks; continue
        fi
        # Capture THIS run's systemd invocation id so the journal scrape can't
        # pick up a BENCH_OK from a prior sweep (unit names repeat across runs).
        sink_inv=$(ssh_o "$ACC" "systemctl show -p InvocationID --value $SINK_UNIT 2>/dev/null" 2>/dev/null || true)

        # Measured run: source blasts for DUR; the acceptor delivers to the
        # already-listening sink, which reports goodput on the source's FIN.
        # Counters are cumulative with no reset, so snapshot both ends before
        # and after and report per-run deltas.
        init_before=$(snap "$INIT" "$INIT_EP")
        acc_before=$(snap "$ACC" "$ACC_EP")
        ssh_o "$INIT" "$BENCH --connect $INIT_SOCK --id 1 --mode blocking --pattern stream \
          --msg-size $local_msg --batch 16 --duration $DUR --verify none" >/dev/null 2>&1 || true
        init_after=$(snap "$INIT" "$INIT_EP")
        acc_after=$(snap "$ACC" "$ACC_EP")

        # Poll for the sink's BENCH_OK: it lands after the source's close
        # propagates through urp to the sink's UDS (can take a few seconds).
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
        # credit-stalls on the source (tx gate); reorder-drops on the sink (rx).
        stalls=$(delta "$init_before" "$init_after" credit-stalls)
        drops=$(delta "$acc_before" "$acc_after" reorder-drops)
        # Delivery sanity: acceptor rx-frames (successful UDS deliveries) must
        # advance; if it doesn't the sink wasn't really receiving.
        rxd=$(delta "$acc_before" "$acc_after" rx-frames)
        # Achieved source frame rate = Δtx-frames / DUR — the copy-vs-post-bound
        # tell (flat across buffer_size => post-bound; scales => copy-bound).
        txf=$(delta "$init_before" "$init_after" tx-frames)
        if [ "$txf" != "?" ] && [ "$DUR" -gt 0 ]; then fps=$((txf / DUR)); else fps="?"; fi
        if [ -n "$sink" ]; then
          echo "buffer_size=$bs num_qps=$q credit_stalls=''${stalls:-?} reorder_drops=''${drops:-?} rx_delivered=''${rxd:-?} frame_rate=''${fps:-?} $sink" | tee -a "$RESULTS"
        else
          echo "WARN: no sink BENCH_OK at bs=$bs q=$q (rx_delivered=''${rxd:-?})" >&2
        fi
      done
    done

    # --- Baselines (optional) -----------------------------------------------
    IPERF_MBPS="skip"; IBW_MBPS="skip"
    if [ -n "''${BW_BASELINES:-}" ]; then
      echo "--- baselines (iperf2 TCP, ib_write_bw raw RDMA) ---"
      IPERF=${iperf2}/bin/iperf
      echo "  copying iperf2 closure to hosts..."
      nix copy --no-check-sigs --to "ssh://root@$ACC"  "${iperf2}"
      nix copy --no-check-sigs --to "ssh://root@$INIT" "${iperf2}"
      # Server via a transient unit (the `-D &` daemonize was unreliable and
      # left nothing listening), then poll until :5001 is actually up before
      # the client connects.
      ssh_o "$ACC" "systemctl stop urp-iperf 2>/dev/null; systemctl reset-failed urp-iperf 2>/dev/null; \
        pkill -x iperf 2>/dev/null; sleep 0.3; systemd-run --unit=urp-iperf --collect $IPERF -s -f m" >/dev/null 2>&1 || true
      ssh_o "$ACC" "for i in \$(seq 1 12); do ss -ltn 2>/dev/null | grep -q :5001 && break; sleep 0.5; done" >/dev/null 2>&1 || true
      if ip_out=$(ssh_o "$INIT" "$IPERF -c $ACC_IP -t $DUR -f m 2>/dev/null"); then
        m=$(echo "$ip_out" | awk '/Mbits\/sec/{v=$(NF-1)} END{if(v)printf "%.0f", v/8}')  # Mbit->MB/s
        [ -n "$m" ] && IPERF_MBPS="$m"
      fi
      ssh_o "$ACC" "systemctl stop urp-iperf 2>/dev/null; systemctl reset-failed urp-iperf 2>/dev/null" >/dev/null 2>&1 || true
      # ib_write_bw only if present on the host PATH (not in nixpkgs).
      if ssh_o "$ACC" "command -v ib_write_bw >/dev/null 2>&1"; then
        ssh_o "$ACC" "pkill ib_write_bw 2>/dev/null; (ib_write_bw -d mlx5_0 --report_gbits >/dev/null 2>&1 &)" || true
        sleep 1
        if ibw_out=$(ssh_o "$INIT" "ib_write_bw -d mlx5_0 --report_gbits $ACC_IP 2>/dev/null"); then
          g=$(echo "$ibw_out" | awk '/[0-9]/{x=$(NF-1)} END{if(x)printf "%.0f", x*1000/8}')  # Gbit->MB/s
          [ -n "$g" ] && IBW_MBPS="$g"
        fi
        ssh_o "$ACC" "pkill ib_write_bw 2>/dev/null" || true
      else
        echo "  ib_write_bw not on host PATH -> RDMA baseline skipped" >&2
      fi
    fi

    # --- Report --------------------------------------------------------------
    # Link line rate in Mb/s (bits) for the %-of-link column. 25 GbE = 25000.
    LINE_MBPS="''${URP_BW_LINE_MBPS:-25000}"
    echo ""
    echo "=== bulk-throughput (sink-measured goodput) — line=$LINE_MBPS Mb/s  iperf2=''${IPERF_MBPS} MB/s ib_write_bw=''${IBW_MBPS} MB/s ==="
    # The bench's `mbps` field is bytes (MB/s); bits (Mb/s) = MB/s * 8.
    awk -v ip="$IPERF_MBPS" -v ibw="$IBW_MBPS" -v line="$LINE_MBPS" '
      /BENCH_OK/ {
        bs=""; q=""; mbps=""; msz=""; st=""; dr=""; fr=""
        for (i=1;i<=NF;i++){ split($i,kv,"="); k=kv[1]; v=kv[2]
          if(k=="buffer_size")bs=v; if(k=="num_qps")q=v; if(k=="mbps")mbps=v
          if(k=="msg_size")msz=v; if(k=="credit_stalls")st=v; if(k=="reorder_drops")dr=v
          if(k=="frame_rate")fr=v }
        mbit = mbps*8
        pcln = (line>0) ? sprintf("%.2f%%", 100*mbit/line) : "-"
        pcip = (ip!="skip" && ip>0) ? sprintf("%.1f%%", 100*mbps/ip) : "-"
        pcibw= (ibw!="skip"&&ibw>0)? sprintf("%.1f%%", 100*mbps/ibw): "-"
        printf "  bufsz=%-6s qps=%-2s msg=%-6s goodput=%8s MB/s =%9.1f Mb/s  %%line=%-7s %%iperf=%-7s %%ibw=%-7s fps=%-8s stalls=%-6s drops=%-6s\n",\
          bs,q,msz,mbps,mbit,pcln,pcip,pcibw,fr,st,dr
      }' "$RESULTS"
    echo ""
    echo "URP_BW_MATRIX_DONE points=$(grep -c BENCH_OK "$RESULTS" || echo 0)"
  '';
}
