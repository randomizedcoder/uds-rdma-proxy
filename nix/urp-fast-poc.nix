# Build the urp-fast proof-of-concept driver (design 31, PR1).
# Drives the /dev/urp uring_cmd interface (REGISTER / UNREGISTER + the
# validated-but-unimplemented SEND path) against a loaded urp.ko. Because it
# needs io_uring and the live kernel module, it is a `nix run` app and a
# microVM phase, never a sandboxed check (design 30 section 30.14).
#
#   nix run .#urp-fast-poc            # uses /dev/urp, 4096 x 8 pool
#   nix run .#urp-fast-poc -- /dev/urp 65536 4
{ pkgs }:

pkgs.stdenv.mkDerivation {
  name = "urp-fast-poc";
  src = pkgs.lib.fileset.toSource {
    root = ../.;
    fileset = pkgs.lib.fileset.unions [
      ../tools/urp-fast-poc.c
      ../kernel/include/uapi/linux/urp_cmd.h
    ];
  };

  buildInputs = [ pkgs.liburing ];

  # -I kernel lets "include/uapi/linux/urp_cmd.h" resolve to the shared ABI
  # header, exactly as it does in the kbuild and the userspace validator check.
  buildPhase = ''
    $CC -Wall -Wextra -Werror -O2 \
      -I kernel \
      -o urp-fast-poc tools/urp-fast-poc.c \
      -luring
  '';

  installPhase = ''
    mkdir -p $out/bin
    cp urp-fast-poc $out/bin/
  '';

  meta.mainProgram = "urp-fast-poc";
}
