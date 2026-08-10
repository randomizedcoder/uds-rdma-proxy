# nix/analysis/kbuild-check.nix
#
# mkKbuildReport: run the urp.ko kbuild with a checker bolted on
# (sparse/smatch via C=2 CHECK=..., gcc via W=1, coccinelle via the
# coccicheck target) and capture the diagnostics for OUR files into
#   $out/report.txt   findings in kernel/*.c|h only (in-tree header
#                     noise filtered out), sorted and deduped
#   $out/count.txt    line count of report.txt
#   $out/build.log    the full unfiltered make output, for triage
#
# These are REPORT derivations, not gates: findings never fail the
# build (xdp2's nix/analysis contract). Only infrastructure errors
# (empty build log = make never ran) fail.
#
# The environment mirrors buildUrpKoWith in nix/checks.nix exactly --
# same moduleBuildDependencies, same kdir, same ARCH -- so the
# analyzers see the very build the CI gates compile.
{ pkgs, src }:

{ name
, kernelPackages
, makeTarget ? "modules"
, makeFlags ? ""
, extraNativeBuildInputs ? [ ]
  # kbuild emits diagnostics for the M= tree as paths RELATIVE to the
  # module dir (urp_rdma.c:24:45: ...), while in-tree headers show up
  # as absolute /nix/store/... paths -- so "starts with a relative path
  # ending in .c/.h" selects exactly our files.
, filterPattern ? "^[^/: ][^: ]*\\.[ch]:[0-9]+"
}:

pkgs.stdenv.mkDerivation {
  name = "urp-analysis-${name}-${kernelPackages.kernel.version}";
  inherit src;

  nativeBuildInputs = [ pkgs.gnumake ]
    ++ kernelPackages.kernel.moduleBuildDependencies
    ++ extraNativeBuildInputs;

  buildPhase =
    let
      kdir = "${kernelPackages.kernel.dev}/lib/modules/${kernelPackages.kernel.modDirVersion}/build";
      crossVars = "ARCH=${pkgs.stdenv.hostPlatform.linuxArch} CROSS_COMPILE=${pkgs.stdenv.cc.targetPrefix}";
    in ''
      runHook preBuild
      make -C ${kdir} M=$PWD/kernel ${crossVars} ${makeFlags} ${makeTarget} 2>&1 \
        | tee build.log || true
      test -s build.log
      runHook postBuild
    '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out
    cp build.log $out/build.log
    grep -E '${filterPattern}' build.log | sort -u > $out/report.txt || true
    wc -l < $out/report.txt | tr -d ' ' > $out/count.txt
    echo "urp-analysis-${name}: $(cat $out/count.txt) findings"
    runHook postInstall
  '';
}
