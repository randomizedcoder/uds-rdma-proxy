# Project Structure

> **Status: historical (userspace-proxy era, 2026-05).** This document
> describes the original *userspace Rust proxy* design, which was superseded:
> the project was implemented as a **Linux kernel module** instead — see
> [DESIGN.md](../DESIGN.md) and [21-kernel-module.md](21-kernel-module.md).
> Retained for design rationale and history. Details below (crates, io_uring,
> tokio, TOML config, Prometheus, the v0–v4 roadmap) do not match the
> implementation.

```
uds-rdma-proxy/
├── Cargo.toml                    # Workspace root
├── docs/
│   └── DESIGN.md                 # This document
├── crates/
│   ├── uds-rdma-proxy/           # Main proxy binary
│   │   ├── Cargo.toml
│   │   └── src/
│   │       ├── main.rs           # Entry point, CLI parsing, startup
│   │       ├── config.rs         # Configuration (TOML + CLI)
│   │       ├── proxy.rs          # Top-level orchestration
│   │       ├── uds.rs            # UDS listener/connector, io_uring
│   │       ├── pump.rs           # Bidirectional data pump
│   │       ├── metrics.rs        # Prometheus metrics definitions
│   │       ├── transport/
│   │       │   ├── mod.rs        # Transport trait
│   │       │   ├── tcp.rs        # v0: TCP transport
│   │       │   ├── rsocket.rs    # v1: rsocket transport
│   │       │   ├── rdma.rs       # v2+: native ibverbs transport
│   │       │   └── frame.rs      # Frame encoder/decoder
│   │       └── rdma/
│   │           ├── mod.rs        # RDMA subsystem
│   │           ├── connection.rs # rdma_cm connection management
│   │           ├── qp.rs         # QP setup and configuration
│   │           ├── buffer_pool.rs # Registered buffer pool
│   │           ├── flow_control.rs # Credit-based flow control
│   │           ├── cq.rs         # CQ polling (busy/event/adaptive)
│   │           └── reorder.rs    # Multi-QP reorder buffer (BTreeMap)
│   ├── uds-rdma-bench/           # Load generator binary
│   │   ├── Cargo.toml
│   │   └── src/
│   │       ├── main.rs           # Entry point, mode selection
│   │       ├── producer.rs       # Write data to UDS
│   │       ├── consumer.rs       # Read data from UDS
│   │       └── echo.rs           # Echo server for latency testing
│   └── urp-cli/                  # Kernel module CLI tool (Section 23)
│       ├── Cargo.toml
│       └── src/
│           ├── main.rs           # clap CLI entry point
│           ├── netlink.rs        # GENL socket, message build/parse (neli)
│           ├── commands/
│           │   ├── mod.rs        # Command dispatch
│           │   ├── add.rs        # urp add (NEW_ENDPOINT)
│           │   ├── remove.rs     # urp remove (DEL_ENDPOINT)
│           │   ├── set.rs        # urp set (SET_ENDPOINT)
│           │   ├── show.rs       # urp show (GET_ENDPOINT)
│           │   ├── stats.rs      # urp stats (GET_ENDPOINT, stats view)
│           │   ├── monitor.rs    # urp monitor (multicast subscription)
│           │   └── drain.rs      # urp drain (SET_ENDPOINT state=draining)
│           ├── format.rs         # Human-readable, JSON, oneline formatters
│           └── uapi.rs           # Rust constants mirroring linux/urp.h
├── kernel/                       # Kernel module source (Section 21-22)
│   ├── Kbuild                    # Kernel build rules
│   ├── urp_main.c                # module_init/exit, GENL family registration
│   ├── urp_netlink.c             # GENL command handlers, policy arrays, events
│   ├── urp_endpoint.c            # Endpoint lifecycle (create, activate, drain, destroy)
│   ├── urp_rdma.c                # RDMA CM + verbs (QP setup, buffer pool, CQ)
│   ├── urp_socket.c              # Virtual UDS endpoint (proto_ops, accept loop)
│   ├── urp_pump.c                # Bidirectional data pump (TX/RX kthreads)
│   ├── urp_proc.c                # /proc/urp/* stats export
│   └── include/
│       └── uapi/
│           └── linux/
│               └── urp.h         # UAPI header (commands, attributes, enums)
├── tests/
│   ├── integration/              # Integration tests (require rdma_rxe)
│   │   ├── basic_transfer.rs
│   │   ├── bidirectional.rs
│   │   ├── half_close.rs
│   │   ├── multi_connection.rs
│   │   ├── multi_qp_reorder.rs
│   │   └── backpressure.rs
│   └── common/
│       └── mod.rs                # Test helpers (namespace setup, proxy spawn)
├── benches/
│   ├── frame_codec.rs            # Frame encode/decode benchmarks
│   ├── buffer_pool.rs            # Allocation benchmarks
│   ├── reorder_buffer.rs         # Reorder buffer benchmarks
│   ├── credit_accounting.rs      # Credit grant/decrement hot loop
│   └── batching.rs               # AdaptiveBatcher decision overhead
├── fuzz/
│   ├── Cargo.toml                # cargo-fuzz configuration
│   └── fuzz_targets/
│       ├── frame_decode.rs       # Decode arbitrary bytes as frame
│       ├── frame_roundtrip.rs    # Encode then decode, assert equivalence
│       ├── reorder_buffer.rs     # Random insert/drain sequences
│       ├── credit_state_machine.rs # Random grant/decrement/query
│       ├── config_parse.rs       # Parse arbitrary bytes as TOML config
│       └── protocol_session.rs   # Simulated session: frame sequence with state
├── deploy/
│   ├── uds-rdma-proxy.service    # Systemd unit file (userspace proxy)
│   ├── urp.service               # Systemd unit file (kernel module load)
│   ├── urp-endpoints.service     # Systemd unit file (kernel module endpoint setup)
│   ├── urp-endpoint@.service     # Templated per-endpoint systemd unit
│   ├── Dockerfile                # Container image
│   └── k8s/
│       └── daemonset.yaml        # Kubernetes deployment
├── scripts/
│   ├── setup-rxe.sh              # Software RDMA setup for testing
│   └── benchmark.sh              # Automated benchmark suite
├── flake.nix                     # Nix flake (minimal, delegates to ./nix/)
├── flake.lock                    # Nix flake lock file
└── nix/                          # Modular Nix configuration
    ├── packages.nix              # Dependency lists (build, runtime, dev)
    ├── env-vars.nix              # Environment variable definitions
    ├── devshell.nix              # Development shell configuration
    ├── derivation.nix            # Main build derivation
    ├── cross-compilation.nix     # Cross-compiled proxy for aarch64/riscv64
    ├── tests/
    │   ├── default.nix           # Test coordinator
    │   ├── unit.nix              # Unit test runner
    │   ├── integration.nix       # Integration tests (requires rdma_rxe)
    │   └── lossy-network.nix     # tc-netem impairment tests
    ├── bench/
    │   ├── default.nix           # Benchmark coordinator
    │   └── mkBenchExperiment.nix # Experiment factory (JSON + markdown output)
    ├── microvms/                  # MicroVM-based integration testing
    │   ├── default.nix           # Entry point: generates VM pairs, tests, lifecycle scripts
    │   ├── mkVm.nix              # Parameterized VM: proxy, rdma_rxe, bench tools
    │   ├── mkVmPair.nix          # Listener+connector VM pair with shared RDMA network
    │   ├── constants.nix         # Architecture configs, ports, timeouts
    │   ├── lib.nix               # Lifecycle helpers, polling, expect wrappers
    │   └── scripts/
    │       ├── vm-expect.exp     # Generic command execution via console
    │       └── vm-rdma-test.exp  # RDMA proxy test orchestration
    ├── shell-functions/
    │   ├── build.nix             # build-proxy, build-bench, build-all
    │   ├── clean.nix             # clean-proxy, clean-bench, clean-all
    │   ├── rdma-setup.nix        # setup-rxe, teardown-rxe, check-rdma
    │   ├── validation.nix        # check-platform, check-kernel-modules
    │   └── navigation.nix        # navigate-to-*, aliases
    └── ci.nix                    # CI derivations (GitHub Actions)
```

