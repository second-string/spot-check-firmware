#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME := spot-check-embedded

include $(IDF_PATH)/make/project.mk

debug:
	idf.py openocd gdb --gdbinit .gdbinit --gdb-tui 1

# Run this manually - you want the second cert, the root CA one
server_cert:
	echo -n | openssl s_client -connect spotcheck.brianteam.dev:443 -verify 5
