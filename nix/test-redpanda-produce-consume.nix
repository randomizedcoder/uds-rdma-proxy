{ pkgs, urpKo, urpCli, redpanda, rpk }:

# test-redpanda-produce-consume -- FULL Kafka data plane (produce + consume)
# over the URP RDMA tunnel, not just the metadata bootstrap.
#
# The trick: rpk/franz use a UDS seed only for the initial Metadata fetch;
# produce/consume then follow the *advertised* address (UDS is non-advertisable).
# So we make the broker advertise a client-local bridge address that funnels
# back into the tunnel, so ALL Kafka traffic rides RDMA:
#
#   rpk --TCP 127.0.0.2:9092--> socat --UDS--> urp initiator ==RDMA==> urp acceptor --UDS--> redpanda
#     seed + all data              (rpk.sock)                             (kafka.sock)   uds kafka_api
#
# The broker's `tcp` listener binds 127.0.0.1:9092 but is advertised as
# 127.0.0.2:9092 (a distinct loopback alias where socat listens). rpk seeds
# 127.0.0.2:9092; every connection it opens (metadata, CreateTopics to the
# controller, Produce/Fetch to the leader) resolves to the bridge -> tunnel ->
# the broker's UDS listener. Single host, soft-RoCE, RDMA loopback over one dev.
#
# Usage: sudo test-redpanda-produce-consume [path-to-urp.ko]

