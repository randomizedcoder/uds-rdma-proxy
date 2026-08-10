# nix/analysis/sparse.nix
#
# Sparse over the module build: make C=2 CHECK=sparse (kbuild supplies
# CHECKFLAGS: -D__CHECKER__ plus the arch defines).
#
# nixpkgs' sparse (0.6.4-unstable-2024-02-03) predates __typeof_unqual__,
# which kernels >= 7.x use unconditionally under __CHECKER__
# (include/linux/compiler_types.h: USE_TYPEOF_UNQUAL is forced on for
# __CHECKER__, and __unqual_scalar_typeof backs READ_ONCE -- i.e. every
# TU). So build sparse from upstream master, pinned by commit (same
# known-good snapshot xdp2 uses; hash verified against this commit).
{ pkgs, mkKbuildReport, kernelPackages }:

let
  sparseMaster = pkgs.sparse.overrideAttrs (old: {
    version = "0.6.4-master";
    src = pkgs.fetchgit {
      url = "https://git.kernel.org/pub/scm/devel/sparse/sparse.git";
      rev = "37156835e3d725b6d750f000be33ba3814bb2310"; # master, 2025-12-18
      sha256 = "sha256-662n1ENn8ZsiBtSBx6Vr1MrRAwzvob0Y1ifnBVtfB5k=";
    };
  });
in
{
  inherit sparseMaster;

  report = mkKbuildReport {
    name = "sparse";
    inherit kernelPackages;
    extraNativeBuildInputs = [ sparseMaster ];
    makeFlags = "C=2 CHECK=sparse";
  };
}
