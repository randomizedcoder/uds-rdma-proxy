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
  diagDir = "/tmp/urp-microvm-pair${pairLabel}/diag";

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
      # shellcheck disable=SC2034  # used inline by per-step timeout
      T_DRAIN=${if sanitizer then "30" else toString (t.drainRemove * mult)}
      T_SHUTDOWN=${if sanitizer then "60" else toString (t.shutdown * mult)}

      RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[1;33m'; NC=$'\033[0m'
      now_ms() { date +%s%3N; }
      pass() { printf "  %sPASS:%s %s\n" "$GREEN" "$NC" "$1"; }
      fail() { printf "  %sFAIL:%s %s\n" "$RED" "$NC" "$1"; exit 1; }
      info() { printf "  %sINFO:%s %s\n" "$YELLOW" "$NC" "$1"; }

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
      # urp show after echo: traffic counters should be > 0 on the
      # initiator if data left the box.
      for label in vm1 vm2; do
        if [ "$label" = vm1 ]; then port=$VM1_VIRTIO host=$VM1_PROC; else port=$VM2_VIRTIO host=$VM2_PROC; fi
        vm_run "$port" "$host" "urp show" 10 \
          > "$RUNDIR/diag/$label.urp-show-post.txt"
      done
      info "diagnostics written to $RUNDIR/diag/"
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
