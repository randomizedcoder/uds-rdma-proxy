# nix/analysis/default.nix
#
# Static-analysis targets for the urp kernel module + Rust workspace,
# patterned on xdp2's nix/analysis: one file per tool, a uniform
# report.txt + count.txt output contract, findings never fail the
# build. All targets are MANUAL-run (`nix build .#analysis-all`) --
# deliberately not wired into checks/CI until baselines are clean.
#
#   nix build .#analysis-all -L      # everything + summary table
#   nix build .#analysis-sparse      # individual tools
#   cat result/summary.txt           # per-tool finding counts
#   cat result/<tool>/report.txt     # the findings themselves
#
# Kernel-side tools run hermetically against the same
# linuxPackages_latest build the kernel-module-build CI gate compiles
# (the nixpkgs kernel .dev output ships the full source tree, so
# checkpatch.pl and the coccinelle scripts need no external checkout).
{ pkgs, lib, src, rustToolchain, advisory-db
, kernelPackages ? pkgs.linuxPackages_latest }:

let
  mkKbuildReport = import ./kbuild-check.nix { inherit pkgs src; };

  sparse = import ./sparse.nix { inherit pkgs mkKbuildReport kernelPackages; };
  smatch = import ./smatch.nix { inherit pkgs mkKbuildReport kernelPackages; };
  gccW = import ./gcc-warnings.nix { inherit mkKbuildReport kernelPackages; };
  checkpatch = import ./checkpatch.nix { inherit pkgs src kernelPackages; };
  cocci = import ./coccicheck.nix { inherit pkgs mkKbuildReport kernelPackages; };
  rustLints = import ./rust-lints.nix { inherit pkgs src rustToolchain; };
  rustAudit = import ./rust-audit.nix { inherit pkgs src advisory-db; };
  userlandC = import ./userland-c.nix { inherit pkgs; };

  all = pkgs.runCommand "urp-analysis-all" { } ''
    mkdir -p $out
    ln -s ${sparse.report} $out/sparse
    ln -s ${smatch.report} $out/smatch
    ln -s ${checkpatch} $out/checkpatch
    ln -s ${gccW.w1} $out/gcc-w1
    ln -s ${cocci.report} $out/coccicheck
    ln -s ${rustLints.clippy} $out/clippy
    ln -s ${rustLints.rustfmt-check} $out/rustfmt
    ln -s ${rustAudit} $out/rust-audit
    ln -s ${userlandC.clang-tidy} $out/clang-tidy
    ln -s ${userlandC.cppcheck} $out/cppcheck
    {
      echo "=== urp static analysis (kernel ${kernelPackages.kernel.version}) ==="
      for t in sparse smatch checkpatch gcc-w1 coccicheck clippy rustfmt rust-audit clang-tidy cppcheck; do
        printf '%-12s %s findings\n' "$t:" "$(cat $out/$t/count.txt)"
      done
    } > $out/summary.txt
    cat $out/summary.txt
  '';
in
{
  analysis-sparse = sparse.report;
  analysis-smatch = smatch.report;
  analysis-checkpatch = checkpatch;
  analysis-w1 = gccW.w1;
  analysis-w2 = gccW.w2;
  analysis-coccicheck = cocci.report;
  analysis-clippy = rustLints.clippy;
  analysis-rustfmt = rustLints.rustfmt-check;
  analysis-rust-audit = rustAudit;
  analysis-clang-tidy = userlandC.clang-tidy;
  analysis-cppcheck = userlandC.cppcheck;
  analysis-all = all;
}
