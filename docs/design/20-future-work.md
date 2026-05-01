# Open Questions & Future Work

## 20.1 Kernel Bypass for UDS Side

Could `AF_XDP` (Express Data Path) or `io_uring` zero-copy features be used to eliminate the kernel copies on the UDS side? This would require the application to use a compatible socket type, partially defeating the "transparent proxy" goal, but could be offered as an optional fast path for aware applications.

> **See also**: [Section 21 — Kernel Module Alternative](21-kernel-module.md) explores a more radical approach: implementing the entire proxy as a kernel module, eliminating the UDS copies entirely by intercepting socket operations before they leave kernel space (2 copies instead of 4, with zero-copy potential via page flipping).

## 20.2 Shared Memory Fast Path

If both the application and the proxy are on the same host (which they always are, by definition), they could share a memory region directly instead of going through UDS. The application would write directly into RDMA-registered buffers. This eliminates the UDS copy entirely but requires application modification (a client library).

> **See also**: [Section 21.7](21-kernel-module.md#217-code-sharing-strategy) proposes extracting a `uds-rdma-protocol` crate (`no_std + alloc`) that could also serve as the foundation for a shared-memory client library — the frame codec and credit state machine are reusable regardless of transport.

## 20.3 Falcon Hardware Reordering

Google's Falcon NIC handles multi-path reordering in hardware. When Falcon hardware is available, the proxy's software reorder buffer becomes unnecessary. The proxy should have a configuration flag to disable software reordering when the hardware provides it.

## 20.4 Optional Compression

For bandwidth-limited links or compressible payloads, optional LZ4 or zstd compression of frame payloads could improve effective throughput. LZ4 is preferred for its speed (>1GB/s encode/decode). This should be negotiated during connection setup and configurable per-connection.

## 20.5 Security Tiers 1, 2, and 2-HW

The tiered security architecture is defined in [Section 17](17-security.md). Tier 0.5 (shared password) is the initial implementation target. Remaining work:

- **Tier 1 (certificate-based mTLS)**: Implement the two-phase authentication protocol over `stream_id=0` CONTROL frames with the `AUTH` flag. Support PEM certificate files on disk.
- **Tier 2 (software TLS)**: Encapsulate TLS 1.3 records inside DATA frame payloads. Modern CPUs with AES-NI may perform better than expected — benchmarking on target hardware should inform whether this tier is practical.
- **Tier 2-HW (IPsec offload)**: Integrate with ConnectX-6 Dx+ inline IPsec via `mlx5dv_create_flow_action_esp()` or out-of-band SA configuration. Zero CPU overhead path.

See [Appendix: RoCEv2 Security Practices](appendix-rocev2-security.md) for industry context.

## 20.6 Multi-Host Mesh

For deployments with more than two machines, the proxy could form a mesh network where any machine can reach any other machine. This would require a control plane for route discovery and QP management across multiple peers.

## 20.7 Adaptive Buffer Sizing

Instead of fixed-size buffer slots, dynamically adjust slot sizes based on observed message size distributions. Small messages use small slots (reducing waste), large messages use large slots (reducing fragmentation). Requires a more complex slab allocator but could significantly reduce memory usage for mixed workloads.


[Back to Design Overview](../DESIGN.md)
