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

  mkCFuzzer = { name, harness, units, extraFlags ? [ ] }:
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
          -I kernel -I nix/fuzz ${lib.concatStringsSep " " extraFlags} \
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

  # A plain standalone C binary (NOT libFuzzer) for the live-kernel/live-wire
  # fuzzers. These drive the loaded module or a real RDMA peer from inside the
  # sanitizer microVM, so they build as ordinary binaries and get baked into
  # the VM rootfs (nix/microvms/mkVm.nix). @libs are extra `-l` link flags;
  # @buildInputs are propagated headers/libs (e.g. rdma-core).
  mkPlainCTool = { name, source, libs ? [ ], buildInputs ? [ ] }:
    pkgs.stdenv.mkDerivation {
      pname = "urp-${name}";
      version = "0.1.0";
      src = fuzzSrc;
      nativeBuildInputs = [ pkgs.clang ];
      inherit buildInputs;
      buildPhase = ''
        runHook preBuild
        clang -O1 -std=gnu11 -Wall -Wextra \
          nix/fuzz/${source} -o ${name} ${lib.concatStringsSep " " libs}
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

  # Default C reorder backend fuzzer (design 27 F1.2 / F0.3). Compiles the REAL
  # kernel/urp_reorder.c against the kernel's OWN userspace rbtree
  # (tools/lib/rbtree.c), extracted at build time from the nixpkgs-pinned kernel
  # source -- narHash-secured, not vendored. libc satisfies the kernel slab.
  # Until now only the optional Rust reorder backend was fuzzed (cargo-fuzz
  # reorder_ops); this covers the DEFAULT C rbtree backend that actually runs.
  reorderKernelSrc = pkgs.linuxPackages_latest.kernel.src;
  fuzz-reorder = pkgs.stdenv.mkDerivation {
    pname = "urp-fuzz-reorder";
    version = "0.1.0";
    src = fuzzSrc;
    nativeBuildInputs = [ pkgs.clang pkgs.gnutar pkgs.xz ];
    buildPhase = ''
      runHook preBuild
      # Pull the kernel's userspace rbtree from the pinned source tarball.
      mkdir -p ksrc
      tar xf ${reorderKernelSrc} --strip-components=1 -C ksrc \
        --wildcards '*/tools/lib/rbtree.c' '*/tools/include/*'
      clang -g -O1 -std=gnu11 \
        -fsanitize=fuzzer,address,undefined \
        -fno-omit-frame-pointer \
        -Wall -Wextra \
        -DU64_MAX=0xffffffffffffffffULL \
        -I ksrc/tools/include -I kernel -I nix/fuzz \
        nix/fuzz/reorder_fuzz.c kernel/urp_reorder.c ksrc/tools/lib/rbtree.c \
        -o fuzz-reorder
      runHook postBuild
    '';
    installPhase = ''
      runHook preInstall
      mkdir -p $out/bin
      cp fuzz-reorder $out/bin/
      runHook postInstall
    '';
    meta.mainProgram = "fuzz-reorder";
  };
  # Live-kernel + live-wire fuzzers (design 27 F2). Plain standalone binaries
  # baked into the sanitizer microVM rootfs (see nix/microvms/mkVm.nix).
  liveFuzzers = {
    # Blind netlink fuzzer (S3): hammers the genl control plane.
    fuzz-netlink = mkPlainCTool { name = "fuzz-netlink"; source = "netlink_fuzz.c"; };
    # KCOV coverage-guided netlink fuzzer (S3); needs the CONFIG_KCOV kernel.
    fuzz-netlink-cov = mkPlainCTool { name = "fuzz-netlink-cov"; source = "netlink_cov_fuzz.c"; };
    # Concurrent NEW/DEL/SET/GET racer (S3) for endpoint-lifecycle UAF (design 26).
    fuzz-netlink-race = mkPlainCTool { name = "fuzz-netlink-race"; source = "netlink_race.c"; libs = [ "-lpthread" ]; };
    # Hostile-peer RDMA wire fuzzer (S1/S2): malformed frames into urp_recv_done.
    fuzz-wire = mkPlainCTool {
      name = "fuzz-wire";
      source = "wire_fuzz.c";
      libs = [ "-lrdmacm" "-libverbs" ];
      buildInputs = [ pkgs.rdma-core ];
    };
  };
in
liveFuzzers // {
  # RX frame classifier -- the 27.8 disclosure surface.
  fuzz-classify = mkCFuzzer {
    name = "fuzz-classify";
    harness = "classify_fuzz.c";
    units = [ "kernel/urp_frame.c" ];
  };

  # RX decision pipeline as a SEQUENCE: classify -> flag/event dispatch ->
  # stream state machine, statefully (design 27 item 3). Compiles the real
  # kernel C (urp_frame.c + urp_stream_sm.c).
  fuzz-rx-seq = mkCFuzzer {
    name = "fuzz-rx-seq";
    harness = "rx_seq_fuzz.c";
    units = [ "kernel/urp_frame.c" "kernel/urp_stream_sm.c" ];
  };

  # urp-bench incremental deframer (design 30 §30.12) — the userland
  # benchmark's parsing surface, chunk schedule fuzzer-controlled.
  fuzz-bench-deframe = mkCFuzzer {
    name = "fuzz-bench-deframe";
    harness = "bench_deframe_fuzz.c";
    units = [ "tools/urp-bench-core.c" ];
    extraFlags = [ "-I" "tools" ];
  };

  # Default C reorder backend + spec-model differential (real rbtree bundled).
  inherit fuzz-reorder;

  # The live-kernel / live-wire fuzzer binaries (fuzz-netlink[-cov|-race],
  # fuzz-wire) are spliced in from liveFuzzers above.
}
