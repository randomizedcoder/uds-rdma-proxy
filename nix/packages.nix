{ pkgs }:

let
  # Stable Rust toolchain for regular builds
  rustStable = pkgs.rust-bin.stable.latest.default.override {
    extensions = [ "rust-src" "rust-analyzer" ];
  };

  # Nightly Rust toolchain for miri and cargo-fuzz
  rustNightly = pkgs.rust-bin.nightly.latest.default.override {
    extensions = [ "rust-src" "miri" ];
  };
in
{
  inherit rustStable rustNightly;

  nativeBuildInputs = [
    rustStable
    pkgs.pkg-config
  ];

  buildInputs = [
    pkgs.linuxHeaders
  ];

  devTools = [
    rustNightly
    pkgs.gnumake
    pkgs.cargo-fuzz
  ];
}
