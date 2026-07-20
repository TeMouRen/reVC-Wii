// gxmatfx.cpp -- GX MatFX fallback pipeline
//
// We don't implement full RenderWare MatFX on GameCube yet.
// The important part for now is to keep MatFX atomics off the
// PLATFORM_NULL dummy pipe so they still render through the
// normal GX object pipeline.

#ifdef GAMECUBE

#include <stdio.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "../rwanim.h"
#include "../rwplugins.h"
#include "rwgx.h"

// Define GX_PIPELINE_DIAGNOSTICS when targeted MatFX tracing is needed.
#ifndef GX_PIPELINE_DIAGNOSTICS
#define printf(...) ((void)sizeof((::printf)(__VA_ARGS__)))
#endif

namespace rw {

static void*
matfxOpen(void *o, int32, int32)
{
	matFXGlobals.pipelines[PLATFORM_GX] = makeMatFXPipeline();
	printf("[GX-MATFX] open: fallback pipeline=%p\n",
	       (void*)matFXGlobals.pipelines[PLATFORM_GX]);
	return o;
}

static void*
matfxClose(void *o, int32, int32)
{
	if(matFXGlobals.pipelines[PLATFORM_GX]){
		((ObjPipeline*)matFXGlobals.pipelines[PLATFORM_GX])->destroy();
		matFXGlobals.pipelines[PLATFORM_GX] = nil;
	}
	return o;
}

void
initMatFX(void)
{
	Driver::registerPlugin(PLATFORM_GX, 0, ID_MATFX,
	                       matfxOpen, matfxClose);
}

ObjPipeline*
makeMatFXPipeline(void)
{
	ObjPipeline *pipe = makeDefaultPipeline();
	pipe->pluginID = ID_MATFX;
	pipe->pluginData = 0;
	return pipe;
}

} // namespace rw

#endif // GAMECUBE
