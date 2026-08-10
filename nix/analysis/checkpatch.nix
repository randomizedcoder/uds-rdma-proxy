# nix/analysis/checkpatch.nix
#
# checkpatch.pl --strict over every kernel source file. Fully hermetic:
# the nixpkgs kernel .dev output ships the complete source tree at
# lib/modules/<ver>/source/, including scripts/checkpatch.pl and its
# data files (spelling.txt, const_structs.checkpatch -- resolved by the
# script relative to its own directory, so they load with --no-tree).
#
# Flags: --no-tree (out-of-tree module), --strict (netdev standard),
# --terse --show-types (one greppable line per finding, type-tagged so
# an --ignore list can be curated after baseline review). Deliberately
# no --ignore initially; known-intentional hits (e.g. the three
# LINUX_VERSION_CODE compat shims) are triaged in
# docs/design/26-upstream-readiness.md rather than suppressed blindly.
{ pkgs, src, kernelPackages }:

let
  ksrc = "${kernelPackages.kernel.dev}/lib/modules/${kernelPackages.kernel.modDirVersion}/source";
in
pkgs.runCommand "urp-analysis-checkpatch-${kernelPackages.kernel.version}"
  { nativeBuildInputs = [ pkgs.perl ]; }
  ''
    mkdir -p $out
    : > $out/report.txt
    for f in ${src}/kernel/*.c ${src}/kernel/*.h \
             ${src}/kernel/include/urp_ffi.h \
             ${src}/kernel/include/uapi/linux/urp.h \
             ${src}/kernel/Kbuild ${src}/kernel/Makefile; do
      perl ${ksrc}/scripts/checkpatch.pl \
        --no-tree --strict --terse --show-types --file "$f" \
        >> $out/report.txt 2>&1 || true
    done
    grep -Ec '(ERROR|WARNING|CHECK):' $out/report.txt > $out/count.txt || true
    echo "urp-analysis-checkpatch: $(cat $out/count.txt) findings"
  ''
