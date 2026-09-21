#!/bin/bash
set -euo pipefail

IMG= KERNEL= INITRD= REPO_PATH= SSH_KEY= OUT_DIR= CMD=()
while [ $# -gt 0 ]; do
  case "$1" in
    --image) IMG="$2"; shift 2 ;;
    --kernel) KERNEL="$2"; shift 2 ;;
    --initrd) INITRD="$2"; shift 2 ;;
    --repo-path) REPO_PATH="$2"; shift 2 ;;
    --ssh-key) SSH_KEY="$2"; shift 2 ;;
    --out-dir) OUT_DIR="$2"; shift 2 ;;
    --) shift; CMD=("$@"); break ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

if [ -z "$IMG" ] || [ -z "$KERNEL" ] || [ -z "$INITRD" ] || [ -z "$REPO_PATH" ] || [ -z "$OUT_DIR" ]; then
  echo "Usage: $0 --image IMG --kernel KERNEL --initrd INITRD --repo-path PATH --ssh-key KEY --out-dir DIR -- <cmd>" >&2
  exit 1
fi

if [ -z "$SSH_KEY" ]; then
  SSH_KEY="$HOME/.ssh/id_rsa"
fi

if [ ! -f "$SSH_KEY" ]; then
  echo "ERROR: SSH key not found at $SSH_KEY" >&2
  exit 1
fi

ACCEL="-enable-kvm"
if [ ! -e /dev/kvm ]; then
  echo "KVM not available, falling back to TCG"
  ACCEL="-accel tcg -cpu host"
fi

mkdir -p "$OUT_DIR"

echo "==> Booting QEMU..."
qemu-system-x86_64 \
  $ACCEL \
  -m 4G \
  -smp 2 \
  -display none \
  -serial stdio \
  -kernel "$KERNEL" \
  -initrd "$INITRD" \
  -append "root=/dev/vda1 console=ttyS0 panic=-1" \
  -drive file="$IMG",format=raw,if=virtio \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  -fsdev local,id=repo,path="$REPO_PATH",security_model=none \
  -device virtio-9p-pci,fsdev=repo,mount_tag=repo \
  -fsdev local,id=out,path="$OUT_DIR",security_model=none \
  -device virtio-9p-pci,fsdev=out,mount_tag=out \
  -pidfile "$OUT_DIR/qemu.pid" \
  &

QEMU_PID=$!
trap 'kill $QEMU_PID 2>/dev/null || true' EXIT

echo "==> Waiting for SSH (up to 120s)..."
SSH_READY=0
for i in $(seq 1 120); do
  if ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no -o ConnectTimeout=1 root@localhost -p 2222 echo ok 2>/dev/null; then
    SSH_READY=1
    break
  fi
  sleep 1
done

if [ "$SSH_READY" -ne 1 ]; then
  echo "ERROR: SSH did not become available within 120s" >&2
  exit 1
fi

echo "==> SSH ready. Running command in VM..."

ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no root@localhost -p 2222 /usr/local/bin/run-tests "${CMD[@]}"
TEST_RESULT=$?

echo "==> Test exit code: $TEST_RESULT"

echo "==> Waiting for QEMU shutdown..."
ELAPSED=0
while kill -0 $QEMU_PID 2>/dev/null; do
  if [ "$ELAPSED" -ge 60 ]; then
    echo "WARNING: QEMU did not exit within 60s, killing..."
    kill $QEMU_PID 2>/dev/null || true
    break
  fi
  sleep 1
  ELAPSED=$((ELAPSED + 1))
done

echo "==> Done."
exit $TEST_RESULT
