# Build the userspace RDMA test client.
#
# Connects to the urp kernel module's RDMA listener and sends/receives
# URP DATA frames through an echo server backend.
#
# The client compiles against the REAL wire definitions -- the UAPI
# header + kernel/urp_frame.h via nix/fuzz/urp_fuzz_shim.h -- instead of
# a private copy of the codec, so the source set includes those headers.
{ pkgs }:

pkgs.stdenv.mkDerivation {
  name = "urp-test-client";
  src = pkgs.lib.fileset.toSource {
    root = ../.;
    fileset = pkgs.lib.fileset.unions [
      ../tools/urp-test-client.c
      ../kernel/urp_frame.h
      ../kernel/include/uapi/linux/urp.h
      ../nix/fuzz/urp_fuzz_shim.h
    ];
  };

  buildInputs = [ pkgs.rdma-core ];

  buildPhase = ''
    $CC -Wall -Wextra -O2 \
      -I nix/fuzz -I kernel \
      -o urp-test-client tools/urp-test-client.c \
      -lrdmacm -libverbs
  '';

  installPhase = ''
    mkdir -p $out/bin
    cp urp-test-client $out/bin/
  '';
}
