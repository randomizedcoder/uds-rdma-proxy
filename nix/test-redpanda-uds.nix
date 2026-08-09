{ pkgs, urpKo, urpCli, redpanda, rpk }:

# test-redpanda-uds -- Redpanda Kafka UDS listener over the URP RDMA tunnel.
#
# Milestone: prove a real Kafka client (`rpk`) fetches cluster metadata from a
# real Redpanda broker with the Kafka bytes carried over the URP RDMA data path.
#
# Topology (single host, host netns, RDMA loopback over one rxe device -- the
# kernel module is init_net-only, so netns would not isolate endpoints):
#
#   rpk --UDS--> urp initiator ==RDMA/rxe loopback==> urp acceptor --UDS--> redpanda
#     unix://rpk.sock  --listen-path rpk.sock       --connect-path kafka.sock  kafka_api unix_path
#                      --peer $RXE_IP:4791          --bind $RXE_IP:4791        (+ TCP 9092 for advertisement)
#
# Scope: Redpanda's UDS listener is bootstrap-only -- rpk uses it for the
# initial Metadata fetch; UDS is non-advertisable so produce/consume would ride
# the advertised TCP endpoint (see redpanda src/go/rpk/pkg/kafka/client_franz.go).
# Hence the assertion is `rpk cluster info` (pure Kafka Metadata), which
# round-trips entirely over the tunnel.
#
# Usage: sudo test-redpanda-uds [path-to-urp.ko]

pkgs.writeShellApplication {
  name = "test-redpanda-uds";

  runtimeInputs = with pkgs; [
    iproute2
    rdma-core
    kmod
    coreutils
    gnugrep
    gnused
    procps
    util-linux
    redpanda
    rpk
    urpCli
  ];

  text = ''
    URP_KO="''${1:-${urpKo}/lib/modules/$(uname -r)/urp.ko}"

    WORK="/tmp/urp-rp"
    KAFKA_SOCK="''${WORK}/kafka.sock"     # redpanda's UDS listener (acceptor connects here)
    RPK_SOCK="''${WORK}/rpk.sock"         # urp initiator's UDS listener (rpk connects here)
    CFG="''${WORK}/redpanda.yaml"
    RP_LOG="''${WORK}/redpanda.log"
    TCP_BROKER="127.0.0.1:9092"        # redpanda's own TCP listener (direct, bypasses tunnel)
    # rpk connects to the urp initiator's UDS via a `unix://` seed broker. This
    # requires an rpk built from local source with PR #30240 (rewriteUnixBrokers
    # / UDSDialer in pkg/kafka); the redpanda flake's nix/rpk.nix builds rpk
    # locally for exactly this reason. rpk uses UDS only for the initial Kafka
    # Metadata fetch (bootstrap-only by design), which is precisely what we
    # assert here.
    UDS_BROKER="unix://''${RPK_SOCK}"
    PORT=4791

    ACCEPTOR="rp_acceptor"
    INITIATOR="rp_initiator"
    RXE_DEV="rxe_rp"
    RXE_IP=""
    RP_PID=""

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
        urp remove "''${INITIATOR}" 2>/dev/null || true
        urp remove "''${ACCEPTOR}" 2>/dev/null || true
        if [ -n "''${RP_PID}" ]; then
            kill "''${RP_PID}" 2>/dev/null || true
            for _ in $(seq 1 20); do
                kill -0 "''${RP_PID}" 2>/dev/null || break
                sleep 0.5
            done
            kill -9 "''${RP_PID}" 2>/dev/null || true
        fi
        # Belt and braces: any stray redpanda from this workdir.
        pkill -f "redpanda-cfg ''${CFG}" 2>/dev/null || true
        rmmod urp 2>/dev/null || true
        if [ -n "''${RXE_IP}" ]; then
            rdma link delete "''${RXE_DEV}" 2>/dev/null || true
        fi
        rm -rf "''${WORK}"
        log "Cleanup done"
    }
    trap cleanup EXIT

    # ---- Preflight ----
    if [ "$(id -u)" -ne 0 ]; then
        fail "Must run as root"
        exit 1
    fi
    if [ ! -f "''${URP_KO}" ]; then
        fail "urp.ko not found at ''${URP_KO}"
        fail "Module version may not match running kernel $(uname -r)"
        exit 1
    fi

    rm -rf "''${WORK}"
    mkdir -p "''${WORK}/data"

    # ---- Setup rdma_rxe on the host NIC ----
    log "Setting up rdma_rxe..."
    modprobe ib_core 2>/dev/null || true
    modprobe rdma_cm 2>/dev/null || true
    modprobe rdma_rxe 2>/dev/null || true

    NETDEV=""
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

    rdma link delete "''${RXE_DEV}" 2>/dev/null || true
    rdma link add "''${RXE_DEV}" type rxe netdev "$NETDEV"
    if rdma link show | grep -q "''${RXE_DEV}"; then
        pass "rdma_rxe device ''${RXE_DEV} on $NETDEV ($RXE_IP)"
    else
        err "rdma_rxe device ''${RXE_DEV} not present"
        exit 1
    fi

    # ---- insmod urp.ko ----
    log "Loading urp.ko..."
    if insmod "''${URP_KO}"; then
        pass "insmod succeeded"
    else
        err "insmod failed"
        exit 1
    fi

    # ---- Write redpanda.yaml (TCP + UDS kafka_api) ----
    # A TCP listener is required alongside UDS: UDS is non-advertisable, so the
    # broker must advertise a TCP endpoint for cluster formation / metadata.
    log "Writing redpanda config..."
    cat > "''${CFG}" <<EOF
