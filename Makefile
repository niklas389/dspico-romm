# SPDX-License-Identifier: MIT
#
# dspico-romm proof of concept.
#
# Required packages (Wonderful Toolchain / BlocksDS):
#   wf-pacman -Sy blocksds-toolchain toolchain-gcc-arm-none-eabi-zlib \
#                 blocksds-mbedtls blocksds-libcurl
#
# Targets:
#   make            romm-poc.nds with the stock DSWiFi (TCP window 5840 B)
#   make variants   also builds romm-poc-wnd32.nds with a 46720 B TCP window

BLOCKSDS	?= /opt/blocksds/core
BLOCKSDSEXT	?= /opt/blocksds/external

NAME		?= romm-poc
GAME_TITLE	:= RomM PoC
GAME_SUBTITLE	:= DSPico RomM client
GAME_AUTHOR	:= dspico-romm

# Alternative DSWiFi build (see tools/build_dswifi.sh). Defaults to the SDK's.
DSWIFI_DIR	?= $(BLOCKSDS)/libs/dswifi
WND_LABEL	?= stock-4x1460
WND_BYTES	?= 5840

SOURCEDIRS	:= source
INCLUDEDIRS	:= source source/third_party/cjson source/third_party/qrcodegen

ARM7ELF		:= $(BLOCKSDS)/sys/arm7/main_core/arm7_dswifi_maxmod.elf

LIBS		:= -ldswifi9 -lnds9 -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz

LIBDIRS		:= $(DSWIFI_DIR) \
		   $(BLOCKSDS)/libs/libnds \
		   $(BLOCKSDSEXT)/libcurl \
		   $(BLOCKSDSEXT)/mbedtls

DEFINES		:= -DPOC_VERSION=\"0.2.0\" -DTCP_WND_LABEL="\"$(WND_LABEL)\"" \
		   -DTCP_WND_BYTES=$(WND_BYTES)

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile

.PHONY: variants

build/dswifi-wnd32/lib/libdswifi9.a:
	tools/build_dswifi.sh 32 build/dswifi-wnd32

variants: all build/dswifi-wnd32/lib/libdswifi9.a
	$(MAKE) NAME=romm-poc-wnd32 DSWIFI_DIR=$(CURDIR)/build/dswifi-wnd32 \
		WND_LABEL=32x1460 WND_BYTES=46720