## 19.2 Nix Development Environment

The project uses a modular `./nix/` directory structure (following the pattern established in the [xdp2](../../../xdp2) project) to keep `flake.nix` minimal while supporting sophisticated build, test, and benchmark infrastructure. Each `.nix` file handles a single concern, composed through explicit parameter passing.

#### Architecture: Delegation Chain

```
 flake.nix (~120 lines)
   |
   +-- import ./nix/packages.nix             { inherit pkgs rustToolchain; }
   |     -> nativeBuildInputs, buildInputs, devTools, allPackages
   |
   +-- import ./nix/env-vars.nix             { inherit pkgs packages; }
   |     -> shell fragment setting PKG_CONFIG_PATH, LD_LIBRARY_PATH, etc.
   |
   +-- import ./nix/devshell.nix             { inherit pkgs packages envVars; }
   |     -> mkShell with all dependencies + shell functions
   |
   +-- import ./nix/derivation.nix           { inherit pkgs packages; }
   |     -> buildable proxy binary (native)
   |
   +-- import ./nix/cross-compilation.nix    { inherit pkgs proxy; }
   |     -> cross-compiled proxy for aarch64-linux, riscv64-linux
   |
   +-- import ./nix/tests/default.nix        { inherit pkgs proxy bench; }
   |     -> test derivations (unit, integration, lossy-network)
   |
   +-- import ./nix/bench/default.nix        { inherit pkgs proxy bench; }
   |     -> benchmark experiment derivations
   |
   +-- import ./nix/microvms/default.nix     { inherit pkgs lib proxy proxyAarch64 proxyRiscv64; }
         -> VM pairs per architecture, lifecycle scripts, cross-arch integration tests
```

#### flake.nix (Top-Level)

```nix
#
# flake.nix for uds-rdma-proxy
#
# Development environment:   nix develop
# Build (native):            nix build
# Build (aarch64):           nix build .#proxy-aarch64
# Build (riscv64):           nix build .#proxy-riscv64
# Run tests:                 nix build .#tests.unit
# Run integration tests:     nix build .#tests.integration (requires rdma_rxe)
# MicroVM test (x86_64):     nix run .#microvms.test-x86_64
# MicroVM test (all archs):  nix run .#microvms.test-all
# VM console:                nix run .#microvms.helpers.vm-serial-x86_64
#
# Debugging:
#   UDS_RDMA_NIX_DEBUG=1 nix develop
#
{
  description = "UDS-RDMA Proxy: tunnel Unix Domain Sockets over RoCEv2";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    rust-overlay.url = "github:oxalica/rust-overlay";
    flake-utils.url = "github:numtide/flake-utils";
    microvm.url = "github:astro/microvm.nix";
    microvm.inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs = { self, nixpkgs, rust-overlay, flake-utils, microvm }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        overlays = [ (import rust-overlay) ];
        pkgs = import nixpkgs { inherit system overlays; };
        lib = nixpkgs.lib;

        # Rust toolchain (pinned via rust-overlay)
        rustToolchain = pkgs.rust-bin.stable.latest.default.override {
          extensions = [ "rust-src" "rust-analyzer" ];
        };

        # 1. Dependencies
        packagesModule = import ./nix/packages.nix {
          inherit pkgs rustToolchain;
        };

        # 2. Environment variables
        envVars = import ./nix/env-vars.nix {
          inherit pkgs;
          packages = packagesModule;
        };

        # 3. Build derivation (native)
        proxy = import ./nix/derivation.nix {
          inherit pkgs lib rustToolchain;
          inherit (packagesModule) nativeBuildInputs buildInputs;
        };

        # 4. Cross-compiled builds (x86_64 host only)
        crossBuilds = import ./nix/cross-compilation.nix {
          inherit pkgs lib nixpkgs rustToolchain system;
        };

        # 5. Development shell
        devshell = import ./nix/devshell.nix {
          inherit pkgs lib envVars;
          packages = packagesModule;
        };

        # 6. Tests
        tests = import ./nix/tests {
          inherit pkgs proxy;
        };

        # 7. Benchmarks
        benchmarks = import ./nix/bench {
          inherit pkgs proxy;
        };

        # 8. MicroVM integration testing
        microvms = import ./nix/microvms {
          inherit pkgs lib microvm proxy system;
          proxyAarch64 = crossBuilds.proxy-aarch64 or null;
          proxyRiscv64 = crossBuilds.proxy-riscv64 or null;
        };
      in
      {
        packages = {
          default = proxy;
          inherit proxy;
        } // crossBuilds // tests // benchmarks // {
          inherit microvms;
        };

        devShells.default = devshell;
      }
    );
}
```
```

#### nix/packages.nix

Separates dependencies into three categories (following the xdp2 pattern):

```nix
# nix/packages.nix
#
# Package definitions for uds-rdma-proxy
#
# Properly separated into:
# - nativeBuildInputs: Build-time tools (compilers, linkers, etc.)
# - buildInputs: Libraries needed at build and runtime
# - devTools: Additional tools for development only
#
{ pkgs, rustToolchain }:

