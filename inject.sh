#!/usr/bin/env bash
set -euo pipefail

IMG="${1:-hdd.img}"
ELF="${2:-browser.elf}"
NAME="${3:-$(basename "$ELF")}"

python3 - "$IMG" "$ELF" "$NAME" <<'PY'
import sys, struct

SECTOR=512
ROOT_LBA=100
DIR_SECTORS=8            # 커널 DIR_SECTORS=8 (루트=100..107, 데이터는 108부터)
DIRENT=32
NAME_LEN=24
ENTRIES = DIR_SECTORS*SECTOR // DIRENT   # 128

img_path, elf_path, fs_name = sys.argv[1], sys.argv[2], sys.argv[3]
if len(fs_name) > NAME_LEN-1:
    sys.exit(f"error: name too long (max {NAME_LEN-1} chars)")

elf = open(elf_path,"rb").read()
size = len(elf)
sectors = (size + SECTOR - 1)//SECTOR

def unpack(buf, off):
    raw = buf[off:off+DIRENT]
    name = raw[:NAME_LEN].split(b'\0',1)[0].decode('latin1','ignore')
    lba, sz = struct.unpack_from("<II", raw, NAME_LEN)
    return raw, name, lba, sz

def pack(name, lba, size):
    nb = name.encode('ascii','ignore')[:NAME_LEN]
    nb = nb + b'\0'*(NAME_LEN-len(nb))
    return nb + struct.pack("<II", lba, size)

with open(img_path,"r+b") as f:
    # 루트 디렉터리 8섹터 전체 읽기
    f.seek(ROOT_LBA*SECTOR)
    root = bytearray(f.read(DIR_SECTORS*SECTOR))
    if len(root) != DIR_SECTORS*SECTOR:
        sys.exit("error: image too small / cannot read root dir")

    empty = None
    max_end = ROOT_LBA + DIR_SECTORS        # ★ 데이터는 108부터 (핵심 수정)
    for i in range(ENTRIES):
        off = i*DIRENT
        raw, name, lba, sz = unpack(root, off)
        if raw[0] == 0:
            if empty is None: empty = off
            continue                        # 빈 슬롯 뒤에도 파일 있을 수 있으니 계속 스캔
        if name == fs_name:
            sys.exit("error: file already exists in root")
        used = 1 if sz == 0 else (sz + SECTOR - 1)//SECTOR
        end = lba + used
        if end > max_end: max_end = end

    if empty is None:
        sys.exit("error: root directory full")

    alloc_lba = max_end

    # 파일 데이터 기록 (섹터 패딩)
    f.seek(alloc_lba*SECTOR)
    f.write(elf + b'\0'*(sectors*SECTOR - size))

    # 디렉터리 엔트리 기록 (해당 섹터만 다시 씀)
    root[empty:empty+DIRENT] = pack(fs_name, alloc_lba, size)
    sec = empty // SECTOR
    f.seek((ROOT_LBA+sec)*SECTOR)
    f.write(root[sec*SECTOR:(sec+1)*SECTOR])

print(f"OK: injected '{fs_name}' into /")
print(f"    lba={alloc_lba}, size={size}, sectors={sectors}")
PY
