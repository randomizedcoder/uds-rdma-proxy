# Security Considerations

## 17.1 Security Tier Overview

The proxy provides a tiered security model. Operators choose the level appropriate for their deployment:

| Tier | Authentication | Encryption | CPU Overhead | When to Use |
|------|---------------|------------|-------------|-------------|
| **0** | Peer address allowlist | None | Zero | Trusted fabric with network isolation (industry default) |
| **0.5** | Shared password (PSK) | None | Negligible | Trusted fabric, protect against accidental misconfiguration |
| **1** (future) | mTLS certificates | None | Handshake only | Trusted fabric, need identity verification |
| **2** (future) | mTLS certificates | TLS 1.3 software | Moderate (AES-NI) | Untrusted fabric, no HW offload available |
| **2-HW** (future) | mTLS certificates | IPsec inline HW | Zero (NIC offload) | Untrusted fabric, ConnectX-6 Dx+ NIC |

See [Appendix: RoCEv2 Security Practices](appendix-rocev2-security.md) for industry context justifying this tiered approach.

## 17.2 UDS Socket Permissions

The proxy's UDS socket file (created via `listen_path`) must have restrictive permissions:

```rust
// Create UDS socket with mode 0660 (owner + group only)
let listener = UnixListener::bind(path)?;
fs::set_permissions(path, Permissions::from_mode(0o660))?;
```

The proxy should support configuring the socket owner, group, and mode via the `[uds]` configuration section. Sockets created via `connect_path` are not owned by the proxy — the target application controls their permissions.

## 17.3 Buffer Security

Registered memory regions are accessible by the NIC via DMA. To prevent information leakage between connections:

- **Zero buffers** before returning to the pool (or before first use after allocation)
- In multi-tenant environments, consider zeroing on every allocation (trade-off: memset overhead vs. security)
- Buffer pool memory is `mlock`'d (pinned), so it won't be swapped to disk

## 17.4 Denial of Service

A malicious UDS client could exhaust proxy resources:

| Attack | Mitigation |
|--------|------------|
| Many connections | Configurable `max_connections` limit |
| Slow read (Slowloris-style) | Per-connection inactivity timeout |
| Large writes | Frame size limit (enforced by buffer slot size) |
| Connection churn | Rate limit new connections per second |

## 17.5 RDMA Security (Tier 0)

- RDMA QP keys (`rkey`, `lkey`) are exchanged during connection setup. With two-sided SEND/RECV, remote keys are not exposed (unlike one-sided RDMA READ/WRITE).
- The proxy should only accept RDMA connections from configured peer addresses (`peer_address` / `bind_address`).
- No credential forwarding: the remote application appears to the local kernel as the proxy process. Authentication must happen at the application level.

## 17.6 Tier 0.5: Shared Password (PSK)

A simple pre-shared key mechanism that protects against **accidental misconfiguration** — the same threat model as VRRP Type 1 authentication (RFC 5798). This is NOT a defense against active attackers; the hash is static and replayable. Its purpose is to prevent two proxies that should not be connected from accidentally forming a tunnel.

### Configuration

```toml
[security]
password = "mysecretpassword"    # Up to 16 characters. Omit or leave empty for no auth.
```

### Wire Protocol

The SHA-256 hash of the configured password is carried in the RDMA CM `private_data` field during connection setup. No new frame types are required.