redpanda:
  data_directory: ''${WORK}/data
  seed_servers: []
  empty_seed_starts_cluster: true
  rpc_server:
    address: 127.0.0.1
    port: 33145
  advertised_rpc_api:
    address: 127.0.0.1
    port: 33145
  kafka_api:
    - name: tcp
      address: 127.0.0.1
      port: 9092
    - name: uds
      unix_path: ''${KAFKA_SOCK}
      unix_socket_mode: 0660
  advertised_kafka_api:
    - address: 127.0.0.1
      port: 9092
  admin:
    - address: 127.0.0.1
      port: 9644
  developer_mode: true
EOF

    # ---- Start Redpanda ----
    # Launch recipe from redpanda tests/rptest/services/redpanda.py (start_redpanda
    # + ResourceSettings.to_cli): direct broker binary, single shard, overprovisioned.
    log "Starting Redpanda broker..."
    redpanda --redpanda-cfg "''${CFG}" \
        --default-log-level info \
        --kernel-page-cache=true \
        --overprovisioned \
        --reserve-memory=0M \
        --smp=1 \
        --memory=1024M \
        --unsafe-bypass-fsync=1 \
        >"''${RP_LOG}" 2>&1 &
    RP_PID=$!

    # ---- Wait for broker readiness (UDS present + Kafka answers over TCP) ----
    log "Waiting for Redpanda to become ready..."
    ready=0
    for _ in $(seq 1 60); do
        if ! kill -0 "''${RP_PID}" 2>/dev/null; then
            err "Redpanda process died during startup"
            tail -30 "''${RP_LOG}" | while IFS= read -r l; do echo "    $l"; done
            exit 1
        fi
        if [ -S "''${KAFKA_SOCK}" ] && rpk cluster info -X brokers="''${TCP_BROKER}" >/dev/null 2>&1; then
            ready=1
            break
        fi
        sleep 1
    done
    if [ "$ready" -eq 1 ]; then
        pass "Redpanda ready (UDS listening + Kafka metadata answering on TCP)"
    else
        err "Redpanda did not become ready in time"
        tail -30 "''${RP_LOG}" | while IFS= read -r l; do echo "    $l"; done
        exit 1
    fi

    # ---- Configure URP endpoints ----
    # Acceptor connects to redpanda's UDS on CM-established, so redpanda (above)
    # must already be listening. Add acceptor first, then initiator.
    log "urp add ''${ACCEPTOR} (acceptor -> redpanda UDS)..."
    if urp add "''${ACCEPTOR}" --connect-path "''${KAFKA_SOCK}" --bind "''${RXE_IP}:''${PORT}"; then
        pass "acceptor endpoint added"
    else
        err "acceptor urp add failed"
        exit 1
    fi

    log "urp add ''${INITIATOR} (initiator UDS -> RDMA -> acceptor)..."
    if urp add "''${INITIATOR}" --listen-path "''${RPK_SOCK}" --peer "''${RXE_IP}:''${PORT}"; then
        pass "initiator endpoint added"
    else
        err "initiator urp add failed"
        exit 1
    fi

    # ---- Wait for CM ESTABLISHED (best-effort dmesg poll) ----
    log "Waiting for RDMA CM ESTABLISHED..."
    cm_ready=0
    for _ in $(seq 1 20); do
        if dmesg | tail -20 | grep -qiE "CM ESTABLISHED|established|qp 0x"; then
            cm_ready=1
            break
        fi
        sleep 1
    done
    if [ "$cm_ready" -eq 1 ]; then
        pass "RDMA CM established"
    else
        warn "no explicit CM ESTABLISHED marker in dmesg -- proceeding (rpk assertion is the real proof)"
    fi
    rdma resource show qp 2>/dev/null | sed -n '1,6p' | while IFS= read -r l; do echo "    $l"; done || true

    # ---- Milestone assertion: rpk metadata over the UDS->RDMA tunnel ----
    log "rpk cluster info via ''${UDS_BROKER} (Kafka metadata over RDMA)..."
    if timeout 30 rpk cluster info -X brokers="''${UDS_BROKER}" >"''${WORK}/rpk-info.txt" 2>&1; then
        if grep -qiE "broker|cluster" "''${WORK}/rpk-info.txt"; then
            pass "rpk cluster info round-tripped over UDS->RDMA"
            sed -n '1,20p' "''${WORK}/rpk-info.txt" | while IFS= read -r l; do echo "    $l"; done
        else
            err "rpk cluster info returned but output lacked broker/cluster info"
            cat "''${WORK}/rpk-info.txt" | while IFS= read -r l; do echo "    $l"; done
        fi
    else
        err "rpk cluster info over UDS->RDMA failed/timed out"
        cat "''${WORK}/rpk-info.txt" | while IFS= read -r l; do echo "    $l"; done
    fi

    # ---- Traffic counters should reflect data movement ----
    log "urp show (post-metadata) -- expect nonzero counters..."
    urp show "''${INITIATOR}" 2>/dev/null | while IFS= read -r l; do echo "    init:  $l"; done || true
    urp show "''${ACCEPTOR}"  2>/dev/null | while IFS= read -r l; do echo "    accept:$l"; done || true

    # ---- Negative attribution check ----
    # Remove the acceptor: the initiator still accepts on rpk.sock, but the RDMA
    # path to redpanda is broken, so metadata cannot return. redpanda's own TCP
    # 9092 is still up -- so a failure here proves the metadata came via the tunnel.
    log "Negative check: remove acceptor, rpk over UDS must now fail..."
    urp remove "''${ACCEPTOR}" 2>/dev/null || true
    if timeout 20 rpk cluster info -X brokers="''${UDS_BROKER}" >/dev/null 2>&1; then
        err "rpk still succeeded over UDS with acceptor removed (metadata not via tunnel?)"
    else
        pass "rpk fails over UDS once tunnel is broken (metadata was via RDMA)"
    fi
    # Sanity: redpanda itself is still alive on TCP.
    if rpk cluster info -X brokers="''${TCP_BROKER}" >/dev/null 2>&1; then
        pass "redpanda still reachable directly on TCP (broker unaffected)"
    else
        warn "redpanda no longer answering on TCP (unexpected, but not the tunnel's fault)"
    fi

    # ---- Summary ----
    echo ""
    echo "========================================"
    echo "  Redpanda UDS-over-RDMA Test Results"
    echo "========================================"
    echo "  Passed: ''${GREEN}''${TEST_PASSED}''${NC}"
    echo "  Failed: ''${RED}''${TEST_FAILED}''${NC}"
    echo "========================================"

    if [ "''${TEST_FAILED}" -gt 0 ]; then
        exit 1
    fi
  '';
}
