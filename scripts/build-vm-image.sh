#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROVISION_SCRIPT="$SCRIPT_DIR/provision-chroot.sh"
IMG="lowgui-test.img"
SIZE="8G"
CHROOT="/tmp/lowgui-chroot"
DIST="bookworm"
ARCH="amd64"
MIRROR="http://deb.debian.org/debian"
PART_OFFSET=$((2048 * 512))
SSH_KEY="/tmp/lowgui-vm-key"
SSH_KEY_PUB="/tmp/lowgui-vm-key.pub"

echo "==> Building VM image: $IMG"

# If image and key already exist, skip build (cached)
if [ -f "$IMG" ] && [ -f vmlinuz ] && [ -f initrd.img ] && [ -f vm-key ]; then
  echo "==> VM image already exists, skipping build"
  exit 0
fi

# 1. Create raw disk image
rm -f "$IMG"
qemu-img create -f raw "$IMG" "$SIZE"

# 2. Create partition table (single Linux partition starting at sector 2048)
if command -v parted >/dev/null 2>&1; then
  parted -s "$IMG" mklabel msdos mkpart primary ext4 2048s 100%
else
  sfdisk "$IMG" <<EOF
label: dos
label-id: 0xDEADBEEF
device: $IMG
unit: sectors

: start=2048, size=0, type=83
EOF
fi

# 3. debootstrap minimal system
echo "==> Running debootstrap..."
#rm -rf "$CHROOT"
debootstrap --arch="$ARCH" "$DIST" "$CHROOT" "$MIRROR"

# 4. Set up loop device for the partition
LOOP_DEV=$(losetup --show -f -o "$PART_OFFSET" "$IMG")
trap 'losetup -d "$LOOP_DEV" 2>/dev/null || true' EXIT

# 5. Format partition
echo "==> Formatting root partition..."
mkfs.ext4 -F "$LOOP_DEV"

# 6. Mount partition
MNT="/mnt/lowgui-img"
mkdir -p "$MNT"
mount "$LOOP_DEV" "$MNT"

# 7. Copy debootstrap contents
echo "==> Copying rootfs..."
cp -a "$CHROOT/." "$MNT/"

# 8. Mount bind mounts for chroot
mount --bind /dev "$MNT/dev"
mount --bind /proc "$MNT/proc"
mount --bind /sys "$MNT/sys"

# 9. Generate SSH key pair for CI
echo "==> Generating SSH key pair..."
ssh-keygen -t rsa -b 4096 -f "$SSH_KEY" -C "root@lowgui-vm"
mkdir -p "$MNT/root/.ssh"
cp "$SSH_KEY_PUB" "$MNT/root/.ssh/authorized_keys"
chmod 700 "$MNT/root/.ssh"
chmod 600 "$MNT/root/.ssh/authorized_keys"

# 10. Copy provision script into chroot
cp "$PROVISION_SCRIPT" "$MNT/tmp/provision-chroot.sh"

# 11. Run provision script inside chroot
echo "==> Running provision script in chroot..."
chroot "$MNT" /bin/bash /tmp/provision-chroot.sh

# 12. Copy run-tests script into VM
cp "$SCRIPT_DIR/run-tests-in-vm.sh" "$MNT/usr/local/bin/run-tests"
chmod +x "$MNT/usr/local/bin/run-tests"

# 13. Extract kernel and initrd
echo "==> Extracting kernel and initrd..."
KERNEL=$(ls "$MNT/boot/vmlinuz-"* | sort -V | tail -n1)
INITRD=$(ls "$MNT/boot/initrd.img-"* | sort -V | tail -n1)
cp "$KERNEL" ./vmlinuz
cp "$INITRD" ./initrd.img

echo "==> Kernel: $(basename "$KERNEL")"
echo "==> Initrd: $(basename "$INITRD")"

# Copy SSH private key to workspace for CI use
cp "$SSH_KEY" ./vm-key
chmod 666 ./vm-key
chmod 666 "$IMG"

# 14. Cleanup
echo "==> Cleaning up..."
umount "$MNT/dev" || true
umount "$MNT/proc" || true
umount "$MNT/sys" || true
umount "$MNT" || true
losetup -d "$LOOP_DEV" || true
#rm -rf "$CHROOT" "$MNT"

echo "==> Done. Created:"
echo "    $IMG"
echo "    vmlinuz"
echo "    initrd.img"
echo "    SSH private key: $SSH_KEY"
