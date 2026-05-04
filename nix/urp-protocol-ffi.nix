{ pkgs, rustToolchain }:

# Builds the `uds-rdma-protocol-ffi` crate as a `staticlib` -- a `.a`
# archive the kernel module links against when CONFIG_URP_REORDER_RUST=y.
# The archive provides C-ABI exports for the Rust `ReorderBuffer`; see
# crates/uds-rdma-protocol-ffi/src/ffi.rs for the surface and
# kernel/include/urp_ffi.h for the matching C prototypes.
#
# The kernel module supplies three symbols (urp_kalloc, urp_kfree,
# urp_panic_abort) that the staticlib pulls in at link time. Those
# symbols are NOT defined in the .a -- they're undefined references that
# the kernel-module link resolves.
#
# Output layout:
#   $out/lib/liburp_protocol_ffi.a   (renamed from cargo's
#                                     libuds_rdma_protocol_ffi.a so the
#                                     kernel Kbuild rule has a stable,
#                                     short name)
#   $out/include/urp_ffi.h           (the matching C header)

let
  src = builtins.path {
    path = ../.;
    name = "uds-rdma-proxy-ffi-src";
    filter = path: type:
      let baseName = builtins.baseNameOf path; in
      # Include all workspace member directories. The workspace Cargo.toml
      # references urp-cli, so cargo needs to read every member's manifest
      # even when we only build the FFI crate.
      (type == "directory" && (
        baseName == "crates" ||
        baseName == "uds-rdma-protocol" ||
        baseName == "uds-rdma-protocol-ffi" ||
        baseName == "urp-cli" ||
        baseName == "src" ||
        baseName == "commands"
      )) ||
      (baseName == "Cargo.toml") ||
      (baseName == "Cargo.lock") ||
      (pkgs.lib.hasSuffix ".rs" baseName);
  };
in
pkgs.rustPlatform.buildRustPackage {
  pname = "urp-protocol-ffi";
  version = "0.1.0";
  inherit src;

  cargoLock.lockFile = ../Cargo.lock;

  # Build only the FFI crate as a release staticlib. The workspace's
  # release profile pins panic=abort + LTO + size optimization.
  cargoBuildFlags = [ "-p" "uds-rdma-protocol-ffi" "--lib" ];
  doCheck = false;  # tests live in `uds-rdma-protocol`, not here.

  nativeBuildInputs = [ rustToolchain ];

  installPhase = ''
    runHook preInstall
    mkdir -p $out/lib $out/include
    # Stable archive name for the kernel Kbuild rule.
    cp target/*/release/libuds_rdma_protocol_ffi.a $out/lib/liburp_protocol_ffi.a
    # Ship the matching C header alongside, so the kernel build can
    # ${"#"}include <urp_ffi.h> from $out/include.
    cp ${../kernel/include/urp_ffi.h} $out/include/urp_ffi.h
    runHook postInstall
  '';

  meta = with pkgs.lib; {
    description = "C-ABI staticlib of uds-rdma-protocol for the urp.ko kernel module";
    license = with licenses; [ mit asl20 ];
  };
}
