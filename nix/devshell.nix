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
    echo "  build-kmod       - build kernel module"
    echo "  load-kmod        - insmod urp.ko"
    echo "  unload-kmod      - rmmod urp"
  '';
}
