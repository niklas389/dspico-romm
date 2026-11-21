# SPDX-License-Identifier: MIT
#
# dspico-romm proof of concept.
#
# Required packages (Wonderful Toolchain / BlocksDS):
#   wf-pacman -Sy blocksds-toolchain toolchain-gcc-arm-none-eabi-zlib \
#                 blocksds-mbedtls blocksds-libcurl

BLOCKSDS	?= /opt/blocksds/core
BLOCKSDSEXT	?= /opt/blocksds/external

NAME		:= romm-poc
GAME_TITLE	:= RomM PoC
GAME_SUBTITLE	:= DSPico RomM client
GAME_AUTHOR	:= dspico-romm

SOURCEDIRS	:= source
INCLUDEDIRS	:= source source/third_party/cjson source/third_party/qrcodegen

ARM7ELF		:= $(BLOCKSDS)/sys/arm7/main_core/arm7_dswifi_maxmod.elf

LIBS		:= -ldswifi9 -lnds9 -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz

LIBDIRS		:= $(BLOCKSDS)/libs/dswifi \
		   $(BLOCKSDS)/libs/libnds \
		   $(BLOCKSDSEXT)/libcurl \
		   $(BLOCKSDSEXT)/mbedtls

DEFINES		:= -DPOC_VERSION=\"0.1.0\"

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile
