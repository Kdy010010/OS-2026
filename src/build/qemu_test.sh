#!/bin/bash
# qemu_test.sh — Run OS/2025 in QEMU with ISO + HDD

set -e

ISO="os2025.iso"
DISK="hdd.img"

# 확인
if [[ ! -f "$ISO" ]]; then
  echo "[!] ISO not found: $ISO"; exit 1
fi

if [[ ! -f "$DISK" ]]; then
  echo "[!] Disk image not found: $DISK"
  echo "    To create: qemu-img create -f raw hdd.img 64M"
  exit 1
fi

# 실행
echo "[+] Launching QEMU..."
qemu-system-i386 \
  -cdrom "$ISO" \
  -hda "$DISK" \
  -boot d \
  -m 128M \
  -serial stdio \
  -no-reboot \
  -rtc base=utc
