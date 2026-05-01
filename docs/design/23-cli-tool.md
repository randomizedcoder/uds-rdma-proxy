# `urp` CLI Tool

`urp` is a standalone Rust command-line tool for managing the UDS-RDMA proxy kernel module. It communicates with the kernel module via Generic Netlink ([Section 22](22-genl-interface.md)), following the pattern established by WireGuard's `wg` tool: short name, speaks GENL directly, no dependency on iproute2.

## 23.1 Why a Standalone Tool

| Approach | Language | Distribution | Netlink | Tradeoff |
|----------|----------|-------------|---------|----------|
| iproute2 plugin | C | Must integrate into iproute2 build system | libnl/libmnl | Familiar `ip` interface, but C-only and hard to distribute independently |
| pyroute2 script | Python | Pip/system package | pyroute2 | Easy prototyping, but runtime dependency on Python + pyroute2 |
| **Standalone `urp`** | **Rust** | **Single static binary** | **neli crate** | **No runtime dependencies, type-safe, ships alongside kernel module** |

The `wg` tool demonstrates that a standalone binary is the right approach for module-specific configuration: it's simpler to install, version, and distribute than an iproute2 plugin, and the CLI can evolve independently of the kernel networking tool ecosystem.


## 23.2 Subcommands

```
urp add <name> [options]        Create a new endpoint (URP_CMD_NEW_ENDPOINT)
urp remove <name>               Remove an endpoint (URP_CMD_DEL_ENDPOINT)
urp set <name> [options]        Modify a live endpoint (URP_CMD_SET_ENDPOINT)
urp show [name]                 Show endpoint config and state (URP_CMD_GET_ENDPOINT)
urp stats [name]                Show focused statistics view (URP_CMD_GET_ENDPOINT)
urp monitor                     Subscribe to multicast events (events group)
urp drain <name>                Graceful drain (URP_CMD_SET_ENDPOINT, state=draining)
```

### 23.2.1 `urp add`

Creates and activates a new endpoint. Maps to `URP_CMD_NEW_ENDPOINT`.

```
urp add <name>
    --listen-path <path>         UDS socket path for listen mode
    --connect-path <path>        UDS socket path for connect mode
    --rdma-device <dev>          RDMA device (e.g. mlx5_0, rxe0)
    --peer-address <addr:port>   Remote RDMA peer (makes this side the RDMA initiator)
    --bind-address <addr:port>   Local RDMA bind (makes this side the RDMA acceptor)
    --num-qps <n>                Number of QPs (default: 1, max: 32)
    --buffer-count <n>           Buffer pool slots (default: 1024)
    --buffer-size <n>            Per-buffer size in bytes (default: 4076)
    --password <secret>          Tier 0.5 PSK (max 16 chars)
```

**Validation** (client-side, before sending to kernel):
- At least one of `--listen-path` or `--connect-path` must be specified
- Exactly one of `--peer-address` or `--bind-address` must be specified
- `--num-qps` must be 1-32
- `--password` must be at most 16 characters

**Examples**:

```bash
# Bidirectional endpoint for Redpanda peer B
urp add peer-b \
  --listen-path /var/run/urp/to-B.sock \
  --connect-path /var/run/redpanda/rpc.sock \
  --rdma-device mlx5_0 \
  --peer-address 10.0.1.2:4791 \
  --num-qps 8 \
  --buffer-count 1024 \
  --password mysecret

# Listen-only endpoint (client-side proxy, RDMA initiator)
urp add client \
  --listen-path /var/run/urp/app.sock \
  --rdma-device mlx5_0 \
  --peer-address 10.0.1.2:4791

# Connect-only endpoint (server-side proxy, RDMA acceptor)
urp add server \
  --connect-path /var/run/app/server.sock \
  --rdma-device mlx5_0 \
  --bind-address 0.0.0.0:4791
```

### 23.2.2 `urp remove`

Removes an endpoint. If the endpoint is in ACTIVE state, it is implicitly drained first (all streams receive FIN, then the endpoint is destroyed). Maps to `URP_CMD_DEL_ENDPOINT`.

```bash
urp remove peer-b
```

### 23.2.3 `urp set`

