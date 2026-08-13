{ pkgs, packages, envVars, shellFunctions }:

pkgs.mkShell {
  nativeBuildInputs = packages.nativeBuildInputs;
  buildInputs = packages.buildInputs;
  packages = packages.devTools;

  shellHook = ''
    ${envVars}
    ${shellFunctions}

    echo "uds-rdma-proxy dev shell"
    echo "  cargo test       - run protocol crate tests"
    echo "  run-miri         - cargo miri test (UB check)"
    echo "  run-fuzz [t] [s] - cargo fuzz (default: frame_decode 60s)"
    echo "  build-kmod       - build kernel module"
    echo "  load-kmod        - insmod urp.ko"
    echo "  unload-kmod      - rmmod urp"
    echo "  nix run .#urp-vm - test VM (start/ssh/stop/console/status)"
    echo "  nix run .#urp-bench-local - io_uring UDS bench smoke (C+Rust, design 30)"
    echo "  nix run .#fuzz-rust [t] [s] - cargo-fuzz via nix (default: bench_differential 60s)"
    echo "  nix build .#analysis-all -L - static analysis (sparse/smatch/checkpatch/W=1/cocci/clippy)"
  '';
}
