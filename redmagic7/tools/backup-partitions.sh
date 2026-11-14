#!/bin/bash
# Back up every RedMagic 7 partition except userdata and super (and the whole-disk nodes), gzip on the Mac,
# plus the first 1 MiB of each UFS LUN (the GPT). Needs Magisk root granted to the adb shell.
# usage: backup-partitions.sh OUTDIR
set -u
OUT=$1
S=${ANDROID_SERIAL:-${RM_SERIAL:?export RM_SERIAL=<your adb serial>}}
mkdir -p "$OUT"
names=$(adb -s "$S" shell "su -c 'ls /dev/block/by-name'" | tr -d '\r')
for p in $names; do
  case $p in userdata|super|sd[a-f]) continue ;; esac
  [ -s "$OUT/$p.img.gz" ] && continue
  adb -s "$S" exec-out "su -c 'dd if=/dev/block/by-name/$p bs=1048576 2>/dev/null'" | gzip -1 > "$OUT/$p.img.gz.part" &&
    mv "$OUT/$p.img.gz.part" "$OUT/$p.img.gz"
  echo "$p $(gzip -l "$OUT/$p.img.gz" | awk 'NR==2{print $2}') bytes"
done
for l in sda sdb sdc sdd sde sdf; do
  adb -s "$S" exec-out "su -c 'dd if=/dev/block/$l bs=1048576 count=1 2>/dev/null'" > "$OUT/gpt-$l-first1M.bin"
done
( cd "$OUT" && shasum -a 256 *.gz *.bin > SHA256SUMS )
echo "BACKUP DONE: $(ls "$OUT"/*.img.gz | wc -l) partitions, $(du -sh "$OUT" | cut -f1)"
