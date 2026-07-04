cmake_minimum_required(VERSION 3.10)
cmake_policy(SET CMP0079 NEW)
cmake_policy(SET CMP0076 NEW)

project(librw
    VERSION 0.0.1
    LANGUAGES C CXX
)

if(NOT (WII OR GAMECUBE))
    message(FATAL_ERROR "This cleaned librw copy is trimmed for Wii/GameCube GX only.")
endif()

set(LIBRW_PLATFORMS "GX")
set(LIBRW_PLATFORM "GX" CACHE STRING "Platform" FORCE)
set_property(CACHE LIBRW_PLATFORM PROPERTY STRINGS ${LIBRW_PLATFORMS})
set(LIBRW_PLATFORM_GX ON)
message(STATUS "LIBRW_PLATFORM = ${LIBRW_PLATFORM} (Wii/GameCube GX-only clean copy)")

if(NOT COMMAND librw_platform_target)
    function(librw_platform_target)
    endfunction()
endif()

option(LIBRW_TOOLS "Build librw tools" OFF)
option(LIBRW_INSTALL "Install librw files" OFF)
set(LIBRW_EXAMPLES OFF CACHE BOOL "Build librw examples" FORCE)

add_subdirectory(src)
