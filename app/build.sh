#!/bin/sh
# Build Quest1-SteamOS.exe (Windows, amd64) from Linux or WSL: Go 1.27+, curl, unzip.
# Google's platform-tools for Windows (adb, fastboot) are downloaded here and embedded in the exe; they are
# never committed. usage: app/build.sh [version]       (output: app/Quest1-SteamOS.exe)
set -e
cd "$(dirname "$0")"
V=${1:-dev}
if [ ! -f platform-tools/fastboot.exe ]; then
	curl -fsSL https://dl.google.com/android/repository/platform-tools-latest-windows.zip -o pt.zip
	rm -rf platform-tools && unzip -q -j pt.zip 'platform-tools/adb.exe' 'platform-tools/fastboot.exe' \
		'platform-tools/AdbWinApi.dll' 'platform-tools/AdbWinUsbApi.dll' 'platform-tools/NOTICE.txt' -d platform-tools
	rm pt.zip
fi
GOOS=windows GOARCH=amd64 CGO_ENABLED=0 go build -trimpath -buildvcs=false -ldflags "-H windowsgui -s -w -X main.Version=$V" \
	-o Quest1-SteamOS.exe .
ls -la Quest1-SteamOS.exe
