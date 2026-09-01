# 3-node full-mesh concurrency runner (design 32 mesh extension, 2026-08-31),
# driven over SSH from `l` against hp1/hp2/hp3 running `services.urp`.
#
#   nix run .#urp-mesh-matrix -- [h1 h2 h3]        # default: hp1 hp2 hp3
#   nix run .#urp-mesh-matrix -- hp1 hp2 hp3
#
# Where urp-bw-matrix / urp-f2-matrix measure ONE edge (a back-to-back pair),
# this measures the NEW "2-way" regime the mesh unlocks: a node driving/serving
# two concurrent RDMA sessions on its TWO ports (to two DIFFERENT peers) at once
# — separate QP sets, pump kthreads, reorder windows and credit state per link.
# That stresses per-port fairness, head-of-line and credit interaction that a
# single back-to-back link never exercises.
#
# Topology is fixed by the deploy (node-pair /29, host octet = node number):
#   edge 1-2 = 10.10.12.0/29 (h1=.1 h2=.2)   [hp1 f1np1 <-> hp2 f0np0]
#   edge 1-3 = 10.10.13.0/29 (h1=.1 h3=.3)   [hp1 f0np0 <-> hp3 f1np1]
#   edge 2-3 = 10.10.23.0/29 (h2=.2 h3=.3)   [hp2 f1np1 <-> hp3 f0np0]
# A directed flow s->d makes d the acceptor/sink (binds 10.10.<sorted(s,d)>.<d>,
# runs bench --listen) and s the initiator/source (bench --connect); data flows
# s->d. It uses its OWN per-run endpoints (ma_* / mi_*) and never
# touches the declarative pair, so it composes with a live services.urp.
#
# Scenarios (URP_MESH_SCENARIOS to filter; default all):
#   per-edge   one flow per edge, run ALONE (apples-to-apples baseline)
#   hub-rx     each node as SINK receiving from BOTH neighbors at once (RX 2-way)
#   hub-tx     each node as SOURCE sending to BOTH neighbors at once  (TX 2-way)
#   ring       1->2->3->1 : every node sends on one port, receives on the other
#   all2all    all 6 directed flows : every node sends AND receives on both ports
#
# Env: URP_MESH_BUFSIZE (65516), URP_MESH_BUFCOUNT (1024), URP_MESH_NUMQPS (1),
# URP_MESH_DUR (5), URP_MESH_VERIFY (none|full, default none), URP_MESH_LINE_MBPS
# (25000), URP_MESH_BASEPORT (4860), URP_MESH_SCENARIOS.
#
# NOT in ci-local (needs real hardware) — like urp-bw-matrix / urp-f2-matrix.
{ pkgs }:

let
  benchC = import ./urp-bench.nix { inherit pkgs; };
