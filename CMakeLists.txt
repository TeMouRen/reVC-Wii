cmake_minimum_required(VERSION 3.14)

set(EXECUTABLE reVC)
set(PROJECT REVC)

project(${EXECUTABLE} C CXX)

if(NOT (WII OR GAMECUBE))
    message(FATAL_ERROR "This cleaned tree only supports Wii/GameCube builds.")
endif()

option(ENABLE_MISSION_REPLAY "Enable mobile mission replay support on PC_MENU targets" OFF)
option(WII_STREAM_MEMORY_DIAGNOSTICS "Enable detailed Wii streaming diagnostics" OFF)
option(WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS "Emit per-resource Wii streaming diagnostic events" OFF)
option(WII_ISLAND_READ_LIVENESS "Keep normal view streaming live during Wii island READ/COMMIT" OFF)
option(WII_AUDIO_DECODE_ENABLE "Enable Wii audio allocation, decode, and playback" ON)
set(WII_MEMORY_PROFILE_ID "P7-noaudio-ps2-world-scan" CACHE STRING "Wii memory policy profile identifier")
set(WII_BUILD_ID "unknown" CACHE STRING "Unique Wii build identifier")
set(WII_GX_ARENA2_EXPANSION_BYTES "0" CACHE STRING "Additional fixed GX Arena2 bytes for a validated Wii memory profile")
set(WII_MEM2_RESOURCE_POOL_BYTES "18874368" CACHE STRING "Fixed generic MEM2 resource-pool bytes for a validated Wii memory profile")
set(WII_AUDIO_MEM2_POOL_BYTES "0" CACHE STRING "Fixed dedicated audio MEM2 bytes reserved at Wii startup")

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
message(STATUS "  Mission Replay:  ${ENABLE_MISSION_REPLAY}")
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
    foreach(identifier WII_MEMORY_PROFILE_ID WII_BUILD_ID)
        if(NOT "${${identifier}}" MATCHES "^[A-Za-z0-9._-]+$")
            message(FATAL_ERROR "${identifier} must contain only A-Z, a-z, 0-9, dot, underscore, or hyphen")
        endif()
    endforeach()
    if(NOT WII_GX_ARENA2_EXPANSION_BYTES MATCHES "^(0|2097152|6291456|8388608)$")
        message(FATAL_ERROR "WII_GX_ARENA2_EXPANSION_BYTES must be 0, 2097152, 6291456, or 8388608")
    endif()
    if(NOT WII_MEM2_RESOURCE_POOL_BYTES MATCHES "^(6291456|8388608|18874368)$")
        message(FATAL_ERROR "WII_MEM2_RESOURCE_POOL_BYTES must be 6291456, 8388608, or 18874368")
    endif()
    if(NOT WII_AUDIO_MEM2_POOL_BYTES MATCHES "^(0|4194304|6291456)$")
        message(FATAL_ERROR "WII_AUDIO_MEM2_POOL_BYTES must be 0, 4194304, or 6291456")
    endif()
    if(WII_MEMORY_PROFILE_ID STREQUAL "P0-current")
        if(WII_ISLAND_READ_LIVENESS OR NOT WII_AUDIO_DECODE_ENABLE OR
           NOT WII_GX_ARENA2_EXPANSION_BYTES STREQUAL "0" OR
           NOT WII_MEM2_RESOURCE_POOL_BYTES STREQUAL "18874368" OR
           NOT WII_AUDIO_MEM2_POOL_BYTES STREQUAL "0")
            message(FATAL_ERROR "P0-current requires liveness=OFF, audio=ON, GX expansion=0, generic MEM2=18874368, and audio MEM2=0")
        endif()
    elseif(WII_MEMORY_PROFILE_ID STREQUAL "P1-read-liveness")
        if(NOT WII_ISLAND_READ_LIVENESS OR NOT WII_AUDIO_DECODE_ENABLE OR
           NOT WII_GX_ARENA2_EXPANSION_BYTES STREQUAL "0" OR
           NOT WII_MEM2_RESOURCE_POOL_BYTES STREQUAL "18874368" OR
           NOT WII_AUDIO_MEM2_POOL_BYTES STREQUAL "0")
            message(FATAL_ERROR "P1-read-liveness requires liveness=ON, audio=ON, GX expansion=0, generic MEM2=18874368, and audio MEM2=0")
        endif()
    elseif(WII_MEMORY_PROFILE_ID STREQUAL "P2-gx-plus2m")
        if(WII_ISLAND_READ_LIVENESS OR NOT WII_AUDIO_DECODE_ENABLE OR
           NOT WII_GX_ARENA2_EXPANSION_BYTES STREQUAL "2097152" OR
           NOT WII_MEM2_RESOURCE_POOL_BYTES STREQUAL "18874368" OR
           NOT WII_AUDIO_MEM2_POOL_BYTES STREQUAL "0")
            message(FATAL_ERROR "P2-gx-plus2m requires liveness=OFF, audio=ON, GX expansion=2097152, generic MEM2=18874368, and audio MEM2=0")
        endif()
    elseif(WII_MEMORY_PROFILE_ID STREQUAL "P3-noaudio-reclaim12m")
        if(WII_ISLAND_READ_LIVENESS OR WII_AUDIO_DECODE_ENABLE OR
           NOT WII_GX_ARENA2_EXPANSION_BYTES STREQUAL "8388608" OR
           NOT WII_MEM2_RESOURCE_POOL_BYTES STREQUAL "6291456" OR
           NOT WII_AUDIO_MEM2_POOL_BYTES STREQUAL "0")
            message(FATAL_ERROR "P3-noaudio-reclaim12m requires liveness=OFF, audio=OFF, GX expansion=8388608, generic MEM2=6291456, and audio MEM2=0")
        endif()
    elseif(WII_MEMORY_PROFILE_ID STREQUAL "P4-noaudio-balanced" OR
           WII_MEMORY_PROFILE_ID STREQUAL "P5-noaudio-global-lru" OR
           WII_MEMORY_PROFILE_ID STREQUAL "P6-noaudio-ps2-policy" OR
           WII_MEMORY_PROFILE_ID STREQUAL "P7-noaudio-ps2-world-scan" OR
           WII_MEMORY_PROFILE_ID STREQUAL "P14-noaudio-atomic-handoff" OR
           WII_MEMORY_PROFILE_ID STREQUAL "P15-noaudio-hud-weapon-pin" OR
           WII_MEMORY_PROFILE_ID STREQUAL "P16-noaudio-gx-headroom-guard")
        if(WII_ISLAND_READ_LIVENESS OR WII_AUDIO_DECODE_ENABLE OR
           NOT WII_GX_ARENA2_EXPANSION_BYTES STREQUAL "6291456" OR
           NOT WII_MEM2_RESOURCE_POOL_BYTES STREQUAL "8388608" OR
           NOT WII_AUDIO_MEM2_POOL_BYTES STREQUAL "0")
            message(FATAL_ERROR "P4/P5/P6/P7/P14/P15/P16 no-audio profiles require liveness=OFF, audio=OFF, GX expansion=6291456, generic MEM2=8388608, and audio MEM2=0")
        endif()
    elseif(WII_MEMORY_PROFILE_ID STREQUAL "A0-audio-cache" OR
           WII_MEMORY_PROFILE_ID STREQUAL "A1-audio-ps2-world-scan")
        if(WII_ISLAND_READ_LIVENESS OR NOT WII_AUDIO_DECODE_ENABLE OR
           NOT WII_GX_ARENA2_EXPANSION_BYTES STREQUAL "6291456" OR
           NOT WII_MEM2_RESOURCE_POOL_BYTES STREQUAL "8388608" OR
           NOT WII_AUDIO_MEM2_POOL_BYTES STREQUAL "4194304")
            message(FATAL_ERROR "A0/A1 audio-cache profiles require liveness=OFF, audio=ON, GX expansion=6291456, generic MEM2=8388608, and audio MEM2=4194304")
        endif()
    else()
        message(FATAL_ERROR "Unsupported Wii memory profile: ${WII_MEMORY_PROFILE_ID}")
    endif()
    if(WII_MEMORY_PROFILE_ID STREQUAL "P3-noaudio-reclaim12m" OR
       WII_MEMORY_PROFILE_ID STREQUAL "P4-noaudio-balanced" OR
       WII_MEMORY_PROFILE_ID STREQUAL "P5-noaudio-global-lru" OR
       WII_MEMORY_PROFILE_ID STREQUAL "P6-noaudio-ps2-policy" OR
       WII_MEMORY_PROFILE_ID STREQUAL "P14-noaudio-atomic-handoff" OR
       WII_MEMORY_PROFILE_ID STREQUAL "P15-noaudio-hud-weapon-pin" OR
       WII_MEMORY_PROFILE_ID STREQUAL "P16-noaudio-gx-headroom-guard" OR
       WII_MEMORY_PROFILE_ID STREQUAL "A0-audio-cache" OR
       WII_MEMORY_PROFILE_ID STREQUAL "A1-audio-ps2-world-scan")
        set(WII_STREAM_ADAPTIVE_ARCHIVE_CEILING_VALUE 1)
    else()
        set(WII_STREAM_ADAPTIVE_ARCHIVE_CEILING_VALUE 0)
    endif()
    if(WII_MEMORY_PROFILE_ID STREQUAL "P5-noaudio-global-lru" OR
       WII_MEMORY_PROFILE_ID STREQUAL "A0-audio-cache")
        set(WII_STREAM_ARCHIVE_GLOBAL_LRU_VALUE 1)
    else()
        set(WII_STREAM_ARCHIVE_GLOBAL_LRU_VALUE 0)
    endif()
    if(WII_MEMORY_PROFILE_ID STREQUAL "P6-noaudio-ps2-policy")
        set(WII_STREAM_PS2_TRANSITION_PURGE_VALUE 1)
    else()
        set(WII_STREAM_PS2_TRANSITION_PURGE_VALUE 0)
    endif()
    if(WII_MEMORY_PROFILE_ID STREQUAL "P7-noaudio-ps2-world-scan" OR
       WII_MEMORY_PROFILE_ID STREQUAL "P14-noaudio-atomic-handoff" OR
       WII_MEMORY_PROFILE_ID STREQUAL "P15-noaudio-hud-weapon-pin" OR
       WII_MEMORY_PROFILE_ID STREQUAL "P16-noaudio-gx-headroom-guard" OR
       WII_MEMORY_PROFILE_ID STREQUAL "A1-audio-ps2-world-scan")
        set(WII_STREAM_PS2_WORLD_SCAN_RADIUS_VALUE 1)
    else()
        set(WII_STREAM_PS2_WORLD_SCAN_RADIUS_VALUE 0)
    endif()
    if(WII_MEMORY_PROFILE_ID STREQUAL "P14-noaudio-atomic-handoff")
        set(WII_STREAM_ATOMIC_BIG_HANDOFF_VALUE 1)
    elseif(WII_MEMORY_PROFILE_ID STREQUAL "P15-noaudio-hud-weapon-pin" OR
           WII_MEMORY_PROFILE_ID STREQUAL "P16-noaudio-gx-headroom-guard")
        set(WII_STREAM_ATOMIC_BIG_HANDOFF_VALUE 1)
    else()
        set(WII_STREAM_ATOMIC_BIG_HANDOFF_VALUE 0)
    endif()
    if(WII_MEMORY_PROFILE_ID STREQUAL "A1-audio-ps2-world-scan" OR
       WII_MEMORY_PROFILE_ID STREQUAL "P15-noaudio-hud-weapon-pin" OR
       WII_MEMORY_PROFILE_ID STREQUAL "P16-noaudio-gx-headroom-guard")
        set(WII_HUD_ACTIVE_WEAPON_RASTER_PIN_VALUE 1)
    else()
        set(WII_HUD_ACTIVE_WEAPON_RASTER_PIN_VALUE 0)
    endif()
    if(WII_MEMORY_PROFILE_ID STREQUAL "P15-noaudio-hud-weapon-pin" OR
       WII_MEMORY_PROFILE_ID STREQUAL "P16-noaudio-gx-headroom-guard")
        set(WII_HUD_USED_WEAPON_RASTER_PERSIST_VALUE 1)
        set(WII_GX_OPAQUE_RGBA8_RGB565_TIER_VALUE 1)
        set(WII_FONT_PERSISTENT_UPLOAD_VALUE 1)
    else()
        set(WII_HUD_USED_WEAPON_RASTER_PERSIST_VALUE 0)
        set(WII_GX_OPAQUE_RGBA8_RGB565_TIER_VALUE 0)
        set(WII_FONT_PERSISTENT_UPLOAD_VALUE 0)
    endif()
    # P7 needs the existing admission guard to start GX retirement before
    # conversion reaches hard pressure. P16 keeps the same guard explicitly.
    if(WII_MEMORY_PROFILE_ID STREQUAL "P7-noaudio-ps2-world-scan" OR
       WII_MEMORY_PROFILE_ID STREQUAL "P16-noaudio-gx-headroom-guard")
        set(WII_STREAM_GX_HEADROOM_GUARD_VALUE 1)
    else()
        set(WII_STREAM_GX_HEADROOM_GUARD_VALUE 0)
    endif()
    if(WII_MEMORY_PROFILE_ID STREQUAL "A1-audio-ps2-world-scan")
        set(WII_STREAM_P7_BLOCKING_HANDOFF_VALUE 1)
    else()
        set(WII_STREAM_P7_BLOCKING_HANDOFF_VALUE 0)
    endif()
    if(WII_MEMORY_PROFILE_ID STREQUAL "P7-noaudio-ps2-world-scan")
        set(WII_STREAM_SPLASH_VISUAL_GATE_VALUE 1)
    else()
        set(WII_STREAM_SPLASH_VISUAL_GATE_VALUE 0)
    endif()
    if(WII_MEMORY_PROFILE_ID STREQUAL "A1-audio-ps2-world-scan")
        set(WII_STREAM_P7_VISIBLE_TXD_GUARD_VALUE 1)
    else()
        set(WII_STREAM_P7_VISIBLE_TXD_GUARD_VALUE 0)
    endif()
    if(WII_MEMORY_PROFILE_ID STREQUAL "A0-audio-cache" OR
       WII_MEMORY_PROFILE_ID STREQUAL "A1-audio-ps2-world-scan")
        set(WII_AUDIO_PCM_CACHE_ENABLE_VALUE 1)
    else()
        set(WII_AUDIO_PCM_CACHE_ENABLE_VALUE 0)
    endif()
    if(WII_AUDIO_DECODE_ENABLE)
        set(WII_AUDIO_DECODE_ENABLE_VALUE 1)
    else()
        set(WII_AUDIO_DECODE_ENABLE_VALUE 0)
    endif()
    math(EXPR WII_MEM2_RECLAIMED_BYTES "18874368 - ${WII_MEM2_RESOURCE_POOL_BYTES}")
    target_compile_definitions(${EXECUTABLE} PUBLIC WII)
    target_compile_definitions(${EXECUTABLE} PUBLIC
        WII_MEMORY_PROFILE_ID="${WII_MEMORY_PROFILE_ID}"
        WII_BUILD_ID="${WII_BUILD_ID}"
        WII_GX_ARENA2_EXPANSION_BYTES=${WII_GX_ARENA2_EXPANSION_BYTES}
        WII_MEM2_RESOURCE_POOL_BYTES=${WII_MEM2_RESOURCE_POOL_BYTES}
        WII_MEM2_RECLAIMED_BYTES=${WII_MEM2_RECLAIMED_BYTES}
        WII_AUDIO_MEM2_POOL_BYTES=${WII_AUDIO_MEM2_POOL_BYTES}
        WII_AUDIO_DECODE_ENABLE=${WII_AUDIO_DECODE_ENABLE_VALUE}
        WII_AUDIO_PCM_CACHE_ENABLE=${WII_AUDIO_PCM_CACHE_ENABLE_VALUE}
        WII_AUDIO_BASELINE_REQUESTED_BYTES=14177248
        WII_STREAM_ADAPTIVE_ARCHIVE_CEILING=${WII_STREAM_ADAPTIVE_ARCHIVE_CEILING_VALUE}
        WII_STREAM_ARCHIVE_GLOBAL_LRU=${WII_STREAM_ARCHIVE_GLOBAL_LRU_VALUE}
        WII_STREAM_PS2_TRANSITION_PURGE=${WII_STREAM_PS2_TRANSITION_PURGE_VALUE}
        WII_STREAM_PS2_WORLD_SCAN_RADIUS=${WII_STREAM_PS2_WORLD_SCAN_RADIUS_VALUE}
        WII_STREAM_GX_HEADROOM_GUARD=${WII_STREAM_GX_HEADROOM_GUARD_VALUE}
        WII_STREAM_P7_BLOCKING_HANDOFF=${WII_STREAM_P7_BLOCKING_HANDOFF_VALUE}
        WII_STREAM_SPLASH_VISUAL_GATE=${WII_STREAM_SPLASH_VISUAL_GATE_VALUE}
        WII_STREAM_P7_VISIBLE_TXD_GUARD=${WII_STREAM_P7_VISIBLE_TXD_GUARD_VALUE}
        WII_STREAM_ATOMIC_BIG_HANDOFF=${WII_STREAM_ATOMIC_BIG_HANDOFF_VALUE}
        WII_HUD_ACTIVE_WEAPON_RASTER_PIN=${WII_HUD_ACTIVE_WEAPON_RASTER_PIN_VALUE}
        WII_HUD_USED_WEAPON_RASTER_PERSIST=${WII_HUD_USED_WEAPON_RASTER_PERSIST_VALUE}
        WII_FONT_PERSISTENT_UPLOAD=${WII_FONT_PERSISTENT_UPLOAD_VALUE}
    )
    # gxmemory.cpp is compiled by librw, not by the game executable. Keep the
    # fixed-boundary experiment definition on that target as well.
    target_compile_definitions(librw PRIVATE
        WII_GX_ARENA2_EXPANSION_BYTES=${WII_GX_ARENA2_EXPANSION_BYTES}
        WII_GX_OPAQUE_RGBA8_RGB565_TIER=${WII_GX_OPAQUE_RGBA8_RGB565_TIER_VALUE}
    )
    if(WII_ISLAND_READ_LIVENESS)
        target_compile_definitions(${EXECUTABLE} PUBLIC WII_ISLAND_READ_LIVENESS=1)
    else()
        target_compile_definitions(${EXECUTABLE} PUBLIC WII_ISLAND_READ_LIVENESS=0)
    endif()
    if(WII_STREAM_MEMORY_DIAGNOSTICS)
        target_compile_definitions(${EXECUTABLE} PUBLIC WII_STREAM_MEMORY_DIAGNOSTICS=1)
    endif()
    if(WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS)
        target_compile_definitions(${EXECUTABLE} PUBLIC WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS=1)
    endif()
    message(STATUS "  Memory profile:  ${WII_MEMORY_PROFILE_ID}")
    message(STATUS "  Island liveness: ${WII_ISLAND_READ_LIVENESS}")
    message(STATUS "  Audio decode:    ${WII_AUDIO_DECODE_ENABLE}")
    message(STATUS "  Adaptive archive ceiling: ${WII_STREAM_ADAPTIVE_ARCHIVE_CEILING_VALUE}")
    message(STATUS "  Archive global LRU: ${WII_STREAM_ARCHIVE_GLOBAL_LRU_VALUE}")
    message(STATUS "  PS2 transition purge: ${WII_STREAM_PS2_TRANSITION_PURGE_VALUE}")
    message(STATUS "  PS2 world-scan radius: ${WII_STREAM_PS2_WORLD_SCAN_RADIUS_VALUE}")
    message(STATUS "  GX headroom guard: ${WII_STREAM_GX_HEADROOM_GUARD_VALUE}")
    message(STATUS "  P7 recent TXD guard: ${WII_STREAM_P7_VISIBLE_TXD_GUARD_VALUE}")
    message(STATUS "  Splash visual gate: ${WII_STREAM_SPLASH_VISUAL_GATE_VALUE}")
    message(STATUS "  Atomic big handoff: ${WII_STREAM_ATOMIC_BIG_HANDOFF_VALUE}")
    message(STATUS "  Active HUD weapon raster pin: ${WII_HUD_ACTIVE_WEAPON_RASTER_PIN_VALUE}")
    message(STATUS "  Used HUD weapon raster persistence: ${WII_HUD_USED_WEAPON_RASTER_PERSIST_VALUE}")
    message(STATUS "  Persistent font upload: ${WII_FONT_PERSISTENT_UPLOAD_VALUE}")
    message(STATUS "  Opaque RGBA8 RGB565 quality tier: ${WII_GX_OPAQUE_RGBA8_RGB565_TIER_VALUE}")
    message(STATUS "  Generic MEM2:    ${WII_MEM2_RESOURCE_POOL_BYTES}")
    message(STATUS "  Audio MEM2:      ${WII_AUDIO_MEM2_POOL_BYTES}")
    message(STATUS "  GX Arena2 expansion: ${WII_GX_ARENA2_EXPANSION_BYTES}")
    message(STATUS "  Build ID:        ${WII_BUILD_ID}")
endif()

if(ENABLE_MISSION_REPLAY)
    target_compile_definitions(${EXECUTABLE} PUBLIC ENABLE_MISSION_REPLAY)
endif()

message(STATUS "[reVC] Nintendo GX compile definitions configured")
