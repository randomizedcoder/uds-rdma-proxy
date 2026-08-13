{ pkgs }:

let
  # Single nightly toolchain — superset of stable, needed for miri and cargo-fuzz.
  # Pin a specific date if nightly breakage becomes an issue.
  rustToolchain = pkgs.rust-bin.nightly.latest.default.override {
    extensions = [ "rust-src" "rust-analyzer" "miri" ];
  };
in
{
  inherit rustToolchain;

  nativeBuildInputs = [
    rustToolchain
    pkgs.pkg-config
  ];

  buildInputs = [
    pkgs.linuxHeaders
    # urp-bench io_uring shell (design 30 B3) — also lets the devshell
    # compile tools/urp-bench.c by hand.
    pkgs.liburing
  ];

  devTools = [
    pkgs.gnumake
    pkgs.cargo-fuzz
  ];
}
