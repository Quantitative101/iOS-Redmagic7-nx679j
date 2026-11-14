#!/bin/bash
# Build HackiOS's ath11k object (Linux ath11k on FreeBSD LinuxKPI + wifi/lx) for the RedMagic 7,
# outside HackiOS's tree: stage their mk-ath11k.sh, wifi/ (+ the Mi 10 QCA6390 PCIe overlay, then
# ours) and the kernel headers it asks for into redmagic7/wifi/build, with this Mac's tools.
#
#   redmagic7/wifi/build-ath11k.sh [fetch]
#
# Output: redmagic7/wifi/build/xnu-deps/build/ath11k/out/ath11k.o (+ stubs.txt). The Linux and
# FreeBSD archives (~400 MB) are downloaded once and kept there.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
H=$HERE/../../ref/hackios-hackios-xnu-kernel-umi
H=$(cd "$H" && pwd)
UMI=$H/devices/xiaomi-mi-10/overlays
B=$HERE/build
mkdir -p "$B/tools" "$B/xnu-deps/host/bin" "$B/xnu-src/bsd/sys"

# their build script, unchanged
cp "$H/tools/mk-ath11k.sh" "$B/tools/"
# our kernel (the mondrian tree) is built as a macOS target: the object must say the same platform
sed -i "" "s/-target arm64-apple-ios16.5/-target arm64-apple-macos13.0/" "$B/tools/mk-ath11k.sh"
# wifi/: upstream, then the Mi 10 overlay (QCA6390 on PCIe/MHI), then ours (redmagic7/wifi/overlay)
rsync -a --delete "$H/wifi/" "$B/wifi/"
rsync -a "$UMI/wifi/" "$B/wifi/"
[ -d "$HERE/overlay/wifi" ] && rsync -a "$HERE/overlay/wifi/" "$B/wifi/"
# the kernel headers the object is compiled against: upstream, then the Mi 10's versions
cp "$H"/xnu/files/bsd/sys/hakios.*.h "$B/xnu-src/bsd/sys/"
cp "$UMI"/xnu/files/bsd/sys/*.h "$B/xnu-src/bsd/sys/"
[ -d "$HERE/overlay/xnu/bsd/sys" ] && cp "$HERE"/overlay/xnu/bsd/sys/*.h "$B/xnu-src/bsd/sys/"

# their environment, reduced to what mk-ath11k.sh reads
cat > "$B/xnu-env.sh" <<EOF
export XNU_SRC=$B/xnu-src
export PATH=$B/xnu-deps/host/bin:/opt/homebrew/opt/llvm/bin:\$PATH
EOF
rm -f "$B/xnu-deps/host/bin/ld64"; ln -s "$HERE/ld64-wrap" "$B/xnu-deps/host/bin/ld64"
ln -sf "$HERE/sha256sum-wrap" "$B/xnu-deps/host/bin/sha256sum"
# Apple's clang (it knows -mkernel and arm64-apple-ios); Homebrew's llvm only for nm/size
ln -sf /usr/bin/clang "$B/xnu-deps/host/bin/clang"

export PATH=$B/xnu-deps/host/bin:/opt/homebrew/opt/llvm/bin:$PATH
exec bash "$B/tools/mk-ath11k.sh" "$@"
