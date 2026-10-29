# Builds Diary for MUI (MorphOS, AmigaOS 3.x/4 with MUI) with bebbo's
# amiga-gcc and its MUI SDK (make sdk=mui sdk=mcc_texteditor):
#   make            build Diary and its icon into dist/
#   make install    copy dist/ to the Amiberry shared drive
TOOLCHAIN ?= $(HOME)/Projects/amiga-toolchain
CC := $(TOOLCHAIN)/bin/m68k-amigaos-gcc
CFLAGS := -Os -m68020 -fno-common -Wall -Wextra -Wno-unused-parameter -Wno-pointer-sign -fomit-frame-pointer
LDFLAGS := -s -noixemul
LIBS := -lmui
INSTALL_DIR ?= $(HOME)/Amiberry/HardDrives/Shared/Diary-MUI

all: dist/Diary dist/Diary.info

dist/Diary: diary.c net.c net.h | dist
	$(CC) $(CFLAGS) -o $@ diary.c net.c $(LDFLAGS) $(LIBS)

dist/Diary.info: tools/make_icon.py | dist
	python3 tools/make_icon.py $@

dist:
	mkdir -p dist

install: all
	mkdir -p "$(INSTALL_DIR)"
	cp dist/Diary dist/Diary.info "$(INSTALL_DIR)/"

clean:
	rm -rf dist

.PHONY: all install clean
