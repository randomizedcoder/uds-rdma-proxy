{ }:

''
  build-kmod() {
    make -C kernel
  }

  load-kmod() {
    sudo insmod kernel/urp.ko
  }

  unload-kmod() {
    sudo rmmod urp 2>/dev/null || true
  }

  run-miri() {
    cargo miri test -p uds-rdma-protocol "$@"
  }

  run-fuzz() {
    local target=''${1:-frame_decode}
    local duration=''${2:-60}
    (cd fuzz && cargo fuzz run "$target" -- -max_total_time="$duration")
  }
''
