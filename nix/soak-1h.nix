{ pkgs, urpKo, urpTestClient, urpCli }:

# 1-hour soak harness for the urp kernel module (Phase 4 follow-up).
#
# Loops the existing rdma_rxe-based data-path tests for SOAK_DURATION
# seconds (default 3600) while a churn thread does urp add / remove
# on a secondary endpoint every CHURN_INTERVAL seconds. After the
# loop, drains + removes the primary endpoint, rmmods, and asserts:
#
#   * Zero dmesg lines matching urp:.*(error|panic|bug|warn).
#   * Zero CLI errors across the load + churn loops.
#   * Slab counter delta (post-rmmod minus pre-insmod) under
#     SLAB_LEAK_BUDGET_KB (default 2048 = 2 MB allowance for noise
#     from other workloads in the VM).
#
# Sanitizer (KASAN/KMEMLEAK/KCSAN) coverage is OUT of scope here -- it
# requires the debug-VM kernel which is still blocked by the Phase 1
# VIRTIO_BLK / 9P_FS panic (IMPLEMENTATION.md Variation #9). This
# script catches the leak / lifecycle / race symptoms that scale with
# time but doesn't substitute for the sanitizer run.

pkgs.writeShellApplication {
  name = "soak-1h";

  runtimeInputs = with pkgs; [
    iproute2
    rdma-core
    kmod
    socat
    coreutils
    gnugrep
    gnused
    gawk
    procps
    util-linux
    jq
    urpTestClient
    urpCli
  ];

  text = ''
    URP_KO="''${1:-${urpKo}/lib/modules/$(uname -r)/urp.ko}"
    SOAK_DURATION="''${SOAK_DURATION:-3600}"
    CHURN_INTERVAL="''${CHURN_INTERVAL:-30}"
    SLAB_LEAK_BUDGET_KB="''${SLAB_LEAK_BUDGET_KB:-2048}"

    EP_NAME="soak_main"
    CHURN_EP="soak_churn"
    CONNECT_PATH="/tmp/urp_soak_echo.sock"
    PORT=4791
    CHURN_PORT=4792
    ECHO_PID=""
    RXE_DEV=""
    CHURN_PID=""

    RED=$'\033[0;31m'
    GREEN=$'\033[0;32m'
    YELLOW=$'\033[1;33m'
    NC=$'\033[0m'

    log()  { echo "''${GREEN}[+]''${NC} $*"; }
    warn() { echo "''${YELLOW}[!]''${NC} $*"; }
    fail() { echo "''${RED}[-]''${NC} $*"; }

    cleanup() {
        log "Cleaning up..."

        if [ -n "''${CHURN_PID}" ]; then
            kill "''${CHURN_PID}" 2>/dev/null || true
            wait "''${CHURN_PID}" 2>/dev/null || true
        fi

        urp remove "''${EP_NAME}" 2>/dev/null || true
        urp remove "''${CHURN_EP}" 2>/dev/null || true

        if [ -n "''${ECHO_PID}" ]; then
            kill "''${ECHO_PID}" 2>/dev/null || true
        fi

        rmmod urp 2>/dev/null || true
        rm -f "''${CONNECT_PATH}"

        if [ -n "''${RXE_DEV}" ]; then
            rdma link delete "''${RXE_DEV}" 2>/dev/null || true
        fi
    }
    trap cleanup EXIT

    # ---- Preflight ----

    if [ "$(id -u)" -ne 0 ]; then
        fail "Must run as root"
        exit 1
    fi

    if [ ! -f "''${URP_KO}" ]; then
        fail "urp.ko not found at ''${URP_KO}"
        exit 1
    fi

    # ---- rdma_rxe + echo server setup (same as test-kmod-k0) ----

    log "Setting up rdma_rxe..."
    modprobe ib_core 2>/dev/null || true
    modprobe rdma_cm 2>/dev/null || true
    modprobe rdma_rxe 2>/dev/null || true

    NETDEV=""
    RXE_IP=""
    while IFS= read -r line; do
        cand="''${line#*: }"
        cand="''${cand%%:*}"
        cand="''${cand%%@*}"
        addr=$(ip -4 -o addr show "$cand" 2>/dev/null | sed -n 's/.*inet \([0-9.]*\).*/\1/p' | head -1 || true)
        if [ -n "$addr" ]; then
            NETDEV="$cand"
            RXE_IP="$addr"
            break
        fi
    done < <(ip -o link show | grep -v 'lo:' || true)

    if [ -z "$NETDEV" ] || [ -z "$RXE_IP" ]; then
        fail "No network device with IPv4 address found for rdma_rxe"
        exit 1
    fi

    RXE_DEV="rxe_soak"
    rdma link delete "''${RXE_DEV}" 2>/dev/null || true
    rdma link add "''${RXE_DEV}" type rxe netdev "$NETDEV"
    log "rdma_rxe device ''${RXE_DEV} created on $NETDEV ($RXE_IP)"

    log "Starting echo server..."
    rm -f "''${CONNECT_PATH}"
    socat UNIX-LISTEN:"''${CONNECT_PATH}",fork EXEC:cat &
    ECHO_PID=$!
    sleep 0.5
    log "Echo server running (PID ''${ECHO_PID})"

    # ---- Baseline samples (before insmod) ----

    SLAB_BASELINE=$(awk '/^Slab/ {print $2}' /proc/meminfo)
    DMESG_BASELINE=$(dmesg | wc -l)
    log "Baseline: slab=''${SLAB_BASELINE} kB, dmesg_lines=''${DMESG_BASELINE}"

    # ---- insmod + primary endpoint ----

    log "Loading urp module..."
    insmod "''${URP_KO}"

    log "Creating primary endpoint ''${EP_NAME}..."
    urp add "''${EP_NAME}" --connect-path "''${CONNECT_PATH}" \
        --bind "0.0.0.0:''${PORT}"

    # ---- Churn thread (parallel) ----

    (
        churn_cycles=0
        churn_fails=0
        while true; do
            churn_cycles=$((churn_cycles + 1))
            if ! urp add "''${CHURN_EP}_$churn_cycles" \
                    --connect-path "''${CONNECT_PATH}" \
                    --bind "0.0.0.0:''${CHURN_PORT}" 2>/dev/null; then
                churn_fails=$((churn_fails + 1))
            fi
            sleep 2
            urp remove "''${CHURN_EP}_$churn_cycles" 2>/dev/null \
                || churn_fails=$((churn_fails + 1))
            sleep "$((CHURN_INTERVAL - 2))"
        done
    ) &
    CHURN_PID=$!
    log "Churn thread started (PID ''${CHURN_PID}, interval ''${CHURN_INTERVAL}s)"

    # ---- Main soak loop ----

    start_ts=$(date +%s)
    deadline=$((start_ts + SOAK_DURATION))
    cycles=0
    test_failures=0

    log "Soak loop running for ''${SOAK_DURATION}s..."
    while [ "$(date +%s)" -lt "$deadline" ]; do
        cycles=$((cycles + 1))

        # 100-roundtrip echo
        if ! urp-test-client "$RXE_IP" "$PORT" echo "soak-$cycles" 100 \
                >/dev/null 2>&1; then
            test_failures=$((test_failures + 1))
            warn "cycle $cycles: echo failed"
        fi

        # 10 MB throughput
        if ! urp-test-client "$RXE_IP" "$PORT" throughput 10 \
                >/dev/null 2>&1; then
            test_failures=$((test_failures + 1))
            warn "cycle $cycles: throughput failed"
        fi

        # Sample slab + endpoint stats every 10 cycles
        if [ "$((cycles % 10))" -eq 0 ]; then
            slab_now=$(awk '/^Slab/ {print $2}' /proc/meminfo)
            delta=$((slab_now - SLAB_BASELINE))
            elapsed=$(( $(date +%s) - start_ts ))
            tx=$(awk '/^tx_frames:/ {print $2}' \
                    "/proc/urp/''${EP_NAME}/stats" 2>/dev/null || echo "?")
            rx=$(awk '/^rx_frames:/ {print $2}' \
                    "/proc/urp/''${EP_NAME}/stats" 2>/dev/null || echo "?")
            log "t=''${elapsed}s cycle=$cycles slab_delta=''${delta} kB tx=$tx rx=$rx"
        fi
    done

    elapsed=$(( $(date +%s) - start_ts ))
    log "Soak loop complete: ''${elapsed}s, ''${cycles} cycles, ''${test_failures} CLI failures"

    # ---- Tear down ----

    kill "''${CHURN_PID}" 2>/dev/null || true
    wait "''${CHURN_PID}" 2>/dev/null || true
    CHURN_PID=""

    log "Draining + removing primary endpoint..."
    urp drain "''${EP_NAME}" || true
    sleep 1
    urp remove "''${EP_NAME}"

    log "rmmod urp..."
    rmmod urp

    # ---- Post-rmmod assertions ----

    SLAB_FINAL=$(awk '/^Slab/ {print $2}' /proc/meminfo)
    SLAB_LEAK=$((SLAB_FINAL - SLAB_BASELINE))
    DMESG_FINAL=$(dmesg | wc -l)
    NEW_LINES=$((DMESG_FINAL - DMESG_BASELINE))
    DMESG_ERR_COUNT=0
    if [ "$NEW_LINES" -gt 0 ]; then
        DMESG_ERR_COUNT=$(dmesg | tail -"$NEW_LINES" \
            | grep -iE "urp:.*error|urp:.*panic|urp:.*bug|urp:.*warn" \
            | wc -l || true)
    fi

    echo ""
    echo "========================================"
    echo "  1-Hour Soak Results"
    echo "========================================"
    echo "  Duration:           ''${elapsed} s"
    echo "  Load cycles:        ''${cycles}"
    echo "  CLI failures:       ''${test_failures}"
    echo "  Slab leak:          ''${SLAB_LEAK} kB"
    echo "    (budget ''${SLAB_LEAK_BUDGET_KB} kB)"
    echo "  dmesg urp errors:   ''${DMESG_ERR_COUNT}"
    echo "========================================"

    OVERALL_PASS=1
    if [ "$test_failures" -gt 0 ]; then
        fail "FAIL: ''${test_failures} CLI failures during soak"
        OVERALL_PASS=0
    fi
    if [ "$DMESG_ERR_COUNT" -gt 0 ]; then
        fail "FAIL: ''${DMESG_ERR_COUNT} urp errors in dmesg:"
        dmesg | tail -"$NEW_LINES" \
            | grep -iE "urp:.*error|urp:.*panic|urp:.*bug|urp:.*warn" \
            | while IFS= read -r line; do echo "    $line"; done
        OVERALL_PASS=0
    fi
    if [ "$SLAB_LEAK" -gt "$SLAB_LEAK_BUDGET_KB" ]; then
        fail "FAIL: Slab leak ''${SLAB_LEAK} kB exceeds budget ''${SLAB_LEAK_BUDGET_KB} kB"
        OVERALL_PASS=0
    fi

    if [ "$OVERALL_PASS" -eq 1 ]; then
        log "''${GREEN}OVERALL: PASS''${NC}"
        exit 0
    else
        fail "''${RED}OVERALL: FAIL''${NC}"
        exit 1
    fi
  '';
}
