#!/bin/bash
# Install a minimal Android SDK/NDK in WSL (~/android) for building the qbridge APK.
set -e
sudo_apt() { :; }
A=$HOME/android; mkdir -p $A && cd $A
if [ ! -x cmdline-tools/latest/bin/sdkmanager ]; then
  curl -sLo clt.zip https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
  python3 -c "import zipfile;zipfile.ZipFile('clt.zip').extractall('clt')"
  mkdir -p cmdline-tools && rm -rf cmdline-tools/latest && mv clt/cmdline-tools cmdline-tools/latest && rm -rf clt clt.zip
  chmod +x cmdline-tools/latest/bin/*
fi
export JAVA_HOME=$(dirname $(dirname $(readlink -f $(which java))))
yes | cmdline-tools/latest/bin/sdkmanager --sdk_root=$A --licenses >/dev/null 2>&1 || true
cmdline-tools/latest/bin/sdkmanager --sdk_root=$A "platforms;android-32" "build-tools;34.0.0" "ndk;27.2.12479018" "cmake;3.22.1" 2>&1 | grep -vE "^\[|^$" | tail -3
ls $A $A/ndk
