# Appendix: RoCEv2 Security Practices in Production

This appendix surveys how RoCEv2 and RDMA networks are secured in production deployments, justifying the project's tiered security model ([Section 17](17-security.md)).

## Industry Survey

### Cloud Providers

| Provider | RDMA Technology | Encryption | Security Model |
|----------|----------------|------------|----------------|
| **Azure** | InfiniBand (HPC), RoCEv2 (storage) | None on RDMA path | Physically separate fabric; SmartNIC (FPGA) enforces tenant isolation at hardware level |
| **AWS EFA** | Elastic Fabric Adapter (custom) | None on EFA path | VPC network isolation; Nitro card enforces tenant boundaries in hardware; EFA docs explicitly note RDMA traffic is unencrypted |
| **Google (Falcon)** | Custom Falcon NIC | None | Physically isolated fabric within data center; Google's overall DC security model |
| **Oracle Cloud** | Cluster Networking (RoCEv2) | None | Dedicated, non-routable RDMA network; no cross-tenant RDMA traffic possible by design |

### Storage Systems

| System | RDMA Transport | Encryption | Auth Mechanism |
|--------|---------------|------------|----------------|
| **Ceph** (msgr) | RoCEv2 via librdmacm | None | cephx (shared-secret, protocol-level); dedicated storage VLAN recommended |
| **SPDK** (NVMe-oF) | RDMA SEND/RECV | None | NVMe-oF spec added TLS for TCP transport only; RDMA transport has no TLS; fabric zoning |
| **DAOS** (Intel) | libfabric/OFI | None | Fabric isolation |
| **GPUDirect RDMA** | RDMA WRITE | None | Assumed dedicated, trusted network |

### HPC / MPI

MPI implementations (OpenMPI, MVAPICH, Intel MPI) over RDMA/InfiniBand do not encrypt data. HPC clusters typically have physically isolated InfiniBand fabrics. The security boundary is the cluster itself. Job schedulers (SLURM, PBS) control access to the cluster, not encryption of the fabric.

### Financial Services

Ultra-low-latency trading systems use RDMA on dedicated, air-gapped networks with physical security controls rather than encryption. The performance cost of any encryption is considered unacceptable for latency-sensitive market data and order execution paths.

## Common Security Practices

| Practice | Adoption | Description |
|----------|----------|-------------|
| **Dedicated RDMA VLAN** | Very common | Separate VLAN for RDMA traffic, not routable from general network |
| **Physical network isolation** | Common (cloud, HPC) | Dedicated switches/fabric for RDMA; not shared with non-RDMA traffic |
| **Subnet Manager ACLs** | Common (IB only) | InfiniBand Subnet Manager controls which nodes can join the fabric |
| **PKey partitioning** | Common (IB only) | Partition keys isolate traffic within a shared InfiniBand fabric |
| **MACsec (802.1AE)** | Emerging | Link-layer encryption on Ethernet; ConnectX-6 Dx+ supports HW offload |
| **IPsec inline offload** | Emerging | Per-flow IPsec at NIC line rate; ConnectX-6 Dx+ supports this |
| **Application-level auth** | Variable | Some apps authenticate at the protocol level (Ceph cephx, NVMe-oF auth) |
| **TLS over RDMA** | Rare | Almost never used in production; defeats RDMA performance advantages |

## NIC Crypto Capabilities (Mellanox/NVIDIA ConnectX)

| NIC | IPsec Inline | MACsec | Line Rate | Notes |
|-----|-------------|--------|-----------|-------|
| ConnectX-5 | No | No | 100G | Basic RDMA, no crypto offload |
| ConnectX-6 | No | No | 200G | PFC, ECN improvements |
| ConnectX-6 Dx | Yes | Yes | 100G | First generation with inline crypto |
| ConnectX-6 Lx | No | No | 25G | Lower-cost variant, no crypto |
| ConnectX-7 | Yes | Yes | 200G+ | Full crypto offload at 200+ Gbps |
| BlueField-2 DPU | Yes | Yes | 100G | Fully programmable SmartNIC/DPU |
| BlueField-3 DPU | Yes | Yes | 400G | IPsec + TLS offload |

## Key Takeaway

**Network isolation is the industry standard for RDMA security.** No major cloud provider, storage system, or HPC framework encrypts RDMA traffic on the data path. Security relies on fabric isolation (VLANs, separate physical networks, hardware-enforced tenant boundaries) rather than cryptography.

This project's tiered approach matches these norms:
- **Tier 0** (network isolation) is what everyone does today
- **Tier 0.5** (shared password) adds lightweight accidental-mismatch protection — analogous to VRRP or OSPF simple authentication
- **Tier 1** (certificate auth) provides cryptographic identity verification for environments that need it, without data plane overhead
- **Tier 2-HW** (IPsec offload) provides encryption at line rate for the rare cases where fabric encryption is required, leveraging NIC hardware that is increasingly available

Software TLS over RDMA (Tier 2) is documented for completeness but is not recommended as the primary encryption path. Hardware offload (Tier 2-HW) preserves the performance properties that make RDMA worthwhile.


[Back to Design Overview](../DESIGN.md)
