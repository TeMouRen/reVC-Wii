# ============================================================
# Wii toolchain configuration
# ============================================================
set(CMAKE_SYSTEM_NAME Generic)

# 1. Compiler configuration
set(CMAKE_C_COMPILER "/c/devkitPro/devkitPPC/bin/powerpc-eabi-gcc.exe")
set(CMAKE_CXX_COMPILER "/c/devkitPro/devkitPPC/bin/powerpc-eabi-g++.exe")

# 2. devkitPro / libogc2 paths
set(DEVKITPRO "/c/devkitPro")
set(LIBOGC2_INC "${DEVKITPRO}/libogc2/include")
set(LIBOGC2_LIB "${DEVKITPRO}/libogc2/lib/wii")
set(GCDSPTOOL_EXECUTABLE "${DEVKITPRO}/tools/bin/gcdsptool.exe")

# 3. Wii-specific compile flags
set(WII_COMPILE_FLAGS "-O2 -mrvl -mcpu=750 -meabi -mhard-float -I${LIBOGC2_INC}")

set(CMAKE_CXX_FLAGS "${WII_COMPILE_FLAGS}" CACHE STRING "CXX Flags" FORCE)
set(CMAKE_C_FLAGS   "${WII_COMPILE_FLAGS}" CACHE STRING "C Flags" FORCE)

# 4. Wii-specific link flags
set(WII_LINK_FLAGS "-mrvl -mcpu=750 -meabi -mhard-float -L\"${LIBOGC2_LIB}\"")

set(CMAKE_EXE_LINKER_FLAGS "${WII_LINK_FLAGS}" CACHE STRING "Linker Flags" FORCE)

# 5. Global defines
add_definitions(-DGEKKO -DHW_RVL -DWII -DGAMECUBE)

# 6. Include path
include_directories(${LIBOGC2_INC})
