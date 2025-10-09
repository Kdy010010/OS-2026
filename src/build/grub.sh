#!/bin/bash
# build.sh - 빌드 및 GRUB 부팅 ISO 자동 생성

set -e

# 📁 경로 및 이름 설정
KERNEL_SRC="kernel.c"
BOOT_SRC="boot.asm"
LINKER="link.ld"
KERNEL_ELF="kernel.elf"
ISO_NAME="os2025.iso"
GRUB_DIR="iso/boot/grub"

echo "[+] Cleaning..."
rm -rf iso/ *.o *.elf *.bin "$ISO_NAME"

echo "[+] Assembling Multiboot header..."
nasm -f elf32 "$BOOT_SRC" -o boot.o

echo "[+] Compiling kernel..."
gcc -m32 -ffreestanding -nostdlib -c "$KERNEL_SRC" -o kernel.o

echo "[+] Linking..."
ld -m elf_i386 -T "$LINKER" -nostdlib -o "$KERNEL_ELF" boot.o kernel.o

echo "[+] Preparing GRUB config..."
mkdir -p "$GRUB_DIR"
cp "$KERNEL_ELF" iso/boot/kernel.elf

cat > "$GRUB_DIR/grub.cfg" <<EOF
set timeout=0
set default=0
menuentry "OS/2025" {
    multiboot /boot/kernel.elf
    boot
}
EOF

echo "[+] Creating ISO..."
grub-mkrescue -o "$ISO_NAME" iso/

echo "[✓] Done. ISO created: $ISO_NAME"
