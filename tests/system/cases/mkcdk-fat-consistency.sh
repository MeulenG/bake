#!/usr/bin/env bash
# mkcdk-fat-consistency.sh - verify that mkcdk produces FAT volumes fsck.fat accepts
#
# What this test checks, for a FAT16 (64M) and a FAT32 (2G) image that each get a
# file and a raw boot sector written to sector 0:
#   1. fsck.fat -n finds nothing to fix: every FAT copy matches, the FAT32 backup
#      boot sector matches sector 0, and the boot sector label has a matching
#      volume label entry in the root directory
#   2. On FAT32, writing the raw boot sector keeps the whole BPB (boot signature,
#      label) and only replaces the boot code from offset 90

set -euo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$TESTS_DIR/lib/env.sh"
source "$TESTS_DIR/lib/cleanup.sh"
source "$TESTS_DIR/lib/assert.sh"

TEST_NAME="mkcdk-fat-consistency"
TEST_LOG_DIR="$(mktemp -d)"
WORK_DIR="$TEST_LOG_DIR/image"
MKCDK_LOG="$TEST_LOG_DIR/mkcdk.log"
FSCK_LOG="$TEST_LOG_DIR/fsck.log"
CMD_MKCDK="$BUILD_DIR/bin/mkcdk"

mkdir -p "$WORK_DIR"

register_tmpdir "$TEST_LOG_DIR"
register_log    "$MKCDK_LOG"
register_log    "$FSCK_LOG"
trap 'teardown_test' EXIT

echo "=== $TEST_NAME ==="

read_u32() {
    od -An -t u4 -j "$2" -N 4 "$1" | tr -d ' '
}

read_hex() {
    od -An -t x1 -j "$2" -N "$3" "$1" | tr -d ' \n'
}

# ── Preflight ─────────────────────────────────────────────────────────────────
echo "[1/3] Checking prerequisites..."
if [[ ! -x "$CMD_MKCDK" ]]; then
    echo "FAIL: required binary not found or not executable: $CMD_MKCDK" >&2
    exit 1
fi
if ! command -v fsck.fat >/dev/null 2>&1; then
    echo "FAIL: fsck.fat not found, install dosfstools" >&2
    exit 1
fi

# A boot sector like a bootloader would provide: a jump, an empty BPB template and
# recognisable boot code. Only the jump and the code from offset 90 should land.
python3 - "$WORK_DIR/boot.bin" <<'PY'
import sys
sector = bytearray(512)
sector[0:3] = b"\xeb\x58\x90"
sector[90:510] = b"\xcc" * 420
sector[510:512] = b"\x55\xaa"
open(sys.argv[1], "wb").write(sector)
PY
echo "hello" > "$WORK_DIR/hello.txt"
cat > "$WORK_DIR/image.yaml" <<EOF
schema: mbr

partitions:
  - label: test1
    type: fat32
    id: 0C
    attributes:
      - boot
    sources:
      - type: file
        source: hello.txt
        target: /HELLO.TXT
      - type: raw
        source: boot.bin
        target: sector=0
EOF

# build_and_check <size> <expected fat type>
build_and_check() {
    local size="$1" fat="$2"
    local image="$WORK_DIR/pc.img" part="$WORK_DIR/part-$fat.img"

    rm -f "$image"
    local mkcdk_rc=0
    (cd "$WORK_DIR" && "$CMD_MKCDK" -s "$size" image.yaml) >"$MKCDK_LOG" 2>&1 || mkcdk_rc=$?
    if [[ $mkcdk_rc -ne 0 ]]; then
        echo "FAIL: mkcdk exited with code $mkcdk_rc for the $size image"
        dump_log "$MKCDK_LOG" 200
        exit 1
    fi
    assert_file_nonempty "$image" "$size disk image"

    local lba sectors
    lba="$(read_u32 "$image" $((446 + 8)))"
    sectors="$(read_u32 "$image" $((446 + 12)))"
    dd if="$image" of="$part" bs=512 skip="$lba" count="$sectors" status=none

    local fstype
    fstype="$(od -An -c -j $(( fat == 32 ? 82 : 54 )) -N 5 "$part" | tr -d ' ')"
    if [[ "$fstype" != "FAT$fat" ]]; then
        echo "FAIL: the $size image is not FAT$fat (filesystem type field: '$fstype')"
        exit 1
    fi

    local fsck_rc=0
    fsck.fat -n "$part" >"$FSCK_LOG" 2>&1 || fsck_rc=$?
    if [[ $fsck_rc -ne 0 ]]; then
        echo "FAIL: fsck.fat -n exited with $fsck_rc on the FAT$fat image"
        dump_log "$FSCK_LOG" 50
        exit 1
    fi
    echo "      FAT$fat ($size): fsck.fat -n is clean"

    if [[ "$fat" -eq 32 ]]; then
        local bootsig label code backup
        bootsig="$(read_hex "$part" 66 1)"
        label="$(od -An -c -j 71 -N 5 "$part" | tr -d ' ')"
        code="$(read_hex "$part" 90 4)"
        if [[ "$bootsig" != "29" || "$label" != "test1" || "$code" != "cccccccc" ]]; then
            echo "FAIL: raw boot sector write damaged the FAT32 BPB (boot signature $bootsig, label '$label', code $code)"
            exit 1
        fi
        backup="$(od -An -t u2 -j 50 -N 2 "$part" | tr -d ' ')"
        if ! cmp -s <(dd if="$part" bs=512 count=1 status=none) <(dd if="$part" bs=512 skip="$backup" count=1 status=none); then
            echo "FAIL: backup boot sector $backup does not match sector 0"
            exit 1
        fi
        echo "      FAT32: BPB kept through the raw write, backup boot sector matches"
    fi
    rm -f "$image" "$part"
}

# ── Build and verify ──────────────────────────────────────────────────────────
echo "[2/3] FAT16 image..."
build_and_check 64M 16

echo "[3/3] FAT32 image..."
build_and_check 2G 32

echo ""
echo "PASS: $TEST_NAME"
