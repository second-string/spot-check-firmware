#! /usr/bin/env bash

TAG=$(git describe --tag)
TAG_REGEX="^([0-9]+)\.([0-9]+)\.([0-9]+)$"
REMOTE_BUILD_DIR=\$HOME/spot-check-api/fw_versions
REMOTE_BUILD_VERSION_FILE=current_version.txt

if ! [[ "$TAG" =~ $TAG_REGEX ]]; then
    echo Commit not correctly tagged, should only be 3 version levels separated by periods \(i.e. 0.3.12\)
    exit 1
fi

echo
echo Building new binary of "$TAG" to ensure correct version released to server
echo
idf.py clean
idf.py build

echo
echo Releasing version "$TAG"
echo

VERSION="${BASH_REMATCH[0]}"
MAJOR="${BASH_REMATCH[1]}"
MID="${BASH_REMATCH[2]}"
MINOR="${BASH_REMATCH[3]}"

scp build/spot-check-firmware.bin pi:"$REMOTE_BUILD_DIR"/spot-check-firmware-"$MAJOR"-"$MID"-"$MINOR".bin
ssh pi "echo $VERSION > $REMOTE_BUILD_DIR/$REMOTE_BUILD_VERSION_FILE"

echo Successfully transferred new binary and updated current_version.txt to "$VERSION"

