#ifndef LIBRW_LEGACY_PLATFORMS_H
#define LIBRW_LEGACY_PLATFORMS_H

namespace rw {

namespace ps2 {
void registerPlatformPlugins(void);
void *destroyNativeData(void *object, int32, int32);
Stream *readNativeData(Stream *stream, int32 len, void *object, int32, int32);
Stream *writeNativeData(Stream *stream, int32 len, void *object, int32, int32);
int32 getSizeNativeData(void *object, int32, int32);
Texture *readNativeTexture(Stream *stream);
void writeNativeTexture(Texture *tex, Stream *stream);
uint32 getSizeNativeTexture(Texture *tex);
Stream *readNativeSkin(Stream *stream, int32, void *object, int32 offset);
Stream *writeNativeSkin(Stream *stream, int32 len, void *object, int32 offset);
int32 getSizeNativeSkin(void *object, int32 offset);
void initSkin(void);
void initMatFX(void);
}

namespace wdgl {
extern int32 nativeRasterOffset;
void registerPlatformPlugins(void);
void *destroyNativeData(void *object, int32, int32);
Stream *readNativeData(Stream *stream, int32 len, void *object, int32, int32);
Stream *writeNativeData(Stream *stream, int32 len, void *object, int32, int32);
int32 getSizeNativeData(void *object, int32, int32);
Stream *readNativeSkin(Stream *stream, int32, void *object, int32 offset);
Stream *writeNativeSkin(Stream *stream, int32 len, void *object, int32 offset);
int32 getSizeNativeSkin(void *object, int32 offset);
void initSkin(void);
void initMatFX(void);
}

namespace gl3 {
struct Gl3Caps {
	int gles;
	int glversion;
	bool dxtSupported;
	bool astcSupported;
	float maxAnisotropy;
};

extern int32 nativeRasterOffset;
extern Gl3Caps gl3Caps;
extern bool32 needToReadBackTextures;

void registerPlatformPlugins(void);
void *destroyNativeData(void *object, int32, int32);
Texture *readNativeTexture(Stream *stream);
void writeNativeTexture(Texture *tex, Stream *stream);
uint32 getSizeNativeTexture(Texture *tex);
void initSkin(void);
void initMatFX(void);
void allocateDXT(Raster *raster, int32 dxt, int32 numLevels, bool32 hasAlpha);
}

} // namespace rw

#endif
