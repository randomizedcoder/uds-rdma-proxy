# urp-vm — test VM management tool.
#
# Usage:
#   nix run .#urp-vm -- start     Start the VM (waits for SSH)
#   nix run .#urp-vm -- ssh       SSH into the VM
#   nix run .#urp-vm -- ssh CMD   Run a command in the VM
#   nix run .#urp-vm -- stop      Stop the VM
#   nix run .#urp-vm -- console   Tail the VM console log
#   nix run .#urp-vm -- status    Show VM status
{ pkgs, testVm }:

let
  vmRunScript = "${testVm.vm}/bin/run-urp-test-vm";
  sshKeyStore = "${testVm.sshKeys}/id_ed25519";
in
pkgs.writeShellApplication {
  name = "urp-vm";

  runtimeInputs = with pkgs; [
    openssh
    coreutils
    procps
  ];

  text = ''
    VM_DIR="/tmp/urp-test-vm"
    PID_FILE="$VM_DIR/vm.pid"

    # SSH key lives in the nix store (world-readable 0444).
    # ssh refuses keys with loose permissions, so copy to a temp file with 0600.
    SSH_KEY=$(mktemp /tmp/urp-vm-key.XXXXXX)
    cp "${sshKeyStore}" "$SSH_KEY"
    chmod 600 "$SSH_KEY"
    cleanup_key() { rm -f "$SSH_KEY"; }
    trap cleanup_key EXIT

    do_ssh() {
      ssh -i "$SSH_KEY" -p 2222 \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR \
        root@localhost "$@"
    }

    cmd_start() {
      mkdir -p "$VM_DIR"

      if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
        echo "VM already running (PID $(cat "$PID_FILE"))"
        return 0
      fi

      echo "Starting test VM..."
      (cd "$VM_DIR" && ${vmRunScript} > "$VM_DIR/console.log" 2>&1 < /dev/null) &
      VM_PID=$!
      echo "$VM_PID" > "$PID_FILE"

      echo "Waiting for SSH on port 2222..."
      for _ in $(seq 1 120); do
        if do_ssh true 2>/dev/null; then
          echo "VM ready (PID $VM_PID)"
          return 0
        fi
        if ! kill -0 "$VM_PID" 2>/dev/null; then
          echo "ERROR: VM process exited unexpectedly"
          tail -30 "$VM_DIR/console.log" 2>/dev/null || true
          rm -f "$PID_FILE"
          return 1
        fi
        sleep 1
      done

      echo "ERROR: SSH not reachable after 120s"
      kill "$VM_PID" 2>/dev/null || true
      rm -f "$PID_FILE"
      return 1
    }

    cmd_ssh() {
      if [ ! -f "$PID_FILE" ] || ! kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
        echo "VM not running. Start with: urp-vm start" >&2
        return 1
      fi
      do_ssh "$@"
    }

    cmd_stop() {
      if [ -f "$PID_FILE" ]; then
        PID=$(cat "$PID_FILE")
        if kill -0 "$PID" 2>/dev/null; then
          echo "Stopping VM (PID $PID)..."
          kill "$PID" 2>/dev/null || true
        fi
        rm -f "$PID_FILE"
      fi
      # Also kill any stale QEMU process using our port
      QEMU_PID=$(ss -tlnp 2>/dev/null | grep ':2222 ' | grep -o 'pid=[0-9]*' | head -1 | cut -d= -f2)
      if [ -n "$QEMU_PID" ]; then
        echo "Killing stale QEMU (PID $QEMU_PID)..."
        kill "$QEMU_PID" 2>/dev/null || true
      fi
      echo "VM stopped"
    }

    cmd_console() {
      if [ -f "$VM_DIR/console.log" ]; then
        tail -f "$VM_DIR/console.log"
      else
        echo "No console log found"
        return 1
      fi
    }

    cmd_status() {
      if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
        echo "VM running (PID $(cat "$PID_FILE"))"
        do_ssh uname -a 2>/dev/null || echo "  (SSH not yet reachable)"
      else
        echo "VM not running"
      fi
    }

    case "''${1:-help}" in
      start)   cmd_start ;;
      ssh)     shift; cmd_ssh "$@" ;;
      stop)    cmd_stop ;;
      console) cmd_console ;;
      status)  cmd_status ;;
      *)
        echo "Usage: urp-vm {start|ssh|stop|console|status}"
        echo ""
        echo "Commands:"
        echo "  start    Start the test VM (waits for SSH readiness)"
        echo "  ssh      SSH into the running VM (extra args passed as remote command)"
        echo "  stop     Stop the VM"
        echo "  console  Tail the VM console log"
        echo "  status   Show VM status"
        ;;
    esac
  '';
}