{
  # Build-time tools only - these run on the build machine
  nativeBuildInputs = [
    rustToolchain
    pkgs.pkg-config
    pkgs.cmake           # Some rdma-sys build scripts need cmake
    pkgs.protobuf        # If proto-based metrics are used
  ];

  # Libraries needed at build and runtime
  buildInputs = [
    # RDMA
    pkgs.rdma-core       # libibverbs, librdmacm, rdma-cm headers
    pkgs.linuxHeaders    # <linux/types.h>, <infiniband/verbs.h> kernel UAPI

    # io_uring
    pkgs.liburing        # io_uring library (if using C bindings)
  ];

  # Development-only tools (not needed for building)
  devTools = [
    # Networking / RDMA tools
    pkgs.iproute2        # ip, rdma, tc (netem) commands
    pkgs.ethtool         # NIC configuration
    pkgs.perftest        # RDMA performance tests (ib_send_bw, ib_read_lat)

    # Profiling
    pkgs.linuxPackages.perf
    pkgs.flamegraph
    pkgs.hotspot         # perf GUI

    # Monitoring
    pkgs.prometheus
    pkgs.grafana

    # Rust development
    pkgs.cargo-audit     # Dependency vulnerability scanner
    pkgs.cargo-flamegraph
    pkgs.cargo-show-asm  # Assembly output viewer

    # Debugging
    pkgs.gdb
    pkgs.strace
    pkgs.valgrind

    # Utilities
    pkgs.shellcheck      # Shell script linter
    pkgs.jq              # JSON processing for benchmark results
  ];

  # Combined list for dev shell
  allPackages =
    let self = import ./packages.nix { inherit pkgs rustToolchain; };
    in self.nativeBuildInputs ++ self.buildInputs ++ self.devTools;
}
```

#### nix/env-vars.nix

Returns a shell fragment (string) for environment setup -- avoids hardcoding Nix store paths in scripts:

```nix
# nix/env-vars.nix
#
# Environment variable definitions for uds-rdma-proxy
# Returns a shell script fragment injected into devShell/derivation.
#
{ pkgs, packages }:

''
  # RDMA library paths
  export PKG_CONFIG_PATH=${pkgs.lib.makeSearchPath "lib/pkgconfig" packages.allPackages}
  export LD_LIBRARY_PATH=${pkgs.lib.makeLibraryPath [ pkgs.rdma-core ]}:''${LD_LIBRARY_PATH:-}

  # Ensure Cargo can find rdma-core headers for rdma-sys build script
  export RDMA_CORE_INCLUDE="${pkgs.rdma-core}/include"
  export RDMA_CORE_LIB="${pkgs.rdma-core}/lib"

  # Linux kernel headers (for UAPI InfiniBand headers)
  export LINUX_HEADERS_PATH="${pkgs.linuxHeaders}/include"

  # Build configuration
  export CARGO_BUILD_JOBS="$(nproc)"
''
```

#### nix/devshell.nix

Composes shell functions from modular files and wires up the environment:

```nix
# nix/devshell.nix
#
# Development shell configuration for uds-rdma-proxy
#
{ pkgs, lib, packages, envVars }:

let
  # Import shell function modules
  rdmaSetupFns = import ./shell-functions/rdma-setup.nix { };
  validationFns = import ./shell-functions/validation.nix { inherit lib; };
  buildFns = import ./shell-functions/build.nix { };
  cleanFns = import ./shell-functions/clean.nix { };
  navigationFns = import ./shell-functions/navigation.nix { };

  colored-prompt = ''
    export PS1="\[\033[0;36m\][uds-rdma-proxy] \[\033[01;34m\][\u@\h:\w]\$ \[\033[0m\]"
  '';
in
pkgs.mkShell {
  packages = packages.allPackages;

  shellHook = ''
    # Environment
    ${envVars}

    # Shell functions
    ${rdmaSetupFns}
    ${validationFns}
    ${buildFns}
    ${cleanFns}
    ${navigationFns}

    # Validation (runs on shell entry)
    check-kernel-modules
    check-platform

    # Prompt
    ${colored-prompt}

    echo "uds-rdma-proxy development shell"
    echo "  cargo build        - build proxy"
    echo "  setup-rxe          - configure software RDMA (requires sudo)"
    echo "  teardown-rxe       - remove software RDMA devices"
    echo "  check-rdma         - verify RDMA device status"
    echo "  cargo test         - run unit tests"
    echo "  local-test         - quick end-to-end test over local rdma_rxe"
  '';
}
```

#### Local RDMA Development with rdma_rxe

Software RDMA via `rdma_rxe` is the primary development tool — not just for CI or microVMs, but for everyday local testing during active development. The devshell provides everything needed to spin up a complete RDMA environment on the developer's machine in seconds, test changes immediately, and tear it down when done.

**Quick-start workflow** (inside `nix develop`):

```bash
# 1. Build
cargo build

# 2. Set up local software RDMA (two namespaces, veth pair, rxe devices)
setup-rxe
#   -> ns_rdma_a: 10.0.99.1 (rxe_a)
#   -> ns_rdma_b: 10.0.99.2 (rxe_b)

# 3. Run proxy pair (each in its own namespace)
sudo ip netns exec ns_rdma_a ./target/debug/uds-rdma-proxy \
  --uds-listen-path /tmp/proxy-a.sock --peer-address 10.0.99.2:4791 &
sudo ip netns exec ns_rdma_b ./target/debug/uds-rdma-proxy \
  --uds-connect-path /tmp/proxy-b.sock --rdma-bind-address 0.0.0.0:4791 &

# 4. Test: write to one UDS, read from the other
echo "hello RDMA" | socat - UNIX-CONNECT:/tmp/proxy-a.sock
# (appears on /tmp/proxy-b.sock consumer)

# 5. Run the bench tool
sudo ip netns exec ns_rdma_a ./target/debug/uds-rdma-bench \
  --mode producer --uds-path /tmp/proxy-a.sock --duration 10s

# 6. Inject network impairment (optional)
inject-netem 0.1 100 50    # 0.1% loss, 100us delay, 50us jitter

# 7. Tear down
teardown-rxe
```

This gives the full RDMA code path (ibverbs, rdma_cm, CQ polling, credit flow control) running locally with no VMs or hardware — just kernel modules. The `rdma_rxe` module processes RDMA verbs in software through the kernel network stack, so the functional behavior is identical to hardware RDMA.

**Ensuring rdma_rxe availability on the host**:

The `rdma_rxe` kernel module must be available in the host kernel. The devshell's `check-kernel-modules` function warns on entry if it's missing.

For **NixOS** hosts, add to your system configuration:

```nix
# /etc/nixos/configuration.nix (or equivalent)
{
  boot.kernelModules = [ "rdma_rxe" ];

  # Ensure the kernel is built with RDMA support (usually default, but explicit is better)
  boot.kernelPatches = [{
    name = "rdma-support";
    patch = null;
    extraStructuredConfig = with lib.kernel; {
      INFINIBAND      = yes;
      INFINIBAND_USER_ACCESS = module;
      RDMA_RXE        = module;
    };
  }];
}
```

For **non-NixOS** Linux (Ubuntu, Fedora, Arch, etc.), the module is typically available in standard kernels:

```bash
# Check availability
modinfo rdma_rxe

