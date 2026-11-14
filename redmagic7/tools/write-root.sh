#!/bin/bash
# Write an iOS root image raw into the extents of /data/ios/root.img (pinned f2fs file) on the RedMagic 7.
#
#   redmagic7/tools/write-root.sh IMAGE [verify]
#
# The file's blocks are written through the userdata block device (sda12), never through the file API: f2fs
# under Android's metadata encryption would encrypt them, and the iOS kernel reads them raw. Every write is
# bounded by an extent from disk/root.extents, so nothing outside the file is touched. The image is streamed
# gzip-compressed (most of it is zeros) and each extent is checked by SHA-256 afterwards, read back raw.
set -euo pipefail
IMG=$1
MODE=${2:-write}
SERIAL=${RM_SERIAL:?export RM_SERIAL=<your adb serial>}
here=$(cd "$(dirname "$0")/.." && pwd)
EXT=$here/disk/root.extents
BLK=4096
img_blocks=$(( $(stat -f %z "$IMG") / BLK ))
file_blocks=8388608
[ "$img_blocks" -le "$file_blocks" ] || { echo "image larger than the iOS file" >&2; exit 1; }
adb -s $SERIAL shell "su -c 'test -b /dev/block/by-name/userdata && readlink -f /dev/block/by-name/userdata'" | grep -q sda12 ||
	{ echo "userdata is not sda12" >&2; exit 1; }

while read -r -u 3 _ n _ lo _ ph _ len _; do
	flo=$((lo / BLK)); fph=$((ph / BLK)); fn=$((len / BLK))
	[ "$flo" -lt "$img_blocks" ] || continue
	cnt=$fn; [ $((flo + cnt)) -le "$img_blocks" ] || cnt=$((img_blocks - flo))
	if [ "$MODE" = write ]; then
		echo "extent $n: file blk $flo -> part blk $fph, $cnt blocks ($((cnt * BLK >> 20)) MiB)"
		dd if="$IMG" bs=$BLK skip=$flo count=$cnt 2>/dev/null | gzip -1 |
			adb -s $SERIAL shell "su -c 'zcat | dd of=/dev/block/by-name/userdata bs=$BLK seek=$fph conv=notrunc 2>&1 | tail -1'"
	fi
	want=$(dd if="$IMG" bs=$BLK skip=$flo count=$cnt 2>/dev/null | shasum -a 256 | cut -c1-64)
	got=$(adb -s $SERIAL shell "su -c 'dd if=/dev/block/by-name/userdata bs=$BLK skip=$fph count=$cnt 2>/dev/null | sha256sum'" < /dev/null | cut -c1-64)
	[ "$want" = "$got" ] && echo "extent $n verify OK" || { echo "extent $n verify MISMATCH" >&2; exit 1; }
done 3< <(grep "^extent" "$EXT")
echo "ROOT WRITE DONE"
