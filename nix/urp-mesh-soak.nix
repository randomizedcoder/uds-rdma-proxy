# Long-duration soak of the 3-node mesh benchmark (design 38), to catch slow
# leaks and performance drift that a short run cannot. Drives the mesh over SSH
# from `l` against hp1/hp2/hp3 running `services.urp`.
#
#   nix run .#urp-mesh-soak -- [h1 h2 h3]          # default hp1 hp2 hp3, 8h
#   SOAK_SECONDS=28800 nix run .#urp-mesh-soak -- hp1 hp2 hp3
#
# Each iteration runs ONE mesh scenario (default all2all — every node RX+TX on
# both ports, the heaviest steady load) via the tested urp-mesh-matrix binary,
# and BEFORE each iteration samples every host's kernel memory + endpoint count +
# dmesg fault patterns. At the deadline it analyses the trend and prints a
# verdict:
#   - PERF: median goodput of the last 10% of iters vs the first 10% — RED if it
#     dropped more than SOAK_DROP_THRESH_PCT (default 10%).
#   - LEAK: per-host SUnreclaim (kernel slab) growth first→last — RED if it grew
#     more than SOAK_MEM_GROWTH_MIB (default 256 MiB). MemAvailable reported for
#     context.
#   - ENDPOINTS: standing endpoint count must stay put (hp1=1, hp3=1, hp2=0);
#     any drift => leaked mesh endpoints.
#   - DMESG: any new WARN/BUG/Call-Trace/oom/KASAN/hung-task on any host => RED.
# Median (not mean) makes the perf check robust to the known intermittent
# startup-starvation latch (design 38 §38.5).
#
# A full CSV of every iteration is written to $SOAK_LOG_DIR (default
# /tmp/urp-mesh-soak/<timestamp>/soak.csv) so a run can be inspected/plotted
# after the fact — it outlives the terminal.
#
# Env: SOAK_SECONDS (28800), SOAK_SCENARIO (all2all), SOAK_BENCH_DUR (30),
# SOAK_BUFSIZE (65516), SOAK_VERIFY (none), SOAK_DROP_THRESH_PCT (10),
# SOAK_MEM_GROWTH_MIB (256), SOAK_PROGRESS_EVERY (10), SOAK_LOG_DIR.
#
# NOT in ci-local (needs real hardware).
{ pkgs }:

let
  meshMatrix = import ./urp-mesh-matrix.nix { inherit pkgs; };