# Load (persists until reboot)
sudo modprobe rdma_rxe

# Load on every boot (distro-specific)
echo "rdma_rxe" | sudo tee /etc/modules-load.d/rdma_rxe.conf
```

If the module is missing, the kernel needs `CONFIG_RDMA_RXE=m` enabled. Most distribution kernels include this by default; if not, a custom kernel build is required.

#### nix/shell-functions/rdma-setup.nix

Encapsulates software RDMA environment setup as shell functions:

```nix
# nix/shell-functions/rdma-setup.nix
#
# Shell functions for managing software RDMA (rxe/siw) test environments.
# These run inside `nix develop` and require sudo for kernel module operations.
#
{ }:

''
  setup-rxe() {
    echo "Setting up software RDMA (rxe) test environment..."

    # Load kernel module
    sudo modprobe rdma_rxe || { echo "ERROR: rdma_rxe module not available"; return 1; }

    # Create network namespaces and veth pair
    sudo ip netns add ns_rdma_a 2>/dev/null || true
    sudo ip netns add ns_rdma_b 2>/dev/null || true
    sudo ip link add veth_a type veth peer name veth_b 2>/dev/null || true
    sudo ip link set veth_a netns ns_rdma_a
    sudo ip link set veth_b netns ns_rdma_b

    # Configure addresses
    sudo ip netns exec ns_rdma_a ip addr add 10.0.99.1/24 dev veth_a
    sudo ip netns exec ns_rdma_a ip link set veth_a up
    sudo ip netns exec ns_rdma_a ip link set lo up
    sudo ip netns exec ns_rdma_b ip addr add 10.0.99.2/24 dev veth_b
    sudo ip netns exec ns_rdma_b ip link set veth_b up
    sudo ip netns exec ns_rdma_b ip link set lo up

    # Create RXE devices
    sudo ip netns exec ns_rdma_a rdma link add rxe_a type rxe netdev veth_a
    sudo ip netns exec ns_rdma_b rdma link add rxe_b type rxe netdev veth_b

    echo "Software RDMA ready:"
    echo "  ns_rdma_a: 10.0.99.1 (rxe_a)"
    echo "  ns_rdma_b: 10.0.99.2 (rxe_b)"
    echo ""
    echo "Usage:"
    echo "  sudo ip netns exec ns_rdma_a ./target/release/uds-rdma-proxy --uds-listen-path /tmp/a.sock --peer-address 10.0.99.2:4791"
    echo "  sudo ip netns exec ns_rdma_b ./target/release/uds-rdma-proxy --uds-connect-path /tmp/b.sock --rdma-bind-address 0.0.0.0:4791"
  }

  teardown-rxe() {
    echo "Tearing down software RDMA environment..."
    sudo ip netns exec ns_rdma_a rdma link delete rxe_a 2>/dev/null || true
    sudo ip netns exec ns_rdma_b rdma link delete rxe_b 2>/dev/null || true
    sudo ip netns delete ns_rdma_a 2>/dev/null || true
    sudo ip netns delete ns_rdma_b 2>/dev/null || true
    echo "Cleaned up."
  }

  check-rdma() {
    echo "=== RDMA Device Status ==="
    if command -v rdma &>/dev/null; then
      rdma dev 2>/dev/null || echo "No RDMA devices (run setup-rxe first)"
    else
      echo "rdma command not found"
    fi
    echo ""
    echo "=== Kernel Modules ==="
    lsmod | grep -E "rdma_rxe|siw|ib_core" || echo "No RDMA modules loaded"
  }

  # Inject tc-netem impairment for lossy network testing (see 12-testing.md#127-lossy-network-simulation-rocev2-flappiness)
  inject-netem() {
    local loss="''${1:-0.1}"
    local delay="''${2:-0}"
    local jitter="''${3:-0}"
    echo "Injecting netem: loss=''${loss}% delay=''${delay}us jitter=''${jitter}us"
    sudo ip netns exec ns_rdma_a tc qdisc replace dev veth_a root netem \
      loss "''${loss}%" delay "''${delay}us" "''${jitter}us"
  }

  clear-netem() {
    sudo ip netns exec ns_rdma_a tc qdisc del dev veth_a root 2>/dev/null || true
    echo "Cleared netem impairment."
  }
''
```

#### nix/shell-functions/validation.nix

```nix
# nix/shell-functions/validation.nix
#
# Platform and kernel module validation, run on shell entry.
#
{ lib }:

''
  check-kernel-modules() {
    local warnings=0

    if ! modinfo rdma_rxe &>/dev/null 2>&1; then
      echo "WARNING: rdma_rxe kernel module not available."
      echo "  Integration tests require: sudo modprobe rdma_rxe"
      echo "  Your kernel may need CONFIG_RDMA_RXE=m"
      warnings=$((warnings + 1))
    fi

    if ! modinfo siw &>/dev/null 2>&1; then
      if [ "$warnings" -eq 0 ]; then
        echo "NOTE: siw (Soft-iWARP) module not available (optional)"
      fi
    fi

    if [ "$warnings" -eq 0 ]; then
      echo "Kernel RDMA modules available (rdma_rxe)"
    fi
  }

  check-platform() {
    if [ "$(uname -s)" != "Linux" ]; then
      echo "ERROR: uds-rdma-proxy requires Linux (io_uring + RDMA)"
      return 1
    fi

    # Check kernel version >= 5.1 (io_uring minimum)
    local kver
    kver=$(uname -r | cut -d. -f1-2)
    local major minor
    major=$(echo "$kver" | cut -d. -f1)
    minor=$(echo "$kver" | cut -d. -f2)
    if [ "$major" -lt 5 ] || { [ "$major" -eq 5 ] && [ "$minor" -lt 1 ]; }; then
      echo "WARNING: Kernel $kver detected. io_uring requires >= 5.1"
    fi
  }
''
```

#### nix/bench/mkBenchExperiment.nix (Experiment Factory)

Following the xdp2 `mkBenchExperiment` pattern -- creates self-contained, repeatable benchmark wrappers with JSON + Markdown output:

```nix
# nix/bench/mkBenchExperiment.nix
#
# Factory for creating reproducible benchmark experiments.
# Each experiment is a writeShellApplication that runs the proxy + bench tool,
# captures metrics, and outputs structured results.
#
# Usage:
#   baseline = mkBenchExperiment {
#     name = "tcp-baseline-4k";
#     description = "v0 TCP transport, 4KB messages, 1 connection";
#     benchArgs = "--mode producer --message-size 4096 --duration 30s";
#     transport = "tcp";
#   };
#
{ pkgs, proxy, bench }:

