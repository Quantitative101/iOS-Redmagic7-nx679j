#!/bin/bash
# Build the HakiosMetal CoreAnimation plug-in (HakiosMetal.bundle/HakiosMetal) against ../hakios-metal and the iOS Mesa
# build in ref/mesa-host/build-ios. Run from anywhere.
set -e
cd "$(dirname "$0")"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
HM=../hakios-metal
B=${MIGA_ROOT:-$HOME/MakeiOSGreatAgain}/ref/mesa-host/build-ios
xcrun --sdk iphoneos clang -arch arm64 -isysroot "$SDK" -miphoneos-version-min=16.0 -O2 -bundle -fno-objc-arc \
	-framework Foundation -framework IOKit -framework IOSurface -framework Metal -lobjc -I$HM/include \
	-o HakiosMetal HakiosMetalCA.m m2v_stubs.c $HM/out/ios/libhakios-metal.a -lc++ \
	$B/src/compiler/air/libair.a $B/src/freedreno/ir3/libfreedreno_ir3.a $B/src/freedreno/isa/libir3encode.a \
	$B/src/freedreno/isa/libir3decode.a $B/src/compiler/isaspec/libisaspec.a $B/src/compiler/nir/libnir.a \
	$B/src/compiler/libcompiler.a $B/src/freedreno/fdl/libfreedreno_layout.a $B/src/freedreno/common/libfreedreno_common.a \
	$B/src/util/libmesa_util.a $B/src/util/libmesa_util_sse41.a $B/src/util/blake3/libblake3.a \
	$B/src/c11/impl/libmesa_util_c11.a -lz -lm
strip -x HakiosMetal
codesign -f -s - HakiosMetal
ls -la HakiosMetal
