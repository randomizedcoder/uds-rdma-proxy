# nix/analysis/gcc-warnings.nix
#
# Kbuild's extra-warnings tiers over the module build. W=1 is what
# kernel maintainers expect new code to be clean under; W=2 is
# aspirational (noisy even for in-tree code) and exposed separately.
{ mkKbuildReport, kernelPackages }:

let
  warnPattern = "^[^/: ][^: ]*\\.[ch]:[0-9]+(:[0-9]+)?: (warning|error):";
in
{
  w1 = mkKbuildReport {
    name = "w1";
    inherit kernelPackages;
    makeFlags = "W=1";
    filterPattern = warnPattern;
  };

  w2 = mkKbuildReport {
    name = "w2";
    inherit kernelPackages;
    makeFlags = "W=12";
    filterPattern = warnPattern;
  };
}
