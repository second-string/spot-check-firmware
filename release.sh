#! /usr/bin/env bash

TAG=$(git describe --tag)
TAG_REGEX="^([0-9]+)\.([0-9]+)\.([0-9]+)$"
REMOTE_BUILD_DIR=\$HOME/spot-check-api/fw_versions
REMOTE_BUILD_VERSION_FILE=current_version.txt

MEMFAULT=memfault
MEMFAULT_ORG_TOKEN=$MEMFAULT_ORG_TOKEN
MEMFAULT_ORG_SLUG=second-string-studios
MEMFAULT_PROJECT_SLUG=spot-check
MEMFAULT_SOFTWARE_TYPE=spot-check-fw

if ! [[ "$TAG" =~ $TAG_REGEX ]]; then
    echo Commit not correctly tagged, should only be 3 version levels separated by periods \(i.e. 0.3.12\)
    exit 1
fi

echo
echo Building new binary of "$TAG" to ensure correct version released to server
echo

if ! command -v idf.py &> /dev/null; then
    echo "idf.py not found on path, make sure esp-idf environment set up. Aborting"
    exit 1
fi

idf.py clean
idf.py build

echo
echo Releasing version "$TAG"
echo

# Pull version parts from the stored regex fields from the earlier '=~' bash regex check
VERSION="${BASH_REMATCH[0]}"
MAJOR="${BASH_REMATCH[2]}"
MID="${BASH_REMATCH[2]}"
MINOR="${BASH_REMATCH[3]}"

scp build/spot-check-firmware.bin pi:"$REMOTE_BUILD_DIR"/spot-check-firmware-"$MAJOR"-"$MID"-"$MINOR".bin
ssh pi "echo $VERSION > $REMOTE_BUILD_DIR/$REMOTE_BUILD_VERSION_FILE"

echo Successfully transferred new binary and updated current_version.txt to "$VERSION"

echo
echo "Attempting to upload symbol file to memfault..."
echo

if ! [[ $(command -v "$MEMFAULT" ) ]]; then
    echo "'memfault' command not found on PATH, cannot upload symbol file to memfault automatically"
    exit 1
fi

if [[ -z "$MEMFAULT_ORG_TOKEN" ]]; then
    echo "'MEMFAULT_ORG_TOKEN' environment variable not set, cannot upload symbol file to memfault automatically"
    exit 1
fi

ELF_SHA_POSTFIX=$(shasum  -a 256 build/spot-check-firmware.elf | head -c 8)
if [[ -z "$ELF_SHA_POSTFIX" ]]; then
    echo "Could not properly parse SHA build hash out of elf file, cannot upload symbol file to memfault automatically"
    exit 1
fi

echo "$MEMFAULT" --org-token "$MEMFAULT_ORG_TOKEN" --org "$MEMFAULT_ORG_SLUG" --project "$MEMFAULT_PROJECT_SLUG" upload-mcu-symbols --software-type "$MEMFAULT_SOFTWARE_TYPE" --software-version "$VERSION"-"$ELF_SHA_POSTFIX" build/spot-check-firmware.elf

echo
echo "Upload success!"
echo