**`private_data` layout** (see also [Section 5.1](05-rdma-transport.md#51-connection-establishment)):

| Field | Size | Description |
|-------|------|-------------|
| `num_qps` | 4B | Existing |
| `initial_credits` | 4B | Existing |
| `buffer_size` | 4B | Existing |
| `max_payload` | 4B | Existing |
| `auth_method` | 1B | `0` = none, `1` = password, `2` = certificate (future) |
| `auth_hash` | 32B | SHA-256(password) when `auth_method=1`, zeroed when `auth_method=0` |
| **Total** | **49B** | << 196B RC limit |

### Authentication Flow

```
 RDMA Initiator                                RDMA Acceptor

 1. Compute hash = SHA-256(password)
 2. rdma_connect(private_data = {
      ...params...,
      auth_method: 1,
      auth_hash: hash
    })
                                               3. RDMA_CM_EVENT_CONNECT_REQUEST
                                               4. Extract auth_method, auth_hash
                                               5. Compute local_hash = SHA-256(own password)
                                               6. Compare auth_hash == local_hash
                                                  │
                                                  ├── mismatch → rdma_reject()
                                                  │              log "auth failed from <peer>"
                                                  │
                                                  └── match → rdma_accept(private_data = {
                                                                ...params...,
                                                                auth_method: 1,
                                                                auth_hash: local_hash
                                                              })
 7. RDMA_CM_EVENT_ESTABLISHED
 8. Verify acceptor's auth_hash == hash
    (reject if mismatch — should not happen
     if both sides have the same password)

 --- QP set authenticated, streams can begin ---
```

### Compatibility

If either side has `auth_method=0` (no password configured), the authentication step is skipped. If one side has a password and the other doesn't, the side with the password will reject the connection (it will see `auth_method=0` from the peer while expecting `auth_method=1`).

## 17.7 Tier 1: Certificate-Based Mutual Authentication (Future)

For deployments that require cryptographic identity verification without data plane encryption. Uses X.509 certificates from PEM files on disk (infrastructure-agnostic — works with cert-manager, Vault PKI, SPIFFE, or manual provisioning).

### Design Sketch

The 196-byte `private_data` field is too small for full X.509 certificates (typically 1-2 KB). Authentication uses a two-phase approach:

**Phase 1 (rdma_cm `private_data`)**: Exchange certificate fingerprints (SHA-256, 32B) and a random nonce (32B). Both sides look up the full certificate locally using the fingerprint.

**Phase 2 (post-QP, `stream_id=0` CONTROL frames)**: After QPs are established but before application streams are allowed, exchange full certificate chains and signed challenges over the RDMA data path using CONTROL frames with the `AUTH` flag (Bit 4, see [Section 4.5](04-framing-protocol.md#45-per-type-flag-definitions)).

An `AUTHENTICATING` state is inserted into the connection lifecycle — SYN frames for application streams are rejected until authentication completes.

### Configuration

```toml
[security]
mode = "auth"                        # Tier 1
cert_file = "/etc/certs/proxy.crt"   # PEM certificate
key_file = "/etc/certs/proxy.key"    # PEM private key
ca_file = "/etc/certs/ca.crt"        # CA certificate for peer verification
auth_timeout = "5s"                  # Max time for post-QP auth handshake
```

## 17.8 Tier 2: Software TLS over RDMA (Future)

For deployments where the RDMA fabric is not fully trusted and hardware crypto offload is unavailable. TLS 1.3 records are encapsulated inside DATA frame payloads, reusing the Tier 1 certificate infrastructure for the TLS handshake.

**Performance considerations**: Modern CPUs with AES-NI hardware instructions can achieve reasonable throughput (5-15+ Gbps per core for AES-256-GCM). This is slower than unencrypted RDMA on 25-100 Gbps links but may be acceptable for workloads that are not bandwidth-bound. Benchmarking on the target hardware is recommended before dismissing this option.

**Tradeoffs**:
- Adds per-frame latency for encrypt/decrypt
- Reduces the CPU offload advantage of RDMA (encryption is CPU-bound)
- Still faster end-to-end than TCP + TLS for latency-sensitive workloads

## 17.9 Tier 2-HW: Hardware IPsec Offload (Future)

ConnectX-6 Dx and ConnectX-7 NICs support IPsec inline crypto offload, encrypting and decrypting packets at line rate (100-200+ Gbps) in NIC hardware. This is transparent to the proxy — the data path code is unchanged.

```
 Proxy ibv_post_send(plaintext) → NIC IPsec engine (encrypt) → wire (ESP) →
 → Remote NIC IPsec engine (decrypt) → ibv_poll_cq(plaintext)
```

**Advantages**:
- Zero CPU overhead for encryption/decryption
- Zero additional latency (sub-microsecond NIC crypto pipeline)
- Preserves RDMA's performance characteristics entirely

**Integration**: IPsec Security Associations (SAs) are configured out-of-band via `ip xfrm` commands or a key management daemon (strongSwan, Libreswan). The Tier 1 certificate handshake could derive IKEv2 keying material to establish SAs programmatically. Alternatively, the proxy can invoke `mlx5dv_create_flow_action_esp()` to configure inline IPsec on its QPs.

**NIC requirements**: ConnectX-6 Dx or later with IPsec offload firmware enabled.


[Back to Design Overview](../DESIGN.md)