{ name
, description
, expectation ? ""
, benchArgs
, transport ? "tcp"
, numQps ? 1
, proxyExtraArgs ? ""
}:

pkgs.writeShellApplication {
  name = "uds-rdma-exp-${name}";
  runtimeInputs = [ proxy bench pkgs.jq pkgs.curl ];
  text = ''
    set -euo pipefail

    RESULTS_DIR="bench-results/exp-${name}-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$RESULTS_DIR"

    echo "=== Experiment: ${name} ==="
    echo "Description: ${description}"
    echo "Expectation: ${expectation}"
    echo "Transport: ${transport}, QPs: ${toString numQps}"
    echo "Results: $RESULTS_DIR"
    echo ""

    # Start proxy pair
    uds-rdma-proxy --uds-listen-path /tmp/bench-a.sock \
      --peer-address 127.0.0.1:4791 \
      --transport ${transport} \
      --num-qps ${toString numQps} ${proxyExtraArgs} \
      --metrics-bind 127.0.0.1:9090 &
    PROXY_A=$!

    uds-rdma-proxy --uds-connect-path /tmp/bench-b.sock \
      --rdma-bind-address 0.0.0.0:4791 \
      --transport ${transport} \
      --num-qps ${toString numQps} ${proxyExtraArgs} \
      --metrics-bind 127.0.0.1:9091 &
    PROXY_B=$!

    sleep 1  # Allow connection establishment

    # Run benchmark
    uds-rdma-bench ${benchArgs} \
      --report-interval 1s \
      --json-output "$RESULTS_DIR/bench.json" \
      2>&1 | tee "$RESULTS_DIR/run.log"

    # Scrape Prometheus metrics
    curl -s http://127.0.0.1:9090/metrics > "$RESULTS_DIR/metrics-a.prom" || true
    curl -s http://127.0.0.1:9091/metrics > "$RESULTS_DIR/metrics-b.prom" || true

    # Generate summary
    jq '{
      experiment: "${name}",
      transport: "${transport}",
      num_qps: ${toString numQps},
      throughput_mbps: .summary.throughput_mbps,
      latency_p50_us: .summary.latency_p50_us,
      latency_p99_us: .summary.latency_p99_us,
      messages_total: .summary.messages_total
    }' "$RESULTS_DIR/bench.json" > "$RESULTS_DIR/summary.json"

    # Markdown report
    cat > "$RESULTS_DIR/SUMMARY.md" << REPORT
    # Experiment: ${name}
    **${description}**
    Transport: ${transport} | QPs: ${toString numQps}
    $(jq -r '"Throughput: \(.throughput_mbps) MB/s | p50: \(.latency_p50_us)us | p99: \(.latency_p99_us)us"' "$RESULTS_DIR/summary.json")
    REPORT

    # Cleanup
    kill $PROXY_A $PROXY_B 2>/dev/null || true
    wait $PROXY_A $PROXY_B 2>/dev/null || true

    echo ""
    echo "Results saved to: $RESULTS_DIR/"
    cat "$RESULTS_DIR/SUMMARY.md"
  '';
}
```

## 19.3 MicroVM Integration Testing

The project uses [microvm.nix](https://github.com/astro/microvm.nix) to provide full-system RDMA integration testing inside lightweight virtual machines. This goes beyond the namespace-based testing in [Section 12](12-testing.md) — microVMs give us isolated kernel environments, controlled kernel versions, and real cross-machine RDMA communication.

#### Why MicroVMs for RDMA Testing

| Approach | Isolation | Kernel control | Cross-arch | Real NIC emulation |
|----------|-----------|---------------|------------|-------------------|
| Network namespaces + rdma_rxe | Process-level | Host kernel only | No | No (shared kernel) |
| **MicroVMs + rdma_rxe** | **Full kernel** | **Pinned kernel version** | **Yes (QEMU TCG)** | **Per-VM NIC** |
| Hardware RDMA | Physical | Production kernel | Limited | Yes |

MicroVMs provide the middle ground: more realistic than namespaces (separate kernel instances, independent RDMA subsystems), reproducible (Nix-pinned kernel + userspace), and capable of cross-architecture testing — without requiring RDMA hardware.

#### Key Difference from xdp2

The xdp2 project tests eBPF programs inside a **single VM**. The uds-rdma-proxy needs a **pair** of VMs communicating over RDMA — one running the RDMA initiator proxy and the other the RDMA acceptor proxy. This requires coordinated VM lifecycle management and shared virtual networking.

```
 Build Host (x86_64)
 ┌─────────────────────────────────────────────────────────────────┐
 │                                                                 │
 │  ┌─────────────────────┐  virtio-net   ┌─────────────────────┐ │
 │  │ VM-A (initiator)    │  (bridge or   │ VM-B (acceptor)     │ │
 │  │                     │   P2P veth)   │                     │ │
 │  │  App <-UDS-> Proxy  │<=============>│  Proxy <-UDS-> App  │ │
 │  │                     │  rdma_rxe     │                     │ │
 │  │  10.0.99.1          │               │  10.0.99.2          │ │
 │  │  rxe0 on eth0       │               │  rxe0 on eth0       │ │
 │  └─────────────────────┘               └─────────────────────┘ │
 │                                                                 │
 │  Lifecycle orchestrator: start A, start B, run tests, collect   │
 │  metrics, shutdown B, shutdown A                                │
 └─────────────────────────────────────────────────────────────────┘
```

#### nix/microvms/constants.nix

Architecture-specific configuration following xdp2's pattern — port allocation, timeouts, kernel selection:

```nix
# nix/microvms/constants.nix
#
# Per-architecture configuration for MicroVM test pairs.
# Port scheme: base 24500, +10 per architecture, +0 listener, +1 connector.
#
{ lib }:

