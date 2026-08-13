# nix/analysis/userland-c.nix
#
# clang-tidy + cppcheck over the userland C tools (tools/*.c) — the
# repo's first userland C linting (design 30 §30.13; nix/analysis/ was
# kernel-only before). Same $out/{report.txt,count.txt,build.log}
# report-only contract as every other analysis target; findings never
# fail the build. Own fileset: tools/ is outside the shared kbuild `src`
# allowlist, and urp-test-client.c additionally needs the kernel UAPI +
# fuzz-shim headers and rdma-core.
{ pkgs }:

let
  src = pkgs.lib.fileset.toSource {
    root = ../..;
    fileset = pkgs.lib.fileset.unions [
      ../../tools
      ../../kernel/urp_frame.h
      ../../kernel/include
      ../fuzz/urp_fuzz_shim.h
    ];
  };
  tidyChecks = "-*,bugprone-*,clang-analyzer-*,cert-*,performance-*";
in
{
  clang-tidy = pkgs.runCommand "urp-analysis-clang-tidy"
    { nativeBuildInputs = [ pkgs.clang-tools ]; }
    ''
      mkdir -p $out
      cd ${src}
      {
        clang-tidy --checks='${tidyChecks}' \
          tools/urp-bench-core.c tools/urp-bench-test.c \
          -- -I tools
        clang-tidy --checks='${tidyChecks}' \
          tools/urp-bench.c \
          -- -I tools -I ${pkgs.liburing}/include
        clang-tidy --checks='${tidyChecks}' \
          tools/urp-test-client.c \
          -- -I kernel -I nix/fuzz -I ${pkgs.rdma-core}/include
      } > $out/build.log 2>&1 || true
      grep -E 'tools/.*\.(c|h):[0-9]+.*(warning|error):' $out/build.log \
        | sort -u > $out/report.txt || true
      wc -l < $out/report.txt | tr -d ' ' > $out/count.txt
      echo "urp-analysis-clang-tidy: $(cat $out/count.txt) findings"
    '';

  cppcheck = pkgs.runCommand "urp-analysis-cppcheck"
    { nativeBuildInputs = [ pkgs.cppcheck ]; }
    ''
      mkdir -p $out
      cd ${src}
      cppcheck --enable=warning,portability,performance \
        --suppress=missingIncludeSystem --inline-suppr \
        -I tools -I kernel -I nix/fuzz \
        tools/*.c > $out/build.log 2>&1 || true
      grep -E 'tools/.*\.(c|h):[0-9]+.*(error|warning|portability|performance)' $out/build.log \
        | sort -u > $out/report.txt || true
      wc -l < $out/report.txt | tr -d ' ' > $out/count.txt
      echo "urp-analysis-cppcheck: $(cat $out/count.txt) findings"
    '';
}
