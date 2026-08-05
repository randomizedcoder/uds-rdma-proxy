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
  '';
}
