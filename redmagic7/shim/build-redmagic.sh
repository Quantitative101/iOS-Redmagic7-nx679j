#!/bin/bash
# Build the shim for the RedMagic 7 with Homebrew LLVM (no GCC cross toolchain on this Mac) and wrap it in an
# Android boot image (header v3, no DTB: ABL passes the live tree from the untouched vendor_boot).
#
#   redmagic7/shim/build-redmagic.sh [PAYLOAD]
#
# Always SHIM_NO_UFS for now: mondrian's shim keeps its console log at the end of userdata, which on this phone is
# Android's live data. With SHIM_NO_UFS the UFS host is never touched (no reads, no writes).
set -euo pipefail
PAYLOAD_IN=${1:-}
if [ -n "$PAYLOAD_IN" ]; then
	PAYLOAD_IN=$(cd "$(dirname "$PAYLOAD_IN")" && pwd)/$(basename "$PAYLOAD_IN")
	[ -f "$PAYLOAD_IN" ] || { echo "error: no payload at $PAYLOAD_IN" >&2; exit 1; }
fi
cd "$(dirname "$0")"
LLVM=/opt/homebrew/opt/llvm/bin
CC="$LLVM/clang --target=aarch64-none-elf"
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-jump-tables -Wall -Wextra \
 -Wno-unused-parameter -DBOARD_REDMAGIC7 -DSHIM_NO_UFS ${EXTRA_CFLAGS:-}"
mkdir -p out
objs=()
for f in start.S shim.c console.c menu.c mmu.c fdt.c loader.c ufs.c libc.c lz4.c spmi.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
	objs+=(out/${f%.*}.o)
done
abs=$($LLVM/llvm-readelf -r "${objs[@]:1}" | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
ld.lld -static -T shim.ld --build-id=none -N "${objs[@]}" -o out/shim.elf
$LLVM/llvm-objcopy -O binary out/shim.elf out/Image
end=$($LLVM/llvm-nm out/shim.elf | awk '$3=="_end"{print $1}')
truncate -s $((16#$end)) out/Image
if [ -n "$PAYLOAD_IN" ]; then
	# Same as mondrian: LZ4P blob appended at _end, unpacked by shim/lz4.c.
	/opt/homebrew/bin/lz4 -BI -9 -f "$PAYLOAD_IN" out/payload.lz4
	python3 -I ../tools/lz4p.py "$PAYLOAD_IN" out/payload.lz4 out/payload.lz4p
	cat out/payload.lz4p >> out/Image
fi
(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
python3 -I ../tools/mkbootimg.py --header_version 3 --kernel out/Image --ramdisk out/ramdisk.bin \
	--os_version 13.0.0 --os_patch_level 2035-06 --cmdline "console=null" -o out/shim-boot.img
ls -l out/Image out/shim-boot.img
echo "SHIM BUILD DONE"
