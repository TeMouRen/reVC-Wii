#include "rwbase.h"
#include "rwerror.h"
#include "rwplg.h"
#include "rwpipeline.h"
#include "rwobjects.h"
#include "rwengine.h"
#include "rwanim.h"
#include "rwplugins.h"
#include "d3d/rwxbox.h"
#include "d3d/rwd3d9.h"
#include "legacy_platforms.h"

namespace rw {

static Stream*
skipUnsupportedStream(Stream *stream, int32 len)
{
	if(stream)
		stream->seek(len);
	return stream;
}

static Texture*
unsupportedTextureRead(Stream *)
{
	return nil;
}

namespace ps2 {

int32 nativeRasterOffset = 0;

void registerPlatformPlugins(void) {}
void *destroyNativeData(void *object, int32, int32) { return object; }
Stream *readNativeData(Stream *stream, int32 len, void *, int32, int32)
	{ return skipUnsupportedStream(stream, len); }
Stream *writeNativeData(Stream *stream, int32, void *, int32, int32) { return stream; }
int32 getSizeNativeData(void *, int32, int32) { return 0; }
Texture *readNativeTexture(Stream *stream) { return unsupportedTextureRead(stream); }
void writeNativeTexture(Texture *, Stream *) {}
uint32 getSizeNativeTexture(Texture *) { return 0; }
Stream *readNativeSkin(Stream *stream, int32 len, void *, int32)
	{ return skipUnsupportedStream(stream, len); }
Stream *writeNativeSkin(Stream *stream, int32, void *, int32) { return stream; }
int32 getSizeNativeSkin(void *, int32) { return 0; }
void initSkin(void) {}
void initMatFX(void) {}

} // namespace ps2

namespace xbox {

int32 nativeRasterOffset = 0;
uint32 vertexFormatSizes[6] = { 0, 0, 0, 0, 0, 0 };

void registerPlatformPlugins(void) {}
void registerVertexFormatPlugin(void) {}
void *destroyNativeData(void *object, int32, int32) { return object; }
Stream *readNativeData(Stream *stream, int32 len, void *, int32, int32)
	{ return skipUnsupportedStream(stream, len); }
Stream *writeNativeData(Stream *stream, int32, void *, int32, int32) { return stream; }
int32 getSizeNativeData(void *, int32, int32) { return 0; }
Texture *readNativeTexture(Stream *stream) { return unsupportedTextureRead(stream); }
void writeNativeTexture(Texture *, Stream *) {}
uint32 getSizeNativeTexture(Texture *) { return 0; }
Stream *readNativeSkin(Stream *stream, int32 len, void *, int32)
	{ return skipUnsupportedStream(stream, len); }
Stream *writeNativeSkin(Stream *stream, int32, void *, int32) { return stream; }
int32 getSizeNativeSkin(void *, int32) { return 0; }
void initSkin(void) {}
void initMatFX(void) {}

} // namespace xbox

namespace d3d9 {

void registerPlatformPlugins(void) {}
void *destroyNativeData(void *object, int32, int32) { return object; }
Stream *readNativeData(Stream *stream, int32 len, void *, int32, int32)
	{ return skipUnsupportedStream(stream, len); }
Stream *writeNativeData(Stream *stream, int32, void *, int32, int32) { return stream; }
int32 getSizeNativeData(void *, int32, int32) { return 0; }
Texture *readNativeTexture(Stream *stream) { return unsupportedTextureRead(stream); }
void writeNativeTexture(Texture *, Stream *) {}
uint32 getSizeNativeTexture(Texture *) { return 0; }
void initSkin(void) {}
void initMatFX(void) {}

} // namespace d3d9

namespace wdgl {

int32 nativeRasterOffset = 0;

void registerPlatformPlugins(void) {}
void *destroyNativeData(void *object, int32, int32) { return object; }
Stream *readNativeData(Stream *stream, int32 len, void *, int32, int32)
	{ return skipUnsupportedStream(stream, len); }
Stream *writeNativeData(Stream *stream, int32, void *, int32, int32) { return stream; }
int32 getSizeNativeData(void *, int32, int32) { return 0; }
Stream *readNativeSkin(Stream *stream, int32 len, void *, int32)
	{ return skipUnsupportedStream(stream, len); }
Stream *writeNativeSkin(Stream *stream, int32, void *, int32) { return stream; }
int32 getSizeNativeSkin(void *, int32) { return 0; }
void initSkin(void) {}
void initMatFX(void) {}

} // namespace wdgl

namespace gl3 {

int32 nativeRasterOffset = 0;
Gl3Caps gl3Caps = { 0, 0, false, false, 1.0f };
bool32 needToReadBackTextures = false;

void registerPlatformPlugins(void) {}
void *destroyNativeData(void *object, int32, int32) { return object; }
Texture *readNativeTexture(Stream *stream) { return unsupportedTextureRead(stream); }
void writeNativeTexture(Texture *, Stream *) {}
uint32 getSizeNativeTexture(Texture *) { return 0; }
void initSkin(void) {}
void initMatFX(void) {}
void allocateDXT(Raster *, int32, int32, bool32) {}

} // namespace gl3

} // namespace rw
