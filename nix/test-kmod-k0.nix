{ pkgs, urpKo, urpTestClient }:

pkgs.writeShellApplication {
  name = "test-kmod-k0";

  runtimeInputs = with pkgs; [
    iproute2
    rdma-core
    kmod
    socat
    coreutils
    gnugrep
    gnused
    procps
    util-linux
    urpTestClient
  ];

  text = ''
    # test-kmod-k0 — Phase k0 kernel module integration test
    #
    # Single-module test using rdma_rxe loopback:
    #   1. Create rxe device on a veth pair
    #   2. Start echo server on UDS socket
    #   3. Load urp.ko as acceptor (RDMA listen, UDS connect to echo)
    #   4. Use urp-test-client to send data via RDMA → module → echo → back
    #
    # Usage: sudo test-kmod-k0 [path-to-urp.ko]

    URP_KO="''${1:-${urpKo}/lib/modules/$(uname -r)/urp.ko}"
    CONNECT_PATH="/tmp/urp_test_echo.sock"
    PORT=4791
    ECHO_PID=""
    RXE_DEV=""
    TEST_PASSED=0
    TEST_FAILED=0

    RED=$'\033[0;31m'
    GREEN=$'\033[0;32m'
    YELLOW=$'\033[1;33m'
    NC=$'\033[0m'

    log()  { echo "''${GREEN}[+]''${NC} $*"; }
    warn() { echo "''${YELLOW}[!]''${NC} $*"; }
    fail() { echo "''${RED}[-]''${NC} $*"; }
    pass() { TEST_PASSED=$((TEST_PASSED + 1)); log "PASS: $*"; }
    err()  { TEST_FAILED=$((TEST_FAILED + 1)); fail "FAIL: $*"; }

    cleanup() {
        log "Cleaning up..."

        # Stop echo server
        if [ -n "''${ECHO_PID}" ]; then
            kill "''${ECHO_PID}" 2>/dev/null || true
        fi

        # Unload module
        rmmod urp 2>/dev/null || true

        # Remove sockets
        rm -f "''${CONNECT_PATH}"

        # Remove RXE device
        if [ -n "''${RXE_DEV}" ]; then
            rdma link delete "''${RXE_DEV}" 2>/dev/null || true
        fi

        log "Cleanup done"
    }

    trap cleanup EXIT

    # ---- Preflight checks ----

    if [ "$(id -u)" -ne 0 ]; then
        fail "Must run as root"
        exit 1
    fi

    if [ ! -f "''${URP_KO}" ]; then
        fail "urp.ko not found at ''${URP_KO}"
        fail "Module version may not match running kernel $(uname -r)"
        exit 1
    fi

    # ---- Setup rdma_rxe ----

    log "Setting up rdma_rxe..."

    # Load RDMA stack
    modprobe ib_core 2>/dev/null || true
    modprobe rdma_cm 2>/dev/null || true
    modprobe rdma_rxe 2>/dev/null || true

    # Find a suitable network device for rxe (prefer eth0, fall back to first non-lo)
    NETDEV=$(ip -o link show | grep -v lo: | head -1 | sed 's/^[0-9]*: \([^:@]*\).*/\1/')
    if [ -z "$NETDEV" ]; then
        fail "No network device found for rdma_rxe"
        exit 1
    fi

    RXE_DEV="rxe_test"
    rdma link delete "''${RXE_DEV}" 2>/dev/null || true
    rdma link add "''${RXE_DEV}" type rxe netdev "$NETDEV"

    # Get the IP of the rdma_rxe device's underlying interface (not 127.0.0.1 — loopback has no rxe)
    RXE_IP=$(ip -4 addr show "$NETDEV" | grep 'inet ' | sed 's/.*inet \([0-9.]*\).*/\1/' | head -1)
    if [ -z "$RXE_IP" ]; then
        fail "No IPv4 address on $NETDEV"
        exit 1
    fi
    log "rdma_rxe device ''${RXE_DEV} created on $NETDEV ($RXE_IP)"

    # ---- Start echo server ----

    log "Starting echo server..."
    rm -f "''${CONNECT_PATH}"
    socat UNIX-LISTEN:"''${CONNECT_PATH}",fork EXEC:cat &
    ECHO_PID=$!
    sleep 0.5

    if ! kill -0 "''${ECHO_PID}" 2>/dev/null; then
        fail "Echo server failed to start"
        exit 1
    fi

    log "Echo server running (PID ''${ECHO_PID})"

    # ---- Test 1: insmod ----

    log "Test 1: insmod..."
    if insmod "''${URP_KO}" connect_path="''${CONNECT_PATH}" bind_port="''${PORT}"; then
        pass "insmod succeeded"
    else
        err "insmod failed"
        exit 1
    fi

    # ---- KUnit tests (if CONFIG_KUNIT=y) ----

    if dmesg | grep -q "KTAP version"; then
        log "KUnit tests detected, checking results..."
        KUNIT_FAIL=$(dmesg | grep -c "not ok.*urp" || true)
        KUNIT_PASS=$(dmesg | grep -c "ok.*urp" || true)
        if [ "$KUNIT_FAIL" -gt 0 ]; then
            err "KUnit: $KUNIT_FAIL test(s) failed"
            dmesg | grep "not ok.*urp" | while IFS= read -r line; do echo "    $line"; done
        elif [ "$KUNIT_PASS" -gt 0 ]; then
            pass "KUnit: $KUNIT_PASS test(s) passed"
        else
            log "KUnit: no urp tests found (CONFIG_KUNIT may be off)"
        fi
    else
        log "KUnit not available (run with urp-vm-debug for KUnit tests)"
    fi

    # ---- Test 2: /proc/urp/stats ----

    log "Test 2: /proc/urp/stats..."
    if [ -f /proc/urp/stats ]; then
        STATS=$(cat /proc/urp/stats)
        if echo "$STATS" | grep -q "tx_frames:"; then
            pass "/proc/urp/stats readable"
        else
            err "/proc/urp/stats format unexpected"
        fi
    else
        err "/proc/urp/stats not found"
    fi

    # ---- Wait for RDMA + UDS connection ----

    log "Waiting for RDMA connection and UDS setup..."
    CONNECTED=0
    for _ in $(seq 1 30); do
        if grep -q "connected: yes" /proc/urp/stats 2>/dev/null; then
            CONNECTED=1
            break
        fi
        sleep 1
    done

    if [ "$CONNECTED" -eq 1 ]; then
        pass "RDMA connection established"
    else
        warn "RDMA connection not established (no peer to connect — expected for acceptor-only)"
        log "Acceptor is listening on RDMA port ''${PORT}, waiting for a client..."
    fi

    # ---- Test 3: Echo via RDMA test client ----

    log "Test 3: Basic echo via RDMA..."
    if urp-test-client "$RXE_IP" "''${PORT}" echo "hello RDMA kernel" 1 2>/dev/null; then
        pass "Basic RDMA echo"
    else
        # Expected: the test client connects via RDMA, the module accepts,
        # connects to echo server, forwards data. This exercises the full path.
        warn "RDMA echo test failed (may need rdma_rxe loopback connectivity)"
        err "Basic RDMA echo"
    fi

    # ---- Test 4: 1000 echo roundtrips ----

    if [ "$TEST_FAILED" -eq 0 ]; then
        log "Test 4: 1000 echo roundtrips..."
        if urp-test-client "$RXE_IP" "''${PORT}" echo "roundtrip-test" 1000 2>/dev/null; then
            pass "1000 echo roundtrips"
        else
            err "Echo roundtrips failed"
        fi
    fi

    # ---- Test 5: Throughput (100 MB) ----

    if [ "$TEST_FAILED" -eq 0 ]; then
        log "Test 5: Throughput test (100 MB)..."
        if urp-test-client "$RXE_IP" "''${PORT}" throughput 100 2>&1 | tee /tmp/urp_throughput.txt; then
            pass "Throughput test (100 MB)"
            grep "Throughput:" /tmp/urp_throughput.txt || true
        else
            err "Throughput test failed"
        fi
    fi

    # ---- Test 6: Latency (1000 roundtrips) ----

    if [ "$TEST_FAILED" -eq 0 ]; then
        log "Test 6: Latency test (1000 x 64B roundtrips)..."
        if urp-test-client "$RXE_IP" "''${PORT}" latency 1000 2>&1 | tee /tmp/urp_latency.txt; then
            pass "Latency test"
            grep "RTT" /tmp/urp_latency.txt || true
        else
            err "Latency test failed"
        fi
    fi

    # ---- Test 7: Stats verification ----

    log "Test 7: Stats after data transfer..."
    if [ -f /proc/urp/stats ]; then
        log "Stats:"
        while IFS= read -r line; do echo "    $line"; done < /proc/urp/stats
    fi

    # ---- Test 8: rmmod ----

    log "Test 8: rmmod..."
    if rmmod urp; then
        pass "rmmod succeeded"
    else
        err "rmmod failed"
    fi

    # ---- KASAN check ----
    log "Checking for KASAN errors..."
    if grep -q "CONFIG_KASAN=y" /boot/config-"$(uname -r)" 2>/dev/null || \
       zgrep -q "CONFIG_KASAN=y" /proc/config.gz 2>/dev/null; then
        if dmesg | grep -i "BUG: KASAN" >/dev/null 2>&1; then
            err "KASAN errors detected"
            dmesg | grep -A5 "BUG: KASAN" | while IFS= read -r line; do echo "    $line"; done
        else
            pass "KASAN clean"
        fi
    else
        log "KASAN not enabled in kernel (run with urp-vm-debug for sanitizer testing)"
    fi

    # ---- KMEMLEAK check ----
    log "Checking for memory leaks..."
    if [ -f /sys/kernel/debug/kmemleak ]; then
        # Trigger a scan after rmmod
        echo scan > /sys/kernel/debug/kmemleak 2>/dev/null || true
        sleep 1
        LEAKS=$(cat /sys/kernel/debug/kmemleak 2>/dev/null || echo "")
        if [ -n "$LEAKS" ] && echo "$LEAKS" | grep -q "urp"; then
            err "KMEMLEAK: urp-related leaks detected"
            echo "$LEAKS" | grep -A5 "urp" | while IFS= read -r line; do echo "    $line"; done
        else
            pass "KMEMLEAK clean (no urp leaks)"
        fi
    else
        log "KMEMLEAK not enabled in kernel (run with urp-vm-debug for sanitizer testing)"
    fi

    # ---- Check dmesg for errors ----
    log "Checking dmesg for urp errors..."
    if dmesg | tail -100 | grep -i "urp:.*error\|urp:.*panic\|urp:.*bug\|urp:.*warn" >/dev/null 2>&1; then
        warn "Kernel messages detected in dmesg:"
        dmesg | tail -100 | grep -i "urp:" | while IFS= read -r line; do echo "    $line"; done
    else
        pass "No kernel errors in dmesg"
    fi

    # ---- Summary ----
    echo ""
    echo "========================================"
    echo "  k0 Test Results"
    echo "========================================"
    echo "  Passed: ''${GREEN}''${TEST_PASSED}''${NC}"
    echo "  Failed: ''${RED}''${TEST_FAILED}''${NC}"
    echo "========================================"

    if [ "''${TEST_FAILED}" -gt 0 ]; then
        exit 1
    fi
  '';
}
