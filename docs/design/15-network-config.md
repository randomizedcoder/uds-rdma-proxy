# Network Configuration Requirements

## 15.1 Software RDMA (Development/Testing)

No special network configuration required. `rdma_rxe` runs over any Ethernet interface. Performance is not representative of hardware RDMA (all processing is in software, goes through the kernel network stack) but functional fidelity is high.

```bash
# Minimal setup
modprobe rdma_rxe
rdma link add rxe0 type rxe netdev eth0
```

## 15.2 RoCEv2 Production

RoCEv2 expects a properly configured Ethernet fabric. Without proper configuration, performance can be catastrophically bad due to packet drops causing Go-Back-N retransmissions.

**Required**:

| Feature | Purpose | Configuration |
|---------|---------|---------------|
| **PFC (Priority Flow Control)** | Prevents packet drops on congested ports | Enable on the RoCEv2 traffic class (typically TC 3). Configure on every switch and NIC in the path. |
| **ECN (Explicit Congestion Notification)** | Marks packets approaching congestion instead of dropping | Enable ECN on the RoCEv2 traffic class. |
| **DCQCN** | Congestion control protocol for RoCEv2 | NIC-level configuration. Reacts to ECN marks by reducing send rate. |
| **DSCP Marking** | Classifies RoCEv2 traffic for PFC/ECN treatment | Typically DSCP 26 (AF31) for data, DSCP 48 for CNP (Congestion Notification Packets). |

**Recommended**:

| Feature | Purpose |
|---------|---------|
| **Jumbo frames (MTU 9000)** | Larger RDMA messages per Ethernet frame, reduces per-packet overhead |
| **PFC watchdog** | Detects PFC storms (pathological feedback loops) and breaks them |
| **RDMA traffic isolation** | Separate VLAN or traffic class for RDMA to isolate from general traffic |

## 15.3 MTU Considerations

| Ethernet MTU | IB PMTU | Max single-packet RDMA payload | Notes |
|-------------|---------|-------------------------------|-------|
| 1496 (VLAN) | 1024 | 1024 bytes | VLAN-tagged networks with conservative MTU. |
| 1500 | 1024 | 1024 bytes | Standard Ethernet. Messages > 1024 are segmented. |
| 9000 | 4096 | 4096 bytes | Jumbo frames. Good match for 4KB buffer slots. |
| 9216 | 4096 | 4096 bytes | Common jumbo ceiling. Same PMTU as 9000. |

The proxy's `buffer_size` should match the IB PMTU for single-packet frames. See [Section 4.6](04-framing-protocol.md#46-mtu-and-payload-sizing) for the full header overhead breakdown and recommended payload sizes.


[Back to Design Overview](../DESIGN.md)
