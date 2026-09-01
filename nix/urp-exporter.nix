# Build the `urp-exporter` Prometheus exporter (design 39).
#
# It scrapes the kernel module's generic-netlink ("urp") counter surface via
# the shared `urp-netlink` lib and serves /metrics from a single blocking
# thread -- no async runtime, no HTTP framework. Same build shape as
# urp-cli.nix (vendored Cargo.lock, optional pinned nightly toolchain).
#
# rustToolchain is optional: when provided (native builds) it is prepended to
# PATH so the workspace's pinned nightly toolchain is used. When omitted -- as
# for the Phase 5 cross-arch builds where `pkgs` is a pkgsCross.<arch> set --
# the derivation falls back to `pkgs.rustPlatform`. urp-exporter is
# edition-2021 with no nightly features, so stable builds it fine.
{ pkgs, rustToolchain ? null }:

let
  src = builtins.path {
    path = ../.;
    name = "uds-rdma-proxy-src";
    filter = path: type:
      let baseName = builtins.baseNameOf path; in
      # Keep the workspace skeleton + every crate under crates/.
      (type == "directory" && (
        baseName == "crates" ||
        baseName == "uds-rdma-protocol" ||
        baseName == "uds-rdma-protocol-ffi" ||
        baseName == "urp-bench" ||
        baseName == "urp-cli" ||
        baseName == "urp-netlink" ||
        baseName == "urp-control" ||
        baseName == "urp-exporter" ||
        baseName == "src" ||
        baseName == "commands" ||
        baseName == "tests"
      )) ||
      # Plus the kernel UAPI header -- urp-cli's tests re-parse it for
      # constant-consistency checks against the Rust mirror.
      (type == "directory" && (
        baseName == "kernel" ||
        baseName == "include" ||
        baseName == "uapi" ||
        baseName == "linux"
      )) ||
      # Plus the proto tree -- urp-control's build.rs codegens from it, and
      # cargo parses every workspace member even for `-p urp-exporter`.
      (type == "directory" && (
        baseName == "proto" ||
        baseName == "urp_control" ||
        baseName == "v1"
      )) ||
      (baseName == "Cargo.toml") ||
      (baseName == "Cargo.lock") ||
      (pkgs.lib.hasSuffix ".rs" baseName) ||
      (pkgs.lib.hasSuffix ".h" baseName) ||
      (pkgs.lib.hasSuffix ".proto" baseName);
  };
in
pkgs.rustPlatform.buildRustPackage {
  pname = "urp-exporter";
  version = "0.1.0";
  inherit src;

  cargoLock.lockFile = ../Cargo.lock;

  cargoBuildFlags = [ "-p" "urp-exporter" ];
  cargoTestFlags  = [ "-p" "urp-exporter" ];

  # Cross builds run the tests under an emulator they may not have, so skip the
  # check phase when cross-compiling (rustToolchain == null).
  doCheck = rustToolchain != null;

  nativeBuildInputs = pkgs.lib.optional (rustToolchain != null) rustToolchain;

  meta = with pkgs.lib; {
    description = "Prometheus exporter for the urp kernel module (generic netlink)";
    license = licenses.gpl2Only;
    mainProgram = "urp-exporter";
  };
}
