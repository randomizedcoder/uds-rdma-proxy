# Build the urp-bench C shell (design 30 B3): io_uring UDS benchmark.
# Same shape as urp-test-client.nix — precise fileset closure, plain
# stdenv compile. liburing is the repo's first userland io_uring dep.
#
#   nix run .#urp-bench-c -- --listen /tmp/b.sock --id 2 --mode uring-rw \
#     --msg-size 4076 --batch 32 --count 100000
{ pkgs }:

pkgs.stdenv.mkDerivation {
  name = "urp-bench";
  src = pkgs.lib.fileset.toSource {
    root = ../.;
    fileset = pkgs.lib.fileset.unions [
      ../tools/urp-bench.c
      ../tools/urp-bench-core.c
      ../tools/urp-bench-core.h
      # Shared fast-path ABI (design 31): urp-bench.c's uring-cmd backend
      # includes "include/uapi/linux/urp_cmd.h", resolved via -I kernel below,
      # exactly as urp-fast-poc.nix and the kbuild do.
      ../kernel/include/uapi/linux/urp_cmd.h
    ];
  };

  buildInputs = [ pkgs.liburing ];

  buildPhase = ''
    $CC -Wall -Wextra -Werror -O2 \
      -I tools -I kernel \
      -o urp-bench tools/urp-bench.c tools/urp-bench-core.c \
      -luring
  '';

  installPhase = ''
    mkdir -p $out/bin
    cp urp-bench $out/bin/
  '';

  meta.mainProgram = "urp-bench";
}