Modifies mutable attributes on a live endpoint. Only a subset of attributes can be changed after creation — see the mutability table in [Section 22.3.2](22-genl-interface.md#2232-endpoint-attributes). Maps to `URP_CMD_SET_ENDPOINT`.

```bash
# Scale up QPs on a live endpoint
urp set peer-b --num-qps 16

# Change the PSK password
urp set peer-b --password newsecret

# Increase buffer pool
urp set peer-b --buffer-count 2048
```

### 23.2.4 `urp show`

Displays endpoint configuration, state, per-QP health, and per-stream state. Without a name argument, dumps all endpoints. Maps to `URP_CMD_GET_ENDPOINT` (`doit` for one, `dumpit` for all).

```bash
# Show all endpoints
urp show

# Show one endpoint
urp show peer-b
```

**Default output format** (human-readable, inspired by `wg show`):

```
endpoint: peer-b
  listen-path: /var/run/urp/to-B.sock
  connect-path: /var/run/redpanda/rpc.sock
  rdma-device: mlx5_0
  peer-address: 10.0.1.2:4791
  num-qps: 8
  buffer-count: 1024
  buffer-size: 4076
  state: active
  active-streams: 12
  qps:
    qp[0]: state=active rtt=42us tx=1.2GB rx=890MB (3.1M/2.8M frames)
    qp[1]: state=active rtt=38us tx=1.1GB rx=920MB (2.9M/3.0M frames)
    qp[2]: state=qualifying rtt=-- tx=0B rx=0B (0/0 frames)
    ...
  streams:
    stream[1]: established tx=45MB rx=32MB reorder=0 credits=64/60
    stream[3]: established tx=120MB rx=98MB reorder=2 credits=58/64
    stream[5]: fin-wait tx=12MB rx=11MB reorder=0 credits=64/64
    ...

endpoint: server
  connect-path: /var/run/app/server.sock
  rdma-device: mlx5_0
  bind-address: 0.0.0.0:4791
  num-qps: 2
  state: active
  active-streams: 3
  ...
```

### 23.2.5 `urp stats`

Focused statistics view — shows aggregate counters without per-QP/per-stream detail. Useful for monitoring dashboards.

```bash
urp stats peer-b
```

**Output**:

```
endpoint: peer-b (active, 12 streams)
  tx: 15.2 GB (3.8M frames)  rx: 12.1 GB (3.1M frames)
  credit stalls: 42  reorder inserts: 1.2K  reorder drops: 0
  buffer alloc fails: 0  auth failures: 0
```

### 23.2.6 `urp monitor`

Subscribes to the GENL `"events"` multicast group and prints state changes as they occur. Runs until interrupted (Ctrl-C).

```bash
urp monitor
```

**Output**:

```
[2026-05-01 14:32:01.234] peer-b: state creating -> active
[2026-05-01 14:32:01.891] peer-b: qp[0] qualifying -> active (rtt=42us)
[2026-05-01 14:32:01.892] peer-b: qp[1] qualifying -> active (rtt=38us)
[2026-05-01 14:32:05.123] peer-b: new stream 7 (established)
[2026-05-01 14:33:12.456] peer-b: stream 7 closed
[2026-05-01 14:35:00.789] peer-b: qp[3] active -> draining
[2026-05-01 14:40:22.111] server: auth failure from 10.0.1.99 (hash mismatch)
```

### 23.2.7 `urp drain`

Initiates graceful drain on an endpoint — no new streams are accepted, existing streams receive FIN frames. The endpoint transitions from ACTIVE to DRAINING. Maps to `URP_CMD_SET_ENDPOINT` with `state=DRAINING`.

```bash
# Drain before maintenance
urp drain peer-b

# Wait for drain to complete, then remove
urp monitor &
urp drain peer-b
# [14:40:00] peer-b: state active -> draining
# [14:40:02] peer-b: stream 5 closed
# [14:40:03] peer-b: stream 3 closed
# [14:40:03] peer-b: state draining -> stopped
urp remove peer-b
```


## 23.3 Output Formats

All subcommands that produce output support three formats:

| Flag | Format | Use Case |
|------|--------|----------|
| (default) | Human-readable key-value | Terminal use, debugging |
| `--json` | JSON | Scripting, monitoring integration, `jq` pipelines |
| `--oneline` | Compact one-line-per-endpoint | Dashboards, quick status checks |

**JSON output example** (`urp show --json`):

```json
[
  {
    "name": "peer-b",
    "listen_path": "/var/run/urp/to-B.sock",
    "connect_path": "/var/run/redpanda/rpc.sock",
    "rdma_device": "mlx5_0",
    "peer_address": "10.0.1.2:4791",
    "num_qps": 8,
    "buffer_count": 1024,
    "buffer_size": 4076,
    "state": "active",
    "stats": {
      "active_streams": 12,
      "tx_bytes": 16327884800,
      "rx_bytes": 12993839104,
      "tx_frames": 3981234,
      "rx_frames": 3178456,
      "credit_stalls": 42,
      "reorder_insertions": 1234,
      "reorder_drops": 0,
      "buffer_alloc_fails": 0,
      "auth_failures": 0
    },
    "qps": [
      {"index": 0, "state": "active", "rtt_ns": 42000, "tx_bytes": 1288490188, "rx_bytes": 933957632}
    ],
    "streams": [
      {"id": 1, "state": "established", "tx_bytes": 47185920, "rx_bytes": 33554432, "reorder_depth": 0, "credits_local": 64, "credits_remote": 60}
    ]
  }
]
```

**Oneline output example** (`urp show --oneline`):

```
peer-b  active  8qps  12streams  tx=15.2GB  rx=12.1GB  /var/run/urp/to-B.sock<->10.0.1.2:4791
server  active  2qps  3streams   tx=1.8GB   rx=2.1GB   /var/run/app/server.sock<-0.0.0.0:4791
```


## 23.4 Rust Crate Structure

The CLI is implemented as a standalone Rust crate in the workspace:

```
crates/urp-cli/
  Cargo.toml
  src/
    main.rs              # CLI entry point, clap derive
    netlink.rs           # GENL socket management, message build/parse
    commands/
      mod.rs             # Command dispatch
      add.rs             # urp add — build NEW_ENDPOINT message
      remove.rs          # urp remove — build DEL_ENDPOINT message
      set.rs             # urp set — build SET_ENDPOINT message
      show.rs            # urp show — parse GET_ENDPOINT reply
      stats.rs           # urp stats — parse GET_ENDPOINT reply (stats subset)
      monitor.rs         # urp monitor — multicast subscription loop
      drain.rs           # urp drain — build SET_ENDPOINT with state=draining
    format.rs            # Output formatters: human, json, oneline
    uapi.rs              # Rust constants mirroring include/uapi/linux/urp.h
```

### 23.4.1 Dependencies

```toml
[package]
name = "urp-cli"
version = "0.1.0"
edition = "2021"

[[bin]]
name = "urp"
path = "src/main.rs"

[dependencies]
clap = { version = "4", features = ["derive"] }
neli = "0.7"
neli-proc-macros = "0.2"
serde = { version = "1", features = ["derive"] }
serde_json = "1"
```

- **`neli`**: Type-safe Generic Netlink for Rust. Handles socket creation, GENL family resolution via the controller, attribute encoding/decoding, and multicast group subscription.
- **`neli-proc-macros`**: Derive macros for generating `neli` enum types from Rust enums (maps to UAPI constants).
- **`clap`** (derive): CLI argument parsing with subcommand support.
- **`serde`** + **`serde_json`**: JSON output serialization.

### 23.4.2 UAPI Constants in Rust

`src/uapi.rs` mirrors the C UAPI header, using `neli` procedural macros for type-safe enum generation:

```rust
use neli_proc_macros::neli_enum;

pub const URP_GENL_NAME: &str = "urp";
pub const URP_GENL_VERSION: u8 = 1;

#[neli_enum(serialized_type = "u8")]
pub enum UrpCmd {
    NewEndpoint = 1,
    DelEndpoint = 2,
    SetEndpoint = 3,
    GetEndpoint = 4,
}

#[neli_enum(serialized_type = "u16")]
pub enum UrpAttr {
    Unspec = 0,
    Endpoint = 1,
}

#[neli_enum(serialized_type = "u16")]
pub enum UrpEndpointAttr {
    Unspec = 0,
    Name = 1,
    ListenPath = 2,
    ConnectPath = 3,
    RdmaDevice = 4,
    PeerAddr = 5,
    BindAddr = 6,
    NumQps = 7,
    BufferCount = 8,
    BufferSize = 9,
    Password = 10,
    State = 11,
    Qps = 12,
    Streams = 13,
    Stats = 14,
}

#[neli_enum(serialized_type = "u16")]
pub enum UrpQpAttr {
    Unspec = 0,
    Index = 1,
    State = 2,
    RttNs = 3,
    TxBytes = 4,
    RxBytes = 5,
    TxFrames = 6,
    RxFrames = 7,
}

// ... UrpStreamAttr, UrpStatsAttr, UrpEndpointState, UrpQpState, UrpStreamState
```

### 23.4.3 Netlink Communication

`src/netlink.rs` encapsulates the GENL socket and message construction:

```rust
use neli::consts::nl::{NlmF, NlmFFlags};
use neli::consts::socket::NlFamily;
use neli::genl::{Genlmsghdr, Nlattr};
use neli::nl::{NlPayload, Nlmsghdr};
use neli::socket::NlSocketHandle;

pub struct UrpSocket {
    socket: NlSocketHandle,
    family_id: u16,
}

impl UrpSocket {
    /// Open a GENL socket and resolve the "urp" family ID.
    pub fn connect() -> Result<Self, Box<dyn std::error::Error>> {
        let mut socket = NlSocketHandle::connect(NlFamily::Generic, None, &[])?;
        let family_id = socket.resolve_genl_family(URP_GENL_NAME)?;
        Ok(Self { socket, family_id })
    }

    /// Send a NEW_ENDPOINT command with the given endpoint attributes.
    pub fn new_endpoint(&mut self, attrs: Vec<Nlattr<UrpEndpointAttr, Vec<u8>>>)
        -> Result<(), Box<dyn std::error::Error>>
    {
        let endpoint_nest = Nlattr::new(
            false, false,
            UrpAttr::Endpoint,
            attrs,
        )?;

        let genl = Genlmsghdr::new(UrpCmd::NewEndpoint, URP_GENL_VERSION, vec![endpoint_nest]);
        let nl = Nlmsghdr::new(
            None,
            self.family_id,
            NlmFFlags::new(&[NlmF::Request, NlmF::Ack]),
            None,
            None,
            NlPayload::Payload(genl),
        );

        self.socket.send(nl)?;
        // Wait for ACK or error
        self.check_ack()
    }

    /// Send GET_ENDPOINT dump and parse all endpoint responses.
    pub fn dump_endpoints(&mut self) -> Result<Vec<Endpoint>, Box<dyn std::error::Error>> {
        // ... build dump request, iterate NLMSG_MULTI responses ...
    }

    /// Subscribe to the "events" multicast group for monitoring.
    pub fn subscribe_events(&mut self) -> Result<(), Box<dyn std::error::Error>> {
        let group_id = self.socket.resolve_nl_mcast_group(
            URP_GENL_NAME, "events"
        )?;
        self.socket.add_mcast_membership(&[group_id])?;
        Ok(())
    }
}
```

### 23.4.4 CLI Entry Point

`src/main.rs` uses `clap` derive for subcommand dispatch:

```rust
use clap::{Parser, Subcommand};

#[derive(Parser)]
#[command(name = "urp", about = "Manage UDS-RDMA proxy kernel module endpoints")]
struct Cli {
    #[command(subcommand)]
    command: Commands,

    /// Output format
    #[arg(long, global = true, default_value = "human")]
    format: OutputFormat,
}

#[derive(Subcommand)]
enum Commands {
    /// Create a new endpoint
    Add {
        name: String,
        #[arg(long)]
        listen_path: Option<String>,
        #[arg(long)]
        connect_path: Option<String>,
        #[arg(long)]
        rdma_device: Option<String>,
        #[arg(long)]
        peer_address: Option<String>,
        #[arg(long)]
        bind_address: Option<String>,
        #[arg(long, default_value = "1")]
        num_qps: u32,
        #[arg(long, default_value = "1024")]
        buffer_count: u32,
        #[arg(long, default_value = "4076")]
        buffer_size: u32,
        #[arg(long)]
        password: Option<String>,
    },
    /// Remove an endpoint
    Remove { name: String },
    /// Modify a live endpoint
    Set {
        name: String,
        #[arg(long)]
        num_qps: Option<u32>,
        #[arg(long)]
        buffer_count: Option<u32>,
        #[arg(long)]
        password: Option<String>,
    },
    /// Show endpoint config and state
    Show { name: Option<String> },
    /// Show focused statistics
    Stats { name: Option<String> },
    /// Subscribe to state-change events
    Monitor,
    /// Graceful drain (stop accepting new streams)
    Drain { name: String },
}
```


## 23.5 Error Handling

Kernel errors are returned via `NLMSG_ERROR` with an errno and optional extended ACK message (`NL_SET_ERR_MSG`). The CLI translates these into user-friendly output:

```
$ urp add peer-b --listen-path /var/run/urp/to-B.sock
Error: endpoint name already exists (EEXIST)

$ urp add bad --num-qps 64
Error: num-qps must be 1-32 (EINVAL)

$ urp remove nonexistent
Error: endpoint not found (ENOENT)

$ urp add test --listen-path /tmp/test.sock
Error: urp kernel module not loaded (family "urp" not found)
```

The "module not loaded" case is detected during GENL family resolution — if the controller returns `ENOENT` for the `"urp"` family, the module isn't loaded.


## 23.6 Deployment with systemd

The `urp` CLI replaces both `module_param` and configfs for boot-time endpoint setup. A systemd oneshot service runs `urp add` commands after the kernel module is loaded:

```ini
# /etc/systemd/system/urp.service
[Unit]
Description=Load UDS-RDMA proxy kernel module
DefaultDependencies=no
After=systemd-modules-load.service
Before=network.target

[Service]
Type=oneshot
ExecStart=/sbin/modprobe urp
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
```

```ini
# /etc/systemd/system/urp-endpoints.service
[Unit]
Description=Configure UDS-RDMA proxy endpoints
After=urp.service network-online.target
Requires=urp.service
Wants=network-online.target

[Service]
Type=oneshot
ExecStart=/usr/bin/urp add peer-b --listen-path /var/run/urp/to-B.sock --connect-path /var/run/redpanda/rpc.sock --peer-address 10.0.1.2:4791 --num-qps 8 --password mysecret
ExecStart=/usr/bin/urp add peer-c --listen-path /var/run/urp/to-C.sock --connect-path /var/run/redpanda/rpc.sock --peer-address 10.0.1.3:4791 --num-qps 8 --password mysecret
RemainAfterExit=yes
ExecStop=/usr/bin/urp drain peer-b
ExecStop=/usr/bin/urp drain peer-c
ExecStop=/usr/bin/urp remove peer-b
ExecStop=/usr/bin/urp remove peer-c

[Install]
WantedBy=multi-user.target
```

For a templated multi-peer deployment:

```ini
# /etc/systemd/system/urp-endpoint@.service
[Unit]
Description=UDS-RDMA proxy endpoint %i
After=urp.service network-online.target
Requires=urp.service

[Service]
Type=oneshot
EnvironmentFile=/etc/urp/endpoints/%i.conf
ExecStart=/usr/bin/urp add %i ${URP_ARGS}
RemainAfterExit=yes
ExecStop=/usr/bin/urp drain %i
ExecStop=/usr/bin/urp remove %i

[Install]
WantedBy=multi-user.target
```

```bash
# /etc/urp/endpoints/peer-b.conf
URP_ARGS=--listen-path /var/run/urp/to-B.sock --connect-path /var/run/redpanda/rpc.sock --peer-address 10.0.1.2:4791 --num-qps 8 --password mysecret
```

Enable with: `systemctl enable urp-endpoint@peer-b.service`


## 23.7 Integration with Monitoring

The `urp stats --json` output is designed for easy integration with monitoring systems:

```bash
# Prometheus node_exporter textfile collector
urp stats --json | jq -r '
  .[] | "urp_tx_bytes{endpoint=\"\(.name)\"} \(.stats.tx_bytes)
urp_rx_bytes{endpoint=\"\(.name)\"} \(.stats.rx_bytes)
urp_active_streams{endpoint=\"\(.name)\"} \(.stats.active_streams)
urp_credit_stalls{endpoint=\"\(.name)\"} \(.stats.credit_stalls)"
' > /var/lib/prometheus/node-exporter/urp.prom

# Periodic stats collection (cron or systemd timer)
*/15 * * * * /usr/bin/urp stats --json >> /var/log/urp/stats.jsonl
```

The `/proc/urp/*` interface (see [Section 21](21-kernel-module.md)) provides a complementary stats path for tools that expect procfs — but GENL via `urp stats` is the primary interface.


[Back to Design Overview](../DESIGN.md)
