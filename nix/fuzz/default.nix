# nix/fuzz/default.nix
#
# Userspace libFuzzer harnesses for the pure kernel C parsing surfaces
# (design 27 F1). These compile the REAL kernel code (kernel/urp_frame.c
# + urp_frame.h, via a small userspace shim) under
# -fsanitize=fuzzer,address,undefined -- coverage-guided, hermetic, no VM.
# The classifier is the surface where the design 27 27.8 #1 disclosure
# lived, so it is the first target.
#
#   nix run .#fuzz-classify                 # fuzz forever
#   nix run .#fuzz-classify -- -max_total_time=60 -runs=... corpus/
#   nix build .#fuzz-classify               # just build the binary
#
# Manual-run only; a time-boxed smoke pass belongs in nightly CI (F3).
{ pkgs, lib }:

let
  # Own source closure: kernel/ and nix/fuzz/ .c/.h only (the codec is
  # kernel-infra-free, so this is all the fuzzer needs).
  fuzzSrc = builtins.path {
    path = ../..;
    name = "urp-fuzz-src";
    filter = path: type:
      let b = builtins.baseNameOf path; in
      type == "directory"
      || (lib.hasSuffix ".c" b && !lib.hasSuffix ".mod.c" b)
      || lib.hasSuffix ".h" b;
  };

  mkCFuzzer = { name, harness, units }:
    pkgs.stdenv.mkDerivation {
      pname = "urp-${name}";
      version = "0.1.0";
      src = fuzzSrc;
      nativeBuildInputs = [ pkgs.clang ];
      buildPhase = ''
        runHook preBuild
        clang -g -O1 -std=gnu11 \
          -fsanitize=fuzzer,address,undefined \
          -fno-omit-frame-pointer \
          -Wall -Wextra \
          -I kernel -I nix/fuzz \
          nix/fuzz/${harness} ${lib.concatStringsSep " " units} \
          -o ${name}
        runHook postBuild
      '';
      installPhase = ''
        runHook preInstall
        mkdir -p $out/bin
        cp ${name} $out/bin/
        runHook postInstall
      '';
      meta.mainProgram = name;
    };
  # Live-kernel netlink fuzzer (design 27 F2, S3). A plain standalone
  # binary -- not libFuzzer -- meant to run INSIDE a sanitizer VM against
  # the loaded urp module, hammering the genl control plane. Built here so
  # it can be baked into the microVM rootfs (see nix/microvms/mkVm.nix).
  netlinkFuzz = pkgs.stdenv.mkDerivation {
    pname = "urp-netlink-fuzz";
    version = "0.1.0";
    src = fuzzSrc;
    nativeBuildInputs = [ pkgs.clang ];
    buildPhase = ''
      runHook preBuild
      clang -O1 -std=gnu11 -Wall -Wextra \
        nix/fuzz/netlink_fuzz.c -o netlink_fuzz
      runHook postBuild
    '';
    installPhase = ''
      runHook preInstall
      mkdir -p $out/bin
      cp netlink_fuzz $out/bin/
      runHook postInstall
    '';
    meta.mainProgram = "netlink_fuzz";
  };
  # KCOV coverage-guided netlink fuzzer (design 27 F2, S3). Same libc-only
  # build as netlinkFuzz, but drives inputs by KCOV coverage feedback; needs a
  # CONFIG_KCOV kernel (the sanitizer VM). Baked into the microVM rootfs.
  covFuzz = pkgs.stdenv.mkDerivation {
    pname = "urp-netlink-cov-fuzz";
    version = "0.1.0";
    src = fuzzSrc;
    nativeBuildInputs = [ pkgs.clang ];
    buildPhase = ''
      runHook preBuild
      clang -O1 -std=gnu11 -Wall -Wextra \
        nix/fuzz/netlink_cov_fuzz.c -o netlink_cov_fuzz
      runHook postBuild
    '';
    installPhase = ''
      runHook preInstall
      mkdir -p $out/bin
      cp netlink_cov_fuzz $out/bin/
      runHook postInstall
    '';
    meta.mainProgram = "netlink_cov_fuzz";
  };
  # Concurrent netlink racer (design 27 F2, S3 concurrency). Multi-threaded
  # NEW/DEL/SET/GET churn on a shared name pool to shake out the endpoint
  # lifecycle races (deref-after-rcu-unlock UAF; no kref, design 26). Baked
  # into the microVM rootfs; run in the sanitizer VM.
  raceFuzz = pkgs.stdenv.mkDerivation {
    pname = "urp-netlink-race";
    version = "0.1.0";
    src = fuzzSrc;
    nativeBuildInputs = [ pkgs.clang ];
    buildPhase = ''
      runHook preBuild
      clang -O1 -std=gnu11 -Wall -Wextra \
        nix/fuzz/netlink_race.c -o netlink_race -lpthread
      runHook postBuild
    '';
    installPhase = ''
      runHook preInstall
      mkdir -p $out/bin
      cp netlink_race $out/bin/
      runHook postInstall
    '';
    meta.mainProgram = "netlink_race";
  };
  # Hostile-peer RDMA wire fuzzer (design 27 F2, S1/S2). A standalone
  # librdmacm/libibverbs RC peer that connects to a urp acceptor and injects
  # malformed frames into the RX path (urp_recv_done). Baked into the microVM
  # rootfs and run from the peer VM against the acceptor under KASAN. Links
  # rdma-core exactly like tools/urp-test-client.c.
  wireFuzz = pkgs.stdenv.mkDerivation {
    pname = "urp-wire-fuzz";
    version = "0.1.0";
    src = fuzzSrc;
    nativeBuildInputs = [ pkgs.clang ];
    buildInputs = [ pkgs.rdma-core ];
    buildPhase = ''
      runHook preBuild
      clang -O1 -std=gnu11 -Wall -Wextra \
        nix/fuzz/wire_fuzz.c -o wire_fuzz \
        -lrdmacm -libverbs
      runHook postBuild
    '';
    installPhase = ''
      runHook preInstall
      mkdir -p $out/bin
      cp wire_fuzz $out/bin/
      runHook postInstall
    '';
    meta.mainProgram = "wire_fuzz";
  };
in
{
  # RX frame classifier -- the 27.8 disclosure surface.
  fuzz-classify = mkCFuzzer {
    name = "fuzz-classify";
    harness = "classify_fuzz.c";
    units = [ "kernel/urp_frame.c" ];
  };

  # Live-kernel netlink fuzzer binary (for the microVM rootfs).
  inherit netlinkFuzz;

  # KCOV coverage-guided netlink fuzzer binary (for the microVM rootfs).
  inherit covFuzz;

  # Concurrent netlink racer binary (for the microVM rootfs).
  inherit raceFuzz;

  # Hostile-peer RDMA wire fuzzer binary (for the microVM rootfs).
  inherit wireFuzz;
}
