{ pkgs }:

''
  export LINUX_HEADERS_PATH="${pkgs.linuxHeaders}/include"
  # Prevent system rustup/cargo proxies from interfering with Nix-provided toolchain.
  # ~/.cargo/bin/cargo-miri is a rustup proxy that shadows the Nix-provided one.
  export CARGO_HOME="$PWD/.cargo-nix"
  mkdir -p "$CARGO_HOME"
''
