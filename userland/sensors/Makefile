# sensord Makefile
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

all: $(OUTDIR)/sensord $(OUTDIR)/snsprobe $(OUTDIR)/sensload

$(OUTDIR)/sensord: sensord.c sns_wire.h
	@mkdir -p $(OUTDIR)
	$(CC) -o $@ $< -lm
	$(LDID) -S $@
	@echo "Built: $@"

$(OUTDIR)/snsprobe: snsprobe.c
	@mkdir -p $(OUTDIR)
	$(CC) -o $@ $< -lm
	$(LDID) -S $@
	@echo "Built: $@"

$(OUTDIR)/sensload: sensload.c
	@mkdir -p $(OUTDIR)
	$(CC) -o $@ $<
	$(LDID) -S $@
	@echo "Built: $@"

clean:
	rm -f $(OUTDIR)/sensord $(OUTDIR)/snsprobe $(OUTDIR)/sensload

install: $(OUTDIR)/sensord
	cp $(OUTDIR)/sensord /redwood/bin/sensord
	cp com.sapphire.sensord.plist /redwood/launchd/
	chmod 755 /redwood/bin/sensord
	chmod 644 /redwood/launchd/com.sapphire.sensord.plist

.PHONY: all clean install