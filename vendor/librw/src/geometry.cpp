#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include "rwbase.h"
#include "rwerror.h"
#include "rwplg.h"
#include "rwpipeline.h"
#include "rwobjects.h"
#include "rwengine.h"

#define PLUGIN_ID ID_GEOMETRY

namespace rw {

int32 Geometry::numAllocated;
int32 Material::numAllocated;

PluginList Geometry::s_plglist(sizeof(Geometry));
PluginList Material::s_plglist(sizeof(Material));

static SurfaceProperties defaultSurfaceProps = { 1.0f, 1.0f, 1.0f };

#ifdef RW_GX
// Define GX_MISSING_TEXTURE_DIAGNOSTICS for targeted missing-resource probes.
enum { GX_MISSING_MATERIAL_DIAG_CAPACITY = 256 };
enum { GX_MISSING_MATERIAL_DIAG_PROBE_COUNT = 8 };

struct GxTextureStreamContext
{
	int32 modelId;
	int32 txdSlot;
	char modelName[32];
	char txdName[20];
};

struct GxMissingMaterialEntry
{
	const Material *material;
#ifdef GX_MISSING_TEXTURE_DIAGNOSTICS
	uint32 serial;
	int32 modelId;
	int32 txdSlot;
	char modelName[32];
	char txdName[20];
	char textureName[32];
	char maskName[32];
	uint32 aliasCandidates;
	char reason[24];
#endif
};

static GxTextureStreamContext gTextureStreamContext = { -1, -1, { 0 }, { 0 } };
static GxMissingMaterialEntry gMissingMaterialDiags[GX_MISSING_MATERIAL_DIAG_CAPACITY];
#ifdef GX_MISSING_TEXTURE_DIAGNOSTICS
static uint32 gMissingMaterialDiagSerial = 0;
#endif

static uint32
gxMissingMaterialDiagIndex(const Material *mat)
{
	return ((uintptr_t)mat >> 4) & (GX_MISSING_MATERIAL_DIAG_CAPACITY - 1);
}

static GxMissingMaterialEntry*
gxFindMissingMaterialDiagEntry(const Material *mat, bool forInsert)
{
	const uint32 base = gxMissingMaterialDiagIndex(mat);
	GxMissingMaterialEntry *empty = nil;
	for(uint32 probe = 0; probe < GX_MISSING_MATERIAL_DIAG_PROBE_COUNT; probe++){
		GxMissingMaterialEntry *entry =
			&gMissingMaterialDiags[(base + probe) & (GX_MISSING_MATERIAL_DIAG_CAPACITY - 1)];
		if(entry->material == mat)
			return entry;
		if(forInsert && empty == nil && entry->material == nil)
			empty = entry;
	}
	return forInsert ? (empty ? empty : &gMissingMaterialDiags[base]) : nil;
}

static void
gxCopyDiagString(char *dst, size_t size, const char *src)
{
	if(size == 0)
		return;
	strncpy(dst, src ? src : "", size - 1);
	dst[size - 1] = '\0';
}

void
gxSetTextureStreamContext(int32 modelId, const char *modelName,
	int32 txdSlot, const char *txdName)
{
	gTextureStreamContext.modelId = modelId;
	gTextureStreamContext.txdSlot = txdSlot;
	gxCopyDiagString(gTextureStreamContext.modelName,
	                 sizeof(gTextureStreamContext.modelName), modelName);
	gxCopyDiagString(gTextureStreamContext.txdName,
	                 sizeof(gTextureStreamContext.txdName), txdName);
}

void
gxClearTextureStreamContext(void)
{
	gTextureStreamContext.modelId = -1;
	gTextureStreamContext.txdSlot = -1;
	gTextureStreamContext.modelName[0] = '\0';
	gTextureStreamContext.txdName[0] = '\0';
}

static void
gxForgetMissingMaterialDiag(const Material *mat)
{
	GxMissingMaterialEntry *entry = gxFindMissingMaterialDiagEntry(mat, false);
	if(entry)
		entry->material = nil;
}

static void
gxStoreMissingMaterialDiag(const Material *mat, const GxMissingMaterialDiag *source)
{
	GxMissingMaterialEntry *entry = gxFindMissingMaterialDiagEntry(mat, true);
	entry->material = mat;
#ifdef GX_MISSING_TEXTURE_DIAGNOSTICS
	if(source == nil)
		return;
	entry->serial = source->serial;
	entry->modelId = source->modelId;
	entry->txdSlot = source->txdSlot;
	gxCopyDiagString(entry->modelName, sizeof(entry->modelName), source->modelName);
	gxCopyDiagString(entry->txdName, sizeof(entry->txdName), source->txdName);
	gxCopyDiagString(entry->textureName, sizeof(entry->textureName), source->textureName);
	gxCopyDiagString(entry->maskName, sizeof(entry->maskName), source->maskName);
	entry->aliasCandidates = source->aliasCandidates;
	gxCopyDiagString(entry->reason, sizeof(entry->reason), source->reason);
#else
	(void)source;
#endif
}

static void
gxRememberMissingMaterialDiag(const Material *mat)
{
#ifdef GX_MISSING_TEXTURE_DIAGNOSTICS
	const GxTextureResolveDiag *resolve = gxGetLastTextureResolveDiag();
	GxMissingMaterialDiag source;
	source.serial = ++gMissingMaterialDiagSerial;
	source.modelId = gTextureStreamContext.modelId;
	source.txdSlot = gTextureStreamContext.txdSlot;
	source.modelName = gTextureStreamContext.modelName;
	source.txdName = gTextureStreamContext.txdName;
	source.textureName = resolve ? resolve->name :
		(mat->texture ? mat->texture->name : "");
	source.maskName = resolve ? resolve->mask :
		(mat->texture ? mat->texture->mask : "");
	source.aliasCandidates = resolve ? resolve->aliasCandidates : 0;
	source.reason = resolve ? resolve->reason :
		(mat->texture ? "dummy-texture" : "stream-read-failed");
	gxStoreMissingMaterialDiag(mat, &source);
	const GxMissingMaterialEntry *entry = gxFindMissingMaterialDiagEntry(mat, false);
	if(entry == nil)
		return;

	fprintf(stdout,
	        "[MAT-TEX-MISS] serial=%u mat=%p modelId=%d model=%s txdSlot=%d txd=%s tex='%s' mask='%s' aliases=%u reason=%s\n",
	        (unsigned)entry->serial, (void*)mat, entry->modelId,
	        entry->modelName[0] ? entry->modelName : "<unknown>",
	        entry->txdSlot, entry->txdName[0] ? entry->txdName : "<unknown>",
	        entry->textureName, entry->maskName,
	        (unsigned)entry->aliasCandidates, entry->reason);
#else
	// The normal Wii build only needs the unresolved-material presence set used
	// by both GX pipelines. Omit the 39 KiB string payload unless the targeted
	// diagnostic build is explicitly enabled.
	gxStoreMissingMaterialDiag(mat, nil);
#endif
}

bool
gxGetMissingMaterialDiag(const Material *mat, GxMissingMaterialDiag *diag)
{
	const GxMissingMaterialEntry *entry = gxFindMissingMaterialDiagEntry(mat, false);
	if(entry == nil)
		return false;
	if(diag){
#ifdef GX_MISSING_TEXTURE_DIAGNOSTICS
		diag->serial = entry->serial;
		diag->modelId = entry->modelId;
		diag->txdSlot = entry->txdSlot;
		diag->modelName = entry->modelName;
		diag->txdName = entry->txdName;
		diag->textureName = entry->textureName;
		diag->maskName = entry->maskName;
		diag->aliasCandidates = entry->aliasCandidates;
		diag->reason = entry->reason;
#else
		memset(diag, 0, sizeof(*diag));
		diag->modelId = -1;
		diag->txdSlot = -1;
#endif
	}
	return true;
}

bool
gxShouldSkipUnresolvedTexturedMesh(const Material *mat,
	const char *pipeline, uint32 meshIndex, uint32 numIndices)
{
	GxMissingMaterialDiag diag;
	if(!gxGetMissingMaterialDiag(mat, &diag))
		return false;

	// A material without a texture can be intentional. This table only holds
	// materials whose stream declared a texture that failed to resolve.
#ifdef GX_MISSING_TEXTURE_DIAGNOSTICS
	static uint32 loggedSerials[128];
	static uint32 numLoggedSerials = 0;
	bool alreadyLogged = false;
	for(uint32 i = 0; i < numLoggedSerials; i++){
		if(loggedSerials[i] == diag.serial){
			alreadyLogged = true;
			break;
		}
	}
	if(!alreadyLogged &&
	   numLoggedSerials < sizeof(loggedSerials) / sizeof(loggedSerials[0])){
		loggedSerials[numLoggedSerials++] = diag.serial;
		fprintf(stdout,
		        "[GX-MAT-TEX-SKIP] serial=%u pipe=%s modelId=%d model=%s txdSlot=%d txd=%s tex='%s' mask='%s' aliases=%u reason=%s mesh=%u idx=%u\n",
		        (unsigned)diag.serial, pipeline ? pipeline : "unknown",
		        diag.modelId,
		        diag.modelName && diag.modelName[0] ? diag.modelName : "<unknown>",
		        diag.txdSlot,
		        diag.txdName && diag.txdName[0] ? diag.txdName : "<unknown>",
		        diag.textureName ? diag.textureName : "",
		        diag.maskName ? diag.maskName : "",
		        (unsigned)diag.aliasCandidates,
		        diag.reason ? diag.reason : "unknown",
		        (unsigned)meshIndex, (unsigned)numIndices);
	}
#else
	(void)pipeline;
	(void)meshIndex;
	(void)numIndices;
#endif
	return true;
}
#endif

// We allocate twice because we have to allocate the data separately for uninstancing
Geometry*
Geometry::create(int32 numVerts, int32 numTris, uint32 flags)
{
	Geometry *geo = (Geometry*)rwMalloc(s_plglist.size, MEMDUR_EVENT | ID_GEOMETRY);
	if(geo == nil){
		RWERROR((ERR_ALLOC, s_plglist.size));
		return nil;
	}
	numAllocated++;
	geo->object.init(Geometry::ID, 0);
	geo->flags = flags & 0xFF00FFFF;
	geo->numTexCoordSets = (flags & 0xFF0000) >> 16;
	if(geo->numTexCoordSets == 0)
		geo->numTexCoordSets = (geo->flags & TEXTURED)  ? 1 :
		                       (geo->flags & TEXTURED2) ? 2 : 0;
	geo->numTriangles = numTris;
	geo->numVertices = numVerts;

	geo->colors = nil;
	for(int32 i = 0; i < 8; i++)
		geo->texCoords[i] = nil;
	geo->triangles = nil;
	// Allocate all attributes at once. The triangle pointer
	// will hold the first address (even when there are no triangles)
	// so we can free easily.
	if(!(geo->flags & NATIVE)){
		int32 sz = geo->numTriangles*sizeof(Triangle);
		if(geo->flags & PRELIT)
			sz += geo->numVertices*sizeof(RGBA);
		sz += geo->numTexCoordSets*geo->numVertices*sizeof(TexCoords);

		uint8 *data = (uint8*)rwNew(sz, MEMDUR_EVENT | ID_GEOMETRY);
		geo->triangles = (Triangle*)data;
		data += geo->numTriangles*sizeof(Triangle);
		if(geo->flags & PRELIT && geo->numVertices){
			geo->colors = (RGBA*)data;
			data += geo->numVertices*sizeof(RGBA);
		}
		if(geo->numVertices)
			for(int32 i = 0; i < geo->numTexCoordSets; i++){
				geo->texCoords[i] = (TexCoords*)data;
				data += geo->numVertices*sizeof(TexCoords);
			}

		// init triangles
		for(int32 i = 0; i < geo->numTriangles; i++)
			geo->triangles[i].matId = 0xFFFF;
	}
	geo->numMorphTargets = 0;
	geo->morphTargets = nil;
	geo->addMorphTargets(1);

	geo->matList.init();
	geo->lockedSinceInst = 0;
	geo->meshHeader = nil;
	geo->instData = nil;
	geo->refCount = 1;

	s_plglist.construct(geo);
	return geo;
}

void
Geometry::destroy(void)
{
	this->refCount--;
	if(this->refCount <= 0){
		s_plglist.destruct(this);
		// Also frees colors and tex coords
		rwFree(this->triangles);
		// Also frees their data
		rwFree(this->morphTargets);
		// Also frees indices
		rwFree(this->meshHeader);
		this->matList.deinit();
		rwFree(this);
		numAllocated--;
	}
}

void
Geometry::lock(int32 lockFlags)
{
	lockedSinceInst |= lockFlags;
	if(lockFlags & LOCKPOLYGONS){
		rwFree(this->meshHeader);
		this->meshHeader = nil;
	}
}

void
Geometry::unlock(void)
{
	if(this->meshHeader == nil)
		this->buildMeshes();
		
}

struct GeoStreamData
{
	uint32 flags;
	int32 numTriangles;
	int32 numVertices;
	int32 numMorphTargets;
};

Geometry*
Geometry::streamRead(Stream *stream)
{
	uint32 version;
	GeoStreamData buf;
	SurfaceProperties surfProps;
	MaterialList *ret;
	static SurfaceProperties reset = { 1.0f, 1.0f, 1.0f };

	if(!findChunk(stream, ID_STRUCT, nil, &version)){
		RWERROR((ERR_CHUNK, "STRUCT"));
		return nil;
	}
	stream->read32(&buf, sizeof(buf));
	Geometry *geo = Geometry::create(buf.numVertices,
	                                 buf.numTriangles, buf.flags);
	if(geo == nil)
		return nil;
	geo->addMorphTargets(buf.numMorphTargets-1);
	if(version < 0x34000)
		stream->read32(&surfProps, 12);

	if(!(geo->flags & NATIVE)){
		if(geo->flags & PRELIT)
			stream->read8(geo->colors, 4*geo->numVertices);
		for(int32 i = 0; i < geo->numTexCoordSets; i++)
			stream->read32(geo->texCoords[i],
				    2*geo->numVertices*4);
		for(int32 i = 0; i < geo->numTriangles; i++){
			uint32 tribuf[2];
			stream->read32(tribuf, 8);
			geo->triangles[i].v[0]  = tribuf[0] >> 16;
			geo->triangles[i].v[1]  = tribuf[0];
			geo->triangles[i].v[2]  = tribuf[1] >> 16;
			geo->triangles[i].matId = tribuf[1];
		}
	}

	for(int32 i = 0; i < geo->numMorphTargets; i++){
		MorphTarget *m = &geo->morphTargets[i];
		stream->read32(&m->boundingSphere, 4*4);
		int32 hasVertices = stream->readI32();
		int32 hasNormals = stream->readI32();
		if(hasVertices)
			stream->read32(m->vertices, 3*geo->numVertices*4);
		if(hasNormals)
			stream->read32(m->normals, 3*geo->numVertices*4);
	}

	if(!findChunk(stream, ID_MATLIST, nil, nil)){
		RWERROR((ERR_CHUNK, "MATLIST"));
		goto fail;
	}
	if(version < 0x34000)
		defaultSurfaceProps = surfProps;

	ret = MaterialList::streamRead(stream, &geo->matList);
	if(version < 0x34000)
		defaultSurfaceProps = reset;
	if(ret == nil)
		goto fail;
	if(s_plglist.streamRead(stream, geo))
		return geo;

fail:
	geo->destroy();
	return nil;
}

static uint32
geoStructSize(Geometry *geo)
{
	uint32 size = 0;
	size += sizeof(GeoStreamData);
	if(version < 0x34000)
		size += 12;	// surface properties
	if(!(geo->flags & Geometry::NATIVE)){
		if(geo->flags&geo->PRELIT)
			size += 4*geo->numVertices;
		for(int32 i = 0; i < geo->numTexCoordSets; i++)
			size += 2*geo->numVertices*4;
		size += 4*geo->numTriangles*2;
	}
	for(int32 i = 0; i < geo->numMorphTargets; i++){
		MorphTarget *m = &geo->morphTargets[i];
		size += 4*4 + 2*4; // bounding sphere and bools
		if(!(geo->flags & Geometry::NATIVE)){
			if(m->vertices)
				size += 3*geo->numVertices*4;
			if(m->normals)
				size += 3*geo->numVertices*4;
		}
	}
	return size;
}

bool
Geometry::streamWrite(Stream *stream)
{
	GeoStreamData buf;
	static float32 fbuf[3] = { 1.0f, 1.0f, 1.0f };

	writeChunkHeader(stream, ID_GEOMETRY, this->streamGetSize());
	writeChunkHeader(stream, ID_STRUCT, geoStructSize(this));

	buf.flags = this->flags | this->numTexCoordSets << 16;
	buf.numTriangles = this->numTriangles;
	buf.numVertices = this->numVertices;
	buf.numMorphTargets = this->numMorphTargets;
	stream->write32(&buf, sizeof(buf));
	if(version < 0x34000)
		stream->write32(fbuf, sizeof(fbuf));

	if(!(this->flags & NATIVE)){
		if(this->flags & PRELIT)
			stream->write8(this->colors, 4*this->numVertices);
		for(int32 i = 0; i < this->numTexCoordSets; i++)
			stream->write32(this->texCoords[i],
				    2*this->numVertices*4);
		for(int32 i = 0; i < this->numTriangles; i++){
			uint32 tribuf[2];
			tribuf[0] = this->triangles[i].v[0] << 16 |
			            this->triangles[i].v[1];
			tribuf[1] = this->triangles[i].v[2] << 16 |
			            this->triangles[i].matId;
			stream->write32(tribuf, 8);
		}
	}

	for(int32 i = 0; i < this->numMorphTargets; i++){
		MorphTarget *m = &this->morphTargets[i];
		stream->write32(&m->boundingSphere, 4*4);
		if(!(this->flags & NATIVE)){
			stream->writeI32(m->vertices != nil);
			stream->writeI32(m->normals != nil);
			if(m->vertices)
				stream->write32(m->vertices,
				             3*this->numVertices*4);
			if(m->normals)
				stream->write32(m->normals,
				             3*this->numVertices*4);
		}else{
			stream->writeI32(0);
			stream->writeI32(0);
		}
	}

	this->matList.streamWrite(stream);

	s_plglist.streamWrite(stream, this);
	return true;
}

uint32
Geometry::streamGetSize(void)
{
	uint32 size = 0;
	size += 12 + geoStructSize(this);
	size += 12 + this->matList.streamGetSize();
	size += 12 + s_plglist.streamGetSize(this);
	return size;
}

void
Geometry::addMorphTargets(int32 n)
{
	if(n == 0)
		return;
	int32 oldNumMorphTargets = this->numMorphTargets;
	n += oldNumMorphTargets;

	int32 sz;
	sz = sizeof(MorphTarget);
	if(!(this->flags & NATIVE)){
		sz += this->numVertices*sizeof(V3d);
		if(this->flags & NORMALS)
			sz += this->numVertices*sizeof(V3d);
	}

	// Memory layout: MorphTarget[n]; (vertices and normals)[n]
	MorphTarget *mts;
	if(this->numMorphTargets){
		mts = (MorphTarget*)rwResize(this->morphTargets, n*sz, MEMDUR_EVENT | ID_GEOMETRY);
		if(mts == nil){
			RWERROR((ERR_ALLOC, n*sz));
			return;
		}
		this->morphTargets = mts;
		// Since we now have more morph targets than before, move the vertex data up
		uint8 *src = (uint8*)mts + sz*this->numMorphTargets;
		uint8 *dst = (uint8*)mts + sz*n;
		uint32 len = (sz-sizeof(MorphTarget))*this->numMorphTargets;
		while(len--)
			*--dst = *--src;
	}else{
		mts = (MorphTarget*)rwNew(n*sz, MEMDUR_EVENT | ID_GEOMETRY);
		if(mts == nil){
			RWERROR((ERR_ALLOC, n*sz));
			return;
		}
		this->morphTargets = mts;
	}

	// Set up everything and initialize the bounding sphere for new morph targets
	V3d *data  = (V3d*)&mts[n];
	for(int32 i = 0; i < n; i++){
		mts->parent = this;
		mts->vertices = nil;
		mts->normals = nil;
		if(i >= oldNumMorphTargets){
			mts->boundingSphere.center.x = 0.0f;
			mts->boundingSphere.center.y = 0.0f;
			mts->boundingSphere.center.z = 0.0f;
			mts->boundingSphere.radius = 0.0f;
		}
		if(!(this->flags & NATIVE) && this->numVertices){
			mts->vertices = data;
			data += this->numVertices;
			if(this->flags & NORMALS){
				mts->normals = data;
				data += this->numVertices;
			}
		}
		mts++;
	}
	this->numMorphTargets = n;
}

void
Geometry::calculateBoundingSphere(void)
{
	for(int32 i = 0; i < this->numMorphTargets; i++){
		MorphTarget *m = &this->morphTargets[i];
		m->boundingSphere = m->calculateBoundingSphere();
	}
}

bool32
Geometry::hasColoredMaterial(void)
{
	for(int32 i = 0; i < this->matList.numMaterials; i++)
		if(this->matList.materials[i]->color.red != 255 ||
		   this->matList.materials[i]->color.green != 255 ||
		   this->matList.materials[i]->color.blue != 255 ||
		   this->matList.materials[i]->color.alpha != 255)
			return 1;
	return 0;
}

// Force allocate data, even when native flag is set
void
Geometry::allocateData(void)
{
	// Geometry data
	// Pretty much copy pasted from ::create above
	int32 sz = this->numTriangles*sizeof(Triangle);
	if(this->flags & PRELIT)
		sz += this->numVertices*sizeof(RGBA);
	sz += this->numTexCoordSets*this->numVertices*sizeof(TexCoords);

	uint8 *data = (uint8*)rwNew(sz, MEMDUR_EVENT | ID_GEOMETRY);
	if(data == nil){
		RWERROR((ERR_ALLOC, sz));
		return;
	}
	this->triangles = (Triangle*)data;
	data += this->numTriangles*sizeof(Triangle);
	for(int32 i = 0; i < this->numTriangles; i++)
		this->triangles[i].matId = 0xFFFF;
	if(this->flags & PRELIT){
		this->colors = (RGBA*)data;
		data += this->numVertices*sizeof(RGBA);
	}
	for(int32 i = 0; i < this->numTexCoordSets; i++){
		this->texCoords[i] = (TexCoords*)data;
		data += this->numVertices*sizeof(TexCoords);
	}

	// MorphTarget data
	// Bounding sphere is copied by realloc.
	sz = sizeof(MorphTarget) + this->numVertices*sizeof(V3d);
	if(this->flags & NORMALS)
		sz += this->numVertices*sizeof(V3d);

	MorphTarget *mt = (MorphTarget*)rwResize(this->morphTargets,
		sz*this->numMorphTargets, MEMDUR_EVENT | ID_GEOMETRY);
	if(mt == nil){
		RWERROR((ERR_ALLOC, sz*this->numMorphTargets));
		rwFree(this->triangles);
		this->triangles = nil;
		this->colors = nil;
		for(int32 i = 0; i < 8; i++)
			this->texCoords[i] = nil;
		return;
	}
	this->morphTargets = mt;
	V3d *vdata = (V3d*)&mt[this->numMorphTargets];
	for(int32 i = 0; i < this->numMorphTargets; i++){
		mt->parent = this;
		mt->vertices = nil;
		mt->normals = nil;
		if(this->numVertices){
			mt->vertices = vdata;
			vdata += this->numVertices;
			if(this->flags & NORMALS){
				mt->normals = vdata;
				vdata += this->numVertices;
			}
		}
		mt++;
	}
}

static int
isDegenerate(uint16 *idx)
{
	return idx[0] == idx[1] ||
	       idx[0] == idx[2] ||
	       idx[1] == idx[2];
}

// This functions assumes there is enough space allocated
// for triangles. Use MeshHeader::guessNumTriangles() and
// Geometry::allocateData()
void
Geometry::generateTriangles(int8 *adc)
{
	MeshHeader *header = this->meshHeader;
	assert(header != nil);

	this->numTriangles = 0;
	Mesh *m = header->getMeshes();
	int8 *adcbits = adc;
	for(uint32 i = 0; i < header->numMeshes; i++){
		if(m->numIndices < 3){
			// shouldn't happen but it does
			adcbits += m->numIndices;
			m++;
			continue;
		}
		if(header->flags == MeshHeader::TRISTRIP){
			for(uint32 j = 0; j < m->numIndices-2; j++){
				if(!(adc && adcbits[j+2]) &&
				   !isDegenerate(&m->indices[j]))
					this->numTriangles++;
			}
		}else
			this->numTriangles += m->numIndices/3;
		adcbits += m->numIndices;
		m++;
	}

	Triangle *tri = this->triangles;
	m = header->getMeshes();
	adcbits = adc;
	for(uint32 i = 0; i < header->numMeshes; i++){
		if(m->numIndices < 3){
			adcbits += m->numIndices;
			m++;
			continue;
		}
		int32 matid = this->matList.findIndex(m->material);
		if(header->flags == MeshHeader::TRISTRIP)
			for(uint32 j = 0; j < m->numIndices-2; j++){
				if((adc && adcbits[j+2]) ||
				   isDegenerate(&m->indices[j]))
					continue;
				tri->v[0] = m->indices[j+0];
				tri->v[1] = m->indices[j+1 + (j%2)];
				tri->v[2] = m->indices[j+2 - (j%2)];
				tri->matId = matid;
				tri++;
			}
		else
			for(uint32 j = 0; j < m->numIndices-2; j+=3){
				tri->v[0] = m->indices[j+0];
				tri->v[1] = m->indices[j+1];
				tri->v[2] = m->indices[j+2];
				tri->matId = matid;
				tri++;
			}
		adcbits += m->numIndices;
		m++;
	}
}

static void
dumpMesh(Mesh *m)
{
	for(uint32 i = 0; i < m->numIndices-2; i++) 
//		if(i % 2)
//			printf("%3d %3d %3d\n",
//				m->indices[i+1],
//				m->indices[i],
//				m->indices[i+2]);
//		else
			printf("%d %d %d\n",
				m->indices[i],
				m->indices[i+1],
				m->indices[i+2]);
}

void
Geometry::buildMeshes(void)
{
	Triangle *tri;
	Mesh *mesh;

	if(this->flags & Geometry::NATIVE){
		fprintf(stderr, "WARNING: trying Geometry::buildMeshes() on pre-instanced geometry\n");
		return;
	}

	rwFree(this->meshHeader);
	this->meshHeader = nil;
	int32 numMeshes = this->matList.numMaterials;
	if((this->flags & Geometry::TRISTRIP) == 0){
		int32 *numIndices = rwNewT(int32, numMeshes,
			MEMDUR_FUNCTION | ID_GEOMETRY);
		memset(numIndices, 0, numMeshes*sizeof(int32));

		// count indices per mesh
		tri = this->triangles;
		for(int32 i = 0; i < this->numTriangles; i++){
			assert(tri->matId < numMeshes);
			numIndices[tri->matId] += 3;
			tri++;
		}
		// setup meshes
		this->allocateMeshes(numMeshes, this->numTriangles*3, 0);
		mesh = this->meshHeader->getMeshes();
		for(int32 i = 0; i < numMeshes; i++){
			mesh[i].material = this->matList.materials[i];
			mesh[i].numIndices = numIndices[i];
		}
		this->meshHeader->setupIndices();
		rwFree(numIndices);

		// now fill in the indices
		for(int32 i = 0; i < numMeshes; i++)
			mesh[i].numIndices = 0;
		tri = this->triangles;
		for(int32 i = 0; i < this->numTriangles; i++){
			uint32 idx = mesh[tri->matId].numIndices;
			mesh[tri->matId].indices[idx++] = tri->v[0];
			mesh[tri->matId].indices[idx++] = tri->v[1];
			mesh[tri->matId].indices[idx++] = tri->v[2];
			mesh[tri->matId].numIndices = idx;
			tri++;
		}
	}else
		this->buildTristrips();
}

/* The idea is that even in meshes where winding is not preserved
 * every tristrip starts at an even vertex. So find the start of
 * strips and insert duplicate vertices if necessary. */
void
Geometry::correctTristripWinding(void)
{
	MeshHeader *header = this->meshHeader;
	if(this->flags & NATIVE || header == nil ||
	   header->flags != MeshHeader::TRISTRIP)
		return;
	this->meshHeader = nil;
	// Allocate no indices, we realloc later
	MeshHeader *newhead = this->allocateMeshes(header->numMeshes, 0, 1);
	newhead->flags = header->flags;
	/* get a temporary working buffer */
	uint16 *indices = rwNewT(uint16, header->totalIndices*2,
		MEMDUR_FUNCTION | ID_GEOMETRY);

	Mesh *mesh = header->getMeshes();
	Mesh *newmesh = newhead->getMeshes();
	for(uint16 i = 0; i < header->numMeshes; i++){
		newmesh->numIndices = 0;
		newmesh->indices = &indices[newhead->totalIndices];
		newmesh->material = mesh->material;

		bool inStrip = 0;
		uint32 j;
		for(j = 0; j < mesh->numIndices-2; j++){
			/* Duplicate vertices indicate end of strip */
			if(mesh->indices[j] == mesh->indices[j+1] ||
			   mesh->indices[j+1] == mesh->indices[j+2])
				inStrip = 0;
			else if(!inStrip){
				/* Entering strip now,
				 * make sure winding is correct */
				inStrip = 1;
				if(newmesh->numIndices % 2){
					newmesh->indices[newmesh->numIndices] =
					  newmesh->indices[newmesh->numIndices-1];
					newmesh->numIndices++;
				}
			}
			newmesh->indices[newmesh->numIndices++] = mesh->indices[j];
		}
		for(; j < mesh->numIndices; j++)
			newmesh->indices[newmesh->numIndices++] = mesh->indices[j];
		newhead->totalIndices += newmesh->numIndices;

		mesh++;
		newmesh++;
	}
	rwFree(header);
	// Now allocate indices and copy them
	this->allocateMeshes(newhead->numMeshes, newhead->totalIndices, 0);
	memcpy(this->meshHeader->getMeshes()->indices, indices, this->meshHeader->totalIndices*2);
	rwFree(indices);
}

void
Geometry::removeUnusedMaterials(void)
{
	if(this->meshHeader == nil)
		return;
	MeshHeader *mh = this->meshHeader;
	Mesh *m = mh->getMeshes();
	for(uint32 i = 0; i < mh->numMeshes; i++)
		if(m[i].indices == nil)
			return;

	int32 *map = rwNewT(int32, this->matList.numMaterials,
		MEMDUR_FUNCTION | ID_GEOMETRY);
	Material **materials = rwNewT(Material*,this->matList.numMaterials,
		MEMDUR_EVENT | ID_MATERIAL);
	int32 numMaterials = 0;
	/* Build new material list and map */
	for(uint32 i = 0; i < mh->numMeshes; i++){
		if(m[i].numIndices <= 0)
			continue;
		materials[numMaterials] = m[i].material;
		m[i].material->addRef();
		int32 oldid = this->matList.findIndex(m[i].material);
		map[oldid] = numMaterials;
		numMaterials++;
	}
	for(int32 i = 0; i < this->matList.numMaterials; i++)
		this->matList.materials[i]->destroy();
	rwFree(this->matList.materials);
	this->matList.materials = materials;
	this->matList.space = this->matList.numMaterials;
	this->matList.numMaterials = numMaterials;

	/* Build new meshes */
	this->meshHeader = nil;
	MeshHeader *newmh = this->allocateMeshes(numMaterials, mh->totalIndices, 0);
	newmh->flags = mh->flags;
	Mesh *newm = newmh->getMeshes();
	for(uint32 i = 0; i < mh->numMeshes; i++){
		if(m[i].numIndices <= 0)
			continue;
		newm->numIndices = m[i].numIndices;
		newm->material = m[i].material;
		newm++;
	}
	newmh->setupIndices();
	/* Copy indices */
	newm = newmh->getMeshes();;
	for(uint32 i = 0; i < mh->numMeshes; i++){
		if(m[i].numIndices <= 0)
			continue;
		memcpy(newm->indices, m[i].indices,
		       m[i].numIndices*sizeof(*m[i].indices));
		newm++;
	}
	rwFree(mh);

	/* Remap triangle material IDs */
	for(int32 i = 0; i < this->numTriangles; i++)
		this->triangles[i].matId = map[this->triangles[i].matId];
	rwFree(map);
}

Sphere
MorphTarget::calculateBoundingSphere(void) const
{
	Sphere sphere;
	V3d min = {  1000000.0f,  1000000.0f,  1000000.0f };
	V3d max = { -1000000.0f, -1000000.0f, -1000000.0f };
	V3d *v = this->vertices;
	for(int32 j = 0; j < this->parent->numVertices; j++){
		if(v->x > max.x) max.x = v->x;
		if(v->x < min.x) min.x = v->x;
		if(v->y > max.y) max.y = v->y;
		if(v->y < min.y) min.y = v->y;
		if(v->z > max.z) max.z = v->z;
		if(v->z < min.z) min.z = v->z;
		v++;
	}
	sphere.center = scale(add(min, max), 1/2.0f);
	max = sub(max, sphere.center);
	sphere.radius = length(max);
	return sphere;
}


//
// MaterialList
//
#undef PLUGIN_ID
#define PLUGIN_ID ID_MATERIAL

void
MaterialList::init(void)
{
	this->materials = nil;
	this->numMaterials = 0;
	this->space = 0;
}

void
MaterialList::deinit(void)
{
	if(this->materials){
		for(int32 i = 0; i < this->numMaterials; i++)
			this->materials[i]->destroy();
		rwFree(this->materials);
		this->materials = nil;
	}
}

int32
MaterialList::appendMaterial(Material *mat)
{
	Material **ml;
	int32 space;
	if(this->numMaterials >= this->space){
		space = this->space + 20;
		if(this->materials)
			ml = rwReallocT(Material*, this->materials, space,
						MEMDUR_EVENT | ID_MATERIAL);
		else
			ml = rwMallocT(Material*, space, MEMDUR_EVENT | ID_MATERIAL);
		if(ml == nil)
			return -1;
		this->space = space;
		this->materials = ml;
	}
	this->materials[this->numMaterials++] = mat;
	mat->addRef();
	return this->numMaterials-1;
}

int32
MaterialList::findIndex(Material *mat)
{
	for(int32 i = 0; i < this->numMaterials; i++)
		if(this->materials[i] == mat)
			return i;
	return -1;
}

MaterialList*
MaterialList::streamRead(Stream *stream, MaterialList *matlist)
{
	int32 *indices = nil;
	int32 numMat;
	if(!findChunk(stream, ID_STRUCT, nil, nil)){
		RWERROR((ERR_CHUNK, "STRUCT"));
		goto fail;
	}
	matlist->init();
	numMat = stream->readI32();
	if(numMat == 0)
		return matlist;
	matlist->materials = rwMallocT(Material*,numMat, MEMDUR_EVENT | ID_MATERIAL);
	if(matlist->materials == nil)
		goto fail;
	matlist->space = numMat;

	indices = (int32*)rwMalloc(numMat*4, MEMDUR_FUNCTION | ID_MATERIAL);
	stream->read32(indices, numMat*4);

	Material *m;
	for(int32 i = 0; i < numMat; i++){
		if(indices[i] >= 0){
			m = matlist->materials[indices[i]];
			m->addRef();
		}else{
			if(!findChunk(stream, ID_MATERIAL, nil, nil)){
				RWERROR((ERR_CHUNK, "MATERIAL"));
				goto fail;
			}
			m = Material::streamRead(stream);
			if(m == nil)
				goto fail;
		}
		matlist->appendMaterial(m);
		m->destroy();
	}
	rwFree(indices);
	return matlist;
fail:
	rwFree(indices);
	matlist->deinit();
	return nil;
}

bool
MaterialList::streamWrite(Stream *stream)
{
	uint32 size = this->streamGetSize();
	writeChunkHeader(stream, ID_MATLIST, size);
	writeChunkHeader(stream, ID_STRUCT, 4 + this->numMaterials*4);
	stream->writeI32(this->numMaterials);

	int32 idx;
	for(int32 i = 0; i < this->numMaterials; i++){
		idx = -1;
		for(int32 j = i-1; j >= 0; j--)
			if(this->materials[i] == this->materials[j]){
				idx = j;
				break;
			}
		stream->writeI32(idx);
	}
	for(int32 i = 0; i < this->numMaterials; i++){
		for(int32 j = i-1; j >= 0; j--)
			if(this->materials[i] == this->materials[j])
				goto found;
		this->materials[i]->streamWrite(stream);
		found:;
	}
	return true;
}

uint32
MaterialList::streamGetSize(void)
{
	uint32 size = 12 + 4 + this->numMaterials*4;
	for(int32 i = 0; i < this->numMaterials; i++){
		for(int32 j = i-1; j >= 0; j--)
			if(this->materials[i] == this->materials[j])
				goto found;
		size += 12 + this->materials[i]->streamGetSize();
		found:;
	}
	return size;
}

//
// Material
//

Material*
Material::create(void)
{
	Material *mat = (Material*)rwMalloc(s_plglist.size, MEMDUR_EVENT | ID_MATERIAL);
	if(mat == nil){
		RWERROR((ERR_ALLOC, s_plglist.size));
		return nil;
	}
	numAllocated++;
	mat->texture = nil;
	memset(&mat->color, 0xFF, 4);
	mat->surfaceProps = defaultSurfaceProps;
	mat->pipeline = nil;
	mat->refCount = 1;
	s_plglist.construct(mat);
	return mat;
}

Material*
Material::clone(void)
{
	Material *mat = Material::create();
	if(mat == nil){
		RWERROR((ERR_ALLOC, s_plglist.size));
		return nil;
	}
	mat->color = this->color;
	mat->surfaceProps = this->surfaceProps;
	if(this->texture)
		mat->setTexture(this->texture);
	mat->pipeline = this->pipeline;
	s_plglist.copy(mat, this);
	#ifdef RW_GX
	GxMissingMaterialDiag missingDiag;
	if(gxGetMissingMaterialDiag(this, &missingDiag))
		gxStoreMissingMaterialDiag(mat, &missingDiag);
	#endif
	return mat;
}

void
Material::destroy(void)
{
	this->refCount--;
	if(this->refCount <= 0){
		#ifdef RW_GX
		gxForgetMissingMaterialDiag(this);
		#endif
		s_plglist.destruct(this);
		if(this->texture)
			this->texture->destroy();
		rwFree(this);
		numAllocated--;
	}
}

void
Material::setTexture(Texture *tex)
{
	#ifdef RW_GX
	gxForgetMissingMaterialDiag(this);
	#endif
	if(this->texture)
		this->texture->destroy();
	if(tex)
		tex->addRef();
	this->texture = tex;
}

struct MatStreamData
{
	int32 flags;	// unused according to RW
	RGBA  color;
	int32 unused;
	int32 textured;
};

static uint32 materialRights[2];

Material*
Material::streamRead(Stream *stream)
{
	uint32 length, version;
	MatStreamData buf;

	if(!findChunk(stream, ID_STRUCT, nil, &version)){
		RWERROR((ERR_CHUNK, "STRUCT"));
		return nil;
	}
	stream->read8(&buf, sizeof(buf));
	RGBA col = buf.color;
	memNative32(&buf, sizeof(buf));
	buf.color = col;
	Material *mat = Material::create();
	if(mat == nil)
		return nil;
	mat->color = buf.color;
	if(version < 0x30400)
		mat->surfaceProps = defaultSurfaceProps;
	else
		stream->read32(&mat->surfaceProps, sizeof(SurfaceProperties));
	if(buf.textured){
		if(!findChunk(stream, ID_TEXTURE, &length, nil)){
			RWERROR((ERR_CHUNK, "TEXTURE"));
			goto fail;
		}
		mat->texture = Texture::streamRead(stream);
		#ifdef RW_GX
		const bool placeholderTexture = mat->texture != nil &&
			(mat->texture->raster == nil ||
			 mat->texture->raster->width == 0 ||
			 mat->texture->raster->height == 0);
		if(mat->texture == nil || placeholderTexture ||
		   gxGetLastTextureResolveDiag() != nil)
			gxRememberMissingMaterialDiag(mat);
		#endif
	}

	materialRights[0] = 0;
	if(!s_plglist.streamRead(stream, mat))
		goto fail;
	if(materialRights[0])
		s_plglist.assertRights(mat, materialRights[0], materialRights[1]);
	return mat;

fail:
	mat->destroy();
	return nil;
}

bool
Material::streamWrite(Stream *stream)
{
	MatStreamData buf;

	writeChunkHeader(stream, ID_MATERIAL, this->streamGetSize());
	writeChunkHeader(stream, ID_STRUCT, sizeof(MatStreamData)
		+ (rw::version >= 0x30400 ? 12 : 0));

	buf.color = this->color;
	buf.flags = 0;
	buf.unused = 0;
	buf.textured = this->texture != nil;
	memLittle32(&buf, sizeof(buf));
	buf.color = this->color;
	stream->write8(&buf, sizeof(buf));

	if(rw::version >= 0x30400){
		float32 surfaceProps[3];
		surfaceProps[0] = this->surfaceProps.ambient;
		surfaceProps[1] = this->surfaceProps.specular;
		surfaceProps[2] = this->surfaceProps.diffuse;
		stream->write32(surfaceProps, sizeof(surfaceProps));
	}

	if(this->texture)
		this->texture->streamWrite(stream);

	s_plglist.streamWrite(stream, this);
	return true;
}

uint32
Material::streamGetSize(void)
{
	uint32 size = 0;
	size += 12 + sizeof(MatStreamData);
	if(rw::version >= 0x30400)
		size += 12;
	if(this->texture)
		size += 12 + this->texture->streamGetSize();
	size += 12 + s_plglist.streamGetSize(this);
	return size;
}

// Material Rights plugin

static Stream*
readMaterialRights(Stream *stream, int32, void *, int32, int32)
{
	stream->read32(materialRights, 8);
//	printf("materialrights: %X %X\n", materialRights[0], materialRights[1]);
	return stream;
}

static Stream*
writeMaterialRights(Stream *stream, int32, void *object, int32, int32)
{
	Material *material = (Material*)object;
	uint32 buffer[2];
	buffer[0] = material->pipeline->pluginID;
	buffer[1] = material->pipeline->pluginData;
	stream->write32(buffer, 8);
	return stream;
}

static int32
getSizeMaterialRights(void *object, int32, int32)
{
	Material *material = (Material*)object;
	if(material->pipeline == nil || material->pipeline->pluginID == 0)
		return 0;
	return 8;
}

void
registerMaterialRightsPlugin(void)
{
	Material::registerPlugin(0, ID_RIGHTTORENDER, nil, nil, nil);
	Material::registerPluginStream(ID_RIGHTTORENDER,
	                               readMaterialRights,
	                               writeMaterialRights,
	                               getSizeMaterialRights);
}


}
