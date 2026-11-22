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
#   make variants   also builds, with a patched DSWiFi (tools/build_dswifi.sh):
#                     romm-poc-rx128.nds        46720 B TCP window, 128 KiB RX buffer
#                   (SD driver on the ARM9, cached RX ring reads, MTU 1500)

BLOCKSDS	?= /opt/blocksds/core
BLOCKSDSEXT	?= /opt/blocksds/external

NAME		?= romm-poc
GAME_TITLE	:= RomM PoC
GAME_SUBTITLE	:= DSPico RomM client
GAME_AUTHOR	:= dspico-romm

# Alternative DSWiFi build (see tools/build_dswifi.sh). Defaults to the SDK's.
DSWIFI_DIR	?= $(BLOCKSDS)/libs/dswifi
WND_LABEL	?= stock-4x1460-rx12
WND_BYTES	?= 5840

SOURCEDIRS	:= source
INCLUDEDIRS	:= source source/third_party/cjson source/third_party/qrcodegen \
		   source/third_party/openssl-armv4

# The ARM7 core must match the DSWiFi build (shared RX buffer layout).
ARM7ELF		?= $(BLOCKSDS)/sys/arm7/main_core/arm7_dswifi.elf

LIBS		:= -ldswifi9 -lnds9 -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz \
		   -Wl,--wrap=poll -Wl,--wrap=select -Wl,--wrap=recv \
		   -Wl,--wrap=memcpy -Wl,--wrap=__aeabi_memcpy \
		   -Wl,--wrap=__aeabi_memcpy4 -Wl,--wrap=__aeabi_memcpy8

LIBDIRS		:= $(DSWIFI_DIR) \
		   $(BLOCKSDS)/libs/libnds \
		   $(BLOCKSDSEXT)/libcurl \
		   $(BLOCKSDSEXT)/mbedtls

DEFINES		:= -DPOC_VERSION=\"0.35.0\" -DTCP_WND_LABEL="\"$(WND_LABEL)\"" \
		   -DTCP_WND_BYTES=$(WND_BYTES) $(EXTRA_DEFINES)

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile

.PHONY: variants

build/dswifi-rx128/arm7_dswifi.elf:
	tools/build_dswifi.sh 32 128 build/dswifi-rx128

# The SD driver runs on the ARM9 (measured: SD writes 4.4 vs 1.5 MB/s, and the
# ARM7 stays free for Wi-Fi).
variants: all build/dswifi-rx128/arm7_dswifi.elf
	$(MAKE) NAME=romm-poc-rx128 DSWIFI_DIR=$(CURDIR)/build/dswifi-rx128 \
		ARM7ELF=$(CURDIR)/build/dswifi-rx128/arm7_dswifi.elf \
		WND_LABEL=32x1460-rx128 WND_BYTES=46720 \
		EXTRA_DEFINES="-DDLDI_FORCE_ARM9 -DDSPICO_DSWIFI"
