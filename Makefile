#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME := spot-check-embedded

# include $(IDF_PATH)/make/project.mk

debug:
	idf.py openocd gdb --gdbinit .gdbinit --gdb-tui 1

# Run this manually - you want the second cert, the LetsEncrypt root CA one
server_cert:
	echo -n | openssl s_client -showcerts -connect spotcheck.brianteam.dev:443

# Assumes current commit is the one to be released and is tagged with correct version
release:
	./release.sh

# Just saving rough command for the future, not really needed as a target
font:
	python fontconvert.py FiraSans_15 15 ~/Library/Fonts/FiraSans-Regular.ttf /System/Library/Fonts/HelveticaNeue.ttc > ~/Developer/spot-check-firmware/main/include/firasans_15.h

# Just saving rough command for the future, not really needed as a target
provision:
	python3 ~/Developer/esp/esp-idf/tools/esp_prov/esp_prov.py --transport softap --ssid OceanBreeze211 --passphrase OceanBreeze211