pkgs.writeShellApplication {
  name = "test-redpanda-produce-consume";

  runtimeInputs = with pkgs; [
    iproute2
    rdma-core
    kmod
    coreutils
    gnugrep
    gnused
    procps
    util-linux
    socat
    redpanda
    rpk
    urpCli
  ];

  text = ''
    URP_KO="''${1:-${urpKo}/lib/modules/$(uname -r)/urp.ko}"

    WORK="/tmp/urp-rp-pc"
    KAFKA_SOCK="''${WORK}/kafka.sock"     # redpanda's UDS listener (acceptor connects here)
    RPK_SOCK="''${WORK}/rpk.sock"         # urp initiator's UDS listener (socat connects here)
    CFG="''${WORK}/redpanda.yaml"
    RP_LOG="''${WORK}/redpanda.log"

    TCP_BROKER="127.0.0.1:9092"           # broker's real tcp listener (readiness probe only)
    BRIDGE_IP="127.0.0.2"                 # advertised addr == client-side bridge (loopback alias)
    BRIDGE_PORT=9092
    BRIDGE="127.0.0.2:9092"               # rpk seed: everything flows through here -> tunnel
    PORT=4791

    TOPIC="urp-pc-test"
    PAYLOAD="hello-over-rdma-0123456789"

    ACCEPTOR="rp_acceptor"
    INITIATOR="rp_initiator"
    RXE_DEV="rxe_rp"
    RXE_IP=""
    RP_PID=""
    SHIM_PID=""

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

    # Dump everything useful when a data-plane step fails: the broker's own
    # view (why it closed the connection), the kernel's view, and the urp
    # per-endpoint stream/counter state.
    dump_diag() {
        warn "---- diagnostics ($1) ----"
        echo "  -- redpanda log (tail) --"
        tail -25 "''${RP_LOG}" 2>/dev/null | while IFS= read -r l; do echo "    rp: $l"; done || true
        echo "  -- dmesg urp (tail) --"
        dmesg 2>/dev/null | grep -iE "urp[: ]" | tail -25 | while IFS= read -r l; do echo "    dmesg: $l"; done || true
        echo "  -- urp show initiator --"
        urp show "''${INITIATOR}" 2>/dev/null | while IFS= read -r l; do echo "    init:  $l"; done || true
        echo "  -- urp show acceptor --"
        urp show "''${ACCEPTOR}" 2>/dev/null | while IFS= read -r l; do echo "    accept:$l"; done || true
        warn "---- end diagnostics ----"
    }

    cleanup() {
        log "Cleaning up..."
        if [ -n "''${SHIM_PID}" ]; then
            kill "''${SHIM_PID}" 2>/dev/null || true
        fi
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
        pkill -f "redpanda-cfg ''${CFG}" 2>/dev/null || true
        rmmod urp 2>/dev/null || true
        if [ -n "''${RXE_IP}" ]; then
            rdma link delete "''${RXE_DEV}" 2>/dev/null || true
        fi
        # Preserve logs on failure for post-mortem; only clean on success.
        if [ "''${TEST_FAILED}" -eq 0 ]; then
            rm -rf "''${WORK}"
        else
            warn "TEST_FAILED -- preserving ''${WORK} (redpanda.log, configs) for inspection"
        fi
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
        exit 1
    fi

    rm -rf "''${WORK}"
    mkdir -p "''${WORK}/data"

    # ---- rdma_rxe on the host NIC ----
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
            NETDEV="$cand"; RXE_IP="$addr"; break
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
        err "rdma_rxe device ''${RXE_DEV} not present"; exit 1
    fi

    # ---- insmod urp.ko ----
    log "Loading urp.ko..."
    if insmod "''${URP_KO}"; then pass "insmod succeeded"; else err "insmod failed"; exit 1; fi

    # ---- redpanda config: tcp (advertised as the bridge) + uds ----
    # The tcp listener binds 127.0.0.1:9092 but is advertised as 127.0.0.2:9092
    # (where socat listens), so clients dial the bridge for the data plane.
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
    - name: tcp
      address: ''${BRIDGE_IP}
      port: ''${BRIDGE_PORT}
  admin:
    - address: 127.0.0.1
      port: 9644
  developer_mode: true
EOF

    # ---- Start Redpanda ----
    log "Starting Redpanda broker..."
    redpanda --redpanda-cfg "''${CFG}" \
        --default-log-level info \
        --kernel-page-cache=true --overprovisioned --reserve-memory=0M \
        --smp=1 --memory=1024M --unsafe-bypass-fsync=1 \
        >"''${RP_LOG}" 2>&1 &
    RP_PID=$!

    # ---- Wait for readiness (probe the real tcp listener directly) ----
    log "Waiting for Redpanda to become ready..."
    ready=0
    for _ in $(seq 1 60); do
        if ! kill -0 "''${RP_PID}" 2>/dev/null; then
            err "Redpanda died during startup"
            tail -30 "''${RP_LOG}" | while IFS= read -r l; do echo "    $l"; done
            exit 1
        fi
        if [ -S "''${KAFKA_SOCK}" ] && rpk cluster info -X brokers="''${TCP_BROKER}" >/dev/null 2>&1; then
            ready=1; break
        fi
        sleep 1
    done
    if [ "$ready" -eq 1 ]; then
        pass "Redpanda ready (UDS listening + advertising ''${BRIDGE})"
    else
        err "Redpanda did not become ready in time"
        tail -30 "''${RP_LOG}" | while IFS= read -r l; do echo "    $l"; done
        exit 1
    fi

    # ---- Configure URP endpoints ----
    log "urp add ''${ACCEPTOR} (acceptor -> redpanda UDS)..."
    if urp add "''${ACCEPTOR}" --connect-path "''${KAFKA_SOCK}" --bind "''${RXE_IP}:''${PORT}"; then
        pass "acceptor endpoint added"
    else err "acceptor urp add failed"; exit 1; fi

    log "urp add ''${INITIATOR} (initiator UDS -> RDMA -> acceptor)..."
    if urp add "''${INITIATOR}" --listen-path "''${RPK_SOCK}" --peer "''${RXE_IP}:''${PORT}"; then
        pass "initiator endpoint added"
    else err "initiator urp add failed"; exit 1; fi

    # ---- Wait for CM ESTABLISHED ----
    log "Waiting for RDMA CM ESTABLISHED..."
    cm_ready=0
    for _ in $(seq 1 20); do
        if dmesg | tail -20 | grep -qiE "CM ESTABLISHED|established|qp 0x"; then cm_ready=1; break; fi
        sleep 1
    done
    if [ "$cm_ready" -eq 1 ]; then pass "RDMA CM established"; else
        warn "no explicit CM marker in dmesg -- proceeding (produce/consume is the real proof)"; fi

    # ---- Client-side bridge: advertised TCP -> urp initiator UDS ----
    log "Starting socat ''${BRIDGE} -> UNIX:''${RPK_SOCK} bridge..."
    socat "TCP-LISTEN:''${BRIDGE_PORT},reuseaddr,fork,bind=''${BRIDGE_IP}" "UNIX-CONNECT:''${RPK_SOCK}" &
    SHIM_PID=$!
    sleep 0.5
    if ! kill -0 "''${SHIM_PID}" 2>/dev/null; then err "socat bridge failed to start"; exit 1; fi
    pass "socat bridge running (PID ''${SHIM_PID})"

    # ---- Isolation probe: single-connection metadata over the bridge ----
    # cluster info uses ONE connection (ApiVersions + Metadata). If this passes
    # but topic create (which opens a 2nd connection to the controller) fails,
    # the fault is in urp's concurrent-connection / multi-stream path.
    log "probe: rpk cluster info via ''${BRIDGE} (single connection)..."
    if timeout 30 rpk cluster info -X brokers="''${BRIDGE}" >"''${WORK}/info.txt" 2>&1; then
        pass "single-connection metadata over the bridge works"
        sed -n '1,8p' "''${WORK}/info.txt" | while IFS= read -r l; do echo "    $l"; done
    else
        err "single-connection metadata over the bridge failed"
        cat "''${WORK}/info.txt" | while IFS= read -r l; do echo "    $l"; done
        dump_diag "cluster-info probe"
        exit 1
    fi

    # ---- Create topic over the tunnel ----
    log "rpk topic create ''${TOPIC} via ''${BRIDGE} (over RDMA)..."
    if timeout 30 rpk topic create "''${TOPIC}" -p 1 -r 1 -X brokers="''${BRIDGE}" >"''${WORK}/create.txt" 2>&1; then
        pass "topic created over the tunnel"
    else
        err "topic create over the tunnel failed"
        cat "''${WORK}/create.txt" | while IFS= read -r l; do echo "    $l"; done
        dump_diag "topic create"
        exit 1
    fi

    # ---- Produce a known payload over the tunnel ----
    log "rpk topic produce (payload='$PAYLOAD') via ''${BRIDGE} (over RDMA)..."
    if printf '%s\n' "''${PAYLOAD}" | timeout 30 rpk topic produce "''${TOPIC}" -X brokers="''${BRIDGE}" >"''${WORK}/produce.txt" 2>&1; then
        pass "produce over the tunnel returned ok"
        sed -n '1,3p' "''${WORK}/produce.txt" | while IFS= read -r l; do echo "    $l"; done
    else
        err "produce over the tunnel failed"
        cat "''${WORK}/produce.txt" | while IFS= read -r l; do echo "    $l"; done
        dump_diag "produce"
    fi

    # ---- Consume it back over the tunnel and verify byte-for-byte ----
    log "rpk topic consume ''${TOPIC} via ''${BRIDGE} (over RDMA)..."
    GOT=$(timeout 30 rpk topic consume "''${TOPIC}" -o start -n 1 -f '%v' -X brokers="''${BRIDGE}" 2>/dev/null | tr -d '\r\n')
    echo "    consumed: '$GOT'"
    if [ "''${GOT}" = "''${PAYLOAD}" ]; then
        pass "produce/consume round-trip over RDMA (payload matches)"
    else
        err "consumed payload does not match (got '$GOT', want '$PAYLOAD')"
        dump_diag "consume"
    fi

    # ---- Counters: the data plane moved real bytes over the tunnel ----
    log "urp show -- data-plane counters (expect > the ~470B metadata-only case)..."
    urp show "''${INITIATOR}" 2>/dev/null | sed -n '1,20p' | while IFS= read -r l; do echo "    init:  $l"; done || true
    urp show "''${ACCEPTOR}"  2>/dev/null | sed -n '1,20p' | while IFS= read -r l; do echo "    accept:$l"; done || true

    # ---- Summary ----
    echo ""
    echo "========================================"
    echo "  Redpanda Produce/Consume over RDMA"
    echo "========================================"
    echo "  Passed: ''${GREEN}''${TEST_PASSED}''${NC}"
    echo "  Failed: ''${RED}''${TEST_FAILED}''${NC}"
    echo "========================================"

    if [ "''${TEST_FAILED}" -gt 0 ]; then exit 1; fi
  '';
}
