#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME := spot-check-embedded

include $(IDF_PATH)/make/project.mk

debug:
	idf.py openocd gdb --gdbinit .gdbinit --gdb-tui 1
