#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
#
# Build artefacts -> K1 board (mainline Debian on the SD card).
#
#   tools/deploy.sh fw       install only the firmware (/lib/firmware/updates/powervr)
#   tools/deploy.sh kernel   build + install kernel, modules, dtb, initrd, GRUB entry
#
# The GRUB entry passes modprobe.blacklist=powervr: the driver is never loaded
# automatically and is loaded by hand (tools/gpu-test.sh) after boot.
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
BOARD=${BOARD:-thalles@192.168.0.106}
LINUX=${LINUX:-/home/thalles/0-git/NoBlobs/linux}
KH=${KH:-$HOME/.ssh/known_hosts}
SSH="ssh -o UserKnownHostsFile=$KH $BOARD"
SCP="scp -q -o UserKnownHostsFile=$KH"
KMAKE="make -C $LINUX ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- LOCALVERSION="
FW=$HERE/build/rogue_36.29.52.182_v1.fw

deploy_fw() {
	make -C "$HERE/src"
	$SCP "$FW" "$BOARD:/tmp/"
	$SSH "sudo install -D -m 0644 /tmp/$(basename "$FW") /lib/firmware/updates/powervr/$(basename "$FW") && sync && sha256sum /lib/firmware/updates/powervr/$(basename "$FW")"
}

deploy_kernel() {
	$KMAKE -j"$(nproc)" Image modules dtbs
	local rel stage
	rel=$($KMAKE -s kernelrelease)
	stage=$(mktemp -d)
	$KMAKE -s INSTALL_MOD_PATH="$stage" INSTALL_MOD_STRIP=1 modules_install
	tar -C "$stage" -czf "$stage/mods.tgz" lib/modules
	$SCP "$LINUX/arch/riscv/boot/Image" "$BOARD:/tmp/vmlinuz-$rel"
	$SCP "$LINUX/arch/riscv/boot/dts/spacemit/k1-bananapi-f3.dtb" "$BOARD:/tmp/k1-bananapi-f3.dtb"
	$SCP "$stage/mods.tgz" "$BOARD:/tmp/mods.tgz"
	rm -rf "$stage"
	$SSH "set -e
		sudo rm -rf /lib/modules/$rel
		sudo tar -C / -xzf /tmp/mods.tgz --no-same-owner
		sudo depmod $rel
		sudo install -m 0755 /tmp/vmlinuz-$rel /boot/vmlinuz-$rel
		sudo install -D -m 0644 /tmp/k1-bananapi-f3.dtb /boot/spacemit/$rel/k1-bananapi-f3.dtb
		sudo update-initramfs -c -k $rel >/dev/null 2>&1 || sudo update-initramfs -u -k $rel
		ls -la /boot/vmlinuz-$rel /boot/initrd.img-$rel"
	add_grub_entry "$rel"
}

add_grub_entry() {
	local rel=$1 cfg=/boot/efi/EFI/BOOT/grub.cfg
	$SSH "sudo grep -q 'id img-gpu' $cfg" && { echo "GRUB entry already present"; return; }
	$SSH "set -e
		sudo cp $cfg $cfg.bak-img
		sudo tee -a $cfg >/dev/null <<'EOF'

menuentry \"Debian 13 - Linux $rel (GPU clean-room FW, powervr blacklisted)\" --id img-gpu {
    insmod ext2
    search --no-floppy --fs-uuid --set=root 41805151-225f-4751-bd53-3e70000ea80c
    echo \"carregando kernel $rel...\"
    linux /vmlinuz-$rel \\
        root=UUID=5d8f4431-e5d5-4827-beb5-0369ad52139d rootwait rootfstype=ext4 \\
        earlyprintk clk_ignore_unused pd_ignore_unused swiotlb=65536 \\
        workqueue.default_affinity_scope=system console=ttyS0,115200 earlycon=sbi \\
        modprobe.blacklist=powervr
    devicetree /spacemit/$rel/k1-bananapi-f3.dtb
    initrd /initrd.img-$rel
}
EOF
		sudo grep -n menuentry $cfg"
}

case "${1:-}" in
fw) deploy_fw ;;
kernel) deploy_kernel ;;
*) echo "usage: $0 fw|kernel" >&2; exit 1 ;;
esac
