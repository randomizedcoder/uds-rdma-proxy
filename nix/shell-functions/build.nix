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
''
