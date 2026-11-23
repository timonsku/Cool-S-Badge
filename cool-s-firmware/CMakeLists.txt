#
# Copyright (c) 2018 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#
cmake_minimum_required(VERSION 3.20.0)

# Lua for the LED animation scripts, a git submodule:
# git submodule update --init --recursive
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_zephyr)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(NONE)


# NORDIC SDK APP START
target_sources(app PRIVATE
  src/main.c
  src/lodepng.c
  src/gifdec.c
  src/media_mgmt.c
  src/media_files.c
  src/preview.c
  src/update_approval.c
  src/update_trial.c
  src/lua_anim.c
)

# Only decoding is needed, media_files.c writes its PNGs itself and has a
# smaller CRC. Leaves out about 20 KB of the nearly full application slot.
target_compile_definitions(app PRIVATE
  LODEPNG_NO_COMPILE_ENCODER
  LODEPNG_NO_COMPILE_DISK
  LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
  LODEPNG_NO_COMPILE_CPP
  LODEPNG_NO_COMPILE_CRC
)

# Recursion in C the Lua parser and VM may use, sized for the stack in lua_anim.c
zephyr_compile_definitions(LUAI_MAXCCALLS=32)

# update_approval.c decides whether another image may be scheduled to boot
zephyr_ld_options(-Wl,--wrap=boot_set_next)
target_link_libraries(app PRIVATE MCUBOOT_BOOTUTIL)

# NORDIC SDK APP END
