# vibd & SapphireHaptics Makefile
# Usage: make [OUTDIR=../obj] [SDK=/path/to/iPhoneOS16.5.sdk]

OUTDIR ?= ../obj
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SDK ?= $(shell xcrun --sdk iphoneos --show-sdk-path)
CLANG ?= $(shell xcrun -sdk iphoneos -find clang)
LDEXTRA ?=
else
SDK ?= $(HOME)/sdks/theos-sdks/iPhoneOS16.5.sdk
CLANG ?= /usr/lib/llvm-18/bin/clang
LDEXTRA ?= -fuse-ld=lld
endif
LDID ?= $(shell command -v ldid 2>/dev/null || echo $(HOME)/tools-dl/ldid/ldid)
CC = $(CLANG) -target arm64-apple-ios15.0 -isysroot $(SDK) $(LDEXTRA) -O2 -Wall -Wextra

all: $(OUTDIR)/vibd $(OUTDIR)/SapphireHaptics.dylib

$(OUTDIR)/vibd: vibd.c
	@mkdir -p $(OUTDIR)
	$(CC) -o $@ $<
	$(LDID) -S $@
	@echo "Built: $@"

$(OUTDIR)/SapphireHaptics.dylib: SapphireHaptics.m
	@mkdir -p $(OUTDIR)
	$(CC) -dynamiclib -install_name /var/jb/usr/lib/TweakInject/SapphireHaptics.dylib -o $@ $< -framework Foundation -framework UIKit -framework AudioToolbox -ldl
	$(LDID) -S $@
	@echo "Built: $@"

clean:
	rm -f $(OUTDIR)/vibd $(OUTDIR)/SapphireHaptics.dylib

install: $(OUTDIR)/vibd $(OUTDIR)/SapphireHaptics.dylib
	cp $(OUTDIR)/vibd /redwood/bin/vibd
	cp com.sapphire.vibd.plist /redwood/launchd/
	chmod 755 /redwood/bin/vibd
	chmod 644 /redwood/launchd/com.sapphire.vibd.plist
	mkdir -p /redwood/tweaks
	cp $(OUTDIR)/SapphireHaptics.dylib /redwood/tweaks/
	cp SapphireHaptics.plist /redwood/tweaks/
	chmod 755 /redwood/tweaks/SapphireHaptics.dylib
	chmod 644 /redwood/tweaks/SapphireHaptics.plist

.PHONY: all clean install