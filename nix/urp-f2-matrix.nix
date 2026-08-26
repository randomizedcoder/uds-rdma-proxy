# F2 aggregate / N-flow throughput runner (design 34 Option F2, design 37 §37.6),
# driven over SSH from `l` against the two hp boxes running `services.urp`.
#
#   nix run .#urp-f2-matrix -- <acceptor-host> <initiator-host> <acceptor-ip> [base-port]
#   nix run .#urp-f2-matrix -- hp1 hp3 10.10.2.1
#
# Where urp-bw-matrix measures ONE stream (optionally wire-striped across num_qps
# QPs -- which is reorder-broken for a single ordered stream, design 34 §34.6),
# this measures N genuinely INDEPENDENT streams running concurrently: one per
# endpoint pair, each its own bind port, QP set, pump kthread, reorder window and
# credit state. That is the F2 scale-out shape (one stream per Kafka partition /
# ClickHouse shard). The aggregate goodput is the SUM of the N sink-measured
# BENCH_OK goodputs; a clean point requires every sink to report and the summed
# reorder-drops across all acceptor endpoints to be 0.
#
# It uses its OWN endpoints (f2_acc_$i / f2_init_$i on base-port+i) and never
# touches the declarative pair, so it composes with a live services.urp.
#
# Env overrides: URP_F2_STREAMS (sweep of N), URP_F2_BUFSIZE, URP_F2_BUFCOUNT,
# URP_F2_DUR, URP_F2_MSG, URP_F2_LINE_MBPS.
#
# NOT in ci-local (needs real hardware) — like urp-bw-matrix / urp-hw-matrix.
{ pkgs }:

let
  benchC = import ./urp-bench.nix { inherit pkgs; };
