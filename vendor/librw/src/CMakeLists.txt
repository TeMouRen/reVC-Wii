set(LIBRW_CORE_SOURCES
    anim.cpp
    base.cpp
    bmp.cpp
    camera.cpp
    charset.cpp
    clump.cpp
    engine.cpp
    error.cpp
    frame.cpp
    geometry.cpp
    geoplg.cpp
    hanim.cpp
    image.cpp
    light.cpp
    matfx.cpp
    pipeline.cpp
    plg.cpp
    png.cpp
    prim.cpp
    raster.cpp
    render.cpp
    rwanim.h
    rwengine.h
    rwerror.h
    legacy_platforms.h
    rwobjects.h
    rwpipeline.h
    rwplg.h
    rwplugins.h
    rwrender.h
    rwuserdata.h
    skin.cpp
    texture.cpp
    tga.cpp
    tristrip.cpp
    userdata.cpp
    uvanim.cpp
    world.cpp
)

set(LIBRW_PUBLIC_HEADERS
    "${PROJECT_SOURCE_DIR}/args.h"
    "${PROJECT_SOURCE_DIR}/rw.h"
    d3d/rwd3d8.h
    d3d/rwd3d.h
    d3d/rwd3dimpl.h
    d3d/rwxbox.h
    d3d/rwd3d9.h
    lodepng/lodepng.h
    lodepng/lodepng.cpp
)

set(LIBRW_D3D8_COMPAT_SOURCES
    d3d/d3d.cpp
    d3d/d3d8.cpp
    d3d/d3d8matfx.cpp
    d3d/d3d8render.cpp
    d3d/d3d8skin.cpp
)

set(LIBRW_GX_SOURCES
    gx/gxdevice.cpp
    gx/gxim2d_fixed.cpp
    gx/gxmatfx.cpp
    gx/gxmemory.cpp
    gx/gxpipe.cpp
    gx/gxraster.cpp
    gx/gxread.cpp
    gx/gxskin.cpp
    gx/gxmemory.h
    gx/rwgx.h
)

add_library(librw
    ${LIBRW_PUBLIC_HEADERS}
    ${LIBRW_CORE_SOURCES}
    ${LIBRW_D3D8_COMPAT_SOURCES}
    ${LIBRW_GX_SOURCES}
    platform_stubs.cpp
)
add_library(librw::librw ALIAS librw)

target_include_directories(librw
    INTERFACE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/gx
)

target_compile_definitions(librw
    PRIVATE
        LODEPNG_NO_COMPILE_CPP
        "$<IF:$<CONFIG:DEBUG>,DEBUG,NDEBUG>"
    PUBLIC
        RW_GX
        RW_BIG_ENDIAN
        LIBRW
        RWDEVICE=gx
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_libraries(librw
        PRIVATE
            m
    )
endif()

target_link_libraries(librw PUBLIC ogc)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    target_compile_options(librw
        PUBLIC
            -URW_PS2
            -URW_GL3
    )

    target_compile_options(librw
        PRIVATE
            "-Wall"
            $<$<COMPILE_LANGUAGE:CXX>:-include ${CMAKE_CURRENT_LIST_DIR}/gx/rwgx.h>
    )
    target_compile_options(librw
        PRIVATE
            "-Wextra"
            "-Wdouble-promotion"
            "-Wpedantic"
    )
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(librw
        PUBLIC
            /wd4996 /wd4244
    )
endif()

set_target_properties(librw
    PROPERTIES
        C_STANDARD 11
        C_EXTENSIONS OFF
        C_STANDARD_REQUIRED ON
        CXX_STANDARD 11
        CXX_EXTENSIONS OFF
        CXX_STANDARD_REQUIRED ON
        PREFIX ""
)