in
pkgs.writeShellApplication {
  name = "urp-mesh-matrix";
  runtimeInputs = [ pkgs.coreutils pkgs.gnugrep pkgs.gawk pkgs.openssh pkgs.nix ];
  text = ''
    H1="''${1:-hp1}"; H2="''${2:-hp2}"; H3="''${3:-hp3}"
    HOST=( "" "$H1" "$H2" "$H3" )   # 1-indexed: HOST[n] = hostname of node n

    BENCH=${benchC}/bin/urp-bench

    BUFSIZE="''${URP_MESH_BUFSIZE:-65516}"
    BUFCOUNT="''${URP_MESH_BUFCOUNT:-1024}"
    NUMQPS="''${URP_MESH_NUMQPS:-1}"
    DUR="''${URP_MESH_DUR:-5}"
    VERIFY="''${URP_MESH_VERIFY:-none}"
    LINE_MBPS="''${URP_MESH_LINE_MBPS:-25000}"
    BASEPORT="''${URP_MESH_BASEPORT:-4860}"
    SCENARIOS="''${URP_MESH_SCENARIOS:-per-edge hub-rx hub-tx ring all2all}"
    MSG="$BUFSIZE"

    ssh_o() { ssh -o BatchMode=yes -o ConnectTimeout=10 -l root "$@"; }

    # d's IPv4 on the edge joining nodes s and d (node-pair scheme).
    edge_ip() { local a="$1" b="$2" who="$3" lo hi; \
      if [ "$a" -lt "$b" ]; then lo="$a"; hi="$b"; else lo="$b"; hi="$a"; fi; \
      echo "10.10.''${lo}''${hi}.''${who}"; }

    echo "=== urp-mesh-matrix: nodes 1=$H1 2=$H2 3=$H3 ==="
    echo "    bufsize=$BUFSIZE bufcount=$BUFCOUNT num_qps=$NUMQPS dur=$DUR verify=$VERIFY scenarios=[$SCENARIOS]"

    for h in "$H1" "$H2" "$H3"; do
      ssh_o "$h" 'lsmod | grep -q "^urp "' || { echo "FAIL: urp.ko not loaded on $h" >&2; exit 1; }
    done
    echo "  copying bench closure to all three hosts..."
    for h in "$H1" "$H2" "$H3"; do nix copy --no-check-sigs --to "ssh://root@$h" "${benchC}" >/dev/null; done

    RESULTS=$(mktemp /tmp/urp-mesh.XXXXXX)

    # --- flow arrays (index 0..NF-1), rebuilt per run_flows call ---------------
    declare -a F_ID F_ACCHOST F_ACCIP F_INITHOST F_PORT F_INV F_B4
    NF=0

    flow_hosts_involved() { printf '%s\n' "''${F_ACCHOST[@]}" "''${F_INITHOST[@]}" 2>/dev/null | sort -u; }

    cleanup_flows() {
      local i
      for i in $(seq 0 $((NF - 1)) 2>/dev/null); do
        [ -n "''${F_ID[i]:-}" ] || continue
        ssh_o "''${F_ACCHOST[i]}" "systemctl stop urpmesh-sink-''${F_ID[i]} 2>/dev/null; \
          systemctl reset-failed urpmesh-sink-''${F_ID[i]} 2>/dev/null; \
          urp remove ma_''${F_ID[i]} 2>/dev/null; rm -f /run/urp-mesh-acc-''${F_ID[i]}.sock" >/dev/null 2>&1 || true
        ssh_o "''${F_INITHOST[i]}" "urp remove mi_''${F_ID[i]} 2>/dev/null; \
          rm -f /run/urp-mesh-init-''${F_ID[i]}.sock" >/dev/null 2>&1 || true
      done
    }
    trap 'cleanup_flows; rm -f "$RESULTS"' EXIT

    snap()  { ssh_o "$1" "urp stats $2 2>/dev/null" 2>/dev/null || true; }
    statf() { echo "$1" | awk -v k="$2:" '$1==k{print $2}'; }
    delta() { local a b; a=$(statf "$1" "$3"); b=$(statf "$2" "$3"); \
      if [ -n "$a" ] && [ -n "$b" ]; then echo $((b - a)); else echo "?"; fi; }

    # run_flows <label> <spec...>   where each spec is "s,d" (source node -> dest node)
    run_flows() {
      local label="$1"; shift
      local specs=("$@")
      cleanup_flows
      NF=0; F_ID=(); F_ACCHOST=(); F_ACCIP=(); F_INITHOST=(); F_PORT=(); F_INV=(); F_B4=()

      local port=$BASEPORT sp s d
      for sp in "''${specs[@]}"; do
        s="''${sp%,*}"; d="''${sp#*,}"
        F_ID[NF]="''${label}''${s}''${d}"       # e.g. ring12, hubrx21 (<=15 w/ ma_)
        F_ACCHOST[NF]="''${HOST[$d]}"
        F_ACCIP[NF]="$(edge_ip "$s" "$d" "$d")"
        F_INITHOST[NF]="''${HOST[$s]}"
        F_PORT[NF]="$port"
        port=$((port + 1)); NF=$((NF + 1))
      done

      # Create endpoints: acceptor (sink side) + initiator (source side).
      local i ok=yes
      for i in $(seq 0 $((NF - 1))); do
        ssh_o "''${F_ACCHOST[i]}" "urp add ma_''${F_ID[i]} \
          --connect-path /run/urp-mesh-acc-''${F_ID[i]}.sock --bind ''${F_ACCIP[i]}:''${F_PORT[i]} \
          --num-qps $NUMQPS --buffer-count $BUFCOUNT --buffer-size $BUFSIZE" >/dev/null 2>&1 || { ok=""; break; }
        ssh_o "''${F_INITHOST[i]}" "urp add mi_''${F_ID[i]} \
          --listen-path /run/urp-mesh-init-''${F_ID[i]}.sock --peer ''${F_ACCIP[i]}:''${F_PORT[i]} \
          --num-qps $NUMQPS --buffer-count $BUFCOUNT --buffer-size $BUFSIZE" >/dev/null 2>&1 || { ok=""; break; }
      done
      if [ -z "$ok" ]; then echo "WARN: [$label] endpoint add failed -- skipping" >&2; cleanup_flows; return; fi
      sleep 2

      # Start every sink; confirm each is bound (acceptor lazy-connects the
      # connect-path on the FIRST frame and does NOT retry -> bind before source).
      for i in $(seq 0 $((NF - 1))); do
        ssh_o "''${F_ACCHOST[i]}" "systemctl stop urpmesh-sink-''${F_ID[i]} 2>/dev/null; \
          systemctl reset-failed urpmesh-sink-''${F_ID[i]} 2>/dev/null; rm -f /run/urp-mesh-acc-''${F_ID[i]}.sock; \
          systemd-run --unit=urpmesh-sink-''${F_ID[i]} --collect $BENCH \
            --listen /run/urp-mesh-acc-''${F_ID[i]}.sock --id $((100 + i)) --mode blocking --pattern stream \
            --msg-size $MSG --batch 16 --duration $((DUR + 25)) --verify $VERIFY" >/dev/null 2>&1 || true
      done
      local allbound=yes
      for i in $(seq 0 $((NF - 1))); do
        local bound=""
        for _ in 1 2 3 4 5 6 7 8 9 10; do
          if ssh_o "''${F_ACCHOST[i]}" "grep -q 'urp-mesh-acc-''${F_ID[i]}.sock' /proc/net/unix 2>/dev/null"; then bound=yes; break; fi
          sleep 0.5
        done
        [ -z "$bound" ] && { echo "WARN: [$label] sink ''${F_ID[i]} did not bind" >&2; allbound=""; }
        F_INV[i]=$(ssh_o "''${F_ACCHOST[i]}" "systemctl show -p InvocationID --value urpmesh-sink-''${F_ID[i]} 2>/dev/null" 2>/dev/null || true)
        F_B4[i]=$(snap "''${F_ACCHOST[i]}" "ma_''${F_ID[i]}")
      done
      if [ -z "$allbound" ]; then echo "WARN: [$label] not all sinks bound -- skipping" >&2; cleanup_flows; return; fi

      # Launch ALL sources concurrently (background ssh), then wait.
      local -a SRC_PID
      for i in $(seq 0 $((NF - 1))); do
        ssh_o "''${F_INITHOST[i]}" "$BENCH --connect /run/urp-mesh-init-''${F_ID[i]}.sock --id $((200 + i)) \
          --mode blocking --pattern stream --msg-size $MSG --batch 16 --duration $DUR --verify $VERIFY" >/dev/null 2>&1 &
        SRC_PID[i]=$!
      done
      for i in $(seq 0 $((NF - 1))); do wait "''${SRC_PID[i]}" 2>/dev/null || true; done

      # Scrape each sink BENCH_OK by InvocationID; aggregate.
      local agg=0 got=0 drops=0
      for i in $(seq 0 $((NF - 1))); do
        local line="" m d after
        for _ in $(seq 1 15); do
          [ -n "''${F_INV[i]}" ] && line=$(ssh_o "''${F_ACCHOST[i]}" \
            "journalctl _SYSTEMD_INVOCATION_ID=''${F_INV[i]} --no-pager -o cat 2>/dev/null | grep -h '^BENCH_OK' | tail -1" 2>/dev/null || true)
          [ -n "$line" ] && break; sleep 1
        done
        after=$(snap "''${F_ACCHOST[i]}" "ma_''${F_ID[i]}")
        d=$(delta "''${F_B4[i]}" "$after" reorder-drops); [ "$d" != "?" ] && drops=$((drops + d))
        if [ -n "$line" ]; then
          m=$(echo "$line" | awk '{for(j=1;j<=NF;j++){split($j,kv,"="); if(kv[1]=="mbps")print kv[2]}}')
          [ -n "$m" ] && { agg=$(awk -v a="$agg" -v b="$m" 'BEGIN{printf "%.3f",a+b}'); got=$((got+1)); \
            printf "    flow %s->%s  %-8s MB/s  (sink %s)\n" \
              "''${F_INITHOST[i]}" "''${F_ACCHOST[i]}" "$m" "''${F_ACCIP[i]}"; }
        else
          echo "WARN: [$label] no BENCH_OK from sink ''${F_ID[i]}" >&2
        fi
      done
      local tag="MESH_OK"; [ "$got" -eq "$NF" ] || tag="MESH_PARTIAL"
      echo "$tag scenario=$label flows=$NF got=$got agg_mbps=$agg reorder_drops=$drops" | tee -a "$RESULTS"
      cleanup_flows
    }

    want() { case " $SCENARIOS " in *" $1 "*) return 0;; *) return 1;; esac; }

    # --- per-edge: one directed flow per edge, run ALONE ----------------------
    if want per-edge; then
      echo ""; echo "########## scenario: per-edge (baseline, one edge at a time) ##########"
      run_flows edge 1,3
      run_flows edge 1,2
      run_flows edge 2,3
    fi

    # --- hub-rx: each node SINKS from both neighbors at once ------------------
    if want hub-rx; then
      echo ""; echo "########## scenario: hub-rx (node receives from BOTH neighbors) ##########"
      echo "  -- hub $H1 (rx from $H2,$H3) --"; run_flows hubrx 2,1 3,1
      echo "  -- hub $H2 (rx from $H1,$H3) --"; run_flows hubrx 1,2 3,2
      echo "  -- hub $H3 (rx from $H1,$H2) --"; run_flows hubrx 1,3 2,3
    fi

    # --- hub-tx: each node SOURCES to both neighbors at once ------------------
    if want hub-tx; then
      echo ""; echo "########## scenario: hub-tx (node sends to BOTH neighbors) ##########"
      echo "  -- hub $H1 (tx to $H2,$H3) --"; run_flows hubtx 1,2 1,3
      echo "  -- hub $H2 (tx to $H1,$H3) --"; run_flows hubtx 2,1 2,3
      echo "  -- hub $H3 (tx to $H1,$H2) --"; run_flows hubtx 3,1 3,2
    fi

    # --- ring: 1->2->3->1, all concurrent ------------------------------------
    if want ring; then
      echo ""; echo "########## scenario: ring 1->2->3->1 (all concurrent) ##########"
      run_flows ring 1,2 2,3 3,1
    fi

    # --- all2all: all 6 directed flows concurrent ----------------------------
    if want all2all; then
      echo ""; echo "########## scenario: all-to-all (6 flows concurrent) ##########"
      run_flows all 1,2 2,1 1,3 3,1 2,3 3,2
    fi

    # --- report ---------------------------------------------------------------
    echo ""
    echo "=== mesh concurrency summary — line=$LINE_MBPS Mb/s, bufsize=$BUFSIZE verify=$VERIFY ==="
    awk -v line="$LINE_MBPS" '
      /^MESH_OK|^MESH_PARTIAL/ {
        sc="";fl="";got="";mbps="";dr=""
        for(i=1;i<=NF;i++){split($i,kv,"=");k=kv[1];v=kv[2]
          if(k=="scenario")sc=v; if(k=="flows")fl=v; if(k=="got")got=v; if(k=="agg_mbps")mbps=v; if(k=="reorder_drops")dr=v}
        mbit=mbps*8; pcln=(line>0)?sprintf("%.1f%%",100*mbit/line):"-"
        tag=($1=="MESH_PARTIAL")?sprintf(" (PARTIAL %s/%s)",got,fl):""
        printf "  %-8s flows=%-2s agg=%9s MB/s =%9.1f Mb/s  %%line=%-7s drops=%-5s%s\n",sc,fl,mbps,mbit,pcln,dr,tag
      }' "$RESULTS"
    fails=$(grep -c '^MESH_PARTIAL' "$RESULTS" || true)
    nzdrops=$(awk '/^MESH_OK|^MESH_PARTIAL/{for(i=1;i<=NF;i++){split($i,kv,"=");if(kv[1]=="reorder_drops"&&kv[2]!=0)c++}}END{print c+0}' "$RESULTS")
    echo ""
    echo "URP_MESH_MATRIX_DONE points=$(grep -cE '^MESH_OK|^MESH_PARTIAL' "$RESULTS" || echo 0) partial=$fails nonzero_drop_points=$nzdrops"
    if [ "''${fails:-0}" -eq 0 ] && [ "''${nzdrops:-0}" -eq 0 ]; then echo "URP_MESH_RESULT=GREEN"; else echo "URP_MESH_RESULT=RED"; fi
  '';
}