in
pkgs.writeShellApplication {
  name = "urp-f2-matrix";
  runtimeInputs = [ pkgs.coreutils pkgs.gnugrep pkgs.gawk pkgs.openssh pkgs.nix ];
  text = ''
    if [ "$#" -lt 3 ]; then
      echo "usage: urp-f2-matrix <acceptor-host> <initiator-host> <acceptor-ip> [base-port]" >&2
      exit 2
    fi
    ACC="$1"; INIT="$2"; ACC_IP="$3"; BASEPORT="''${4:-4820}"

    BENCH=${benchC}/bin/urp-bench

    STREAMS="''${URP_F2_STREAMS:-1 2 4 8}"
    BUFSIZE="''${URP_F2_BUFSIZE:-262144}"   # 256 KiB — design-37 copy sweet spot
    BUFCOUNT="''${URP_F2_BUFCOUNT:-128}"
    DUR="''${URP_F2_DUR:-5}"
    MSG="''${URP_F2_MSG:-$BUFSIZE}"
    LINE_MBPS="''${URP_F2_LINE_MBPS:-25000}"

    ssh_o() { ssh -o BatchMode=yes -o ConnectTimeout=10 -l root "$@"; }

    # Largest N in the sweep — bounds the endpoint index range we create/clean.
    MAXN=0; for n in $STREAMS; do [ "$n" -gt "$MAXN" ] && MAXN="$n"; done

    echo "=== urp-f2-matrix: acc=$ACC init=$INIT acc_ip=$ACC_IP base_port=$BASEPORT ==="
    echo "    streams=[$STREAMS] buffer_size=$BUFSIZE buffer_count=$BUFCOUNT dur=$DUR msg=$MSG"

    for h in "$ACC" "$INIT"; do
      ssh_o "$h" 'lsmod | grep -q "^urp "' || { echo "FAIL: urp.ko not loaded on $h" >&2; exit 1; }
    done
    echo "  copying bench closure to hosts..."
    nix copy --no-check-sigs --to "ssh://root@$ACC"  "${benchC}"
    nix copy --no-check-sigs --to "ssh://root@$INIT" "${benchC}"

    acc_sock() { echo "/run/urp-f2-acc-$1.sock"; }
    init_sock() { echo "/run/urp-f2-init-$1.sock"; }

    # Tear down every f2 endpoint we might have created, both hosts. Idempotent.
    cleanup_all() {
      echo "--- cleaning up f2 endpoints ---"
      local i
      for i in $(seq 0 $((MAXN - 1))); do
        ssh_o "$ACC"  "systemctl stop urpf2-sink-$i 2>/dev/null; systemctl reset-failed urpf2-sink-$i 2>/dev/null; \
          urp remove f2_acc_$i 2>/dev/null; rm -f $(acc_sock "$i")" >/dev/null 2>&1 || true
        ssh_o "$INIT" "urp remove f2_init_$i 2>/dev/null; rm -f $(init_sock "$i")" >/dev/null 2>&1 || true
      done
    }
    trap cleanup_all EXIT

    # Snapshot / field-extract / delta helpers (same shape as urp-bw-matrix).
    snap() { ssh_o "$1" "urp stats $2 2>/dev/null" 2>/dev/null || true; }
    statf() { echo "$1" | awk -v k="$2:" '$1==k{print $2}'; }
    delta() { local a b; a=$(statf "$1" "$3"); b=$(statf "$2" "$3"); \
      if [ -n "$a" ] && [ -n "$b" ]; then echo $((b - a)); else echo "?"; fi; }
    sink_bound() { ssh_o "$ACC" "grep -q '$(basename "$1")' /proc/net/unix 2>/dev/null"; }

    RESULTS=$(mktemp /tmp/urp-f2.XXXXXX)
    trap 'rm -f "$RESULTS"; cleanup_all' EXIT

    # --- Sweep over stream counts -------------------------------------------
    for N in $STREAMS; do
      echo "--- streams=$N ---"
      cleanup_all

      # Create N independent endpoint pairs (num_qps=1 each).
      ok=yes
      for i in $(seq 0 $((N - 1))); do
        port=$((BASEPORT + i))
        ssh_o "$ACC" "urp add f2_acc_$i --connect-path $(acc_sock "$i") --bind $ACC_IP:$port \
          --num-qps 1 --buffer-count $BUFCOUNT --buffer-size $BUFSIZE" >/dev/null 2>&1 || { ok=""; break; }
        ssh_o "$INIT" "urp add f2_init_$i --listen-path $(init_sock "$i") --peer $ACC_IP:$port \
          --num-qps 1 --buffer-count $BUFCOUNT --buffer-size $BUFSIZE" >/dev/null 2>&1 || { ok=""; break; }
      done
      if [ -z "$ok" ]; then echo "WARN: endpoint add failed at N=$N -- skipping" >&2; continue; fi
      sleep 2  # let each acceptor rdma_listen come up

      # Start N sinks and confirm each is bound before any source runs (the
      # acceptor lazy-connects its connect-path on the first frame and does NOT
      # retry — same ordering rule as urp-bw-matrix).
      allbound=yes
      declare -a SINK_INV
      for i in $(seq 0 $((N - 1))); do
        s=$(acc_sock "$i")
        ssh_o "$ACC" "systemctl stop urpf2-sink-$i 2>/dev/null; systemctl reset-failed urpf2-sink-$i 2>/dev/null; \
          rm -f $s; systemd-run --unit=urpf2-sink-$i --collect $BENCH --listen $s \
            --id $((100 + i)) --mode blocking --pattern stream --msg-size $MSG --batch 16 \
            --duration $((DUR + 25)) --verify none" >/dev/null 2>&1 || true
      done
      for i in $(seq 0 $((N - 1))); do
        s=$(acc_sock "$i"); bound=""
        for _ in 1 2 3 4 5 6 7 8 9 10; do
          if sink_bound "$s"; then bound=yes; break; fi
          sleep 0.5
        done
        if [ -z "$bound" ]; then echo "WARN: sink $i did not bind $s at N=$N" >&2; allbound=""; fi
        SINK_INV[i]=$(ssh_o "$ACC" "systemctl show -p InvocationID --value urpf2-sink-$i 2>/dev/null" 2>/dev/null || true)
      done
      if [ -z "$allbound" ]; then echo "WARN: not all sinks bound at N=$N -- skipping" >&2; continue; fi

      # Snapshot acceptor counters (aggregate reorder-drops) before.
      declare -a ACC_BEFORE
      for i in $(seq 0 $((N - 1))); do ACC_BEFORE[i]=$(snap "$ACC" "f2_acc_$i"); done

      # Launch all N sources CONCURRENTLY (background ssh, then wait).
      declare -a SRC_PID
      for i in $(seq 0 $((N - 1))); do
        s=$(init_sock "$i")
        ssh_o "$INIT" "$BENCH --connect $s --id $((200 + i)) --mode blocking --pattern stream \
          --msg-size $MSG --batch 16 --duration $DUR --verify none" >/dev/null 2>&1 &
        SRC_PID[i]=$!
      done
      for i in $(seq 0 $((N - 1))); do wait "''${SRC_PID[i]}" 2>/dev/null || true; done

      declare -a ACC_AFTER
      for i in $(seq 0 $((N - 1))); do ACC_AFTER[i]=$(snap "$ACC" "f2_acc_$i"); done

      # Scrape each sink's BENCH_OK (invocation-scoped) and sum goodputs.
      agg_mbps=0; got=0; agg_drops=0
      for i in $(seq 0 $((N - 1))); do
        line=""
        for _ in $(seq 1 15); do
          if [ -n "''${SINK_INV[i]}" ]; then
            line=$(ssh_o "$ACC" "journalctl _SYSTEMD_INVOCATION_ID=''${SINK_INV[i]} --no-pager -o cat 2>/dev/null | grep -h '^BENCH_OK' | tail -1" 2>/dev/null || true)
          fi
          [ -n "$line" ] && break
          sleep 1
        done
        if [ -n "$line" ]; then
          m=$(echo "$line" | awk '{for(j=1;j<=NF;j++){split($j,kv,"="); if(kv[1]=="mbps")print kv[2]}}')
          if [ -n "$m" ]; then agg_mbps=$(awk -v a="$agg_mbps" -v b="$m" 'BEGIN{printf "%.3f", a+b}'); got=$((got + 1)); fi
        else
          echo "WARN: no BENCH_OK from sink $i at N=$N" >&2
        fi
        d=$(delta "''${ACC_BEFORE[i]}" "''${ACC_AFTER[i]}" reorder-drops)
        if [ "$d" != "?" ]; then agg_drops=$((agg_drops + d)); fi
      done

      if [ "$got" -eq "$N" ]; then
        echo "F2_OK streams=$N got=$got agg_mbps=$agg_mbps reorder_drops=$agg_drops" | tee -a "$RESULTS"
      else
        echo "WARN: only $got/$N sinks reported at N=$N (agg_mbps=$agg_mbps drops=$agg_drops)" >&2
        echo "F2_PARTIAL streams=$N got=$got agg_mbps=$agg_mbps reorder_drops=$agg_drops" >> "$RESULTS"
      fi
      cleanup_all
    done

    # --- Report --------------------------------------------------------------
    echo ""
    echo "=== F2 aggregate (sum of N sink-measured goodputs) — line=$LINE_MBPS Mb/s ==="
    awk -v line="$LINE_MBPS" '
      /^F2_OK|^F2_PARTIAL/ {
        n=""; got=""; mbps=""; dr=""
        for (i=1;i<=NF;i++){ split($i,kv,"="); k=kv[1]; v=kv[2]
          if(k=="streams")n=v; if(k=="got")got=v; if(k=="agg_mbps")mbps=v; if(k=="reorder_drops")dr=v }
        mbit = mbps*8
        pcln = (line>0) ? sprintf("%.2f%%", 100*mbit/line) : "-"
        per  = (n>0) ? sprintf("%.1f", mbps/n) : "-"
        tag  = ($1=="F2_PARTIAL") ? " (PARTIAL " got "/" n ")" : ""
        printf "  streams=%-2s agg_goodput=%9s MB/s =%9.1f Mb/s  %%line=%-7s per_stream=%-8s MB/s  drops=%-6s%s\n",\
          n,mbps,mbit,pcln,per,dr,tag
      }' "$RESULTS"
    echo ""
    echo "URP_F2_MATRIX_DONE points=$(grep -cE '^F2_OK|^F2_PARTIAL' "$RESULTS" || echo 0)"
  '';
}