{
  supportedArchs = [ "x86_64" "aarch64" "riscv64" ];

  # Architecture definitions
  archConfig = {
    x86_64 = {
      system = "x86_64-linux";
      qemuCpu = "host";
      qemuMachine = "q35";
      accel = "kvm";               # Native speed
      consoleDevice = "ttyS0";
      kernelPackages = "linuxPackages";  # Stable kernel (KVM)
      listenerPorts = { serial = 24500; virtio = 24501; metrics = 24502; };
      connectorPorts = { serial = 24503; virtio = 24504; metrics = 24505; };
    };
    aarch64 = {
      system = "aarch64-linux";
      qemuCpu = "cortex-a72";
      qemuMachine = "virt";
      accel = "tcg";               # Software emulation
      consoleDevice = "ttyAMA0";
      kernelPackages = "linuxPackages_latest";  # Better RDMA/BTF support
      listenerPorts = { serial = 24510; virtio = 24511; metrics = 24512; };
      connectorPorts = { serial = 24513; virtio = 24514; metrics = 24515; };
    };
    riscv64 = {
      system = "riscv64-linux";
      qemuCpu = "rv64";
      qemuMachine = "virt";
      accel = "tcg";
      consoleDevice = "ttyS0";
      kernelPackages = "linuxPackages_latest";
      listenerPorts = { serial = 24520; virtio = 24521; metrics = 24522; };
      connectorPorts = { serial = 24523; virtio = 24524; metrics = 24525; };
    };
  };

  # Timeouts (seconds) — emulated architectures are significantly slower
  timeouts = {
    x86_64  = { build = 300; boot = 30;  rdmaReady = 15;  testSuite = 120; shutdown = 10; };
    aarch64 = { build = 600; boot = 90;  rdmaReady = 45;  testSuite = 300; shutdown = 30; };
    riscv64 = { build = 900; boot = 180; rdmaReady = 90;  testSuite = 600; shutdown = 60; };
  };
}
```

#### nix/microvms/mkVm.nix

Each VM is a minimal NixOS system with the proxy binary, rdma_rxe, and test tooling. Following xdp2's pattern: disable unnecessary services, use 9P mount for `/nix/store`, enforce kernel requirements.

```nix
# nix/microvms/mkVm.nix
#
# Parameterized MicroVM definition for uds-rdma-proxy testing.
# Produces a minimal NixOS VM with:
#   - The proxy binary (native or cross-compiled)
#   - rdma-core userspace + rdma_rxe kernel module
#   - uds-rdma-bench load generator
#   - Prometheus metrics endpoint
#
{ pkgs, lib, microvm, arch, role, proxyPackage, archConfig, ... }:

let
  cfg = archConfig.${arch};
  ports = if role == "rdma-initiator" then cfg.listenerPorts else cfg.connectorPorts;
  peerAddr = if role == "rdma-initiator" then "10.0.99.2" else "10.0.99.1";
  selfAddr = if role == "rdma-initiator" then "10.0.99.1" else "10.0.99.2";

  # Cross-compilation overlay: disable tests that fail under QEMU emulation
  crossOverlay = final: prev: lib.optionalAttrs (arch != "x86_64") {
    rdma-core = prev.rdma-core.overrideAttrs (old: { doCheck = false; });
    liburing = prev.liburing.overrideAttrs (old: { doCheck = false; });
  };
in
{
  microvm = {
    hypervisor = "qemu";
    vcpu = 2;
    mem = 512;

    qemu.extraArgs = [
      "-cpu" cfg.qemuCpu
      "-machine" cfg.qemuMachine
    ] ++ lib.optionals (cfg.accel == "kvm") [ "-enable-kvm" ];

    # Virtio network interface — connected to partner VM via host bridge
    interfaces = [{
      type = "tap";
      id = "eth-${role}";
      mac = if role == "listener" then "02:00:00:00:00:01" else "02:00:00:00:00:02";
    }];

    # Share /nix/store read-only from host via 9P
    shares = [{
      tag = "store";
      source = "/nix/store";
      mountPoint = "/nix/.ro-store";
      proto = "9p";
    }];
  };

  networking.hostName = "uds-rdma-${role}-${arch}";

  # Minimal system: strip docs, fonts, nix daemon
  documentation.enable = false;
  fonts.fontconfig.enable = false;
  nix.enable = false;

  # Kernel: ensure RDMA and io_uring support is built in
  boot.kernelPatches = [{
    name = "rdma-rxe-io-uring";
    patch = null;
    extraStructuredConfig = with lib.kernel; {
      INFINIBAND      = yes;
      INFINIBAND_USER_ACCESS = module;
      RDMA_RXE        = module;
      INFINIBAND_ADDR_TRANS = yes;
      IO_URING        = yes;
    };
  }];
  boot.kernelModules = [ "rdma_rxe" "ib_core" "ib_uverbs" "ib_cm" "rdma_ucm" ];

  environment.systemPackages = [
    proxyPackage                # uds-rdma-proxy + uds-rdma-bench
    pkgs.rdma-core              # libibverbs, rdma CLI
    pkgs.iproute2               # ip, rdma, tc commands
    pkgs.ethtool
    pkgs.curl                   # For Prometheus scraping
  ];

  # Network configuration — static IP, no DHCP
  networking.interfaces.eth0.ipv4.addresses = [{
    address = selfAddr;
    prefixLength = 24;
  }];

  # Self-test service: configure rdma_rxe on boot
  systemd.services.rdma-setup = {
    description = "Configure software RDMA (rxe) on eth0";
    after = [ "network-online.target" ];
    wants = [ "network-online.target" ];
    wantedBy = [ "multi-user.target" ];
    serviceConfig.Type = "oneshot";
    serviceConfig.RemainAfterExit = true;
    script = ''
      rdma link add rxe0 type rxe netdev eth0
      echo "RDMA device rxe0 ready on eth0 (${selfAddr})"
      rdma dev show
    '';
  };

  # Proxy service: starts after RDMA is configured
  systemd.services.uds-rdma-proxy = {
    description = "UDS-RDMA Proxy (${role})";
    after = [ "rdma-setup.service" ];
    requires = [ "rdma-setup.service" ];
    wantedBy = [ "multi-user.target" ];
    serviceConfig = {
      ExecStart = ''
        ${proxyPackage}/bin/uds-rdma-proxy \
          --uds-listen-path /var/run/urp/outbound.sock \
          --uds-connect-path /var/run/urp/inbound.sock \
          --peer-address ${peerAddr}:4791 \
          --metrics-bind 0.0.0.0:${toString ports.metrics}
      '';
      Restart = "on-failure";
      LimitMEMLOCK = "infinity";
    };
  };
}
```

#### nix/microvms/mkVmPair.nix

Creates a coordinated pair of VMs (listener + connector) with shared networking:

```nix
# nix/microvms/mkVmPair.nix
#
# Creates a listener+connector VM pair for end-to-end RDMA proxy testing.
# Orchestrates: start listener -> start connector -> run test suite -> collect -> shutdown.
#
{ pkgs, lib, mkVm, arch, proxyPackage, archConfig, constants }:

let
  cfg = archConfig.${arch};
  timeouts = constants.timeouts.${arch};

  initiatorVm = mkVm {
    inherit pkgs lib arch archConfig;
    role = "rdma-initiator";
    inherit proxyPackage;
  };

  acceptorVm = mkVm {
    inherit pkgs lib arch archConfig;
    role = "rdma-acceptor";
    inherit proxyPackage;
  };
