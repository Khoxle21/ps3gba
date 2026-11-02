# CMake toolchain for PSL1GHT (PS3 PPU), used to cross-build libmgba.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR powerpc64)

set(PS3DEV $ENV{PS3DEV})
set(CMAKE_C_COMPILER ${PS3DEV}/ppu/bin/ppu-gcc)
set(CMAKE_CXX_COMPILER ${PS3DEV}/ppu/bin/ppu-g++)
set(CMAKE_AR ${PS3DEV}/ppu/bin/ppu-ar CACHE FILEPATH "")
set(CMAKE_RANLIB ${PS3DEV}/ppu/bin/ppu-ranlib CACHE FILEPATH "")

set(PS3_FLAGS "-U__STRICT_ANSI__ -DPATH_MAX=4096 -mcpu=cell -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections -I${PS3DEV}/ppu/include")
set(CMAKE_C_FLAGS_INIT "${PS3_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${PS3_FLAGS}")

set(CMAKE_FIND_ROOT_PATH ${PS3DEV}/ppu ${PS3DEV}/portlibs/ppu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
