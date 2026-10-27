#!/usr/bin/env bash
# mkcdk-fat-layout.sh - verify that mkcdk lays out a FAT partition at the LBA the MBR advertises
#
# What this test checks:
#   1. mkcdk builds an MBR image with a single FAT partition
#   2. The FAT boot sector sits at the partition start LBA from the MBR entry
#      (not offset by the partition start a second time)
#   3. The BPB hidden sectors field equals the partition start LBA
#
# A 64M image is formatted as FAT16 (FAT32 needs ~2GB), both share the same
# boot sector layout code, so only the common BPB fields are checked.

set -euo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$TESTS_DIR/lib/env.sh"
source "$TESTS_DIR/lib/cleanup.sh"
source "$TESTS_DIR/lib/assert.sh"

TEST_NAME="mkcdk-fat-layout"
TEST_LOG_DIR="$(mktemp -d)"
WORK_DIR="$TEST_LOG_DIR/image"
MKCDK_LOG="$TEST_LOG_DIR/mkcdk.log"
CMD_MKCDK="$BUILD_DIR/bin/mkcdk"

mkdir -p "$WORK_DIR"

register_tmpdir "$TEST_LOG_DIR"
register_log    "$MKCDK_LOG"
trap 'teardown_test' EXIT

echo "=== $TEST_NAME ==="

# read a little-endian uint32 from a file at a byte offset
read_u32() {
    od -An -t u4 -j "$2" -N 4 "$1" | tr -d ' '
}

# ── Preflight ─────────────────────────────────────────────────────────────────
echo "[1/3] Checking prerequisites..."
if [[ ! -x "$CMD_MKCDK" ]]; then
    echo "FAIL: required binary not found or not executable: $CMD_MKCDK" >&2
    exit 1
fi

# ── Build image ───────────────────────────────────────────────────────────────
echo "[2/3] Building FAT image..."
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
EOF

mkcdk_rc=0
(cd "$WORK_DIR" && "$CMD_MKCDK" -s 64M image.yaml) >"$MKCDK_LOG" 2>&1 || mkcdk_rc=$?
if [[ $mkcdk_rc -ne 0 ]]; then
    echo "FAIL: mkcdk exited with code $mkcdk_rc"
    dump_log "$MKCDK_LOG" 200
    exit 1
fi

IMAGE="$WORK_DIR/pc.img"
assert_file_nonempty "$IMAGE" "disk image"

# ── Verify layout ─────────────────────────────────────────────────────────────
echo "[3/3] Verifying partition layout..."

# first MBR partition entry starts at 446, its start LBA is at +8
part_lba="$(read_u32 "$IMAGE" $((446 + 8)))"
if [[ "$part_lba" -eq 0 ]]; then
    echo "FAIL: MBR partition entry 0 has start LBA 0"
    exit 1
fi
echo "      partition start LBA: $part_lba"

boot_sector=$((part_lba * 512))
jump="$(od -An -t x1 -j "$boot_sector" -N 1 "$IMAGE" | tr -d ' ')"
bytes_per_sector="$(od -An -t u2 -j $((boot_sector + 11)) -N 2 "$IMAGE" | tr -d ' ')"
signature="$(od -An -t x1 -j $((boot_sector + 510)) -N 2 "$IMAGE" | tr -d ' ')"
if [[ "$jump" != "eb" || "$bytes_per_sector" -ne 512 || "$signature" != "55aa" ]]; then
    echo "FAIL: no FAT boot sector at partition start LBA $part_lba (jump=$jump, bytes/sector=$bytes_per_sector, signature=$signature)"
    exit 1
fi

hidden_sectors="$(read_u32 "$IMAGE" $((boot_sector + 28)))"
if [[ "$hidden_sectors" -ne "$part_lba" ]]; then
    echo "FAIL: BPB hidden sectors is $hidden_sectors, expected $part_lba"
    exit 1
fi

echo ""
echo "PASS: $TEST_NAME"
