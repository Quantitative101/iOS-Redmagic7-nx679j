#!/bin/sh
# RedMagic 7: dropbear (SSH server, one multi-call binary) and dash (/bin/sh) for iOS 16 arm64, for the kernel blob.
# Sources come from iOSPlus10's fetch script (pinned SHA-256): ref/iOSPlus10/userland/ssh/fetch.sh unpacks them into
# ref/iOSPlus10/out/ssh/src/. Options and entitlements are iOSPlus10's (MIT): localoptions.h, entitlements.plist.
# toybox is left out (it needs GNU sed to build).
#   redmagic7/ssh/build-ssh.sh   ->  redmagic7/ssh/out/{dropbearmulti,dash}  (ad-hoc signed with entitlements)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
P10=$(cd "$HERE/../../ref/iOSPlus10" && pwd)
DB=$P10/out/ssh/src/dropbear/dropbear-2026.94
DASH=$P10/out/ssh/src/dash/dash-0.5.13.5
SDK=$(xcrun -sdk iphoneos --show-sdk-path)
ICC=$(xcrun -sdk iphoneos --find clang)
B=$HERE/build
O=$HERE/out
NPROC=$(sysctl -n hw.ncpu)
rm -rf "$B" "$O"
mkdir -p "$B" "$O"
cat > "$B/ios-cc" <<EOS
#!/bin/sh
exec "$ICC" -arch arm64 -isysroot $SDK -mios-version-min=16.0 "\$@"
EOS
chmod +x "$B/ios-cc"

d=$B/dropbear; mkdir -p "$d"; cp "$P10/userland/ssh/localoptions.h" "$d/"
(cd "$d" && CC="$B/ios-cc" "$DB/configure" --host=aarch64-apple-darwin --disable-zlib --disable-lastlog --disable-utmp \
    --disable-utmpx --disable-wtmp --disable-wtmpx --disable-pututline --disable-pututxline --disable-pam \
    --enable-bundled-libtom CFLAGS=-O2 > configure.log 2>&1 &&
 make PROGRAMS="dropbear dropbearkey dbclient scp" MULTI=1 -j"$NPROC" > make.log 2>&1) ||
 { echo "dropbear failed, see $d/*.log" >&2; tail -20 "$d/make.log" >&2 || true; exit 1; }

d=$B/dash; mkdir -p "$d"
(cd "$d" && CC="$B/ios-cc" CC_FOR_BUILD=clang ac_cv_func_strchrnul=no "$DASH/configure" --host=aarch64-apple-darwin \
    --without-libedit CFLAGS=-O2 > configure.log 2>&1 && make -j"$NPROC" > make.log 2>&1) ||
 { echo "dash failed, see $d/*.log" >&2; tail -20 "$d/make.log" >&2 || true; exit 1; }

cp "$B/dropbear/dropbearmulti" "$O/dropbearmulti"
cp "$B/dash/src/dash" "$O/dash"
for f in dropbearmulti dash; do
	codesign --force -s - --entitlements "$P10/userland/ssh/entitlements.plist" "$O/$f"
	printf '%s: %s, ' "$f" "$(lipo -archs "$O/$f")"
	otool -l "$O/$f" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print "minos " $2; exit}'
done
