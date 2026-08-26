# Fast-path F2 aggregate / N-flow runner (design 37 §37.6 — the zero-copy twin of
# `.#urp-f2-matrix`), driven over SSH from `l`.
#
#   nix run .#urp-fast-f2-matrix -- <acceptor-host> <initiator-host> <acceptor-ip> [base-port]
#   nix run .#urp-fast-f2-matrix -- hp1 hp3 10.10.2.1
#
# `.#urp-f2-matrix` showed the COPY path is membw-capped (~1900 MB/s aggregate,
# flat N=1→4) — its per-frame memcpy is the shared bottleneck. This runner asks the
# complementary question on the ZERO-COPY fast path (design 31): with no software
# copy, does F2 actually scale N independent streams toward NIC line rate? Each
# stream is its own `--kind fast` endpoint pair (own port + QP + registered pool),
# driven by `urp-bench --mode uring-cmd`. Aggregate goodput = sum of the N
# sink-measured BENCH_OK goodputs.
#
# Uses its own ff_acc_$i / ff_init_$i endpoints; never touches the declarative pair.
#
# Env overrides: URP_FASTF2_STREAMS, URP_FASTF2_MSG, URP_FASTF2_BUFCOUNT,
# URP_FASTF2_DUR, URP_FASTF2_BATCH, URP_FASTF2_LINE_MBPS.
#
# NOT in ci-local (needs real hardware).
{ pkgs }:

let
  benchC = import ./urp-bench.nix { inherit pkgs; };
  pocC = import ./urp-fast-poc.nix { inherit pkgs; };
