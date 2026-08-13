# Deployment Model

> **Status: historical (userspace-proxy era, 2026-05).** This document
> describes the original *userspace Rust proxy* design, which was superseded:
> the project was implemented as a **Linux kernel module** instead — see
> [DESIGN.md](../DESIGN.md) and [21-kernel-module.md](21-kernel-module.md).
> Retained for design rationale and history. Details below (crates, io_uring,
> tokio, TOML config, Prometheus, the v0–v4 roadmap) do not match the
> implementation.

## 18.1 Systemd Service

```ini
[Unit]
Description=UDS-RDMA Proxy
After=network-online.target rdma.service
Wants=network-online.target

[Service]
Type=notify
ExecStart=/usr/local/bin/uds-rdma-proxy --config /etc/uds-rdma-proxy/peer-B.toml
Restart=on-failure
RestartSec=5

# Capabilities
AmbientCapabilities=CAP_IPC_LOCK CAP_NET_ADMIN CAP_SYS_NICE
NoNewPrivileges=true

# Resource limits
LimitMEMLOCK=infinity
LimitNOFILE=65536

# Security hardening
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/run/urp /run/uds-rdma-proxy

[Install]
WantedBy=multi-user.target
```

**Key capabilities**:
- `CAP_IPC_LOCK`: Required for `mlock` (pinning buffer memory)
- `CAP_NET_ADMIN`: Required for RDMA device access
- `CAP_SYS_NICE`: Required for `IORING_SETUP_SQPOLL` and CPU affinity

The proxy should support `sd_notify()` for readiness notification (signal `READY=1` after RDMA connections are established and UDS endpoints are ready).

For **multi-peer cluster deployments**, run one systemd service instance per peer using templated units:

```ini
# /etc/systemd/system/uds-rdma-proxy@.service
[Unit]
Description=UDS-RDMA Proxy (peer %i)
After=network-online.target rdma.service

[Service]
Type=notify
ExecStart=/usr/local/bin/uds-rdma-proxy --config /etc/uds-rdma-proxy/peer-%i.toml
# ... (same capabilities and security settings as above)

[Install]
WantedBy=multi-user.target
```

```bash
# Enable proxy instances for peers B and C:
systemctl enable --now uds-rdma-proxy@B
systemctl enable --now uds-rdma-proxy@C
```

## 18.2 Container Deployment

```dockerfile
FROM rust:latest as builder
COPY . .
RUN cargo build --release

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y rdma-core libibverbs1 librdmacm1
COPY --from=builder /target/release/uds-rdma-proxy /usr/local/bin/
ENTRYPOINT ["uds-rdma-proxy"]
```

**Runtime requirements**:
- `--privileged` or specific device access (`/dev/infiniband/uverbs0`, `/dev/infiniband/rdma_cm`)
- `--ulimit memlock=-1:-1` (unlimited locked memory)
- Volume mount for UDS socket path

## 18.3 Kubernetes

Deploy as a **DaemonSet** with an RDMA device plugin (e.g., Mellanox `k8s-rdma-shared-dev-plugin`):

```yaml
apiVersion: apps/v1
kind: DaemonSet
metadata:
  name: uds-rdma-proxy
spec:
  template:
    spec:
      containers:
      - name: proxy
        image: uds-rdma-proxy:latest
        resources:
          limits:
            rdma/hca: 1  # Request RDMA device via device plugin
        securityContext:
          capabilities:
            add: [IPC_LOCK, NET_ADMIN, SYS_NICE]
        volumeMounts:
        - name: uds-socket
          mountPath: /run/uds-rdma-proxy
      volumes:
      - name: uds-socket
        hostPath:
          path: /run/uds-rdma-proxy
```


[Back to Design Overview](../DESIGN.md)