in
pkgs.writeShellApplication {
  name = "uds-rdma-vm-test-${arch}";
  runtimeInputs = [ pkgs.qemu pkgs.expect pkgs.curl pkgs.jq ];
  text = ''
    set -euo pipefail
    echo "=== MicroVM Pair Test: ${arch} ==="
    echo "Timeouts: boot=${toString timeouts.boot}s rdma=${toString timeouts.rdmaReady}s test=${toString timeouts.testSuite}s"

    RESULTS_DIR="vm-results/${arch}-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$RESULTS_DIR"

    cleanup() {
      echo "Shutting down VMs..."
      kill "$INITIATOR_PID" 2>/dev/null || true
      kill "$ACCEPTOR_PID" 2>/dev/null || true
      wait "$INITIATOR_PID" "$ACCEPTOR_PID" 2>/dev/null || true
    }
    trap cleanup EXIT

    # Phase 1: Start RDMA acceptor VM
    echo "[1/6] Starting RDMA acceptor VM..."
    ${acceptorVm}/bin/run-vm &
    ACCEPTOR_PID=$!

    # Phase 2: Wait for acceptor RDMA ready
    echo "[2/6] Waiting for acceptor RDMA..."
    # (poll serial console for "RDMA device rxe0 ready" or systemctl status)

    # Phase 3: Start RDMA initiator VM
    echo "[3/6] Starting RDMA initiator VM..."
    ${initiatorVm}/bin/run-vm &
    INITIATOR_PID=$!

    # Phase 4: Wait for initiator RDMA ready + proxy connection established
    echo "[4/6] Waiting for RDMA connection..."

    # Phase 5: Run test suite
    echo "[5/6] Running integration tests..."
    # Execute uds-rdma-bench in initiator VM, collect results
    # Scrape Prometheus from both VMs

    curl -s http://localhost:${toString cfg.listenerPorts.metrics}/metrics \
      > "$RESULTS_DIR/metrics-listener.prom" || true
    curl -s http://localhost:${toString cfg.connectorPorts.metrics}/metrics \
      > "$RESULTS_DIR/metrics-connector.prom" || true

    # Phase 6: Shutdown
    echo "[6/6] Collecting results and shutting down..."
    echo "Results saved to: $RESULTS_DIR/"
  '';
}
```

#### Lifecycle Scripts

Following xdp2's lifecycle pattern, each VM pair generates phased lifecycle scripts for debugging and CI:

| Phase | Script | Description |
|-------|--------|-------------|
| 0 | `vm-build-${arch}` | Build both VM derivations |
| 1 | `vm-start-acceptor-${arch}` | Start RDMA acceptor VM, wait for boot |
| 2 | `vm-start-initiator-${arch}` | Start RDMA initiator VM, wait for boot |
| 3 | `vm-verify-rdma-${arch}` | Verify both VMs have rxe0 configured and can ping |
| 4 | `vm-verify-proxy-${arch}` | Verify proxy connection established between VMs |
| 5 | `vm-run-tests-${arch}` | Execute test suite (basic transfer, bidirectional, multi-QP, backpressure) |
| 6 | `vm-collect-metrics-${arch}` | Scrape Prometheus endpoints from both VMs |
| 7 | `vm-shutdown-${arch}` | Graceful shutdown of both VMs |
| full | `vm-full-test-${arch}` | All phases sequentially with timing + colored output |

#### Flake Outputs (MicroVM)

```
packages.microvms = {
  # VM pair derivations
  x86_64           — listener + connector VMs (KVM, native speed)
  aarch64          — listener + connector VMs (QEMU TCG, cross-compiled)
  riscv64          — listener + connector VMs (QEMU TCG, cross-compiled)

  # Test runners
  test-x86_64      — full integration test on x86_64 VM pair
  test-aarch64     — full integration test on aarch64 VM pair
  test-riscv64     — full integration test on riscv64 VM pair
  test-all         — sequential test on all architectures

  # Lifecycle scripts (per architecture)
  lifecycle.x86_64.{build,start-listener,start-connector,verify-rdma,...}
  lifecycle.aarch64.{...}
  lifecycle.riscv64.{...}

  # Helper scripts
  helpers.vm-serial-x86_64    — connect to x86_64 listener serial console
  helpers.vm-login-x86_64     — interactive login to x86_64 listener
  helpers.vm-run-x86_64       — run command in x86_64 listener
};
```

## 19.4 Multi-Architecture Support

ARM (aarch64) and RISC-V (riscv64) are widely deployed in data center and edge environments where RDMA is relevant — Ampere Altra servers, NVIDIA BlueField DPUs (ARM), and emerging RISC-V platforms. The proxy must build and run correctly on these architectures.

#### Supported Architectures

| Architecture | Build Method | VM Emulation | Typical Use Case |
|-------------|-------------|-------------|-----------------|
| x86_64 | Native | KVM (native speed) | Primary development + production |
| aarch64 | Cross-compiled from x86_64 | QEMU TCG (~3-5× slower) | ARM servers (Ampere, Graviton), DPUs (BlueField) |
| riscv64 | Cross-compiled from x86_64 | QEMU TCG (~5-10× slower) | Emerging RISC-V hardware, research |

#### nix/cross-compilation.nix

True cross-compilation using Nix's `localSystem`/`crossSystem` — the Rust compiler runs on the x86_64 build host and emits target-architecture binaries. No slow binfmt emulation for the build itself.

```nix
# nix/cross-compilation.nix
#
# Cross-compiled uds-rdma-proxy for aarch64-linux and riscv64-linux.
# Build host: x86_64-linux. Uses Nix crossSystem, NOT binfmt emulation.
#
# Usage:
#   nix build .#proxy-aarch64
#   nix build .#proxy-riscv64
#
{ pkgs, lib, nixpkgs, rustToolchain, system }:

let
  # Only cross-compile from x86_64
  canCrossCompile = system == "x86_64-linux";

  mkCrossProxy = targetSystem:
    let
      # Import nixpkgs with cross-compilation support
      crossPkgs = import nixpkgs {
        localSystem = "x86_64-linux";
        crossSystem = targetSystem;
        overlays = [
          # Disable tests for packages that fail under cross-compilation
          (final: prev: {
            rdma-core = prev.rdma-core.overrideAttrs (old: { doCheck = false; });
            liburing = prev.liburing.overrideAttrs (old: { doCheck = false; });
          })
        ];
      };

      # Cross Rust toolchain targeting the remote architecture
      crossRustToolchain = crossPkgs.rust-bin.stable.latest.default.override {
        targets = [ (lib.strings.removeSuffix "-linux" targetSystem + "-unknown-linux-gnu") ];
      };

      crossPackages = import ./packages.nix {
        pkgs = crossPkgs;
        rustToolchain = crossRustToolchain;
      };
    in
    import ./derivation.nix {
      pkgs = crossPkgs;
      inherit lib;
      rustToolchain = crossRustToolchain;
      inherit (crossPackages) nativeBuildInputs buildInputs;
    };
