#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define WITH_D3D
#include "rwbase.h"
#include "rwerror.h"
#include "rwplg.h"
#include "rwpipeline.h"
#include "rwobjects.h"
#include "rwengine.h"
#include "d3d/rwd3d.h"
#include "d3d/rwxbox.h"
#include "d3d/rwd3d8.h"
#include "d3d/rwd3d9.h"
#include "d3d/rwd3dimpl.h"
#include "legacy_platforms.h"
#include "gx/rwgx.h"
#ifdef RW_GX
#include "gx/gxmemory.h"
#endif

#define PLUGIN_ID 0

namespace rw {

#ifdef RW_GX
// 前向声明 — 实现在 gxraster.cpp
void gxConvertRasterToNative(Texture *tex);
#endif

int32 Texture::numAllocated;
int32 TexDictionary::numAllocated;

PluginList TexDictionary::s_plglist(sizeof(TexDictionary));
PluginList Texture::s_plglist(sizeof(Texture));
PluginList Raster::s_plglist(sizeof(Raster));

struct TextureGlobals
{
	TexDictionary *initialTexDict;
	TexDictionary *currentTexDict;
	// load textures from files
	bool32 loadTextures;
	// create dummy textures to store just names
	bool32 makeDummies;
	bool32 mipmapping;
	bool32 autoMipmapping;
	LinkList texDicts;

