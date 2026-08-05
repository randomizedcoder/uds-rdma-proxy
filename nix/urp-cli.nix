# Build the `urp` userspace control CLI.
#
# It speaks the kernel module's generic-netlink ("urp") family to add /
# remove / inspect endpoints. Pure Rust, no extra link deps.
#
# rustToolchain is optional: when provided (native builds) it is prepended
# to PATH so the workspace's pinned nightly toolchain is used. When omitted
# -- as for the Phase 5 cross-arch builds where `pkgs` is a pkgsCross.<arch>
# set -- the derivation falls back to `pkgs.rustPlatform` (nixpkgs' stable,
# cross-aware toolchain). urp-cli is edition-2021 with no nightly features,
# so stable builds it fine.
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
        baseName == "urp-cli" ||
        baseName == "src" ||
        baseName == "commands"
      )) ||
      # Plus the kernel UAPI header -- urp-cli's tests re-parse it for
      # constant-consistency checks against the Rust mirror.
      (type == "directory" && (
        baseName == "kernel" ||
        baseName == "include" ||
        baseName == "uapi" ||
        baseName == "linux"
      )) ||
      (baseName == "Cargo.toml") ||
      (baseName == "Cargo.lock") ||
      (pkgs.lib.hasSuffix ".rs" baseName) ||
      (pkgs.lib.hasSuffix ".h" baseName);
  };
in
pkgs.rustPlatform.buildRustPackage {
  pname = "urp-cli";
  version = "0.1.0";
  inherit src;

  cargoLock.lockFile = ../Cargo.lock;

  cargoBuildFlags = [ "-p" "urp-cli" ];
  cargoTestFlags  = [ "-p" "urp-cli" ];

  # Cross builds run the CLI's tests under an emulator they may not have,
  # so skip the check phase when cross-compiling (rustToolchain == null).
  doCheck = rustToolchain != null;

  nativeBuildInputs = pkgs.lib.optional (rustToolchain != null) rustToolchain;

  meta = with pkgs.lib; {
    description = "Control plane CLI for the urp kernel module (generic netlink)";
    license = licenses.gpl2Only;
    mainProgram = "urp";
  };
}
