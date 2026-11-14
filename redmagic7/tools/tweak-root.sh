#!/bin/bash
# Apply mondrian's bring-up tweaks + UI hook to the RedMagic copy of the iOS root (an APFS clone of the Note 8 one).
#
#   sudo redmagic7/tools/tweak-root.sh
#
# Needs root: the files in the image are root/mobile owned, and the mount must use real owners.
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
here=$(cd "$(dirname "$0")/.." && pwd)
img=$here/root-build/root.img
hook=$here/root-build/mondrian-ui-hook.dylib
tweaks=$here/../ref/redmagic/ios-mondrian/tools/ios-root-tweaks.py
mp=$(mktemp -d /private/tmp/rm7root.XXXX)
dev=$(hdiutil attach -nomount -noverify -imagekey diskimage-class=CRawDiskImage "$img" | awk '{print $1; exit}')
trap 'umount "$mp" 2>/dev/null || true; hdiutil detach "$dev" >/dev/null 2>&1 || true; rmdir "$mp" 2>/dev/null || true' EXIT
mount -t hfs -o owners "$dev" "$mp"
python3 -I "$tweaks" "$mp" --ui-hook "$hook"
ls -l "$mp/usr/libexec/mondrian-ui-hook.dylib"
sync
umount "$mp"
fsck_hfs -fy "$dev" | tail -2
echo "TWEAK DONE"