	LinkList textures;
};
int32 textureModuleOffset;

#define TEXTUREGLOBAL(v) (PLUGINOFFSET(TextureGlobals, engine, textureModuleOffset)->v)

#ifdef RW_GX
static char
texLowerAscii(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool
texNameContainsNoCase(const char *name, const char *needle)
{
	if(name == nil || needle == nil || needle[0] == '\0')
		return false;

	for(const char *p = name; *p; p++){
		const char *a = p;
		const char *b = needle;
		while(*a && *b && texLowerAscii(*a) == texLowerAscii(*b)){
			a++;
			b++;
		}
		if(*b == '\0')
			return true;
	}
	return false;
}

static bool
shouldLogGxTextureResult(const char *name)
{
	if(name == nil || name[0] == '\0')
		return false;

	return strcmp(name, "kbtree4_test") == 0 ||
	       strcmp(name, "newtreeleaves128") == 0 ||
	       strcmp(name, "foliage256") == 0 ||
	       strcmp(name, "planta256") == 0 ||
	       strcmp(name, "plantb256") == 0 ||
	       strcmp(name, "plantc256") == 0 ||
	       strcmp(name, "fuzzyplant256") == 0 ||
	       strcmp(name, "kbtree3_test") == 0 ||
	       texNameContainsNoCase(name, "grass") ||
	       texNameContainsNoCase(name, "hedge") ||
	       texNameContainsNoCase(name, "foliage") ||
	       texNameContainsNoCase(name, "leaf") ||
	       texNameContainsNoCase(name, "tree") ||
	       texNameContainsNoCase(name, "plant");
}

static bool
shouldTraceTextureLookup(const char *name)
{
	return shouldLogGxTextureResult(name) ||
	       (name && (strcmp(name, "black") == 0 ||
	                 strcmp(name, "black64") == 0));
}
#endif

static void*
textureOpen(void *object, int32 offset, int32 size)
{
	TexDictionary *texdict;
	textureModuleOffset = offset;
	TEXTUREGLOBAL(texDicts).init();
	TEXTUREGLOBAL(textures).init();
	texdict = TexDictionary::create();
	TEXTUREGLOBAL(initialTexDict) = texdict;
	TexDictionary::setCurrent(texdict);
	TEXTUREGLOBAL(loadTextures) = 1;
	TEXTUREGLOBAL(makeDummies) = 0;
	TEXTUREGLOBAL(mipmapping) = 0;
	TEXTUREGLOBAL(autoMipmapping) = 0;
	return object;
}
static void*
textureClose(void *object, int32 offset, int32 size)
{
	FORLIST(lnk, TEXTUREGLOBAL(texDicts))
		TexDictionary::fromLink(lnk)->destroy();
	TEXTUREGLOBAL(initialTexDict) = nil;
	TEXTUREGLOBAL(currentTexDict) = nil;

	FORLIST(lnk, TEXTUREGLOBAL(textures)){
		Texture *tex = LLLinkGetData(lnk, Texture, inGlobalList);
		printf("Tex still allocated: %d %s %s\n", tex->refCount, tex->name, tex->mask);
		assert(tex->dict == nil);
		tex->destroy();
	}
	return object;
}

void
Texture::registerModule(void)
{
	Engine::registerPlugin(sizeof(TextureGlobals), ID_TEXTUREMODULE, textureOpen, textureClose);
}

void
Texture::setLoadTextures(bool32 b)
{
	TEXTUREGLOBAL(loadTextures) = b;
}

void
Texture::setCreateDummies(bool32 b)
{
	TEXTUREGLOBAL(makeDummies) = b;
}

void Texture::setMipmapping(bool32 b) { TEXTUREGLOBAL(mipmapping) = b; }
void Texture::setAutoMipmapping(bool32 b) { TEXTUREGLOBAL(autoMipmapping) = b; }
bool32 Texture::getMipmapping(void) { return TEXTUREGLOBAL(mipmapping); }
bool32 Texture::getAutoMipmapping(void) { return TEXTUREGLOBAL(autoMipmapping); }

//
// TexDictionary
//

TexDictionary*
TexDictionary::create(void)
{
	TexDictionary *dict = (TexDictionary*)rwMalloc(s_plglist.size, MEMDUR_EVENT | ID_TEXDICTIONARY);
	if(dict == nil){
		RWERROR((ERR_ALLOC, s_plglist.size));
		return nil;
	}
	numAllocated++;
	dict->object.init(TexDictionary::ID, 0);
	dict->textures.init();
	TEXTUREGLOBAL(texDicts).add(&dict->inGlobalList);
	s_plglist.construct(dict);
	return dict;
}

void
TexDictionary::destroy(void)
{
	if(TEXTUREGLOBAL(currentTexDict) == this)
		TEXTUREGLOBAL(currentTexDict) = nil;
	FORLIST(lnk, this->textures){
		Texture *tex = Texture::fromDict(lnk);
		this->remove(tex);
		tex->destroy();
	}
	s_plglist.destruct(this);
	this->inGlobalList.remove();
	rwFree(this);
	numAllocated--;
}

void
TexDictionary::add(Texture *t)
{
	if(t->dict)
		t->inDict.remove();
	t->dict = this;
	this->textures.append(&t->inDict);
}

void
TexDictionary::remove(Texture *t)
{
	assert(t->dict == this);
	t->inDict.remove();
	t->dict = nil;
}

void
TexDictionary::addFront(Texture *t)
{
	if(t->dict)
		t->inDict.remove();
	t->dict = this;
	this->textures.add(&t->inDict);
}

Texture*
TexDictionary::find(const char *name)
{
	FORLIST(lnk, this->textures){
		Texture *tex = Texture::fromDict(lnk);
		if(strncmp_ci(tex->name, name, 32) == 0)
			return tex;
	}
	return nil;
}

TexDictionary*
TexDictionary::streamRead(Stream *stream)
{
	if(!findChunk(stream, ID_STRUCT, nil, nil)){
		RWERROR((ERR_CHUNK, "STRUCT"));
		return nil;
	}
	int32 numTex = stream->readI16();
	stream->readI16(); // device id (0 = unknown, 1 = d3d8, 2 = d3d9,
	                   // 3 = gcn, 4 = null, 5 = opengl,
	                   // 6 = ps2, 7 = softras, 8 = xbox, 9 = psp)
	TexDictionary *txd = TexDictionary::create();
	if(txd == nil)
		return nil;
	Texture *tex;
	for(int32 i = 0; i < numTex; i++){
		if(!findChunk(stream, ID_TEXTURENATIVE, nil, nil)){
			RWERROR((ERR_CHUNK, "TEXTURENATIVE"));
			goto fail;
		}
		tex = Texture::streamReadNative(stream);
		if(tex == nil)
			goto fail;
#ifdef RW_GX
		if(shouldLogGxTextureResult(tex->name) &&
		   tex->raster != nil){
			printf("[GX-CONVERT-IN] tex=%s srcPlat=%d fmt=0x%X %dx%d depth=%d raster=%p\n",
			       tex->name,
			       tex->raster->platform,
			       tex->raster->format,
			       tex->raster->width,
			       tex->raster->height,
			       tex->raster->depth,
			       tex->raster);
		}
		gxConvertRasterToNative(tex);
		if(tex->raster && tex->raster->platform == PLATFORM_GX)
			gx::texPoolRename(tex->raster, tex->name);
		if(shouldLogGxTextureResult(tex->name) &&
		   tex->raster && tex->raster->platform == PLATFORM_GX){
			static int s_gxTexResultLogCount = 0;
			if(s_gxTexResultLogCount < 192){
				gx::GxRaster *natras =
					PLUGINOFFSET(gx::GxRaster, tex->raster, gx::nativeRasterOffset);
				printf("[GX-TEXRESULT] tex=%s plat=%d fmt=%u hasAlpha=%d texObj=%d %dx%d size=%u\n",
				       tex->name,
				       tex->raster->platform,
				       natras ? (unsigned)natras->gxFmt : 0u,
				       natras ? (natras->hasAlpha ? 1 : 0) : 0,
				       natras ? (natras->texObjValid ? 1 : 0) : 0,
				       tex->raster->width,
				       tex->raster->height,
				       natras ? (unsigned)natras->dataSize : 0u);
				s_gxTexResultLogCount++;
			}
		}
#endif
		Texture::s_plglist.streamRead(stream, tex);
		txd->add(tex);
	}
	if(s_plglist.streamRead(stream, txd))
		return txd;
fail:
	txd->destroy();
	return nil;
}

void
TexDictionary::streamWrite(Stream *stream)
{
	writeChunkHeader(stream, ID_TEXDICTIONARY, this->streamGetSize());
	writeChunkHeader(stream, ID_STRUCT, 4);
	int32 numTex = this->count();
	stream->writeI16(numTex);
	stream->writeI16(0);
	FORLIST(lnk, this->textures){
		Texture *tex = Texture::fromDict(lnk);
		uint32 sz = tex->streamGetSizeNative();
		sz += 12 + Texture::s_plglist.streamGetSize(tex);
		writeChunkHeader(stream, ID_TEXTURENATIVE, sz);
		tex->streamWriteNative(stream);
		Texture::s_plglist.streamWrite(stream, tex);
	}
	s_plglist.streamWrite(stream, this);
}

uint32
TexDictionary::streamGetSize(void)
{
	uint32 size = 12 + 4;
	FORLIST(lnk, this->textures){
		Texture *tex = Texture::fromDict(lnk);
		size += 12 + tex->streamGetSizeNative();
		size += 12 + Texture::s_plglist.streamGetSize(tex);
	}
	size += 12 + s_plglist.streamGetSize(this);
	return size;
}

void
TexDictionary::setCurrent(TexDictionary *txd)
{
	PLUGINOFFSET(TextureGlobals, engine, textureModuleOffset)->currentTexDict = txd;
}

TexDictionary*
TexDictionary::getCurrent(void)
{
	return PLUGINOFFSET(TextureGlobals, engine, textureModuleOffset)->currentTexDict;
}

//
// Texture
//

static Texture *defaultFindCB(const char *name);
static Texture *defaultReadCB(const char *name, const char *mask);

Texture *(*Texture::findCB)(const char *name) = defaultFindCB;
Texture *(*Texture::readCB)(const char *name, const char *mask) = defaultReadCB;

Texture*
Texture::create(Raster *raster)
{
	Texture *tex = (Texture*)rwMalloc(s_plglist.size, MEMDUR_EVENT | ID_TEXTURE);
	if(tex == nil){
		RWERROR((ERR_ALLOC, s_plglist.size));
		return nil;
	}
	numAllocated++;
	tex->dict = nil;
	tex->inDict.init();
	memset(tex->name, 0, 32);
	memset(tex->mask, 0, 32);
	tex->filterAddressing = (WRAP << 12) | (WRAP << 8) | NEAREST;
	tex->raster = raster;
	tex->refCount = 1;
	TEXTUREGLOBAL(textures).add(&tex->inGlobalList);
	s_plglist.construct(tex);
	return tex;
}

void
Texture::destroy(void)
{
	this->refCount--;
	if(this->refCount <= 0){
		s_plglist.destruct(this);
		if(this->dict)
			this->inDict.remove();
		if(this->raster)
			this->raster->destroy();
		this->inGlobalList.remove();
		rwFree(this);
		numAllocated--;
	}
}

static Texture*
defaultFindCB(const char *name)
{
	if(name == nil || name[0] == '\0')
		return nil;

	TexDictionary *current = TEXTUREGLOBAL(currentTexDict);
	Texture *tex = current ? current->find(name) : nil;
#ifdef RW_GX
	if(shouldTraceTextureLookup(name))
		printf("[TEX-FIND] current-%s name='%s' current=%p tex=%p raster=%p\n",
		       tex ? "hit" : "miss", name, (void*)current, (void*)tex,
		       tex ? (void*)tex->raster : nil);
#endif

	// Texture references belong to the dictionary selected while their model is
	// streamed. Searching every loaded dictionary lets an unrelated TXD satisfy
	// a missing reference by name, which makes the result depend on load order.
	// Keep lookup scoped to the current dictionary, matching the GX2 backend.
	return tex;
}

#ifdef RW_GX
static void
considerGxTextureAlias(Texture *tex, TexDictionary *dict, const char *mask,
                       Texture **match, TexDictionary **matchDict,
                       uint32 *numMatches)
{
	if(tex == nil || tex->raster == nil || mask == nil || mask[0] == '\0' ||
	   tex->mask[0] == '\0' || strncmp_ci(tex->mask, mask, 32) != 0)
		return;
	if(*match == tex)
		return;
	*match = tex;
	*matchDict = dict;
	(*numMatches)++;
}

static Texture*
findGxTextureAlias(const char *name, const char *mask)
{
	static int s_aliasLogCount = 0;
	static int s_aliasAmbiguousLogCount = 0;
	TexDictionary *current = TEXTUREGLOBAL(currentTexDict);
	if(current == nil || name == nil || name[0] == '\0' ||
	   mask == nil || mask[0] == '\0')
		return nil;

	// Some converted GX assets keep a platform-resolution suffix while the DFF
	// retains the source name. The source mask is part of the texture identity:
	// accepting a suffix alone can bind an unrelated uniform shadow texture.
	// Search all resident dictionaries only when the full name family + mask
	// identifies exactly one texture, so list order cannot decide the result.
	static const char *resolutionSuffixes[] = {
		"64", "_64", "128", "_128", "32", "_32", "256", "_256"
	};
	char candidate[32];
	size_t nameLen = strlen(name);
	Texture *match = nil;
	TexDictionary *matchDict = nil;
	uint32 numMatches = 0;
	FORLIST(lnk, TEXTUREGLOBAL(texDicts)){
		TexDictionary *dict = TexDictionary::fromLink(lnk);
		considerGxTextureAlias(dict->find(name), dict, mask,
		                       &match, &matchDict, &numMatches);
		for(uint32 i = 0; i < sizeof(resolutionSuffixes)/sizeof(resolutionSuffixes[0]); i++){
			size_t suffixLen = strlen(resolutionSuffixes[i]);
			if(nameLen + suffixLen >= sizeof(candidate))
				continue;
			memcpy(candidate, name, nameLen);
			memcpy(candidate + nameLen, resolutionSuffixes[i], suffixLen + 1);
			considerGxTextureAlias(dict->find(candidate), dict, mask,
			                       &match, &matchDict, &numMatches);
		}
	}
	if(numMatches == 1){
		if(s_aliasLogCount < 128){
			printf("[TEX-ALIAS] exact-mask name='%s' mask='%s' -> '%s' candidates=%u current=%p hit=%p raster=%p\n",
			       name, mask, match->name, (unsigned)numMatches,
			       (void*)current, (void*)matchDict, (void*)match->raster);
			s_aliasLogCount++;
		}
		return match;
	}
	if(numMatches > 1 && s_aliasAmbiguousLogCount < 64){
		printf("[TEX-ALIAS] ambiguous-exact-mask name='%s' mask='%s' candidates=%u current=%p\n",
		       name, mask, (unsigned)numMatches, (void*)current);
		s_aliasAmbiguousLogCount++;
	}
	return nil;
}
#endif


static Texture*
defaultReadCB(const char *name, const char *mask)
{
	Texture *tex;
	Image *img;
	Raster *raster;

#ifdef WII
	// Wii disc builds keep textures inside TXDs; for bare texture names,
	// probing .tga/.bmp/.png on the VFS only adds failed lookups and load hitches.
	if(name &&
	   strchr(name, '\\') == nil &&
	   strchr(name, '/') == nil &&
	   strchr(name, '.') == nil)
		return nil;
#endif

	printf("[TEX-LOAD] defaultReadCB fallback: name='%s' mask='%s' current=%p\n",
	       name ? name : "NULL", mask ? mask : "NULL",
	       TEXTUREGLOBAL(currentTexDict));
	img = Image::readMasked(name, mask);
	if(!img){
		printf("[TEX-LOAD] ERROR: Image::readMasked FAILED for '%s' current=%p\n",
		       name ? name : "NULL", TEXTUREGLOBAL(currentTexDict));
		return nil;
	}
	printf("[TEX-LOAD] OK: %dx%d depth=%d\n", img->width, img->height, img->depth);
	raster = Raster::createFromImage(img);
	if(raster == nil){
		img->destroy();
		return nil;
	}
	tex = Texture::create(raster);
	if(tex == nil){
		raster->destroy();
		img->destroy();
		return nil;
	}
	strncpy(tex->name, name, 32);
	tex->name[31] = '\0';
	if(mask){
		strncpy(tex->mask, mask, 32);
		tex->mask[31] = '\0';
	}
#ifdef RW_GX
	if(tex->raster && tex->raster->platform == PLATFORM_GX)
		gx::texPoolRename(tex->raster, tex->name);
#endif
	img->destroy();
	return tex;
}

Texture*
Texture::read(const char *name, const char *mask)
{
	Raster *raster = nil;
	Texture *tex;

	if(tex = Texture::findCB(name), tex){
		tex->addRef();
		return tex;
	}
#ifdef RW_GX
	if(tex = findGxTextureAlias(name, mask), tex){
		tex->addRef();
		return tex;
	}
#endif
	if(TEXTUREGLOBAL(loadTextures)){
		tex = Texture::readCB(name, mask);
		if(tex == nil)
			goto dummytex;
	}else dummytex: if(TEXTUREGLOBAL(makeDummies)){
//printf("missing texture %s %s\n", name ? name : "", mask ? mask : "");
		tex = Texture::create(nil);
		if(tex == nil)
			return nil;
		strncpy(tex->name, name, 32);
		if(mask)
			strncpy(tex->mask, mask, 32);
		raster = Raster::create(0, 0, 0, Raster::DONTALLOCATE);
		tex->raster = raster;
	}
	if(tex && TEXTUREGLOBAL(currentTexDict)){
		if(tex->dict)
			tex->inDict.remove();
		TEXTUREGLOBAL(currentTexDict)->add(tex);
	}
	return tex;
}

Texture*
Texture::streamRead(Stream *stream)
{
	uint32 length;
	char name[128], mask[128];
	if(!findChunk(stream, ID_STRUCT, nil, nil)){
		RWERROR((ERR_CHUNK, "STRUCT"));
		return nil;
	}
	uint32 filterAddressing = stream->readU32();
	// if V addressing is 0, copy U
	if((filterAddressing & 0xF000) == 0)
		filterAddressing |= (filterAddressing&0xF00) << 4;

	// if using mipmap filter mode, set automipmapping,
	// if 0x10000 is set, set mipmapping

	if(!findChunk(stream, ID_STRING, &length, nil)){
		RWERROR((ERR_CHUNK, "STRING"));
		return nil;
	}
	stream->read8(name, length);

	if(!findChunk(stream, ID_STRING, &length, nil)){
		RWERROR((ERR_CHUNK, "STRING"));
		return nil;
	}
	stream->read8(mask, length);

	bool32 mipState = getMipmapping();
	bool32 autoMipState = getAutoMipmapping();
	int32 filter = filterAddressing&0xFF;
	if(filter == MIPNEAREST || filter == MIPLINEAR ||
	   filter == LINEARMIPNEAREST || filter == LINEARMIPLINEAR){
		setMipmapping(1);
		setAutoMipmapping((filterAddressing&0x10000) == 0);
	}else{
		setMipmapping(0);
		setAutoMipmapping(0);
	}

	Texture *tex = Texture::read(name, mask);

	setMipmapping(mipState);
	setAutoMipmapping(autoMipState);

	if(tex == nil){
		s_plglist.streamSkip(stream);
		return nil;
	}
	if(tex->refCount == 1)
		tex->filterAddressing = filterAddressing&0xFFFF;

	if(s_plglist.streamRead(stream, tex))
		return tex;

	tex->destroy();
	return nil;
}

bool
Texture::streamWrite(Stream *stream)
{
	int size;
	char buf[36];
	writeChunkHeader(stream, ID_TEXTURE, this->streamGetSize());
	writeChunkHeader(stream, ID_STRUCT, 4);
	uint32 filterAddressing = this->filterAddressing;
	if(this->raster && (raster->format & Raster::AUTOMIPMAP) == 0)
		filterAddressing |= 0x10000;
	stream->writeU32(filterAddressing);

	memset(buf, 0, 36);
	strncpy(buf, this->name, 32);
	size = strlen(buf)+4 & ~3;
	writeChunkHeader(stream, ID_STRING, size);
	stream->write8(buf, size);

	memset(buf, 0, 36);
	strncpy(buf, this->mask, 32);
	size = strlen(buf)+4 & ~3;
	writeChunkHeader(stream, ID_STRING, size);
	stream->write8(buf, size);

	s_plglist.streamWrite(stream, this);
	return true;
}

uint32
Texture::streamGetSize(void)
{
	uint32 size = 0;
	size += 12 + 4;
	size += 12 + 12;
	size += strlen(this->name)+4 & ~3;
	size += strlen(this->mask)+4 & ~3;
	size += 12 + s_plglist.streamGetSize(this);
	return size;
}

Texture*
Texture::streamReadNative(Stream *stream)
{
	if(!findChunk(stream, ID_STRUCT, nil, nil)){
		RWERROR((ERR_CHUNK, "STRUCT"));
		return nil;
	}
	uint32 platform = stream->readU32();
	stream->seek(-16);
	if(platform == FOURCC_PS2)
		return ps2::readNativeTexture(stream);
#ifdef RW_GX
	if(platform == PLATFORM_GX)
		return gx::readNativeTexture(stream);
#endif
	if(platform == PLATFORM_D3D8)
		return d3d8::readNativeTexture(stream);
	if(platform == PLATFORM_D3D9)
		return d3d9::readNativeTexture(stream);
	if(platform == PLATFORM_XBOX)
		return xbox::readNativeTexture(stream);
	if(platform == PLATFORM_GL3)
		return gl3::readNativeTexture(stream);
	assert(0 && "unsupported platform");
	return nil;
}

void
Texture::streamWriteNative(Stream *stream)
{
	if(this->raster->platform == PLATFORM_PS2)
		ps2::writeNativeTexture(this, stream);
	else if(this->raster->platform == PLATFORM_D3D8)
		d3d8::writeNativeTexture(this, stream);
	else if(this->raster->platform == PLATFORM_D3D9)
		d3d9::writeNativeTexture(this, stream);
	else if(this->raster->platform == PLATFORM_XBOX)
		xbox::writeNativeTexture(this, stream);
	else if(this->raster->platform == PLATFORM_GL3)
		gl3::writeNativeTexture(this, stream);
	else
		assert(0 && "unsupported platform");
}

uint32
Texture::streamGetSizeNative(void)
{
	if(this->raster->platform == PLATFORM_PS2)
		return ps2::getSizeNativeTexture(this);
	if(this->raster->platform == PLATFORM_D3D8)
		return d3d8::getSizeNativeTexture(this);
	if(this->raster->platform == PLATFORM_D3D9)
		return d3d9::getSizeNativeTexture(this);
	if(this->raster->platform == PLATFORM_XBOX)
		return xbox::getSizeNativeTexture(this);
	if(this->raster->platform == PLATFORM_GL3)
		return gl3::getSizeNativeTexture(this);
	assert(0 && "unsupported platform");
	return 0;
}



int32 anisotOffset;

static void*
createAnisot(void *object, int32 offset, int32)
{
	*GETANISOTROPYEXT(object) = 1;
	return object;
}

static void*
copyAnisot(void *dst, void *src, int32 offset, int32)
{
	*GETANISOTROPYEXT(dst) = *GETANISOTROPYEXT(src);
	return dst;
}

static Stream*
readAnisot(Stream *stream, int32, void *object, int32 offset, int32)
{
	*GETANISOTROPYEXT(object) = stream->readI32();
	return stream;
}

static Stream*
writeAnisot(Stream *stream, int32, void *object, int32 offset, int32)
{
	stream->writeI32(*GETANISOTROPYEXT(object));
	return stream;
}

static int32
getSizeAnisot(void *object, int32 offset, int32)
{
	if(*GETANISOTROPYEXT(object) == 1)
		return 0;
	return sizeof(int32);
}

void
registerAnisotropyPlugin(void)
{
	anisotOffset = Texture::registerPlugin(sizeof(int32), ID_ANISOT, createAnisot, nil, copyAnisot);
	Texture::registerPluginStream(ID_ANISOT, readAnisot, writeAnisot, getSizeAnisot);
}

void
Texture::setMaxAnisotropy(int32 maxaniso)
{
	if(anisotOffset > 0)
		*GETANISOTROPYEXT(this) = maxaniso;
}

int32
Texture::getMaxAnisotropy(void)
{
	if(anisotOffset > 0)
		return *GETANISOTROPYEXT(this);
	return 1;
}

int32
getMaxSupportedMaxAnisotropy(void)
{
#ifdef RW_D3D9
	return d3d::d3d9Globals.caps.MaxAnisotropy;
#endif
#ifdef RW_GL3
	return (int32)gl3::gl3Caps.maxAnisotropy;
#endif
	return 1;
}

}
