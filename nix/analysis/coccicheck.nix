# nix/analysis/coccicheck.nix
#
# The kernel's own coccinelle suite over the module:
#   make M=<us> coccicheck MODE=report
# coccicheck is a no-dot-config-target reachable through the .dev
# output's stub Makefile; with KBUILD_EXTMOD set, scripts/coccicheck
# scans --dir <our kernel/> with --patch <srctree>, running every
# scripts/coccinelle/**/*.cocci in report mode. util-linux supplies
# the lscpu that scripts/coccicheck shells out to; J=2 pins
# parallelism so the sandbox's getconf never decides.
#
# Individual .cocci parse failures are non-fatal in report mode; if the
# whole target ever proves unreliable, fall back to a curated spatch
# loop over the high-signal scripts (null/deref_null, free/kfree,
# free/double_free, api/stream_open, locks/call_kern).
{ pkgs, mkKbuildReport, kernelPackages }:

{
  report = mkKbuildReport {
    name = "coccicheck";
    inherit kernelPackages;
    # which: scripts/coccicheck locates spatch via `which spatch`.
    extraNativeBuildInputs = [ pkgs.coccinelle pkgs.util-linux pkgs.which ];
    makeTarget = "coccicheck";
    makeFlags = "MODE=report J=2";
  };
}
