cmake_minimum_required(VERSION 3.14)

set(EXECUTABLE reVC)
set(PROJECT REVC)

project(${EXECUTABLE} C CXX)

if(NOT (WII OR GAMECUBE))
    message(FATAL_ERROR "This cleaned tree only supports Wii/GameCube builds.")
endif()

set(${PROJECT}_AUTHOR "${PROJECT} Team")
set(${PROJECT}_AUDIOS "NULL")
set(${PROJECT}_AUDIO "NULL" CACHE STRING "Audio" FORCE)
set_property(CACHE ${PROJECT}_AUDIO PROPERTY STRINGS ${${PROJECT}_AUDIOS})
set("${PROJECT}_AUDIO_${${PROJECT}_AUDIO}" ON)

set(LIBRW_PLATFORM_NULL OFF CACHE BOOL "" FORCE)
set(LIBRW_PLATFORM_GL3  OFF CACHE BOOL "" FORCE)
set(LIBRW_GL3          OFF CACHE BOOL "" FORCE)
set(LIBRW_D3D9         OFF CACHE BOOL "" FORCE)
set(LIBRW_PLATFORM_GX  ON  CACHE BOOL "" FORCE)

message(STATUS "------------------------------------------------")
if(WII)
    message(STATUS "  Target Platform: Nintendo Wii (devkitPPC)")
else()
    message(STATUS "  Target Platform: Nintendo GameCube (devkitPPC)")
endif()
message(STATUS "  Renderer:        GX (librw GX backend)")
message(STATUS "  Audio:           NULL")
message(STATUS "------------------------------------------------")

if(NOT COMMAND reVC_platform_target)
    function(reVC_platform_target)
    endfunction()
endif()

add_subdirectory(vendor/librw)
add_subdirectory(src)

target_compile_definitions(${EXECUTABLE} PUBLIC
    GAMECUBE
    GTA_PC
    RW_GX
    RWDEVICE=gx
    RW_BIG_ENDIAN
)

if(WII)
    target_compile_definitions(${EXECUTABLE} PUBLIC WII)
endif()

message(STATUS "[reVC] Nintendo GX compile definitions configured")
