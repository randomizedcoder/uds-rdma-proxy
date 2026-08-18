# Build the urp-bench Rust shell (design 30 B4). Installs the same
# `urp-bench` binary name as the C derivation (urp-bench.nix) with an
# identical CLI, so harnesses swap languages by swapping the package.
# Tests are NOT run here — they run in the sandboxed `urp-bench-rs-tests`
# check (nix/checks.nix).
{ pkgs, rustToolchain ? null }:

let
  src = builtins.path {
    path = ../.;
    name = "uds-rdma-proxy-src";
    filter = path: type:
      let baseName = builtins.baseNameOf path; in
      (type == "directory" && (
        baseName == "crates" ||
        baseName == "uds-rdma-protocol" ||
        baseName == "uds-rdma-protocol-ffi" ||
        baseName == "urp-bench" ||
        baseName == "urp-cli" ||
        baseName == "urp-netlink" ||
        baseName == "urp-control" ||
        baseName == "src" ||
        baseName == "commands" ||
        baseName == "tests"
      )) ||
      # Plus the proto tree -- cargo parses every workspace member (incl.
      # urp-control's build.rs paths) even for `-p urp-bench`.
      (type == "directory" && (
        baseName == "proto" ||
        baseName == "urp_control" ||
        baseName == "v1"
      )) ||
      (pkgs.lib.hasSuffix ".proto" baseName) ||
      (baseName == "Cargo.toml") ||
      (baseName == "Cargo.lock") ||
      (pkgs.lib.hasSuffix ".rs" baseName);
  };
in
pkgs.rustPlatform.buildRustPackage {
  pname = "urp-bench-rs";
  version = "0.1.0";
  inherit src;

  cargoLock.lockFile = ../Cargo.lock;

  cargoBuildFlags = [ "-p" "urp-bench" ];
  doCheck = false;

  nativeBuildInputs = pkgs.lib.optional (rustToolchain != null) rustToolchain;

  meta = with pkgs.lib; {
    description = "io_uring UDS benchmark (design 30), Rust twin of the C shell";
    license = licenses.gpl2Plus;
    mainProgram = "urp-bench";
  };
}
