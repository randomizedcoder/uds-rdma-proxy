# nix/microvms/lib.nix
#
# Lifecycle + helper scripts for the URP microvm pair.
# Patterned after xdp2/nix/microvms/lib.nix; pair-specific extensions
# orchestrate two VMs and drive the URP-to-URP data path.
#
{ pkgs, lib, constants, scriptsDir,
  # Phase 5: when sanitizer = true the pair test:
  #   - uses .#microvm-vm1-debug / .#microvm-vm2-debug
  #   - runs a KMEMLEAK scan after teardown
  #   - greps dmesg on both VMs for KASAN: / kmemleak: reports,
  #     failing the test if any are found
  # `pairLabel` suffixes the derivation names + report dir so the
  # default and sanitizer variants coexist in the nix store.
  pairLabel ? "",
  sanitizer ? false,
  # Phase 5 cross-arch (Track B): scales every phase timeout for the slow
  # TCG-emulated arches (aarch64 ~12x, riscv64 ~25x). 1 for native x86_64.
  timeoutMultiplier ? 1 }:

let
  vm1 = constants.vms.vm1;
  vm2 = constants.vms.vm2;
  t   = constants.timeouts;
  mult = timeoutMultiplier;

  vmAttr1 = "microvm-vm1${pairLabel}";
  vmAttr2 = "microvm-vm2${pairLabel}";

  # Common runtime closure for orchestrator scripts.
  orchTools = with pkgs; [
    coreutils procps netcat-gnu socat expect util-linux
  ];