in
lib.optionalAttrs canCrossCompile {
  proxy-aarch64 = mkCrossProxy "aarch64-linux";
  proxy-riscv64 = mkCrossProxy "riscv64-linux";
}
```

#### Cross-Compilation Overlays

Some dependencies fail their test suites under cross-compilation (the libraries build fine, only tests break). Following xdp2's pattern, overlays selectively disable tests:

| Package | Issue Under Cross-Compilation | Overlay |
|---------|------------------------------|---------|
| `rdma-core` | Test binaries built for target can't run on build host | `doCheck = false` |
| `liburing` | io_uring syscalls unavailable in cross environment | `doCheck = false` |

These overlays are minimal and only applied to the cross-compilation pkgs set, not the native build.

#### Build Commands

```bash
# Native build (default)
nix build                          # x86_64 proxy

# Cross-compiled builds (from x86_64 host)
nix build .#proxy-aarch64          # aarch64 proxy binary
nix build .#proxy-riscv64          # riscv64 proxy binary

# MicroVM integration tests
nix run .#microvms.test-x86_64     # Full test on x86_64 VM pair (KVM, fast)
nix run .#microvms.test-aarch64    # Full test on aarch64 VM pair (TCG, ~5min)
nix run .#microvms.test-riscv64    # Full test on riscv64 VM pair (TCG, ~10min)
nix run .#microvms.test-all        # All architectures sequentially

# Debugging: interactive VM access
nix run .#microvms.helpers.vm-login-x86_64
nix run .#microvms.helpers.vm-serial-aarch64
```

#### Key Design Patterns (from xdp2)

| Pattern | Description |
|---------|-------------|
| **Delegation chain** | `flake.nix` imports `./nix/*.nix` files in dependency order; each file takes explicit parameters via `{ inherit ... }` |
| **Shell fragments as strings** | `env-vars.nix` returns a bash string, not Nix values. Injected directly into `shellHook` via `${envVars}`. |
| **Modular shell functions** | Each file in `./nix/shell-functions/` returns a bash string defining related functions. Composed in `devshell.nix`. |
| **Experiment factory** | `mkBenchExperiment` creates self-contained benchmark wrappers with structured output (JSON + Markdown). Each experiment is a Nix derivation. |
| **Three-tier packages** | `nativeBuildInputs` (build tools), `buildInputs` (libraries), `devTools` (development-only). Combined as `allPackages` for the dev shell. |
| **Runtime debug verbosity** | `UDS_RDMA_NIX_DEBUG=1 nix develop` enables extra shell output without modifying Nix files. |
| **VM pair factory** | `mkVmPair` creates coordinated RDMA initiator+acceptor VM pairs per architecture — extending xdp2's single-VM `mkVm` pattern for two-machine RDMA testing. |
| **True cross-compilation** | Uses `localSystem`/`crossSystem` with test-disabling overlays, not slow binfmt emulation. Build runs at native speed on x86_64 host. |
| **Phased lifecycle scripts** | Each VM pair generates individual phase scripts (build, start, verify-rdma, run-tests, collect, shutdown) for debugging and CI granularity. |
| **Architecture-aware timeouts** | Separate timeout tables for KVM (fast), TCG aarch64 (moderate), TCG riscv64 (slow) — avoids false failures on emulated architectures. |

#### Nix Directory Summary

```
nix/
├── packages.nix              # 3-tier dependency lists
├── env-vars.nix              # Shell fragment: PKG_CONFIG_PATH, LD_LIBRARY_PATH, RDMA paths
├── devshell.nix              # mkShell composition: packages + envVars + shell-functions
├── derivation.nix            # stdenv.mkDerivation or naersk/crane for Rust build
├── cross-compilation.nix     # Cross-compiled proxy for aarch64, riscv64
├── shell-functions/
│   ├── rdma-setup.nix        # setup-rxe, teardown-rxe, check-rdma, inject-netem
│   ├── validation.nix        # check-kernel-modules, check-platform
│   ├── build.nix             # build-proxy, build-bench, build-all
│   ├── clean.nix             # clean-proxy, clean-bench, clean-all
│   └── navigation.nix        # aliases, navigate-to-*
├── tests/
│   ├── default.nix           # Test coordinator (imports sub-tests)
│   ├── unit.nix              # cargo test --lib (no RDMA needed)
│   ├── integration.nix       # End-to-end via rdma_rxe + netns
│   └── lossy-network.nix     # tc-netem impairment matrix
├── bench/
│   ├── default.nix           # Benchmark coordinator
│   └── mkBenchExperiment.nix # Experiment factory
├── microvms/
│   ├── default.nix           # Entry point: VM pairs, tests, lifecycle per arch
│   ├── mkVm.nix              # Parameterized VM (proxy, rdma_rxe, bench tools)
│   ├── mkVmPair.nix          # Listener+connector pair with shared RDMA network
│   ├── constants.nix         # Arch configs, ports, timeouts (KVM/TCG)
│   ├── lib.nix               # Lifecycle helpers, polling, expect wrappers
│   └── scripts/
│       ├── vm-expect.exp     # Generic command execution via console
│       └── vm-rdma-test.exp  # RDMA-specific test orchestration
└── ci.nix                    # GitHub Actions test derivations
```

**Kernel module note**: The `rdma_rxe` kernel module is the foundation of all non-hardware testing — local development, namespace integration tests, and MicroVM tests all depend on it. Nix provides the userspace libraries (rdma-core, liburing) but the kernel module must come from the running kernel:

- **Local development**: The devshell checks for `rdma_rxe` on entry and warns if missing. `setup-rxe` loads it via `sudo modprobe rdma_rxe`. See "Local RDMA Development" above for host kernel configuration (NixOS `boot.kernelPatches` or non-NixOS `modprobe`).
- **MicroVMs**: The NixOS VM configuration uses `boot.kernelPatches` to guarantee `CONFIG_RDMA_RXE=m` is built into the VM kernel, and `boot.kernelModules` to load it at boot. No host kernel dependency — the VM has its own kernel with RDMA support baked in.
- **CI**: GitHub Actions runners use the host kernel's `rdma_rxe` (most Ubuntu kernels ship it). MicroVM tests are self-contained and don't depend on the CI runner's kernel config.


[Back to Design Overview](../DESIGN.md)
