{ pkgs, rustToolchain }:

let
  src = builtins.path {
    path = ../.;
    name = "uds-rdma-proxy-src";
    filter = path: type:
      let
        baseName = builtins.baseNameOf path;
      in
      # Include Cargo workspace and protocol crate
      (type == "directory" && (
        baseName == "crates" ||
        baseName == "uds-rdma-protocol" ||
        baseName == "src"
      )) ||
      (baseName == "Cargo.toml") ||
      (baseName == "Cargo.lock") ||
      (pkgs.lib.hasSuffix ".rs" baseName) ||
      # Include kernel sources and subdirectories
      (type == "directory" && (
        baseName == "kernel" ||
        baseName == "include" ||
        baseName == "uapi" ||
        baseName == "linux"
      )) ||
      (baseName == "Kbuild") ||
      (baseName == "Makefile" && pkgs.lib.hasInfix "/kernel" (toString path)) ||
      (pkgs.lib.hasSuffix ".c" baseName) ||
      (pkgs.lib.hasSuffix ".h" baseName);
  };

  # Build urp.ko against a given kernel package
  buildUrpKo = kernelPackages: pkgs.stdenv.mkDerivation {
    name = "urp-ko-${kernelPackages.kernel.version}";
    inherit src;
    nativeBuildInputs = [ pkgs.gnumake ] ++
      kernelPackages.kernel.moduleBuildDependencies;
    buildPhase =
      let
        kdir = "${kernelPackages.kernel.dev}/lib/modules/${kernelPackages.kernel.modDirVersion}/build";
      in ''
        make -C ${kdir} M=$PWD/kernel modules
      '';
    installPhase = ''
      mkdir -p $out/lib/modules/${kernelPackages.kernel.modDirVersion}
      cp kernel/urp.ko $out/lib/modules/${kernelPackages.kernel.modDirVersion}/
    '';
  };
in
{
  inherit buildUrpKo;

  # cargo test -- runs all protocol crate tests
  protocol-tests = pkgs.stdenv.mkDerivation {
    name = "uds-rdma-protocol-tests";
    inherit src;
    nativeBuildInputs = [ rustToolchain ];
    buildPhase = ''
      export CARGO_HOME=$(mktemp -d)
      cargo test -p uds-rdma-protocol --no-default-features 2>&1
      cargo test -p uds-rdma-protocol 2>&1
    '';
    installPhase = "touch $out";
  };

  # NOTE: miri requires network access to build its sysroot (fetches cfg-if for std).
  # This is incompatible with Nix's sandbox. Run miri via devshell instead:
  #   nix develop --command cargo miri test -p uds-rdma-protocol

  # kernel module build (against flake-pinned kernel, for CI)
  kernel-module-build = buildUrpKo pkgs.linuxPackages;
}