in rec {

  # ==========================================================================
  # mkConnect: nc-based console attach (interactive)
  # ==========================================================================
  mkConnect = { vmId, console }:
  let
    cfg = constants.vms.${vmId};
    port = if console == "serial"
           then cfg.consoleSerialPort
           else cfg.consoleVirtioPort;
    dev = if console == "serial" then "ttyS0" else "hvc0";
  in pkgs.writeShellApplication {
    name = "urp-microvm-${vmId}-${console}";
    runtimeInputs = [ pkgs.netcat-gnu ];
    text = ''
      echo "Connecting to ${vmId} ${dev} (TCP ${toString port})"
      echo "Ctrl-C to disconnect"
      exec nc 127.0.0.1 ${toString port}
    '';
  };

  # ==========================================================================
  # mkStatus: pgrep + port-listen check for both VMs
  # ==========================================================================
  status = pkgs.writeShellApplication {
    name = "urp-microvm-status";
    runtimeInputs = with pkgs; [ procps netcat-gnu ];
    text = ''
      for entry in "${vm1.hostname}:${toString vm1.consoleSerialPort}:${toString vm1.consoleVirtioPort}" \
                   "${vm2.hostname}:${toString vm2.consoleSerialPort}:${toString vm2.consoleVirtioPort}"; do
        IFS=":" read -r name sp vp <<<"$entry"
        if pgrep -f "process=$name" >/dev/null 2>&1; then
          pid=$(pgrep -f "process=$name" | head -1)
          echo "$name: RUNNING (pid $pid)"
        else
          echo "$name: not running"
        fi
        nc -z 127.0.0.1 "$sp" 2>/dev/null \
          && echo "  serial $sp: LISTENING" \
          || echo "  serial $sp: -"
        nc -z 127.0.0.1 "$vp" 2>/dev/null \
          && echo "  virtio $vp: LISTENING" \
          || echo "  virtio $vp: -"
      done
    '';
  };

  # ==========================================================================
  # mkForceKill: SIGTERM then SIGKILL by process=hostname pattern
  # ==========================================================================
  forceKill = pkgs.writeShellApplication {
    name = "urp-microvm-force-kill";
    runtimeInputs = with pkgs; [ procps coreutils ];
    text = ''
      for name in "${vm1.hostname}" "${vm2.hostname}"; do
        if pgrep -f "process=$name" >/dev/null 2>&1; then
          echo "killing $name..."
          pkill -f "process=$name" || true
          sleep 2
          if pgrep -f "process=$name" >/dev/null 2>&1; then
            pkill -9 -f "process=$name" || true
            sleep 1
          fi
          if pgrep -f "process=$name" >/dev/null 2>&1; then
            echo "WARN: $name still running"
          else
            echo "$name killed"
          fi
        else
          echo "$name not running"
        fi
      done
    '';
  };

  # ==========================================================================
  # Full pair lifecycle test.
  #
  # 12 phases. Trap cleanup guarantees both VMs die even on failure.
  # ==========================================================================
  fullPairTest = pkgs.writeShellApplication {
    name = "urp-microvm-pair-test${pairLabel}";
    runtimeInputs = orchTools;
    text = ''
      RUNDIR="/tmp/urp-microvm-pair${pairLabel}"
      VM1_ATTR="${vmAttr1}"
      VM2_ATTR="${vmAttr2}"
      SANITIZER=${if sanitizer then "1" else "0"}
      VM1_PROC="${vm1.hostname}"
      VM2_PROC="${vm2.hostname}"
      VM1_SERIAL=${toString vm1.consoleSerialPort}
      VM1_VIRTIO=${toString vm1.consoleVirtioPort}
      VM2_SERIAL=${toString vm2.consoleSerialPort}
      VM2_VIRTIO=${toString vm2.consoleVirtioPort}
      PAIR_PORT=${toString constants.pairSocketPort}
      VM1_IP="10.99.99.${toString vm1.ipLastOctet}"
      VM2_IP="10.99.99.${toString vm2.ipLastOctet}"
      EXPECT_SCRIPT="${scriptsDir}/vm-expect.exp"

      POLL=${toString constants.pollInterval}
      # Sanitizer kernels: first-time build can take 30 min; runtime
      # ~3x slowdown, so all phase timeouts inflate accordingly.
      # Cross-arch: TCG emulation is ~mult x slower; base timeouts scale
      # by `mult` (sanitizer and cross are mutually exclusive in practice).
      T_BUILD=${if sanitizer then "3600" else toString (t.build * mult)}
      T_PROC=${toString (t.processStart * mult)}
      T_SERIAL=${if sanitizer then "90" else toString (t.serialReady * mult)}
      T_VIRTIO=${if sanitizer then "120" else toString (t.virtioReady * mult)}
      T_SERVICE=${if sanitizer then "180" else toString (t.serviceReady * mult)}
      T_PAIRLINK=${if sanitizer then "90" else toString (t.pairLink * mult)}
      T_RXE=${if sanitizer then "30" else toString (t.rxeReady * mult)}
      T_URP=${if sanitizer then "30" else toString (t.urpReady * mult)}
      T_CM=${toString (t.cmEstablished * mult)}
      T_ECHO=${if sanitizer then "20" else toString (t.echo * mult)}
      T_BENCH=${if sanitizer then "300" else toString (120 * mult)}
      T_SHUTDOWN=${if sanitizer then "60" else toString (t.shutdown * mult)}

      RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[1;33m'; NC=$'\033[0m'
      now_ms() { date +%s%3N; }
      pass() { printf "  %sPASS:%s %s\n" "$GREEN" "$NC" "$1"; }
      fail() { printf "  %sFAIL:%s %s\n" "$RED" "$NC" "$1"; exit 1; }
      info() { printf "  %sINFO:%s %s\n" "$YELLOW" "$NC" "$1"; }
      # scan_splat <diag-file> <phase-label>: a fuzz phase runs its command on
      # the VM console, so any kernel splat (KASAN / GP fault / BUG / Oops)
      # interleaves into the captured output. A crash there must fail the run
      # inline -- the later Phase 11b dmesg scan can miss it if the crash
      # wedged the VM (as the SET-num_qps OOB did: the fuzz phase captured the
      # KASAN report but Phase 11b came back "clean"). Fatal on match.
      SPLAT_RE='KASAN:|general protection fault|BUG:|Oops:|Kernel panic|use-after-free|slab-out-of-bounds'
      scan_splat() {
        if grep -qE "$SPLAT_RE" "$1" 2>/dev/null; then
          grep -nE "$SPLAT_RE" "$1" | head -6 | awk '{print "    "$0}'
          fail "$2: kernel splat detected during fuzzing (see $1)"
        fi
      }

      VM1_PID=""
      VM2_PID=""
      RESULT="(unset)"

      # shellcheck disable=SC2329  # invoked indirectly via trap EXIT
      cleanup() {
        echo ""
        info "Cleaning up..."
        for pid in "$VM1_PID" "$VM2_PID"; do
          [ -z "$pid" ] && continue
          if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
          fi
        done
        # Process-name fallback: catches any orphaned qemu
        # spawned by microvm-run that didn't inherit our PID.
        for name in "$VM1_PROC" "$VM2_PROC"; do
          if pgrep -f "process=$name" >/dev/null 2>&1; then
            pkill -f "process=$name" 2>/dev/null || true
          fi
        done
        sleep 2
        for name in "$VM1_PROC" "$VM2_PROC"; do
          if pgrep -f "process=$name" >/dev/null 2>&1; then
            pkill -9 -f "process=$name" 2>/dev/null || true
          fi
        done
      }
      trap cleanup EXIT INT TERM

      # Helper: poll until <cmd> succeeds or timeout.
      wait_for() {
        local label="$1" timeout="$2"; shift 2
        local waited=0
        while ! "$@" >/dev/null 2>&1; do
          sleep "$POLL"
          waited=$((waited + POLL))
          if [ "$waited" -ge "$timeout" ]; then
            fail "$label: timeout after ''${timeout}s"
          fi
          if [ -n "$VM1_PID" ] && ! kill -0 "$VM1_PID" 2>/dev/null \
             && ! pgrep -f "process=$VM1_PROC" >/dev/null 2>&1; then
            fail "$label: VM 1 died while waiting"
          fi
          if [ -n "$VM2_PID" ] && ! kill -0 "$VM2_PID" 2>/dev/null \
             && ! pgrep -f "process=$VM2_PROC" >/dev/null 2>&1; then
            fail "$label: VM 2 died while waiting"
          fi
          info "$label... ''${waited}/''${timeout}s"
        done
      }

      # Helper: run command in VM via expect-based virtio console.
      vm_run() {
        local port="$1" host="$2" cmd="$3"
        local timeout="''${4:-$T_URP}"
        expect "$EXPECT_SCRIPT" "$port" "$host" "$cmd" "$timeout" 0 2>&1 || true
      }

      echo "========================================"
      echo "  URP MicroVM Pair Test"
      echo "========================================"
      echo "VM 1: $VM1_PROC ($VM1_IP)  serial=$VM1_SERIAL virtio=$VM1_VIRTIO"
      echo "VM 2: $VM2_PROC ($VM2_IP)  serial=$VM2_SERIAL virtio=$VM2_VIRTIO"
      echo "Pair link: 127.0.0.1:$PAIR_PORT (QEMU socket netdev)"
      echo ""

      # -----------------------------------------------------------------
      # Phase 0 — build both VM runners (in parallel via nix build)
      # -----------------------------------------------------------------
      echo "--- Phase 0: Build VMs (timeout ''${T_BUILD}s) ---"
      P0_START=$(now_ms)
      if ! timeout "$T_BUILD" nix build ".#$VM1_ATTR" ".#$VM2_ATTR" \
           --print-out-paths --no-link 2>&1 | tail -10; then
        fail "build timed out / failed"
      fi
      VM1_PATH=$(nix build ".#$VM1_ATTR" --print-out-paths --no-link 2>/dev/null)
      VM2_PATH=$(nix build ".#$VM2_ATTR" --print-out-paths --no-link 2>/dev/null)
      [ -z "$VM1_PATH" ] && fail "vm1 build path empty"
      [ -z "$VM2_PATH" ] && fail "vm2 build path empty"
      P0_MS=$(( $(now_ms) - P0_START ))
      pass "VMs built in ''${P0_MS}ms"
      echo ""

      # Sanity: ports must be free.
      for p in "$VM1_SERIAL" "$VM1_VIRTIO" "$VM2_SERIAL" "$VM2_VIRTIO" "$PAIR_PORT"; do
        if nc -z 127.0.0.1 "$p" 2>/dev/null; then
          fail "port $p already in use (stale VM? run urp-microvm-force-kill)"
        fi
      done

      # -----------------------------------------------------------------
      # Phase 1 — start both VMs. VM 1 listens on the pair socket, so it
      # must start first; VM 2 connects to it.
      # -----------------------------------------------------------------
      echo "--- Phase 1: Start VMs ---"
      P1_START=$(now_ms)
      mkdir -p "$RUNDIR"
      "$VM1_PATH/bin/microvm-run" > "$RUNDIR"/vm1.log 2>&1 &
      VM1_PID=$!
      wait_for "vm1 process" "$T_PROC" pgrep -f "process=$VM1_PROC"
      pass "vm1 process up (pid $VM1_PID)"

      "$VM2_PATH/bin/microvm-run" > "$RUNDIR"/vm2.log 2>&1 &
      VM2_PID=$!
      wait_for "vm2 process" "$T_PROC" pgrep -f "process=$VM2_PROC"
      pass "vm2 process up (pid $VM2_PID)"

      P1_MS=$(( $(now_ms) - P1_START ))
      echo ""

      # -----------------------------------------------------------------
      # Phase 2 — console availability (serial then virtio, both VMs)
      # -----------------------------------------------------------------
      echo "--- Phase 2: Console readiness (timeout ''${T_VIRTIO}s) ---"
      P2_START=$(now_ms)
      wait_for "vm1 serial"  "$T_SERIAL" nc -z 127.0.0.1 "$VM1_SERIAL"
      wait_for "vm2 serial"  "$T_SERIAL" nc -z 127.0.0.1 "$VM2_SERIAL"
      wait_for "vm1 virtio"  "$T_VIRTIO" nc -z 127.0.0.1 "$VM1_VIRTIO"
      wait_for "vm2 virtio"  "$T_VIRTIO" nc -z 127.0.0.1 "$VM2_VIRTIO"
      P2_MS=$(( $(now_ms) - P2_START ))
      pass "all four consoles listening (''${P2_MS}ms)"
      echo ""

      # -----------------------------------------------------------------
      # Phase 3 — wait for shell prompt (systemd multi-user.target).
      # We use a benign `true` command via expect; if the prompt isn't
      # ready yet, vm-expect.exp times out and returns non-zero.
      # -----------------------------------------------------------------
      echo "--- Phase 3: Login prompt ready (timeout ''${T_SERVICE}s) ---"
      P3_START=$(now_ms)
      for attempt in $(seq 1 "$T_SERVICE"); do
        out1=$(vm_run "$VM1_VIRTIO" "$VM1_PROC" "echo READY1" 5 2>/dev/null || true)
        out2=$(vm_run "$VM2_VIRTIO" "$VM2_PROC" "echo READY2" 5 2>/dev/null || true)
        if echo "$out1" | grep -q READY1 && echo "$out2" | grep -q READY2; then
          break
        fi
        sleep "$POLL"
        if [ "$attempt" -ge "$T_SERVICE" ]; then
          fail "shell prompt not ready on both VMs"
        fi
      done
      P3_MS=$(( $(now_ms) - P3_START ))
      pass "both VMs answering prompt (''${P3_MS}ms)"
      echo ""

      # -----------------------------------------------------------------
      # Phase 4 — eth1 up + L3 ping between VMs
      # -----------------------------------------------------------------
      echo "--- Phase 4: Pair link (timeout ''${T_PAIRLINK}s) ---"
      P4_START=$(now_ms)
      vm_run "$VM1_VIRTIO" "$VM1_PROC" "ip link set eth1 up" 5 >/dev/null
      vm_run "$VM2_VIRTIO" "$VM2_PROC" "ip link set eth1 up" 5 >/dev/null
      ping_out=$(vm_run "$VM2_VIRTIO" "$VM2_PROC" "ping -c 2 -W 2 $VM1_IP" "$T_PAIRLINK")
      if ! echo "$ping_out" | grep -q "0% packet loss"; then
        echo "$ping_out" | awk '{print "    "$0}'
        fail "vm2 cannot ping vm1 over eth1"
      fi
      P4_MS=$(( $(now_ms) - P4_START ))
      pass "vm2 -> vm1 ping OK (''${P4_MS}ms)"
      echo ""

      # -----------------------------------------------------------------
      # Phase 5 — rdma_rxe on eth1 on both VMs
      # -----------------------------------------------------------------
      echo "--- Phase 5: rdma_rxe setup ---"
      P5_START=$(now_ms)
      vm_run "$VM1_VIRTIO" "$VM1_PROC" \
        "rdma link delete rxe_pair 2>/dev/null; rdma link add rxe_pair type rxe netdev eth1" "$T_RXE" >/dev/null
      vm_run "$VM2_VIRTIO" "$VM2_PROC" \
        "rdma link delete rxe_pair 2>/dev/null; rdma link add rxe_pair type rxe netdev eth1" "$T_RXE" >/dev/null
      P5_MS=$(( $(now_ms) - P5_START ))
      pass "rxe_pair on both VMs (''${P5_MS}ms)"

      # Phase 5b — verify ib_device is actually present.
      mkdir -p "$RUNDIR"/diag
      for label in vm1 vm2; do
        if [ "$label" = vm1 ]; then port=$VM1_VIRTIO host=$VM1_PROC; else port=$VM2_VIRTIO host=$VM2_PROC; fi
        out=$(vm_run "$port" "$host" "rdma link show" 5)
        echo "$out" > "$RUNDIR/diag/$label.rdma-link.txt"
        echo "$out" | grep -q "rxe_pair" \
          || { echo "$out" | awk '{print "    "$0}'; fail "$label: rxe_pair device not present"; }
      done
      pass "ib_device rxe_pair visible on both VMs"
      echo ""

      # -----------------------------------------------------------------
      # Phase 6 — insmod urp.ko (uses $URP_KO baked into rootfs env)
      # -----------------------------------------------------------------
      echo "--- Phase 6: insmod urp.ko ---"
      P6_START=$(now_ms)
      vm_run "$VM1_VIRTIO" "$VM1_PROC" \
        "rmmod urp 2>/dev/null; insmod \$URP_KO && echo URP1_OK" "$T_URP" \
        | tee "$RUNDIR"/insmod1.log | grep -q URP1_OK \
        || { awk '{print "    "$0}' "$RUNDIR"/insmod1.log; fail "vm1 insmod failed"; }
      vm_run "$VM2_VIRTIO" "$VM2_PROC" \
        "rmmod urp 2>/dev/null; insmod \$URP_KO && echo URP2_OK" "$T_URP" \
        | tee "$RUNDIR"/insmod2.log | grep -q URP2_OK \
        || { awk '{print "    "$0}' "$RUNDIR"/insmod2.log; fail "vm2 insmod failed"; }
      P6_MS=$(( $(now_ms) - P6_START ))
      pass "urp.ko loaded on both VMs (''${P6_MS}ms)"

      # Phase 6b — verify the kernel actually accepted the module:
      # lsmod sees it AND the module printed its boot banner to dmesg.
      for label in vm1 vm2; do
        if [ "$label" = vm1 ]; then port=$VM1_VIRTIO host=$VM1_PROC; else port=$VM2_VIRTIO host=$VM2_PROC; fi
        out=$(vm_run "$port" "$host" "lsmod | grep ^urp; dmesg | grep -i '^\[.*\] urp:' | tail -10" 5)
        echo "$out" > "$RUNDIR/diag/$label.urp-loaded.txt"
        if ! echo "$out" | grep -q "^urp"; then
          echo "$out" | awk '{print "    "$0}'
          fail "$label: urp not in lsmod"
        fi
        if ! echo "$out" | grep -q "urp: module loaded"; then
          info "$label: no 'module loaded' banner in dmesg (may have been trimmed)"
        fi
      done
      pass "urp present in lsmod + dmesg banner seen"
      echo ""

      # -----------------------------------------------------------------
      # Phase 7 — socat echo backend on vm1
      # -----------------------------------------------------------------
      echo "--- Phase 7: socat echo backend on vm1 ---"
      vm_run "$VM1_VIRTIO" "$VM1_PROC" \
        "rm -f /tmp/urp-pair-echo.sock; nohup socat UNIX-LISTEN:/tmp/urp-pair-echo.sock,fork EXEC:cat </dev/null >/dev/null 2>&1 & sleep 0.3; echo SOCAT_OK" 5 \
        | grep -q SOCAT_OK \
        || fail "vm1 socat echo did not start"
      pass "socat echo running on vm1"
      echo ""

      # -----------------------------------------------------------------
      # Phase 8 — configure URP endpoints
      # -----------------------------------------------------------------
      echo "--- Phase 8: urp add on both VMs ---"
      P8_START=$(now_ms)
      vm_run "$VM1_VIRTIO" "$VM1_PROC" \
        "urp add pair_acceptor --connect-path /tmp/urp-pair-echo.sock --bind $VM1_IP:4791" "$T_URP" \
        | tee "$RUNDIR"/add1.log | grep -q "ok:" \
        || { awk '{print "    "$0}' "$RUNDIR"/add1.log; fail "vm1 urp add failed"; }
      vm_run "$VM2_VIRTIO" "$VM2_PROC" \
        "urp add pair_initiator --listen-path /tmp/urp-pair.sock --peer $VM1_IP:4791" "$T_URP" \
        | tee "$RUNDIR"/add2.log | grep -q "ok:" \
        || { awk '{print "    "$0}' "$RUNDIR"/add2.log; fail "vm2 urp add failed"; }
      P8_MS=$(( $(now_ms) - P8_START ))
      pass "both endpoints configured (''${P8_MS}ms)"

      # Phase 8b — `urp show` confirms each side has its endpoint
      # entry. Catches the case where `urp add` returned ok: but the
      # endpoint silently failed to materialise.
      for entry in "vm1:$VM1_VIRTIO:$VM1_PROC:pair_acceptor" \
                   "vm2:$VM2_VIRTIO:$VM2_PROC:pair_initiator"; do
        IFS=":" read -r label port host expected <<<"$entry"
        out=$(vm_run "$port" "$host" "urp show" 10)
        echo "$out" > "$RUNDIR/diag/$label.urp-show.txt"
        if ! echo "$out" | grep -q "$expected"; then
          echo "$out" | awk '{print "    "$0}'
          fail "$label: urp show does not list endpoint '$expected'"
        fi
      done
      pass "urp show lists pair_acceptor on vm1 and pair_initiator on vm2"
      echo ""

      # -----------------------------------------------------------------
      # Phase 9 — wait for CM ESTABLISHED (poll dmesg)
      # -----------------------------------------------------------------
      echo "--- Phase 9: wait CM ESTABLISHED (timeout ''${T_CM}s) ---"
      P9_START=$(now_ms)
      cm_ready=0
      for _ in $(seq 1 "$T_CM"); do
        d=$(vm_run "$VM1_VIRTIO" "$VM1_PROC" "dmesg | tail -5" 5)
        if echo "$d" | grep -qE "CM ESTABLISHED|established|qp 0x"; then
          cm_ready=1; break
        fi
        sleep "$POLL"
      done
      if [ "$cm_ready" -eq 0 ]; then
        info "no explicit CM ESTABLISHED line in dmesg — proceeding anyway"
      fi
      P9_MS=$(( $(now_ms) - P9_START ))
      pass "CM phase complete (''${P9_MS}ms)"

      # Phase 9b — capture QP state on both sides. RTS = ready-to-send
      # (the normal post-CM-ESTABLISHED state). If we see INIT or RTR
      # the CM handshake hasn't fully transitioned the QP.
      for label in vm1 vm2; do
        if [ "$label" = vm1 ]; then port=$VM1_VIRTIO host=$VM1_PROC; else port=$VM2_VIRTIO host=$VM2_PROC; fi
        out=$(vm_run "$port" "$host" "rdma resource show qp" 10)
        echo "$out" > "$RUNDIR/diag/$label.qp-state.txt"
        info "$label QP state: $(echo "$out" | head -3 | tr '\n' '|' )"
      done
      echo ""

      # -----------------------------------------------------------------
      # Phase 10 — data round-trip
      #
      # 10a: dmesg snapshot pre-echo
      # 10:  actual echo attempt
      # 10c: dmesg snapshot post-echo (so the diff isolates kernel
      #      activity caused by the data path)
      # -----------------------------------------------------------------
      echo "--- Phase 10: echo round-trip ---"
      for label in vm1 vm2; do
        if [ "$label" = vm1 ]; then port=$VM1_VIRTIO host=$VM1_PROC; else port=$VM2_VIRTIO host=$VM2_PROC; fi
        vm_run "$port" "$host" "dmesg | tail -40" 10 \
          > "$RUNDIR/diag/$label.dmesg-pre.txt"
      done

      P10_START=$(now_ms)
      # (a) Verifiable single echo FIRST, on a clean console.
      RESULT=$(vm_run "$VM2_VIRTIO" "$VM2_PROC" \
        "echo hello-pair | socat -t 5 - UNIX-CONNECT:/tmp/urp-pair.sock" "$T_ECHO" \
        | tr -d '\r' | grep -v '^$' | tail -1)
      echo "  response: '$RESULT'"

      # (b) Multi-stream exercise (Phase 3a Step 7d): fire a burst of concurrent
      # UDS connections so the initiator opens many streams at once, each
      # tunnelled to its own backend UDS on the acceptor. Stresses stream
      # create + per-stream pump + reap-on-close + teardown -- exactly the
      # concurrency the sanitizer scan (Phase 11b) validates. We only assert it
      # completes; the drain that follows exercises multi-stream teardown.
      if vm_run "$VM2_VIRTIO" "$VM2_PROC" \
        "for i in 1 2 3 4 5 6 7 8 9 10 11 12; do ( echo hp | socat -t 5 - UNIX-CONNECT:/tmp/urp-pair.sock >/dev/null 2>&1 ) & done; wait; echo BURST_DONE" 120 \
        | grep -q BURST_DONE; then
        info "concurrent 12-stream burst completed"
      else
        info "burst may not have completed (check diag)"
      fi
      P10_MS=$(( $(now_ms) - P10_START ))

      for label in vm1 vm2; do
        if [ "$label" = vm1 ]; then port=$VM1_VIRTIO host=$VM1_PROC; else port=$VM2_VIRTIO host=$VM2_PROC; fi
        vm_run "$port" "$host" "dmesg | tail -40" 10 \
          > "$RUNDIR/diag/$label.dmesg-post.txt"
      done
      for label in vm1 vm2; do
        if [ "$label" = vm1 ]; then port=$VM1_VIRTIO host=$VM1_PROC; else port=$VM2_VIRTIO host=$VM2_PROC; fi
        vm_run "$port" "$host" "urp show" 10 \
          > "$RUNDIR/diag/$label.urp-show-post.txt"
      done
      info "diagnostics written to $RUNDIR/diag/"
      echo ""

      # -----------------------------------------------------------------
      # Phase 10b — stats liveness assertions (design 28 §28.8).
      #
      # Unit tests + fuzzers prove each component works in isolation; they
      # CANNOT prove the wired-up data path actually drives it. This asserts
      # that the counters a data round-trip MUST move are non-zero -- the
      # cheap oracle that catches "feature silently not exercised" (the class
      # of bug that hid the inert reorder buffer, design 29 Gap 1). A counter
      # that no scenario can move is a dead-feature smell.
      # -----------------------------------------------------------------
      echo "--- Phase 10b: stats liveness ---"

      # statval <stats-file> <key> -> the integer value, or "" if absent.
      statval() { sed -n "s/^$2://p" "$1" | tail -1; }

      # assert_moved <label> <stats-file> <key>: hard-fail if a counter that a
      # completed round-trip must have driven is missing or still zero.
      assert_moved() {
        local v; v=$(statval "$2" "$3")
        if [ -z "$v" ]; then fail "$1: stat '$3' not reported by urp stats"; fi
        if [ "$v" -gt 0 ] 2>/dev/null; then
          pass "$1: $3 = $v (moved)"
        else
          fail "$1: $3 is '$v' -- data path did not drive it"
        fi
      }

      # expect_pending <label> <stats-file> <key> <why>: a counter we EXPECT
      # to be inert today (a documented gap). Never fails; surfaces its value
      # every run so the gap stays visible, and flags the moment it goes live
      # so the check can be promoted to assert_moved.
      expect_pending() {
        local v; v=$(statval "$2" "$3")
        if [ "''${v:-0}" -gt 0 ] 2>/dev/null; then
          pass "$1: $3 = $v (now LIVE -- promote to assert_moved, design 28 §28.8)"
        else
          info "$1: $3 = ''${v:-0} (expected inert: $4)"
        fi
      }

      for entry in "vm1:$VM1_VIRTIO:$VM1_PROC:pair_acceptor" \
                   "vm2:$VM2_VIRTIO:$VM2_PROC:pair_initiator"; do
        IFS=":" read -r label port host ep <<<"$entry"
        sfile="$RUNDIR/diag/$label.stats.txt"
        vm_run "$port" "$host" "urp stats $ep | tr -d ' '" 10 \
          | tr -d '\r' > "$sfile"
        # A completed echo + 12-stream burst MUST have moved frames and bytes
        # in both directions on both endpoints.
        assert_moved "$label" "$sfile" tx-frames
        assert_moved "$label" "$sfile" rx-frames
        assert_moved "$label" "$sfile" tx-bytes
        assert_moved "$label" "$sfile" rx-bytes
        # The pair traffic above is in-order (single QP, sequential seqs), so
        # nothing is buffered here and reorder-insertions stays 0. The reorder
        # buffer IS wired into the RX path (design 29 Gap 1 fix); Phase 10e
        # proves it fires by sending frames deliberately out of sequence.
        expect_pending "$label" "$sfile" reorder-insertions \
          "0 for in-order pair traffic; exercised in Phase 10e"
      done
      pass "traffic counters moved on both endpoints"
      echo ""

      # -----------------------------------------------------------------
      # Phase 10c — live-kernel netlink fuzz (design 27 F2, S3). Hammer the
      # genl control plane on vm1 while the module is loaded; the Phase 11b
      # sanitizer scan then catches any KASAN / BUG the parser trips. The
      # fuzzer sends random endpoint NAMES, so it does not disturb the real
      # pair_acceptor / pair_initiator endpoints that teardown removes.
      # -----------------------------------------------------------------
      echo "--- Phase 10c: netlink fuzz (vm1, live module) ---"
      NLFUZZ_SECS=${if sanitizer then "25" else "8"}
      vm_run "$VM1_VIRTIO" "$VM1_PROC" \
        "fuzz-netlink $NLFUZZ_SECS 1 2>&1 | tail -3" $((NLFUZZ_SECS + 25)) \
        > "$RUNDIR/diag/vm1.netlink-fuzz.txt" 2>&1 || true
      scan_splat "$RUNDIR/diag/vm1.netlink-fuzz.txt" "netlink fuzz"
      if grep -q NETLINK_FUZZ_DONE "$RUNDIR/diag/vm1.netlink-fuzz.txt" 2>/dev/null; then
        info "netlink fuzz completed ($(grep -o 'iters=[0-9]*' "$RUNDIR/diag/vm1.netlink-fuzz.txt" | tail -1))"
      else
        info "netlink fuzz did not report DONE (check diag/vm1.netlink-fuzz.txt)"
      fi
      echo ""

      # -----------------------------------------------------------------
      # Phase 10c2 — KCOV coverage-guided netlink fuzz (design 27 F2, S3).
      # Same genl control plane, but driven by KCOV coverage feedback so
      # inputs get pushed past the policy layer into the handler bodies that
      # blind fuzzing (10c) rarely reaches. Requires the CONFIG_KCOV sanitizer
      # kernel; on the non-sanitizer kernel it falls back to blind mode (still
      # runs, just no coverage signal). Oracle is the same Phase 11b scan.
      # -----------------------------------------------------------------
      echo "--- Phase 10c2: coverage-guided netlink fuzz (vm1, KCOV) ---"
      COVFUZZ_SECS=${if sanitizer then "25" else "8"}
      vm_run "$VM1_VIRTIO" "$VM1_PROC" \
        "fuzz-netlink-cov $COVFUZZ_SECS 1 2>&1 | tail -4" $((COVFUZZ_SECS + 25)) \
        > "$RUNDIR/diag/vm1.cov-fuzz.txt" 2>&1 || true
      scan_splat "$RUNDIR/diag/vm1.cov-fuzz.txt" "coverage-guided netlink fuzz"
      if grep -q COV_FUZZ_DONE "$RUNDIR/diag/vm1.cov-fuzz.txt" 2>/dev/null; then
        info "cov fuzz completed ($(grep -o 'execs=[0-9]* corpus=[0-9]* edges=[0-9]*' "$RUNDIR/diag/vm1.cov-fuzz.txt" | tail -1))"
        info "kcov: $(grep -o 'kcov=[a-z]*' "$RUNDIR/diag/vm1.cov-fuzz.txt" | tail -1)"
      else
        info "cov fuzz did not report DONE (check diag/vm1.cov-fuzz.txt)"
      fi
      echo ""

      # -----------------------------------------------------------------
      # Phase 10c3 — concurrent netlink race (design 27 F2, S3 concurrency).
      # Multi-threaded NEW/DEL/SET/GET churn on a small shared name pool to
      # trip the endpoint-lifecycle races the single-threaded fuzzers can't:
      # the deref-after-rcu-unlock UAF (no kref on endpoints, design 26) and
      # the double-DEL free. Oracle: the same KASAN scan (scan_splat + 11b).
      # -----------------------------------------------------------------
      echo "--- Phase 10c3: concurrent netlink race (vm1, ${if sanitizer then "KASAN" else "smoke"}) ---"
      RACE_SECS=${if sanitizer then "25" else "6"}
      vm_run "$VM1_VIRTIO" "$VM1_PROC" \
        "fuzz-netlink-race $RACE_SECS $VM1_IP 1 8 2>&1 | tail -4" $((RACE_SECS + 25)) \
        > "$RUNDIR/diag/vm1.race-fuzz.txt" 2>&1 || true
      scan_splat "$RUNDIR/diag/vm1.race-fuzz.txt" "concurrent netlink race"
      if grep -q RACE_DONE "$RUNDIR/diag/vm1.race-fuzz.txt" 2>/dev/null; then
        info "race completed ($(grep -o 'ops=[0-9]* threads=[0-9]*' "$RUNDIR/diag/vm1.race-fuzz.txt" | tail -1))"
      else
        info "race did not report DONE (check diag/vm1.race-fuzz.txt)"
      fi
      echo ""

      # -----------------------------------------------------------------
      # Phase 10d — hostile-peer wire fuzz (design 27 F2, S1/S2). Stand up a
      # DEDICATED acceptor endpoint on vm1 (fuzz_acceptor, port 4792, its own
      # socat backend) — the live pair_acceptor's QP slots are already full
      # with the real initiator, so the acceptor would rdma_reject a second
      # peer (urp_rdma.c: qp_index >= num_qps). From vm2, wire_fuzz connects
      # to fuzz_acceptor and injects malformed frames into the acceptor's RX
      # path: bad lengths, unknown types, and scripted SYN/FIN/RST sequences
      # that spin up + tear down streams (kthread + backend UDS) so the
      # "RST-under-RCU" lifecycle runs under KASAN. The friendly pair test
      # never sends hostile frames, so this is the only phase that reaches
      # urp_recv_done's error/destroy branches. The Phase 11b sanitizer scan
      # is the oracle. fuzz_acceptor is created + hammered + torn down here,
      # so it also exercises endpoint teardown right after hostile traffic.
      # -----------------------------------------------------------------
      echo "--- Phase 10d: hostile wire fuzz (vm2 -> vm1 fuzz_acceptor) ---"
      WIREFUZZ_SECS=${if sanitizer then "25" else "8"}
      vm_run "$VM1_VIRTIO" "$VM1_PROC" \
        "rm -f /tmp/urp-fuzz-echo.sock; nohup socat UNIX-LISTEN:/tmp/urp-fuzz-echo.sock,fork EXEC:cat </dev/null >/dev/null 2>&1 & sleep 0.3; echo FUZZ_SOCAT_OK" 5 \
        | grep -q FUZZ_SOCAT_OK \
        || info "vm1 fuzz socat backend may not have started"
      if vm_run "$VM1_VIRTIO" "$VM1_PROC" \
           "urp add fuzz_acceptor --connect-path /tmp/urp-fuzz-echo.sock --bind $VM1_IP:4792" "$T_URP" \
           | grep -q "ok:"; then
        # rxe listener needs a beat to be routable before the peer resolves it.
        sleep "$POLL"
        vm_run "$VM2_VIRTIO" "$VM2_PROC" \
          "fuzz-wire $VM1_IP 4792 $WIREFUZZ_SECS 1 2>&1 | tail -4" $((WIREFUZZ_SECS + 25)) \
          > "$RUNDIR/diag/vm2.wire-fuzz.txt" 2>&1 || true
        scan_splat "$RUNDIR/diag/vm2.wire-fuzz.txt" "wire fuzz"
        if grep -q WIRE_FUZZ_DONE "$RUNDIR/diag/vm2.wire-fuzz.txt" 2>/dev/null; then
          info "wire fuzz completed ($(grep -o 'frames=[0-9]* reconnects=[0-9]*' "$RUNDIR/diag/vm2.wire-fuzz.txt" | tail -1))"
        else
          info "wire fuzz did not report DONE (check diag/vm2.wire-fuzz.txt)"
        fi
        # Tear down the fuzz acceptor right after hostile traffic.
        vm_run "$VM1_VIRTIO" "$VM1_PROC" "urp drain fuzz_acceptor 2>&1; urp remove fuzz_acceptor 2>&1; echo FUZZ_EP_GONE" 15 \
          > "$RUNDIR/diag/vm1.fuzz-teardown.txt" 2>&1 || true
      else
        info "fuzz_acceptor add failed; skipping wire fuzz (check diag)"
      fi
      vm_run "$VM1_VIRTIO" "$VM1_PROC" "pkill -f urp-fuzz-echo.sock 2>/dev/null; rm -f /tmp/urp-fuzz-echo.sock; echo FZC" 5 >/dev/null 2>&1 || true
      echo ""

      # -----------------------------------------------------------------
      # Phase 10e — reorder-buffer wiring proof (design 29 Gap 1,
      # design 28 §28.8.3). A DEDICATED acceptor (reorder_acc, port
      # 4793, its own echo backend) -- pair_acceptor's single QP slot is
      # taken by the real initiator. From vm2 the urp-test-client `reorder`
      # mode opens a stream and sends frames OUT OF SEQUENCE (SYN, then
      # adjacent pairs swapped), so on the single RC QP they ARRIVE out of
      # order and the acceptor's per-stream reorder buffer must buffer +
      # reassemble them before UDS delivery. We then assert the buffer
      # actually fired (reorder-insertions > 0) and every frame was
      # delivered with none dropped (rx-frames == nframes, reorder-drops
      # == 0). This is the scenario Phase 10b's in-order traffic cannot
      # reach. Byte-exact ordering itself is the reorder buffer's own
      # contract (fuzz-reorder + KUnit + Rust twin), not re-proven here.
      # -----------------------------------------------------------------
      echo "--- Phase 10e: reorder-buffer wiring proof ---"
      RB_FRAMES=8
      vm_run "$VM1_VIRTIO" "$VM1_PROC" \
        "rm -f /tmp/urp-reorder-echo.sock; nohup socat UNIX-LISTEN:/tmp/urp-reorder-echo.sock,fork EXEC:cat </dev/null >/dev/null 2>&1 & sleep 0.3; echo RB_SOCAT_OK" 5 \
        | grep -q RB_SOCAT_OK \
        || info "vm1 reorder socat backend may not have started"
      vm_run "$VM1_VIRTIO" "$VM1_PROC" \
        "urp add reorder_acc --connect-path /tmp/urp-reorder-echo.sock --bind $VM1_IP:4793" "$T_URP" \
        > "$RUNDIR/diag/vm1.reorder-add.txt" 2>&1
      if grep -q "ok:" "$RUNDIR/diag/vm1.reorder-add.txt"; then
        sleep "$POLL"
        vm_run "$VM2_VIRTIO" "$VM2_PROC" \
          "urp-test-client $VM1_IP 4793 reorder $RB_FRAMES 2>&1 | tail -4" 30 \
          > "$RUNDIR/diag/vm2.reorder-client.txt" 2>&1 || true
        scan_splat "$RUNDIR/diag/vm2.reorder-client.txt" "reorder client"
        if grep -q REORDER_SENT "$RUNDIR/diag/vm2.reorder-client.txt" 2>/dev/null; then
          info "reorder client sent $(grep -o 'frames=[0-9]*' "$RUNDIR/diag/vm2.reorder-client.txt" | tail -1)"
        else
          awk '{print "    "$0}' "$RUNDIR/diag/vm2.reorder-client.txt"
          fail "reorder client did not report REORDER_SENT"
        fi
        # Let the acceptor finish delivering the reassembled stream.
        sleep 1
        rbfile="$RUNDIR/diag/vm1.reorder-stats.txt"
        vm_run "$VM1_VIRTIO" "$VM1_PROC" "urp stats reorder_acc | tr -d ' '" 10 \
          | tr -d '\r' > "$rbfile"
        # The buffer must have fired -- frames arrived out of order.
        assert_moved "reorder_acc" "$rbfile" reorder-insertions
        # And every crafted frame must have been delivered, none dropped.
        rxf=$(statval "$rbfile" rx-frames)
        drp=$(statval "$rbfile" reorder-drops)
        if [ "''${rxf:-0}" -eq "$RB_FRAMES" ] 2>/dev/null; then
          pass "reorder_acc: rx-frames = $rxf (all $RB_FRAMES delivered)"
        else
          fail "reorder_acc: rx-frames = ''${rxf:-0}, expected $RB_FRAMES"
        fi
        if [ "''${drp:-0}" -eq 0 ] 2>/dev/null; then
          pass "reorder_acc: reorder-drops = 0"
        else
          fail "reorder_acc: reorder-drops = ''${drp:-0} (frames lost)"
        fi
        vm_run "$VM1_VIRTIO" "$VM1_PROC" "urp drain reorder_acc 2>&1; urp remove reorder_acc 2>&1; echo RB_EP_GONE" 15 \
          > "$RUNDIR/diag/vm1.reorder-teardown.txt" 2>&1 || true
      else
        awk '{print "    "$0}' "$RUNDIR/diag/vm1.reorder-add.txt"
        fail "reorder_acc add failed (see diag/vm1.reorder-add.txt)"
      fi
      vm_run "$VM1_VIRTIO" "$VM1_PROC" "pkill -f urp-reorder-echo.sock 2>/dev/null; rm -f /tmp/urp-reorder-echo.sock; echo RBC" 5 >/dev/null 2>&1 || true
      echo ""

      # -----------------------------------------------------------------
      # Phase 10f — buffer geometry proof (design 29 Gap 2). buffer_count
      # and buffer_size are now wired: pool depth, CQ/SRQ/SQ, DMA slot bytes
      # and the wire max-payload are all sized from the endpoint config. For
      # each (count,size) we (1) add a dedicated acceptor with that geometry,
      # (2) assert `urp show` reports the EFFECTIVE count/size (closes the
      # design 28 observability gap), (3) from vm2 push single DATA frames of
      # payload > the old fixed 4076 ceiling and verify each echo returns
      # byte-exact -- only possible if the slot + max-payload were sized from
      # buffer_size (the old fixed 4096 slot would drop them OVERSIZE / fail
      # to DMA), and (4) assert the acceptor delivered every frame. Per-frame
      # RTT and MB/s are logged so the size / latency-vs-throughput tradeoff
      # is visible. Under the sanitizer kernel this also KASAN-checks the
      # high-order (>PAGE_SIZE) page_pool allocation.
      #
      # The acceptor is added `--mode k0`: urp-test-client's echo/bigframe
      # path (do_echo) speaks the legacy single-connection wire format
      # (stream_id 0, no SYN), so its frames deliver via ep->conn and echo
      # back symmetrically through the k0 pump (stream 0, no SYN). A
      # multistream acceptor deliberately does NOT eager-connect ep->conn
      # (urp_acceptor_should_eager_connect gates on URP_EP_MODE_K0; connecting
      # would steal the one backend a real stream's SYN needs), so stream-0
      # frames would be dropped with no backend -> buffer_alloc_fails, no echo.
      # Multistream RX is exercised by Phase 10e (reorder, stream 1 + SYN);
      # buffer geometry is mode-independent, so k0 proves it just as well.
      # -----------------------------------------------------------------
      echo "--- Phase 10f: buffer geometry (buffer_count + buffer_size) ---"
      vm_run "$VM1_VIRTIO" "$VM1_PROC" \
        "rm -f /tmp/urp-geom-echo.sock; nohup socat -b 65536 UNIX-LISTEN:/tmp/urp-geom-echo.sock,fork EXEC:cat </dev/null >/dev/null 2>&1 & sleep 0.3; echo GEOM_SOCAT_OK" 5 \
        | grep -q GEOM_SOCAT_OK \
        || info "vm1 geom socat backend may not have started"
      geomtab="$RUNDIR/diag/geom-tradeoff.txt"
      : > "$geomtab"
      GEOM_BFRAMES=32
      gport=4794
      # count:size:payload -- swept from tiny (256 B, small order-0 slots near
      # URP_BUFFER_SIZE_MIN) up to 12000 B (order-3 slots > PAGE_SIZE). The
      # small end is the RDMA/kernel-bypass sweet spot (per-message overhead
      # dominates -> the msgs/s column is the interesting one); the payload >
      # 4076 entries prove the slot + max-payload were sized from buffer_size
      # (not the old fixed 4096). buffer_size is matched to the payload -- the
      # realistic "size the pool for the workload" config -- so the sweep also
      # exercises the whole page_pool order range end to end.
      for spec in 64:512:256 64:1024:512 64:2048:1024 64:4096:2048 32:16384:8000 64:32768:12000; do
        gc=''${spec%%:*}; grest=''${spec#*:}; gs=''${grest%%:*}; gp=''${grest##*:}
        vm_run "$VM1_VIRTIO" "$VM1_PROC" \
          "urp add geom_acc --mode k0 --connect-path /tmp/urp-geom-echo.sock --bind $VM1_IP:$gport --buffer-count $gc --buffer-size $gs" "$T_URP" \
          > "$RUNDIR/diag/vm1.geom-add-$gs.txt" 2>&1
        if ! grep -q "ok:" "$RUNDIR/diag/vm1.geom-add-$gs.txt"; then
          awk '{print "    "$0}' "$RUNDIR/diag/vm1.geom-add-$gs.txt"
          fail "geom_acc add failed (count=$gc size=$gs)"
        fi
        # (2) GET must report the EFFECTIVE geometry.
        gshow="$RUNDIR/diag/vm1.geom-show-$gs.txt"
        vm_run "$VM1_VIRTIO" "$VM1_PROC" "urp show geom_acc" 10 | tr -d '\r' > "$gshow"
        rc=$(sed -n 's/.*buffer-count: *//p' "$gshow" | tail -1)
        rs=$(sed -n 's/.*buffer-size: *//p' "$gshow" | tail -1)
        if [ "''${rc:-0}" = "$gc" ] && [ "''${rs:-0}" = "$gs" ]; then
          pass "geom_acc: show reports buffer-count=$rc buffer-size=$rs (effective)"
        else
          fail "geom_acc: show reports count=''${rc:-?}/size=''${rs:-?}, want $gc/$gs"
        fi
        # (3) push single frames of this payload; client verifies each echo byte-exact.
        sleep "$POLL"
        gcli="$RUNDIR/diag/vm2.geom-client-$gs.txt"
        vm_run "$VM2_VIRTIO" "$VM2_PROC" \
          "urp-test-client $VM1_IP $gport bigframe $gp $GEOM_BFRAMES 2>&1 | tail -4" 40 \
          > "$gcli" 2>&1 || true
        scan_splat "$gcli" "geom client"
        if grep -q BIGFRAME_OK "$gcli"; then
          gline=$(grep BIGFRAME_OK "$gcli" | tail -1)
          rtt=$(echo "$gline" | sed -n 's/.*rtt_us=\([0-9.]*\).*/\1/p')
          mbps=$(echo "$gline" | sed -n 's/.*mbps=\([0-9.]*\).*/\1/p')
          msgs=$(echo "$gline" | sed -n 's/.*msgs_per_s=\([0-9.]*\).*/\1/p')
          pass "geom_acc size=$gs payload=$gp: $GEOM_BFRAMES frames echoed byte-exact"
          printf "  %8s %8s %10s %10s %10s\n" "$gs" "$gp" "''${rtt:-?}" "''${mbps:-?}" "''${msgs:-?}" >> "$geomtab"
        else
          awk '{print "    "$0}' "$gcli"
          fail "geom_acc size=$gs payload=$gp: bigframe did not report BIGFRAME_OK"
        fi
        # (4) acceptor must have received + delivered every frame.
        sleep 1
        gstat="$RUNDIR/diag/vm1.geom-stats-$gs.txt"
        vm_run "$VM1_VIRTIO" "$VM1_PROC" "urp stats geom_acc | tr -d ' '" 10 | tr -d '\r' > "$gstat"
        grxf=$(statval "$gstat" rx-frames)
        grxb=$(statval "$gstat" rx-bytes)
        want_b=$((gp * GEOM_BFRAMES))
        if [ "''${grxf:-0}" -ge "$GEOM_BFRAMES" ] 2>/dev/null && [ "''${grxb:-0}" -ge "$want_b" ] 2>/dev/null; then
          pass "geom_acc size=$gs: rx-frames=$grxf rx-bytes=$grxb (>= $GEOM_BFRAMES / $want_b)"
        else
          fail "geom_acc size=$gs: rx-frames=''${grxf:-0} rx-bytes=''${grxb:-0}, want >= $GEOM_BFRAMES / $want_b"
        fi
        vm_run "$VM1_VIRTIO" "$VM1_PROC" "urp drain geom_acc 2>/dev/null; urp remove geom_acc 2>/dev/null; echo GEOM_GONE" 15 >/dev/null 2>&1 || true
        gport=$((gport + 1))
        sleep "$POLL"
      done
      if [ -s "$geomtab" ]; then
        info "geometry tradeoff (small msgs -> more msgs/s, less MB/s; larger buffer_size -> higher MB/s):"
        printf "  %8s %8s %10s %10s %10s\n" "bufsize" "payload" "rtt_us" "mbps" "msgs/s"
        cat "$geomtab"
      fi
      vm_run "$VM1_VIRTIO" "$VM1_PROC" "pkill -f urp-geom-echo.sock 2>/dev/null; rm -f /tmp/urp-geom-echo.sock; echo GBC" 5 >/dev/null 2>&1 || true
      echo ""

      # -----------------------------------------------------------------
      # Phase 10g — urp-bench through the tunnel (design 30 §30.14).
      # vm1 runs `urp-bench --listen` as the tunnel backend (replacing
      # socat's role for a dedicated endpoint pair on port 4795); vm2's
      # bench connects to urp's listen_path. Both sides generate AND echo
      # (full duplex), every payload byte verified (--verify full). Smoke
      # cells by default; URP_BENCH_FULL=1 adds the bigger cell set.
      # -----------------------------------------------------------------
      echo "--- Phase 10g: urp-bench io_uring benchmark through the tunnel ---"
      benchtab="$RUNDIR/diag/bench-results.txt"
      : > "$benchtab"
      # The bench rides the EXISTING pair endpoints: urp-bench --listen
      # takes over socat's echo-backend role on /tmp/urp-pair-echo.sock,
      # and each bench client connection to /tmp/urp-pair.sock is a new
      # stream on the already-ESTABLISHED session (the Phase 10 12-stream
      # burst proves that path). A dedicated endpoint pair does NOT work:
      # a second concurrent initiator endpoint never starts its CM
      # machinery (module gap found by this phase — see status.md).
      vm_run "$VM1_VIRTIO" "$VM1_PROC" \
        "pkill -f 'urp-pair-echo.sock' 2>/dev/null; sleep 0.3; echo SOCAT_GONE" 10 \
        >/dev/null 2>&1 || true
      # State snapshot for post-mortems (captured, never parsed).
      vm_run "$VM2_VIRTIO" "$VM2_PROC" "urp show; urp stats pair_initiator; dmesg | tail -15" 10 \
        > "$RUNDIR/diag/vm2.bench-state-pre.txt" 2>&1 || true
      BENCH_CELLS="blocking:4076:8 uring-rw:4076:8 uring-fixed:4076:8 uring-bufring:4076:8"
      if [ -n "''${URP_BENCH_FULL:-}" ]; then
        BENCH_CELLS="$BENCH_CELLS uring-rw:1024:64 uring-rw:65516:4 uring-fixed:16384:32 uring-bufring:65516:4"
      fi
      for cell in $BENCH_CELLS; do
        bm=''${cell%%:*}; brest=''${cell#*:}; bs=''${brest%%:*}; bb=''${brest##*:}
        bcli="$RUNDIR/diag/vm2.bench-$bm-$bs-$bb.txt"
        blst="$RUNDIR/diag/vm1.bench-$bm-$bs-$bb.txt"
        bench_cell_ok=0
        for attempt in 1 2 3; do
          # fresh listener per attempt (it exits after its FIN handshake)
          vm_run "$VM1_VIRTIO" "$VM1_PROC" \
            "pkill -f 'urp-bench --listen' 2>/dev/null; rm -f /tmp/urp-pair-echo.sock; nohup urp-bench --listen /tmp/urp-pair-echo.sock --id 2 --mode $bm --msg-size $bs --batch $bb --count 300 --verify full >/tmp/urp-bench-l.out 2>&1 & sleep 0.3; echo BENCH_L_UP" 10 \
            >/dev/null 2>&1 || true
          vm_run "$VM2_VIRTIO" "$VM2_PROC" \
            "urp-bench --connect /tmp/urp-pair.sock --id 1 --mode $bm --msg-size $bs --batch $bb --count 300 --verify full 2>&1" "$T_BENCH" \
            > "$bcli" 2>&1 || true
          scan_splat "$bcli" "bench client ($bm)"
          vm_run "$VM1_VIRTIO" "$VM1_PROC" "cat /tmp/urp-bench-l.out" 10 > "$blst" 2>&1 || true
          scan_splat "$blst" "bench listener ($bm)"
          if grep -q BENCH_OK "$bcli" && grep -q BENCH_OK "$blst"; then
            bench_cell_ok=1
            break
          fi
          if grep -q BENCH_SKIP "$bcli" || grep -q BENCH_SKIP "$blst"; then
            bench_cell_ok=2
            break
          fi
          info "bench $bm msg=$bs batch=$bb: attempt $attempt failed (RDMA session may still be settling); retrying"
          sleep 2
        done
        case "$bench_cell_ok" in
          1)
            pass "bench $bm msg=$bs batch=$bb: BENCH_OK both sides (verify=full)"
            grep -h BENCH_OK "$bcli" "$blst" >> "$benchtab"
            ;;
          2)
            info "bench $bm msg=$bs batch=$bb: $(grep -m1 -ho 'reason=[a-z_]*' "$bcli" "$blst" || echo skipped)"
            ;;
          *)
            awk '{print "    "$0}' "$bcli"
            awk '{print "    "$0}' "$blst"
            vm_run "$VM1_VIRTIO" "$VM1_PROC" "urp show; urp stats pair_acceptor; dmesg | tail -25" 10 \
              > "$RUNDIR/diag/vm1.bench-state-fail.txt" 2>&1 || true
            vm_run "$VM2_VIRTIO" "$VM2_PROC" "urp show; urp stats pair_initiator; dmesg | tail -25" 10 \
              > "$RUNDIR/diag/vm2.bench-state-fail.txt" 2>&1 || true
            fail "bench $bm msg=$bs batch=$bb: no BENCH_OK after 3 attempts"
            ;;
        esac
      done
      # C<->Rust interop THROUGH the tunnel: vm1 echoes with the C shell,
      # vm2 generates with the Rust shell — the live cross-language
      # differential (§30.14), now over RDMA. Skipped on VM images
      # without the Rust twin (cross-arch variants).
      if vm_run "$VM2_VIRTIO" "$VM2_PROC" "command -v urp-bench-rs >/dev/null && echo HAVE_RS" 5 | grep -q HAVE_RS; then
        vm_run "$VM1_VIRTIO" "$VM1_PROC" \
          "rm -f /tmp/urp-pair-echo.sock; nohup urp-bench --listen /tmp/urp-pair-echo.sock --id 2 --mode uring-rw --msg-size 4076 --batch 8 --count 300 --verify full >/tmp/urp-bench-l.out 2>&1 & sleep 0.3; echo BENCH_L_UP" 10 \
          | grep -q BENCH_L_UP || fail "vm1 bench listener (interop) did not start"
        bcli="$RUNDIR/diag/vm2.bench-interop.txt"
        vm_run "$VM2_VIRTIO" "$VM2_PROC" \
          "urp-bench-rs --connect /tmp/urp-pair.sock --id 1 --mode uring-rw --msg-size 4076 --batch 8 --count 300 --verify full 2>&1" "$T_BENCH" \
          > "$bcli" 2>&1 || true
        scan_splat "$bcli" "bench interop client"
        blst="$RUNDIR/diag/vm1.bench-interop.txt"
        vm_run "$VM1_VIRTIO" "$VM1_PROC" "cat /tmp/urp-bench-l.out" 10 > "$blst" 2>&1 || true
        scan_splat "$blst" "bench interop listener"
        if grep -q "BENCH_OK lang=rust" "$bcli" && grep -q "BENCH_OK lang=c" "$blst"; then
          pass "bench C<->Rust interop through the tunnel (verify=full)"
          grep -h BENCH_OK "$bcli" "$blst" >> "$benchtab"
        else
          awk '{print "    "$0}' "$bcli"
          awk '{print "    "$0}' "$blst"
          fail "bench C<->Rust interop: no BENCH_OK pair"
        fi
      else
        info "urp-bench-rs not in VM image (cross-arch build); tunneled interop cell skipped"
      fi
      if [ -s "$benchtab" ]; then
        info "tunneled urp-bench results (emulated rxe — correctness gate, not perf):"
        awk '{print "  "$0}' "$benchtab"
      fi
      # No endpoints to remove — the bench rode the pair endpoints. Just
      # stop the last listener; Phase 11 tears the pair down as usual.
      vm_run "$VM1_VIRTIO" "$VM1_PROC" \
        "pkill -f 'urp-bench --listen' 2>/dev/null; rm -f /tmp/urp-bench-l.out; echo BENCH_GONE" 15 >/dev/null 2>&1 || true
      pass "urp-bench listener stopped"
      echo ""

      # -----------------------------------------------------------------
      # Phase 10h — urp-fast uring_cmd REGISTER/UNREGISTER (design 31, PR1+PR2).
      # Drives /dev/urp against the loaded module: mmap an app buffer pool,
      # REGISTER it against the connected `pair_acceptor` endpoint (pins it
      # FOLL_LONGTERM AND DMA-maps every page against the acceptor's RDMA
      # device / PD, PR2), exercise the validator + binding negative cases
      # (unknown endpoint, double-register, out-of-range index, misaligned
      # base), then UNREGISTER (unmaps + unpins). Runs on vm1, where
      # pair_acceptor has a live PD after the pair established (Phase 9/10).
      # Oracle: URP_FAST_POC_OK plus a clean scan_splat (the pin/map/unmap/
      # unpin leaves no KASAN/leak trace).
      # -----------------------------------------------------------------
      echo "--- Phase 10h: urp-fast uring_cmd REGISTER/UNREGISTER (design 31) ---"
      fastout="$RUNDIR/diag/vm1.urp-fast-poc.txt"
      vm_run "$VM1_VIRTIO" "$VM1_PROC" \
        "ls -l /dev/urp 2>&1; urp-fast-poc /dev/urp pair_acceptor 4096 8 2>&1" "$T_BENCH" \
        > "$fastout" 2>&1 || true
      scan_splat "$fastout" "urp-fast poc"
      if grep -q URP_FAST_POC_OK "$fastout"; then
        pass "urp-fast REGISTER/UNREGISTER pin path (design 31 PR1)"
        awk '/^URP_FAST_/ {print "    "$0}' "$fastout"
      else
        awk '{print "    "$0}' "$fastout"
        fail "urp-fast poc: no URP_FAST_POC_OK (see $fastout)"
      fi
      echo ""

      # -----------------------------------------------------------------
      # Phase 10i — urp-fast zero-copy RECV arming + teardown quiesce
      # (design 31, PR4). Stand up a dedicated *fast*-kind endpoint pair
      # (fast_acc on vm1, fast_init on vm2). A fast endpoint suppresses the
      # UDS pump entirely -- the app drives the QP over /dev/urp -- and its
      # QP is created with a real receive queue and NO SRQ so each recv
      # completion carries the donating op's wr_cqe (urp_qp_create_on_cm_id).
      # This phase exercises exactly that path: once the pair reaches
      # ESTABLISHED, urp-fast-poc REGISTERs a pool against fast_acc and arms
      # several zero-copy RECV buffers directly on the fast RQ, then exits
      # WITHOUT reaping (no peer sends a frame). Ring teardown must cancel the
      # in-flight recvs and the fd close must drain the RQ before unpinning --
      # the teardown-quiesce that stops the NIC racing unpin_user_pages. The
      # sanitizer kernel's Phase 11b KASAN/KMEMLEAK sweep is the oracle for a
      # clean cancel+drain; URP_FAST_POC_OK is the functional gate. The full
      # delivered-bytes round trip (uds->fast / fast<->fast) is a follow-up.
      #
      # Retry the poc until REGISTER succeeds: the fast pair connects
      # asynchronously (initiator dials at activate), and a successful smoke
      # drains fast_acc's QP to ERR, so exactly one run can succeed.
      # -----------------------------------------------------------------
      echo "--- Phase 10i: urp-fast zero-copy RECV arming + drain (design 31 PR4) ---"
      fport=4795
      # A fast endpoint drives its data path over /dev/urp and creates no UDS
      # socket, but endpoint role is still derived from the path attr
      # (listen-path => initiator, connect-path => acceptor). Pass dummy paths
      # purely as the role marker; the fast path suppresses the UDS socket/pump
      # so nothing binds or dials them (urp_socket_init skips it for fast, and a
      # fast acceptor never eager-connects -- urp_rdma.c).
      vm_run "$VM1_VIRTIO" "$VM1_PROC" \
        "urp add fast_acc --kind fast --connect-path /tmp/urp-fast-acc.sock --bind $VM1_IP:$fport --buffer-count 16 --buffer-size 4096" "$T_URP" \
        > "$RUNDIR/diag/vm1.fast-add.txt" 2>&1
      vm_run "$VM2_VIRTIO" "$VM2_PROC" \
        "urp add fast_init --kind fast --listen-path /tmp/urp-fast-init.sock --peer $VM1_IP:$fport --buffer-count 16 --buffer-size 4096" "$T_URP" \
        > "$RUNDIR/diag/vm2.fast-add.txt" 2>&1
      if ! grep -q "ok:" "$RUNDIR/diag/vm1.fast-add.txt" || ! grep -q "ok:" "$RUNDIR/diag/vm2.fast-add.txt"; then
        awk '{print "    vm1: "$0}' "$RUNDIR/diag/vm1.fast-add.txt"
        awk '{print "    vm2: "$0}' "$RUNDIR/diag/vm2.fast-add.txt"
        fail "fast endpoint pair add failed"
      fi
      fastrecv="$RUNDIR/diag/vm1.fast-recv-smoke.txt"
      recv_ok=0
      for _try in $(seq 1 12); do
        sleep "$POLL"
        vm_run "$VM1_VIRTIO" "$VM1_PROC" \
          "urp-fast-poc /dev/urp fast_acc 4096 8 recv-smoke 2>&1" "$T_BENCH" \
          > "$fastrecv" 2>&1 || true
        if grep -q URP_FAST_POC_OK "$fastrecv"; then recv_ok=1; break; fi
      done
      scan_splat "$fastrecv" "urp-fast recv smoke"
      if [ "$recv_ok" = 1 ]; then
        pass "urp-fast zero-copy RECV armed on fast RQ + cancel/drain teardown (design 31 PR4)"
        awk '/^URP_FAST_/ {print "    "$0}' "$fastrecv"
      else
        awk '{print "    "$0}' "$fastrecv"
        fail "urp-fast recv smoke: no URP_FAST_POC_OK (see $fastrecv)"
      fi
      # Remove the fast pair (its QP is already ERR after the smoke drain).
      vm_run "$VM2_VIRTIO" "$VM2_PROC" "urp remove fast_init 2>/dev/null; echo FGONE2" 15 >/dev/null 2>&1 || true
      vm_run "$VM1_VIRTIO" "$VM1_PROC" "urp remove fast_acc 2>/dev/null; echo FGONE1" 15 >/dev/null 2>&1 || true
      echo ""

      # -----------------------------------------------------------------
      # Phase 11 — teardown (drain, remove, rmmod), each step timed
      # so the slow command pops out of the verdict table.
      # -----------------------------------------------------------------
      if [ -n "''${URP_PAIR_KEEP_VMS:-}" ]; then
        info "URP_PAIR_KEEP_VMS set; skipping Phase 11 teardown"
        info "vm1 virtio: nc 127.0.0.1 $VM1_VIRTIO ; vm2 virtio: nc 127.0.0.1 $VM2_VIRTIO"
        trap - EXIT INT TERM
        exit 0
      fi
      echo "--- Phase 11: teardown ---"
      P11_START=$(now_ms)
      run_step() {
        # $1 label, $2 marker, $3 port, $4 host, $5 cmd, $6 timeout
        local t0 out elapsed
        t0=$(now_ms)
        out=$(vm_run "$3" "$4" "$5" "$6")
        elapsed=$(( $(now_ms) - t0 ))
        if echo "$out" | grep -q "$2"; then
          info "$1 ''${elapsed}ms"
        else
          info "$1 ''${elapsed}ms (INCOMPLETE — marker $2 not seen)"
        fi
      }
      run_step "vm2 urp drain"  D2D "$VM2_VIRTIO" "$VM2_PROC" \
        "urp drain pair_initiator 2>&1; echo D2D" 10
      run_step "vm2 urp remove" D2R "$VM2_VIRTIO" "$VM2_PROC" \
        "urp remove pair_initiator 2>&1; echo D2R" 10
      run_step "vm2 rmmod urp"  D2M "$VM2_VIRTIO" "$VM2_PROC" \
        "rmmod urp 2>&1; echo D2M" 10
      run_step "vm1 urp drain"  D1D "$VM1_VIRTIO" "$VM1_PROC" \
        "urp drain pair_acceptor 2>&1; echo D1D" 10
      run_step "vm1 urp remove" D1R "$VM1_VIRTIO" "$VM1_PROC" \
        "urp remove pair_acceptor 2>&1; echo D1R" 10
      run_step "vm1 rmmod urp"  D1M "$VM1_VIRTIO" "$VM1_PROC" \
        "pkill socat 2>/dev/null; rmmod urp 2>&1; echo D1M" 10
      P11_MS=$(( $(now_ms) - P11_START ))
      pass "teardown done (''${P11_MS}ms)"

      # Capture post-teardown dmesg from both VMs (before they shut
      # down). Useful for diagnosing any drain failures.
      for label in vm1 vm2; do
        if [ "$label" = vm1 ]; then port=$VM1_VIRTIO host=$VM1_PROC; else port=$VM2_VIRTIO host=$VM2_PROC; fi
        vm_run "$port" "$host" "dmesg | tail -30" 10 \
          > "$RUNDIR/diag/$label.dmesg-teardown.txt" 2>/dev/null || true
      done
      echo ""

      # ---------------------------------------------------------------
      # Phase 11b (sanitizer only) — trigger KMEMLEAK scan, then grep
      # the full dmesg on both VMs for any KASAN: / kmemleak: leak
      # reports. Any hit fails the test. Empty == clean.
      # ---------------------------------------------------------------
      SANITIZER_FAILED=0
      if [ "$SANITIZER" = "1" ]; then
        echo "--- Phase 11b: sanitizer (KASAN / KMEMLEAK) ---"
        for label in vm1 vm2; do
          if [ "$label" = vm1 ]; then port=$VM1_VIRTIO host=$VM1_PROC; else port=$VM2_VIRTIO host=$VM2_PROC; fi
          # Force a fresh KMEMLEAK scan. AUTO_SCAN runs every 10 min
          # by default; we don't want to wait that long. Then dump
          # all leaks. Then capture the full dmesg.
          vm_run "$port" "$host" \
            "echo scan > /sys/kernel/debug/kmemleak 2>/dev/null; sleep 1; cat /sys/kernel/debug/kmemleak 2>/dev/null | tail -200; dmesg | grep -E 'KASAN:|kmemleak:|BUG: ' | tail -200" 30 \
            > "$RUNDIR/diag/$label.sanitizer.txt" 2>/dev/null || true
          # Skip the first line (echoed command, which itself
          # contains "KASAN:" / "BUG: " literals) and look for the
          # markers KASAN and kmemleak actually emit at start of line:
          #   ^BUG: KASAN: ...  (KASAN reports)
          #   ^unreferenced object 0x...  (KMEMLEAK leak entry)
          #   ^==========  (KASAN box separator)
          if tail -n +2 "$RUNDIR/diag/$label.sanitizer.txt" 2>/dev/null \
             | grep -qE '^(BUG: KASAN|unreferenced object|==========)'; then
            SANITIZER_FAILED=1
            fail_msg=$(head -50 "$RUNDIR/diag/$label.sanitizer.txt" || true)
            info "$label sanitizer report (head):"
            echo "$fail_msg" | awk '{print "    "$0}'
          else
            info "$label clean (no KASAN/kmemleak reports)"
          fi
        done
        if [ "$SANITIZER_FAILED" -eq 1 ]; then
          info "see $RUNDIR/diag/{vm1,vm2}.sanitizer.txt for full output"
        else
          pass "KASAN + KMEMLEAK clean on both VMs"
        fi
        echo ""
      fi

      # -----------------------------------------------------------------
      # Phase 12 — graceful shutdown (skipped via URP_PAIR_KEEP_VMS=1)
      # -----------------------------------------------------------------
      if [ -n "''${URP_PAIR_KEEP_VMS:-}" ]; then
        info "URP_PAIR_KEEP_VMS set; leaving VMs running for inspection"
        info "vm1 virtio console: nc 127.0.0.1 $VM1_VIRTIO"
        info "vm2 virtio console: nc 127.0.0.1 $VM2_VIRTIO"
        trap - EXIT INT TERM
        exit 0
      fi

      echo "--- Phase 12: shutdown VMs ---"
      P12_START=$(now_ms)
      vm_run "$VM2_VIRTIO" "$VM2_PROC" "poweroff" 5 >/dev/null 2>&1 || true
      vm_run "$VM1_VIRTIO" "$VM1_PROC" "poweroff" 5 >/dev/null 2>&1 || true
      # Wait for process exit (cleanup trap finishes it if too slow)
      waited=0
      while pgrep -f "process=$VM1_PROC" >/dev/null 2>&1 \
         || pgrep -f "process=$VM2_PROC" >/dev/null 2>&1; do
        sleep "$POLL"
        waited=$((waited + POLL))
        [ "$waited" -ge "$T_SHUTDOWN" ] && break
      done
      P12_MS=$(( $(now_ms) - P12_START ))
      pass "shutdown phase (''${P12_MS}ms)"
      echo ""

      # -----------------------------------------------------------------
      # Verdict
      # -----------------------------------------------------------------
      echo "========================================"
      printf "  %-22s %10s\n" "Phase" "Time (ms)"
      echo "  --------------------------------------"
      printf "  %-22s %10d\n" "0: build"          "$P0_MS"
      printf "  %-22s %10d\n" "1: start"          "$P1_MS"
      printf "  %-22s %10d\n" "2: consoles"       "$P2_MS"
      printf "  %-22s %10d\n" "3: login prompt"   "$P3_MS"
      printf "  %-22s %10d\n" "4: pair link"      "$P4_MS"
      printf "  %-22s %10d\n" "5: rxe setup"      "$P5_MS"
      printf "  %-22s %10d\n" "6: insmod urp"     "$P6_MS"
      printf "  %-22s %10d\n" "8: urp add"        "$P8_MS"
      printf "  %-22s %10d\n" "9: cm wait"        "$P9_MS"
      printf "  %-22s %10d\n" "10: echo"          "$P10_MS"
      printf "  %-22s %10d\n" "11: teardown"      "$P11_MS"
      printf "  %-22s %10d\n" "12: shutdown"      "$P12_MS"
      echo "========================================"
      echo ""

      if [ "$RESULT" != "hello-pair" ]; then
        printf "%sFAIL: expected 'hello-pair', got '%s'%s\n" "$RED" "$RESULT" "$NC"
        exit 1
      fi
      if [ "$SANITIZER_FAILED" -eq 1 ]; then
        printf "%sFAIL: KASAN/KMEMLEAK reports detected (see diag/*.sanitizer.txt)%s\n" "$RED" "$NC"
        exit 1
      fi
      printf "%sPASS: URP-to-URP echo round-trip succeeded%s\n" "$GREEN" "$NC"
      exit 0
    '';
  };

  # Convenience helpers (interactive use).
  vm1Serial = mkConnect { vmId = "vm1"; console = "serial"; };
  vm1Virtio = mkConnect { vmId = "vm1"; console = "virtio"; };
  vm2Serial = mkConnect { vmId = "vm2"; console = "serial"; };
  vm2Virtio = mkConnect { vmId = "vm2"; console = "virtio"; };
}
