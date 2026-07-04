# Collect C++ sources and headers. C sources are added explicitly below so the
# Nintendo disc and DI glue stays intentional.
file(GLOB_RECURSE ${PROJECT}_SOURCES "*.cpp" "*.h" "*.rc")

file(GLOB_RECURSE PLATFORM_C_SOURCES "skel/gamecube/*.c")
if(WII)
    file(GLOB_RECURSE WII_C_SOURCES "skel/wii/*.c")
    list(APPEND PLATFORM_C_SOURCES ${WII_C_SOURCES})
endif()
list(APPEND ${PROJECT}_SOURCES ${PLATFORM_C_SOURCES})

# Keep shared headers, but remove desktop/mobile platform code from Nintendo
# targets before target creation.
set(NINTENDO_UNUSED_PATTERNS
    "audio/eax/*"
    "audio/oal/*"
    "skel/android/*"
    "skel/glfw/*"
    "skel/sdl2/*"
    "skel/win/*"
)
foreach(pattern IN LISTS NINTENDO_UNUSED_PATTERNS)
    file(GLOB_RECURSE matched "${pattern}")
    if(matched)
        list(REMOVE_ITEM ${PROJECT}_SOURCES ${matched})
    endif()
endforeach()

list(REMOVE_ITEM ${PROJECT}_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/audio/sampman_miles.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/audio/sampman_oal.cpp"
)

function(header_directories RETURN_LIST)
    file(GLOB_RECURSE ALL_SRCS "*.h" "*.cpp" "*.c")
    set(RELDIRS)
    foreach(SRC ${ALL_SRCS})
        file(RELATIVE_PATH RELSRC "${CMAKE_CURRENT_SOURCE_DIR}" "${SRC}")
        get_filename_component(RELDIR "${RELSRC}" DIRECTORY)
        list(APPEND RELDIRS ${RELDIR})
    endforeach()
    list(REMOVE_DUPLICATES RELDIRS)
    set(${RETURN_LIST} ${RELDIRS} PARENT_SCOPE)
endfunction()
header_directories(${PROJECT}_INCLUDES)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/extras/GitSHA1.cpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/extras/GitSHA1.cpp"
    @ONLY
)
list(APPEND ${PROJECT}_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/extras/GitSHA1.cpp")

add_executable(${EXECUTABLE} ${${PROJECT}_SOURCES})

target_link_libraries(${EXECUTABLE} PRIVATE librw::librw ogc)

target_link_options(${EXECUTABLE} PRIVATE
    -Wl,--wrap=fopen
)
if(WII)
    target_link_options(${EXECUTABLE} PRIVATE
        -Wl,--wrap=DVD_Mount
    )
endif()

target_include_directories(${EXECUTABLE}
    PRIVATE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${${PROJECT}_INCLUDES}>
)

target_compile_definitions(${EXECUTABLE}
    PRIVATE
        $<IF:$<CONFIG:DEBUG>,DEBUG,NDEBUG>
        LIBRW
        CMAKE_NO_AUTOLINK
        CMAKE_BUILD
        USE_OUR_VERSIONING
)

target_compile_options(${EXECUTABLE} PRIVATE
    -UEXTENDED_PIPELINES
    -UDEBUGMENU
    $<$<COMPILE_LANGUAGE:CXX>:-include ${CMAKE_SOURCE_DIR}/vendor/librw/src/gx/rwgx.h>
)

option(${PROJECT}_DISABLE_WARNINGS "Disable build warnings" ON)
if(${PROJECT}_DISABLE_WARNINGS)
    target_compile_options(${EXECUTABLE} PRIVATE -w)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"      OR
   CMAKE_CXX_COMPILER_ID STREQUAL "Clang"    OR
   CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    target_compile_options(${EXECUTABLE} PRIVATE "-Wall")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(${EXECUTABLE} PUBLIC /Zc:sizedDealloc-)
endif()

set_target_properties(${EXECUTABLE}
    PROPERTIES
        C_STANDARD            11
        C_EXTENSIONS          OFF
        C_STANDARD_REQUIRED   ON
        CXX_STANDARD          11
        CXX_EXTENSIONS        OFF
        CXX_STANDARD_REQUIRED ON
)

reVC_platform_target(${EXECUTABLE} INSTALL)
