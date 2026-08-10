# nix/analysis/smatch.nix
#
# Dan Carpenter's smatch over the module build:
#   make C=2 CHECK="smatch -p=kernel"
# -p=kernel enables the kernel-specific checks. The cross-function DB
# is not built here (that needs a full-tree smatch run), so DB-backed
# checks run in reduced mode -- fine for an out-of-tree module.
# nixpkgs' smatch (1.74) is current; no override needed.
{ pkgs, mkKbuildReport, kernelPackages }:

{
  report = mkKbuildReport {
    name = "smatch";
    inherit kernelPackages;
    extraNativeBuildInputs = [ pkgs.smatch ];
    makeFlags = ''C=2 CHECK="smatch -p=kernel"'';
  };
}
