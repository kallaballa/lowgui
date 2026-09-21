#!/bin/bash
set -e

export DEBIAN_FRONTEND=noninteractive

# Base system + kernel
apt-get update
apt-get install -y \
  linux-image-amd64 \
  linux-headers-amd64 \
  locales

# Generate locale
sed -i 's/^# *en_US.UTF-8/en_US.UTF-8/' /etc/locale.gen
locale-gen

# Build tools
apt-get install -y \
  build-essential cmake ninja-build git pkg-config \
  python3 curl ca-certificates

# OpenGL / EGL / X11 (for headless rendering via llvmpipe)
apt-get install -y \
  libglfw3-dev libgl-dev libegl-dev libgles2-mesa-dev \
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
  libglew-dev libstb-dev \
  mesa-va-drivers mesa-vulkan-drivers \
  xvfb openssh-server

# Networking: DHCP on ens3 (virtio-net predictable naming)
cat > /etc/network/interfaces.d/ens3.cfg <<EOF
auto ens3
iface ens3 inet dhcp
EOF

# SSH: root login with key auth
mkdir -p /root/.ssh
chmod 700 /root/.ssh

sed -i 's/^#\?PermitRootLogin.*/PermitRootLogin yes/' /etc/ssh/sshd_config
sed -i 's/^#\?PasswordAuthentication.*/PasswordAuthentication no/' /etc/ssh/sshd_config
sed -i 's/^#\?PubkeyAuthentication.*/PubkeyAuthentication yes/' /etc/ssh/sshd_config
mkdir -p /run/sshd

# Serial console auto-login
mkdir -p /etc/systemd/system/getty@ttyS0.service.d
cat > /etc/systemd/system/getty@ttyS0.service.d/override.conf <<EOF
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --noclear --keep-baud 115200,38400,9600 ttyS0 $TERM
EOF

# Serial console on kernel boot
sed -i 's/^GRUB_CMDLINE_LINUX_DEFAULT=.*/GRUB_CMDLINE_LINUX_DEFAULT="console=ttyS0"/' /etc/default/grub || true

# Enable SSH on boot
systemctl enable ssh

# fstab for virtio-blk
echo "/dev/sda1 / ext4 defaults 0 1" > /etc/fstab

# Ensure virtio modules are available (they may be built-in or as modules)
# These will be loaded by initrd if they're modules
echo "9p" >> /etc/modules
echo "9pnet" >> /etc/modules
echo "9pnet_virtio" >> /etc/modules

# Set root password to empty (for serial console auto-login)
passwd -d root

# Clean up
apt-get clean
rm -rf /var/lib/apt/lists/*

echo "Provisioning complete."
