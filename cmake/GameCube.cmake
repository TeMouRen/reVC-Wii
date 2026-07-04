# ============================================================
# GameCube toolchain configuration
# ============================================================
set(CMAKE_SYSTEM_NAME Generic)

# 1. Compiler configuration
set(CMAKE_C_COMPILER "/c/devkitPro/devkitPPC/bin/powerpc-eabi-gcc.exe")
set(CMAKE_CXX_COMPILER "/c/devkitPro/devkitPPC/bin/powerpc-eabi-g++.exe")

# 2. devkitPro / libogc2 paths
set(DEVKITPRO "C:/devkitPro")
set(LIBOGC2_INC "${DEVKITPRO}/libogc2/gamecube/include")
set(LIBOGC2_LIB "${DEVKITPRO}/libogc2/gamecube/lib")

# 3. GameCube-specific compile flags
set(GC_COMPILE_FLAGS "-O2 -mogc -mcpu=750 -meabi -mhard-float -I${LIBOGC2_INC}")

set(CMAKE_CXX_FLAGS "${GC_COMPILE_FLAGS}" CACHE STRING "CXX Flags" FORCE)
set(CMAKE_C_FLAGS   "${GC_COMPILE_FLAGS}" CACHE STRING "C Flags" FORCE)

# 4. GameCube-specific link flags
set(GC_LINK_FLAGS "-mogc -mcpu=750 -meabi -mhard-float -L\"${LIBOGC2_LIB}\"")

set(CMAKE_EXE_LINKER_FLAGS "${GC_LINK_FLAGS}" CACHE STRING "Linker Flags" FORCE)

# 5. Global defines
add_definitions(-DGEKKO -DGAMECUBE)

# 6. Include path
include_directories(${LIBOGC2_INC})
