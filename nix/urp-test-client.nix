# Build the userspace RDMA test client.
#
# Connects to the urp kernel module's RDMA listener and sends/receives
# URP DATA frames through an echo server backend.
{ pkgs }:

pkgs.stdenv.mkDerivation {
  name = "urp-test-client";
  src = ../tools;

  buildInputs = [ pkgs.rdma-core ];

  buildPhase = ''
    $CC -Wall -Wextra -O2 \
      -o urp-test-client urp-test-client.c \
      -lrdmacm -libverbs
  '';

  installPhase = ''
    mkdir -p $out/bin
    cp urp-test-client $out/bin/
  '';
}
