#!/usr/bin/env bash
# Builds ssh-relay/scrcpy-ssh-relay.jar: a dex jar run on the device with app_process. It bridges an SSH exec
# channel (stdin/stdout) with the scrcpy server's abstract socket, replacing "adb forward".
# Needs: JDK 11+ (javac) and the Android SDK (platforms/android-XX/android.jar, build-tools/XX/d8).
# Usage:  ANDROID_HOME=/path/to/Sdk ./ssh-relay/build_relay.sh
set -euo pipefail
cd "$(dirname "$0")"
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
[ -n "$SDK" ] || { echo "set ANDROID_HOME to your Android SDK" >&2; exit 1; }
JAR=$(ls -d "$SDK"/platforms/android-*/android.jar | sort -V | tail -1)
BT=$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)
D8="$BT/d8"; [ -f "$D8.bat" ] && D8="$D8.bat"
rm -rf out && mkdir out
javac --release 11 -cp "$JAR" -d out ScrcpySshRelay.java
(cd out && "$D8" --min-api 21 --lib "$JAR" --output ../relay.jar ScrcpySshRelay*.class)
mv -f relay.jar scrcpy-ssh-relay.jar
rm -rf out
echo "built ssh-relay/scrcpy-ssh-relay.jar"
