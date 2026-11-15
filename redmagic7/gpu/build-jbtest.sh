#!/bin/bash
# RedMagic 7 (v180): the jailbreak test program in its three forms, for the kernel blob (gen-blob.py):
#   out-jbtest-adhoc    ad-hoc signed
#   out-jbtest-unsigned no signature at all
#   out-jbtest-ent      ad-hoc signed with jbtest.entitlements
set -euo pipefail
cd "$(dirname "$0")"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC="xcrun --sdk iphoneos clang -target arm64-apple-ios16.0 -isysroot $SDK -O2"
$CC -o out-jbtest-adhoc jbtest.c
codesign -f -s - out-jbtest-adhoc
cp out-jbtest-adhoc out-jbtest-unsigned
codesign --remove-signature out-jbtest-unsigned
cp out-jbtest-adhoc out-jbtest-ent
codesign -f -s - --entitlements jbtest.entitlements out-jbtest-ent
for f in out-jbtest-adhoc out-jbtest-unsigned out-jbtest-ent; do
	printf '%s: ' "$f"; codesign -dv "$f" 2>&1 | grep -E "Signature|flags" | tr '\n' ' ' || true; echo
done
