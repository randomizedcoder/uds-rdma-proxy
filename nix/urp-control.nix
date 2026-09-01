# Build the `urp-control` control-plane gRPC daemon (design 33 Phase 3).
#
# The acceptor serves Rendezvous/Heartbeat; the initiator gates the app on the
# peer reporting ready. It links the shared `urp-netlink` lib to read its own
# endpoint state over generic-netlink (no subprocess).
#
# Unlike urp-cli this crate has a build.rs that runs `protoc` (tonic-build) to
# codegen the proto, so `pkgs.protobuf` is a nativeBuildInput and PROTOC is set
# for the offline sandbox.
#
# rustToolchain is optional (see urp-cli.nix); the crate is edition-2021 with no
# nightly features, so nixpkgs' stable toolchain builds it when cross.
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
        baseName == "urp-control" ||
        baseName == "urp-netlink" ||
        baseName == "urp-exporter" ||
        baseName == "src" ||
        baseName == "commands" ||
        baseName == "tests"
      )) ||
      # Plus the kernel UAPI header -- urp-netlink's tests re-parse it.
      (type == "directory" && (
        baseName == "kernel" ||
        baseName == "include" ||
        baseName == "uapi" ||
        baseName == "linux"
      )) ||
      # Plus the proto tree (build.rs codegen input).
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
  pname = "urp-control";
  version = "0.1.0";
  inherit src;

  cargoLock.lockFile = ../Cargo.lock;

  cargoBuildFlags = [ "-p" "urp-control" ];
  cargoTestFlags  = [ "-p" "urp-control" ];

  # tonic-build needs protoc; point it at the offline nixpkgs one.
  nativeBuildInputs =
    (pkgs.lib.optional (rustToolchain != null) rustToolchain)
    ++ [ pkgs.protobuf ];
  PROTOC = "${pkgs.protobuf}/bin/protoc";

  # Cross builds run the tests under an emulator they may not have.
  doCheck = rustToolchain != null;

  meta = with pkgs.lib; {
    description = "urp control-plane gRPC daemon (Rendezvous/Heartbeat, design 33 Phase 3)";
    license = licenses.gpl2Only;
    mainProgram = "urp-control";
  };
}
