# Build urp.ko against the local system's kernel.
#
# Usage:
#   nix build .#urp-ko-local --impure
#
# This uses <nixpkgs> (the system channel) to find kernel-dev headers
# matching the running kernel, rather than the flake-pinned nixpkgs.
{ buildUrpKo }:

let
  # Import the system's nixpkgs channel (requires --impure)
  sysPkgs = import <nixpkgs> { };
in
buildUrpKo sysPkgs.linuxPackages