in
pkgs.writeShellApplication {
  name = "urp-fast-f2-matrix";
  runtimeInputs = [ pkgs.coreutils pkgs.gnugrep pkgs.gawk pkgs.openssh pkgs.nix ];
  text = ''
    if [ "$#" -lt 3 ]; then
      echo "usage: urp-fast-f2-matrix <acceptor-host> <initiator-host> <acceptor-ip> [base-port]" >&2
      exit 2
    fi
    ACC="$1"; INIT="$2"; ACC_IP="$3"; BASEPORT="''${4:-4840}"

    BENCH=${benchC}/bin/urp-bench
    POC=${pocC}/bin/urp-fast-poc

    STREAMS="''${URP_FASTF2_STREAMS:-1 2 4 8}"
    MSG="''${URP_FASTF2_MSG:-65516}"        # per-stream frame size (fast pool geometry)
    BUFCOUNT="''${URP_FASTF2_BUFCOUNT:-1024}" # suppressed pump knob for fast endpoints
    BUFSIZE=65536                            # suppressed pump knob; bench pool from --msg-size
    DUR="''${URP_FASTF2_DUR:-5}"
    BATCH="''${URP_FASTF2_BATCH:-16}"
    LINE_MBPS="''${URP_FASTF2_LINE_MBPS:-25000}"

    ssh_o() { ssh -o BatchMode=yes -o ConnectTimeout=10 -l root "$@"; }

    MAXN=0; for n in $STREAMS; do [ "$n" -gt "$MAXN" ] && MAXN="$n"; done

    echo "=== urp-fast-f2-matrix: acc=$ACC init=$INIT acc_ip=$ACC_IP base_port=$BASEPORT ==="
    echo "    streams=[$STREAMS] msg=$MSG dur=$DUR batch=$BATCH"

    for h in "$ACC" "$INIT"; do
      ssh_o "$h" 'lsmod | grep -q "^urp "' || { echo "FAIL: urp.ko not loaded on $h" >&2; exit 1; }
    done
    echo "  copying bench + poc closures to hosts..."
    nix copy --no-check-sigs --to "ssh://root@$ACC"  "${benchC}" "${pocC}"
    nix copy --no-check-sigs --to "ssh://root@$INIT" "${benchC}" "${pocC}"

    acc_sock() { echo "/run/ff-acc-$1.sock"; }
    init_sock() { echo "/run/ff-init-$1.sock"; }

    cleanup_all() {
      echo "--- cleaning up fast f2 endpoints ---"
      local i
      for i in $(seq 0 $((MAXN - 1))); do
        ssh_o "$ACC"  "systemctl stop fff-sink-$i 2>/dev/null; systemctl reset-failed fff-sink-$i 2>/dev/null; \
          urp remove ff_acc_$i 2>/dev/null; rm -f $(acc_sock "$i")" >/dev/null 2>&1 || true
        ssh_o "$INIT" "urp remove ff_init_$i 2>/dev/null; rm -f $(init_sock "$i")" >/dev/null 2>&1 || true
      done
    }
    trap cleanup_all EXIT

    RESULTS=$(mktemp /tmp/urp-fast-f2.XXXXXX)
    trap 'rm -f "$RESULTS"; cleanup_all' EXIT

    for N in $STREAMS; do
      echo "--- streams=$N ---"
      cleanup_all

      # Create N independent fast endpoint pairs.
      ok=yes
      for i in $(seq 0 $((N - 1))); do
        port=$((BASEPORT + i))
        ssh_o "$ACC" "urp add ff_acc_$i --kind fast --connect-path $(acc_sock "$i") --bind $ACC_IP:$port \
          --buffer-count $BUFCOUNT --buffer-size $BUFSIZE" >/dev/null 2>&1 || { ok=""; break; }
        ssh_o "$INIT" "urp add ff_init_$i --kind fast --listen-path $(init_sock "$i") --peer $ACC_IP:$port \
          --buffer-count $BUFCOUNT --buffer-size $BUFSIZE" >/dev/null 2>&1 || { ok=""; break; }
      done
      if [ -z "$ok" ]; then echo "WARN: endpoint add failed at N=$N -- skipping" >&2; continue; fi

      # Wait for every pair's RC + pool REGISTER to be ready (poc probe, both ends).
      allready=yes
      for i in $(seq 0 $((N - 1))); do
        rdy=""
        for _ in $(seq 1 20); do
          if ssh_o "$INIT" "$POC /dev/urp ff_init_$i 4096 8 2>&1 | grep -q URP_FAST_POC_OK" \
             && ssh_o "$ACC" "$POC /dev/urp ff_acc_$i 4096 8 2>&1 | grep -q URP_FAST_POC_OK"; then
            rdy=yes; break
          fi
          sleep 0.5
        done
        if [ -z "$rdy" ]; then echo "WARN: pair $i not REGISTER-ready at N=$N" >&2; allready=""; fi
      done
      if [ -z "$allready" ]; then echo "WARN: not all pairs ready at N=$N -- skipping" >&2; continue; fi

      # Sinks first (arm recvs); a source that briefly outruns just RNR-retries.
      declare -a SINK_INV
      for i in $(seq 0 $((N - 1))); do
        s=$(acc_sock "$i")
        ssh_o "$ACC" "systemctl stop fff-sink-$i 2>/dev/null; systemctl reset-failed fff-sink-$i 2>/dev/null; \
          systemd-run --unit=fff-sink-$i --collect $BENCH --listen $s --id $((100 + i)) \
            --mode uring-cmd --fast-endpoint ff_acc_$i --pattern stream --msg-size $MSG \
            --batch $BATCH --duration $((DUR + 25)) --verify none" >/dev/null 2>&1 || true
      done
      sleep 2
      for i in $(seq 0 $((N - 1))); do
        SINK_INV[i]=$(ssh_o "$ACC" "systemctl show -p InvocationID --value fff-sink-$i 2>/dev/null" 2>/dev/null || true)
      done

      # Launch all N sources CONCURRENTLY (background ssh, then wait).
      declare -a SRC_PID
      for i in $(seq 0 $((N - 1))); do
        s=$(init_sock "$i")
        ssh_o "$INIT" "$BENCH --connect $s --id $((200 + i)) --mode uring-cmd --fast-endpoint ff_init_$i \
          --pattern stream --msg-size $MSG --batch $BATCH --duration $DUR --verify none" >/dev/null 2>&1 &
        SRC_PID[i]=$!
      done
      for i in $(seq 0 $((N - 1))); do wait "''${SRC_PID[i]}" 2>/dev/null || true; done

      # Scrape each sink's BENCH_OK (invocation-scoped) and sum goodputs.
      agg_mbps=0; got=0
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
          echo "WARN: no BENCH_OK from fast sink $i at N=$N" >&2
        fi
      done

      if [ "$got" -eq "$N" ]; then
        echo "FASTF2_OK streams=$N got=$got agg_mbps=$agg_mbps" | tee -a "$RESULTS"
      else
        echo "WARN: only $got/$N fast sinks reported at N=$N (agg_mbps=$agg_mbps)" >&2
        echo "FASTF2_PARTIAL streams=$N got=$got agg_mbps=$agg_mbps" >> "$RESULTS"
      fi
      cleanup_all
    done

    echo ""
    echo "=== fast-path F2 aggregate (sum of N sink-measured goodputs) — line=$LINE_MBPS Mb/s ==="
    awk -v line="$LINE_MBPS" '
      /^FASTF2_OK|^FASTF2_PARTIAL/ {
        n=""; got=""; mbps=""
        for (i=1;i<=NF;i++){ split($i,kv,"="); k=kv[1]; v=kv[2]
          if(k=="streams")n=v; if(k=="got")got=v; if(k=="agg_mbps")mbps=v }
        mbit = mbps*8
        pcln = (line>0) ? sprintf("%.2f%%", 100*mbit/line) : "-"
        per  = (n>0) ? sprintf("%.1f", mbps/n) : "-"
        tag  = ($1=="FASTF2_PARTIAL") ? " (PARTIAL " got "/" n ")" : ""
        printf "  streams=%-2s agg_goodput=%9s MB/s =%9.1f Mb/s  %%line=%-7s per_stream=%-8s MB/s%s\n",\
          n,mbps,mbit,pcln,per,tag
      }' "$RESULTS"
    echo ""
    echo "URP_FAST_F2_MATRIX_DONE points=$(grep -cE '^FASTF2_OK|^FASTF2_PARTIAL' "$RESULTS" || echo 0)"
  '';
}
