#!/usr/bin/env bash
# File/layer: packaging/android/sign-apk.sh | release packaging, Android arm64-v8a
# Testing: run by .github/workflows/release.yml on every release tag, and
#          by hand against a local Release build with a throwaway key
#          (docs/development.md, "Releases").
#
# Checks the unsigned Release APK, signs it with the release key and
# checks the result (ADR-0030):
#   - the package, version name and code are the release's (the code is
#     what lets each release install over the one before);
#   - it isn't debuggable: a debuggable build lets any app on a computer
#     with adb read the app's private files through run-as;
#   - arm64-v8a is the only native code;
#   - after signing: APK Signature Scheme v3 verifies (apksigner adds no
#     v1 or v2 for a minimum SDK of 31, which v3 covers), with one signer,
#     whose certificate SHA-256 is the pinned one in
#     release-cert.sha256 beside this script (a wrong keystore fails here,
#     not on a phone that refuses the update);
#   - the native libraries, stored uncompressed, sit on 16 KB pages.
#
# Usage: packaging/android/sign-apk.sh <unsigned.apk> <version> [out-dir]
#   <version>   X.Y.Z, or with a suffix (X.Y.Z-rc1, X.Y.Z-local); it
#               names the file, and its X.Y.Z must be the APK's
#   [out-dir]   default dist; the APK lands as
#               <out-dir>/havne-<version>-android-arm64-v8a.apk
# Environment:
#   HAVNE_KEYSTORE        the release keystore (PKCS12)
#   HAVNE_KEYSTORE_PASS   its password, which is also the key's
#   HAVNE_KEY_ALIAS       the key's alias (default havne)
#   HAVNE_CERT_SHA256     the certificate fingerprint to expect, for a
#                         throwaway key (default: release-cert.sha256)
#   ANDROID_SDK_ROOT      the SDK; its newest build-tools are used
# Run from the repository root.

set -euo pipefail

PACKAGE=io.github.erickramer42.havne
ABI=arm64-v8a
HERE=$(cd "$(dirname "$0")" && pwd)

die() { echo "sign-apk: $*" >&2; exit 1; }

[ $# -ge 2 ] && [ $# -le 3 ] || die "usage: sign-apk.sh <unsigned.apk> <version> [out-dir]"
in=$1 version=$2 outdir=${3:-dist}
[ -f "$in" ] || die "no $in"
[[ "$version" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)(-[0-9A-Za-z]+)?$ ]] || die "$version is not X.Y.Z or X.Y.Z-<suffix>"
name="${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.${BASH_REMATCH[3]}"
code=$(( BASH_REMATCH[1] * 10000 + BASH_REMATCH[2] * 100 + BASH_REMATCH[3] ))
: "${HAVNE_KEYSTORE:?the release keystore's path}"
: "${HAVNE_KEYSTORE_PASS:?the keystore's password}"
[ -f "$HAVNE_KEYSTORE" ] || die "no keystore at $HAVNE_KEYSTORE"
alias=${HAVNE_KEY_ALIAS:-havne}

expected=${HAVNE_CERT_SHA256:-}
if [ -z "$expected" ]; then
    [ -f "$HERE/release-cert.sha256" ] \
        || die "no $HERE/release-cert.sha256: the release key's certificate fingerprint (docs/development.md, \"The Android release key\")"
    expected=$(head -n 1 "$HERE/release-cert.sha256")
fi
expected=$(printf '%s' "$expected" | tr -d ': \r' | tr 'A-F' 'a-f')
[[ "$expected" =~ ^[0-9a-f]{64}$ ]] || die "the expected fingerprint isn't 64 hex digits"

# The SDK's tools, .exe and .bat on Windows
: "${ANDROID_SDK_ROOT:?the Android SDK}"
tools=$(ls -d "$ANDROID_SDK_ROOT"/build-tools/*/ 2>/dev/null | sort -V | tail -n 1 || true)
[ -n "$tools" ] || die "no build-tools under $ANDROID_SDK_ROOT"
tool() {
    local t
    for t in "$tools$1" "$tools$1.exe" "$tools$1.bat"; do
        [ -f "$t" ] && { echo "$t"; return; }
    done
    die "no $1 in $tools"
}
AAPT2=$(tool aapt2) ZIPALIGN=$(tool zipalign) APKSIGNER=$(tool apksigner)
echo "build-tools $(basename "$tools")"

# The unsigned APK
badging=$("$AAPT2" dump badging "$in" | tr -d '\r') || die "aapt2 can't read $in"
want="package: name='$PACKAGE' versionCode='$code' versionName='$name'"
grep -q "^$want" <<< "$badging" \
    || die "$in isn't $want: $(head -n 1 <<< "$badging")"
grep -qx "minSdkVersion:'31'" <<< "$badging" || die "$in's minimum SDK isn't 31"
! grep -qx "application-debuggable" <<< "$badging" || die "$in is debuggable (a Debug build)"
abis=$(sed -n "s/^native-code: //p" <<< "$badging")
[ "$abis" = "'$ABI'" ] || die "$in's native code is $abis, not '$ABI'"
echo "ok    $in: $PACKAGE $name ($code), not debuggable, $ABI"

# Aligned, signed, verified
mkdir -p "$outdir"
out="$outdir/havne-$version-android-$ABI.apk"
aligned="$outdir/.aligned.apk"
# A failed check leaves no APK to upload
trap 'status=$?; rm -f "$aligned"; [ "$status" -eq 0 ] || rm -f "$out"' EXIT
"$ZIPALIGN" -f -P 16 4 "$in" "$aligned"
"$APKSIGNER" sign --ks "$HAVNE_KEYSTORE" --ks-type PKCS12 --ks-key-alias "$alias" \
    --ks-pass env:HAVNE_KEYSTORE_PASS --key-pass env:HAVNE_KEYSTORE_PASS \
    --v1-signing-enabled false --v3-signing-enabled true --v4-signing-enabled false \
    --out "$out" "$aligned" || die "apksigner couldn't sign with $HAVNE_KEYSTORE's $alias"

verify=$("$APKSIGNER" verify --verbose --print-certs "$out" | tr -d '\r') \
    || die "$out doesn't verify"
grep -qx "Verified using v3 scheme (APK Signature Scheme v3): true" <<< "$verify" \
    || die "$out doesn't verify with APK Signature Scheme v3"
grep -qx "Number of signers: 1" <<< "$verify" || die "$out hasn't exactly one signer"
# Every certificate digest apksigner prints, whatever it calls the
# signer ("Signer #1", or by SDK range in newer build-tools), as
# lowercase hex: one certificate, and the pinned one
certs=$(grep -i "certificate SHA-256 digest:" <<< "$verify" || true)
got=$(sed -n 's/.*[Cc]ertificate SHA-256 digest: *//p' <<< "$certs" | tr -d ': ' | tr 'A-F' 'a-f' | sort -u)
if [ "$got" != "$expected" ]; then
    echo "$certs" >&2
    die "$out is signed by certificate '${got//$'\n'/, }', not the release key's $expected"
fi
"$ZIPALIGN" -c -P 16 4 "$out" > /dev/null || die "$out isn't aligned"
echo "ok    $out: v3 signed by $got, aligned"
