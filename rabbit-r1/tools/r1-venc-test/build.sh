#!/bin/bash
# Builds r1-venc-test.jar (dex) from the Android tree's JDK, SDK stub and d8.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
android=${ANDROID_TOP:-/rabbitr1/src/android}
out=${1:-$here/out}
mkdir -p "$out/classes"
"$android/prebuilts/jdk/jdk21/linux-x86/bin/javac" --release 11 -cp "$android/prebuilts/sdk/current/public/android.jar" \
    -d "$out/classes" "$here/R1VencTest.java"
"$android/out/host/linux-x86/bin/d8" --min-api 30 --lib "$android/prebuilts/sdk/current/public/android.jar" \
    --output "$out/r1-venc-test.jar" $(find "$out/classes" -name '*.class')
echo "$out/r1-venc-test.jar"