in
pkgs.writeShellApplication {
  name = "urp-mesh-soak";
  runtimeInputs = [ pkgs.coreutils pkgs.gnugrep pkgs.gawk pkgs.openssh pkgs.nix meshMatrix ];
  text = ''
    H1="''${1:-hp1}"; H2="''${2:-hp2}"; H3="''${3:-hp3}"
    HOSTS=("$H1" "$H2" "$H3")

    SOAK_SECONDS="''${SOAK_SECONDS:-28800}"          # 8h
    SCENARIO="''${SOAK_SCENARIO:-all2all}"
    BENCH_DUR="''${SOAK_BENCH_DUR:-30}"
    BUFSIZE="''${SOAK_BUFSIZE:-65516}"
    VERIFY="''${SOAK_VERIFY:-none}"
    DROP_THRESH_PCT="''${SOAK_DROP_THRESH_PCT:-10}"
    MEM_GROWTH_MIB="''${SOAK_MEM_GROWTH_MIB:-256}"
    PROGRESS_EVERY="''${SOAK_PROGRESS_EVERY:-10}"
    STAMP="$(date +%Y%m%d-%H%M%S)"
    LOG_DIR="''${SOAK_LOG_DIR:-/tmp/urp-mesh-soak/$STAMP}"
    mkdir -p "$LOG_DIR"
    CSV="$LOG_DIR/soak.csv"

    ssh_o() { ssh -o BatchMode=yes -o ConnectTimeout=10 -l root "$@"; }

    # Expected standing endpoint count per host (declarative pair; hp2 empty).
    declare -A WANT_EPS=( ["$H1"]=1 ["$H2"]=0 ["$H3"]=1 )

    # Per-host sample: "<MemAvailable_kB> <SUnreclaim_kB> <endpoints> <badlog>"
    # badlog = count of fault patterns currently in dmesg (monotone baseline diff).
    host_sample() {
      # Single-quoted on purpose: $ma/$su/awk $2 expand on the REMOTE shell.
      # shellcheck disable=SC2016
      ssh_o "$1" '
        ma=$(awk "/^MemAvailable:/{print \$2}" /proc/meminfo)
        su=$(awk "/^SUnreclaim:/{print \$2}" /proc/meminfo)
        ep=$(urp show 2>/dev/null | grep -c "^endpoint:" || echo 0)
        bl=$(dmesg 2>/dev/null | grep -icE "WARNING:|BUG:|call trace|out of memory|oom-kill|kasan|hung task|rcu stall|slab corruption" || true)
        echo "$ma $su $ep $bl"
      ' 2>/dev/null || echo "0 0 0 0"
    }

    echo "=== urp-mesh-soak: nodes=$H1,$H2,$H3 scenario=$SCENARIO dur/iter=''${BENCH_DUR}s ==="
    echo "    total=''${SOAK_SECONDS}s (~$((SOAK_SECONDS/3600))h)  bufsize=$BUFSIZE verify=$VERIFY"
    echo "    log=$CSV  drop_thresh=''${DROP_THRESH_PCT}%  mem_growth_limit=''${MEM_GROWTH_MIB}MiB"

    for h in "''${HOSTS[@]}"; do
      ssh_o "$h" 'lsmod | grep -q "^urp "' || { echo "FAIL: urp.ko not loaded on $h" >&2; exit 1; }
    done

    # CSV header.
    printf 'iter,epoch,agg_mbps,drops,partial' > "$CSV"
    for h in "''${HOSTS[@]}"; do printf ',%s_memavail_kb,%s_sunreclaim_kb,%s_eps,%s_badlog' "$h" "$h" "$h" "$h" >> "$CSV"; done
    printf '\n' >> "$CSV"

    START=$(date +%s); DEADLINE=$((START + SOAK_SECONDS)); iter=0
    ep_drift=0; badlog_events=0

    while :; do
      now=$(date +%s); [ "$now" -ge "$DEADLINE" ] && break
      iter=$((iter + 1))

      # --- sample every host BEFORE the run ---
      declare -a S_MA S_SU S_EP S_BL
      for i in 0 1 2; do
        read -r ma su ep bl <<<"$(host_sample "''${HOSTS[i]}")"
        S_MA[i]="''${ma:-0}"; S_SU[i]="''${su:-0}"; S_EP[i]="''${ep:-0}"; S_BL[i]="''${bl:-0}"
        want="''${WANT_EPS[''${HOSTS[i]}]}"
        [ "''${S_EP[i]}" != "$want" ] && { ep_drift=$((ep_drift + 1)); \
          echo "WARN iter=$iter: ''${HOSTS[i]} endpoints=''${S_EP[i]} want=$want (possible leak)" >&2; }
      done

      # --- one mesh scenario ---
      out=$(URP_MESH_SCENARIOS="$SCENARIO" URP_MESH_DUR="$BENCH_DUR" URP_MESH_BUFSIZE="$BUFSIZE" \
        URP_MESH_VERIFY="$VERIFY" urp-mesh-matrix "$H1" "$H2" "$H3" 2>/dev/null || true)
      line=$(echo "$out" | grep -E '^MESH_OK|^MESH_PARTIAL' | tail -1)
      agg=$(echo "$line" | awk '{for(i=1;i<=NF;i++){split($i,kv,"=");if(kv[1]=="agg_mbps")print kv[2]}}')
      drops=$(echo "$line" | awk '{for(i=1;i<=NF;i++){split($i,kv,"=");if(kv[1]=="reorder_drops")print kv[2]}}')
      partial=0; echo "$line" | grep -q '^MESH_PARTIAL' && partial=1
      agg="''${agg:-0}"; drops="''${drops:-0}"

      # --- CSV row ---
      row="$iter,$now,$agg,$drops,$partial"
      for i in 0 1 2; do row="$row,''${S_MA[i]},''${S_SU[i]},''${S_EP[i]},''${S_BL[i]}"; done
      echo "$row" >> "$CSV"

      # dmesg fault growth (compare this iter's badlog to iter 1's baseline stored later)
      for i in 0 1 2; do
        if [ "$iter" -eq 1 ]; then BASE_BL[i]="''${S_BL[i]}"; fi
        if [ "''${S_BL[i]}" -gt "''${BASE_BL[i]:-0}" ]; then \
          badlog_events=$((badlog_events + 1)); \
          echo "WARN iter=$iter: ''${HOSTS[i]} dmesg fault count ''${BASE_BL[i]}->''${S_BL[i]}" >&2; fi
      done

      if [ $((iter % PROGRESS_EVERY)) -eq 0 ] || [ "$iter" -le 3 ]; then
        el=$(( now - START )); rem=$(( DEADLINE - now ))
        printf '  iter %-4d  t+%-6ss (rem %5ss)  agg=%-9s MB/s drops=%s%s  su[%s/%s/%s]MiB avail[%s/%s/%s]MiB\n' \
          "$iter" "$el" "$rem" "$agg" "$drops" "$([ "$partial" = 1 ] && echo ' PARTIAL')" \
          $(( S_SU[0]/1024 )) $(( S_SU[1]/1024 )) $(( S_SU[2]/1024 )) \
          $(( S_MA[0]/1024 )) $(( S_MA[1]/1024 )) $(( S_MA[2]/1024 ))
      fi
    done

    TOTAL_ITERS=$iter
    echo ""
    echo "=== soak complete: $TOTAL_ITERS iterations over $(( $(date +%s) - START ))s ==="

    # --- analysis (awk over the CSV) ---
    verdict=$(awk -F, -v dropth="$DROP_THRESH_PCT" -v memgrow="$((MEM_GROWTH_MIB*1024))" '
      NR==1{ next }
      { n++; agg[n]=$3; drops_sum+=$4; part_sum+=$5;
        ma1[n]=$6; su1[n]=$7; ma2[n]=$10; su2[n]=$11; ma3[n]=$14; su3[n]=$15 }
      function median(arr, a,c,i){ c=0; for(i in arr) a[++c]=arr[i]; asort(a);
        return (c%2)? a[int(c/2)+1] : (a[c/2]+a[c/2+1])/2 }
      END{
        if(n<4){ print "INSUFFICIENT n="n; exit }
        w=int(n/10); if(w<1)w=1
        for(i=1;i<=w;i++){ first[i]=agg[i]; last[i]=agg[n-w+i] }
        mf=median(first); ml=median(last)
        dp=(mf>0)? 100*(mf-ml)/mf : 0
        # slab growth per host (first vs last sample)
        g1=su1[n]-su1[1]; g2=su2[n]-su2[1]; g3=su3[n]-su3[1]
        printf "median_goodput_first10pct=%.1f MB/s\n", mf
        printf "median_goodput_last10pct=%.1f MB/s\n", ml
        printf "goodput_drop=%.2f%% (limit %s%%)\n", dp, dropth
        printf "slab_growth_MiB: %s=%.1f %s=%.1f %s=%.1f (limit %s MiB)\n", "'"$H1"'",g1/1024,"'"$H2"'",g2/1024,"'"$H3"'",g3/1024, memgrow/1024
        printf "avail_delta_MiB: %s=%.1f %s=%.1f %s=%.1f\n", "'"$H1"'",(ma1[n]-ma1[1])/1024,"'"$H2"'",(ma2[n]-ma2[1])/1024,"'"$H3"'",(ma3[n]-ma3[1])/1024
        printf "iters_with_drops=%d  partial_iters=%d\n", (drops_sum>0), part_sum
        bad=0
        if(dp>dropth) bad=1
        if(g1>memgrow||g2>memgrow||g3>memgrow) bad=1
        print (bad? "ANALYSIS=RED":"ANALYSIS=GREEN")
      }' "$CSV")
    echo "$verdict"

    echo "endpoint_drift_events=$ep_drift  dmesg_fault_events=$badlog_events"
    result=GREEN
    echo "$verdict" | grep -q 'ANALYSIS=RED' && result=RED
    [ "$ep_drift" -gt 0 ] && result=RED
    [ "$badlog_events" -gt 0 ] && result=RED
    echo ""
    echo "URP_MESH_SOAK_DONE iters=$TOTAL_ITERS log=$CSV"
    echo "URP_MESH_SOAK_RESULT=$result"
  '';
}
