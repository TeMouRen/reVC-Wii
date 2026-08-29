#define _CRT_SECURE_NO_WARNINGS
#define WITH_D3D // not WITHD3D, so it's librw define
#include <rwcore.h>
#include <rpworld.h>
#include <rpmatfx.h>
#include <rphanim.h>
#include <rpskin.h>
#include <assert.h>
#include <string.h>
#ifdef RW_GX
#include "gx/rwgx.h"
#include "gx/gxmemory.h"
#endif
#ifdef WITH_D3D
#include "d3d/rwd3dimpl.h"
#endif
#ifndef _WIN32
#include "crossplatform.h"
#endif
#ifdef WII
#include "MemoryMgr.h"
#endif

using namespace rw;

#if defined(RW_GX) && defined(WII)
static bool
ShouldReadExternalTxdNative(void)
{
	// LoadSplash() loads files from TXD\\ through the dedicated "splash"
	// dictionary slot. Keep the legacy native reader scoped to that one-shot
	// path; streamed MODELS TXDs must remain GX-native only.
	return RwWiiIsLoadingSplashTxd();
}
#endif

#ifdef WII
static bool
IsCriticalUiRwAlloc(size_t size)
{
#ifdef RW_GX
	return size >= 64u * 1024u && gx::isCriticalUiUploadContextActive();
#else
	(void)size;
	return false;
#endif
}

static bool
ShouldRouteRwAllocToMem2(size_t size, uint32 hint)
{
	const uint32 rwId = hint & 0xFFFFu;
	const uint32 dur = hint & 0xF0000u;

	if(IsCriticalUiRwAlloc(size))
		return true;
	if(dur < MEMDUR_EVENT)
		return false;
	if(size < 12 * 1024)
		return false;

	switch(rwId){
	case ID_GEOMETRY & 0xFFFFu:
		return true;
	default:
		return false;
	}
}

static void
RecordRwResidentPool(void *mem, uint32 hint)
{
	if(mem == nil || (hint & 0xF0000u) < MEMDUR_EVENT)
		return;
	if(WiiMemoryOwnsGenericMem2(mem))
		WiiMemoryRecordResidentPool(WII_STREAM_PRESSURE_GENERIC);
}
#endif

RwUInt8 RwObjectGetType(const RwObject *obj) { return obj->type; }
RwFrame* rwObjectGetParent(const RwObject *obj) { return (RwFrame*)obj->parent; }

void *RwMalloc(size_t size) { return engine->memfuncs.rwmustmalloc(size, 0); }
void *RwCalloc(size_t numObj, size_t sizeObj) {
	void *mem = RwMalloc(numObj*sizeObj);
	if(mem)
		memset(mem, 0, numObj*sizeObj);
	return mem;
}
void  RwFree(void *mem) { engine->memfuncs.rwfree(mem); }


//RwReal RwV3dNormalize(RwV3d * out, const RwV3d * in);
RwReal RwV3dLength(const RwV3d * in) { return length(*in); }
//RwReal RwV2dLength(const RwV2d * in);
//RwReal RwV2dNormalize(RwV2d * out, const RwV2d * in);
//void RwV2dAssign(RwV2d * out, const RwV2d * ina);
//void RwV2dAdd(RwV2d * out, const RwV2d * ina, const RwV2d * inb);
//void RwV2dLineNormal(RwV2d * out, const RwV2d * ina, const RwV2d * inb);
//void RwV2dSub(RwV2d * out, const RwV2d * ina, const RwV2d * inb);
//void RwV2dPerp(RwV2d * out, const RwV2d * in);
//void RwV2dScale(RwV2d * out, const RwV2d * in, RwReal scalar);
//RwReal RwV2dDotProduct(const RwV2d * ina, const RwV2d * inb);
//void RwV3dAssign(RwV3d * out, const RwV3d * ina);
void RwV3dAdd(RwV3d * out, const RwV3d * ina, const RwV3d * inb) { *out = add(*ina, *inb); }
void RwV3dSub(RwV3d * out, const RwV3d * ina, const RwV3d * inb) { *out = sub(*ina, *inb); }
void RwV3dScale(RwV3d * out, const RwV3d * in, RwReal scalar) { *out = scale(*in, scalar); }
void RwV3dIncrementScaled(RwV3d * out,  const RwV3d * in, RwReal scalar) { *out = add(*out, scale(*in, scalar)); }
void RwV3dNegate(RwV3d * out, const RwV3d * in) { *out = neg(*in); }
RwReal RwV3dDotProduct(const RwV3d * ina, const RwV3d * inb) { return dot(*ina, *inb); }
//void RwV3dCrossProduct(RwV3d * out, const RwV3d * ina, const RwV3d * inb);
RwV3d *RwV3dTransformPoints(RwV3d * pointsOut, const RwV3d * pointsIn, RwInt32 numPoints, const RwMatrix * matrix)
	{ V3d::transformPoints(pointsOut, pointsIn, numPoints, matrix); return pointsOut; }
//RwV3d *RwV3dTransformVectors(RwV3d * vectorsOut, const RwV3d * vectorsIn, RwInt32 numPoints, const RwMatrix * matrix);



RwBool RwMatrixDestroy(RwMatrix *mpMat) { mpMat->destroy(); return true; }
RwMatrix *RwMatrixCreate(void) { return Matrix::create(); }
void RwMatrixCopy(RwMatrix * dstMatrix, const RwMatrix * srcMatrix) { *dstMatrix = *srcMatrix; }
void RwMatrixSetIdentity(RwMatrix * matrix) { matrix->setIdentity(); }
RwMatrix *RwMatrixMultiply(RwMatrix * matrixOut, const RwMatrix * MatrixIn1, const RwMatrix * matrixIn2);
RwMatrix *RwMatrixTransform(RwMatrix * matrix, const RwMatrix * transform, RwOpCombineType combineOp)
	{ matrix->transform(transform, (rw::CombineOp)combineOp); return matrix; }
//RwMatrix *RwMatrixOrthoNormalize(RwMatrix * matrixOut, const RwMatrix * matrixIn);
RwMatrix *RwMatrixInvert(RwMatrix * matrixOut, const RwMatrix * matrixIn) { Matrix::invert(matrixOut, matrixIn); return matrixOut; }
RwMatrix *RwMatrixScale(RwMatrix * matrix, const RwV3d * scale, RwOpCombineType combineOp)
	{ matrix->scale(scale, (rw::CombineOp)combineOp); return matrix; }
RwMatrix *RwMatrixTranslate(RwMatrix * matrix, const RwV3d * translation, RwOpCombineType combineOp)
	{ matrix->translate(translation, (rw::CombineOp)combineOp); return matrix; }
RwMatrix *RwMatrixRotate(RwMatrix * matrix, const RwV3d * axis, RwReal angle, RwOpCombineType combineOp)
	{ matrix->rotate(axis, angle, (rw::CombineOp)combineOp); return matrix; }
//RwMatrix *RwMatrixRotateOneMinusCosineSine(RwMatrix * matrix, const RwV3d * unitAxis, RwReal oneMinusCosine, RwReal sine, RwOpCombineType combineOp);
//const RwMatrix *RwMatrixQueryRotate(const RwMatrix * matrix, RwV3d * unitAxis, RwReal * angle, RwV3d * center);
RwV3d *RwMatrixGetRight(RwMatrix * matrix) { return &matrix->right; }
RwV3d *RwMatrixGetUp(RwMatrix * matrix) { return &matrix->up; }
RwV3d *RwMatrixGetAt(RwMatrix * matrix) { return &matrix->at; }
RwV3d *RwMatrixGetPos(RwMatrix * matrix) { return &matrix->pos; }
RwMatrix *RwMatrixUpdate(RwMatrix * matrix) {
	if(matrix == nil || (uintptr_t)matrix < 0x1000){
		printf("[RW-GUARD] RwMatrixUpdate rejected suspicious matrix=%p\n", (void*)matrix);
		return nil;
	}
	matrix->update();
	return matrix;
}
//RwMatrix *RwMatrixOptimize(RwMatrix * matrix, const RwMatrixTolerance *tolerance);




RwFrame *RwFrameForAllObjects(RwFrame * frame, RwObjectCallBack callBack, void *data) {
	FORLIST(lnk, frame->objectList)
		if(callBack(&ObjectWithFrame::fromFrame(lnk)->object, data) == nil)
			break;
	return frame;
}
RwFrame *RwFrameTranslate(RwFrame * frame, const RwV3d * v, RwOpCombineType combine) { frame->translate(v, (CombineOp)combine); return frame; }
RwFrame *RwFrameRotate(RwFrame * frame, const RwV3d * axis, RwReal angle, RwOpCombineType combine) { frame->rotate(axis, angle, (CombineOp)combine); return frame; }
RwFrame *RwFrameScale(RwFrame * frame, const RwV3d * v, RwOpCombineType combine) { frame->scale(v, (CombineOp)combine); return frame; }
RwFrame *RwFrameTransform(RwFrame * frame, const RwMatrix * m, RwOpCombineType combine) { frame->transform(m, (CombineOp)combine); return frame; }
//TODO: actually implement this!
RwFrame *RwFrameOrthoNormalize(RwFrame * frame) { return frame; }
RwFrame *RwFrameSetIdentity(RwFrame * frame) { frame->matrix.setIdentity(); frame->updateObjects(); return frame; }
//RwFrame *RwFrameCloneHierarchy(RwFrame * root);
//RwBool RwFrameDestroyHierarchy(RwFrame * frame);
RwFrame *RwFrameForAllChildren(RwFrame * frame, RwFrameCallBack callBack, void *data)
	{ return frame->forAllChildren(callBack, data); }
RwFrame *RwFrameRemoveChild(RwFrame * child) { child->removeChild(); return child; }
RwFrame *RwFrameAddChild(RwFrame * parent, RwFrame * child) { parent->addChild(child); return parent; }
RwFrame *RwFrameGetParent(const RwFrame * frame) { return frame->getParent(); }
//RwFrame *RwFrameGetRoot(const RwFrame * frame);
RwMatrix *RwFrameGetLTM(RwFrame * frame) {
	if(frame == nil){
		printf("[RW-GUARD] RwFrameGetLTM(NULL)\n");
		return nil;
	}
	return frame->getLTM();
}
RwMatrix *RwFrameGetMatrix(RwFrame * frame) {
	if(frame == nil){
		printf("[RW-GUARD] RwFrameGetMatrix(NULL)\n");
		return nil;
	}
	return &frame->matrix;
}
RwFrame *RwFrameUpdateObjects(RwFrame * frame) {
	if(frame == nil){
		printf("[RW-GUARD] RwFrameUpdateObjects(NULL)\n");
		return nil;
	}
	frame->updateObjects();
	return frame;
}
RwFrame *RwFrameCreate(void) { return rw::Frame::create(); }
//RwBool RwFrameInit(RwFrame *frame);
//RwBool RwFrameDeInit(RwFrame *frame);
RwBool RwFrameDestroy(RwFrame * frame) { frame->destroy(); return true; }
//void _rwFrameInit(RwFrame *frame);
//void _rwFrameDeInit(RwFrame *frame);
//RwBool RwFrameDirty(const RwFrame * frame);
//RwInt32 RwFrameCount(RwFrame * frame);
//RwBool RwFrameSetStaticPluginsSize(RwInt32 size);
RwInt32 RwFrameRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB, RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB)
	{ return Frame::registerPlugin(size, pluginID, constructCB, destructCB, (CopyConstructor)copyCB); }
//RwInt32 RwFrameGetPluginOffset(RwUInt32 pluginID);
//RwBool RwFrameValidatePlugins(const RwFrame * frame);
//RwFrame *_rwFrameCloneAndLinkClones(RwFrame * root);
//RwFrame *_rwFramePurgeClone(RwFrame *root);

RwInt32 RwFrameRegisterPluginStream(RwUInt32 pluginID, RwPluginDataChunkReadCallBack readCB, RwPluginDataChunkWriteCallBack writeCB, RwPluginDataChunkGetSizeCallBack getSizeCB)
	{ return Frame::registerPluginStream(pluginID, readCB, (StreamWrite)writeCB, (StreamGetSize)getSizeCB); }


rwFrameList *rwFrameListDeinitialize(rwFrameList *frameList) {
	rwFree(frameList->frames);
	frameList->frames = nil;
	return frameList;
}
rwFrameList *rwFrameListStreamRead(RwStream *stream, rwFrameList *fl) { return fl->streamRead(stream); }




RwCamera    *RwCameraBeginUpdate(RwCamera * camera) { camera->beginUpdate(); return camera; }
RwCamera    *RwCameraEndUpdate(RwCamera * camera) { camera->endUpdate(); return camera; }
RwCamera    *RwCameraClear(RwCamera * camera, RwRGBA * colour, RwInt32 clearMode) { camera->clear(colour, clearMode); return camera; }
// WARNING: ignored argument
RwCamera    *RwCameraShowRaster(RwCamera * camera, void *pDev, RwUInt32 flags) { camera->showRaster(flags); return camera; }
RwBool       RwCameraDestroy(RwCamera * camera) { camera->destroy(); return true; }
RwCamera    *RwCameraCreate(void) { return rw::Camera::create(); }
RwCamera    *RwCameraClone(RwCamera * camera) { return camera->clone(); }
RwCamera    *RwCameraSetViewOffset(RwCamera *camera, const RwV2d *offset) { camera->setViewOffset(offset); return camera; }
RwCamera    *RwCameraSetViewWindow(RwCamera *camera, const RwV2d *viewWindow) { camera->setViewWindow(viewWindow); return camera; }
RwCamera    *RwCameraSetProjection(RwCamera *camera, RwCameraProjection projection) { camera->projection = projection; return camera; }
RwCamera    *RwCameraSetNearClipPlane(RwCamera *camera, RwReal nearClip) { camera->setNearPlane(nearClip); return camera; }
RwCamera    *RwCameraSetFarClipPlane(RwCamera *camera, RwReal farClip) { camera->setFarPlane(farClip); return camera; }
RwInt32      RwCameraRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB, RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB);
RwInt32      RwCameraGetPluginOffset(RwUInt32 pluginID);
RwBool       RwCameraValidatePlugins(const RwCamera * camera);
RwFrustumTestResult RwCameraFrustumTestSphere(const RwCamera * camera, const RwSphere * sphere) { return (RwFrustumTestResult)camera->frustumTestSphere(sphere); }
const RwV2d *RwCameraGetViewOffset(const RwCamera *camera) { return &camera->viewOffset; }
RwCamera    *RwCameraSetRaster(RwCamera *camera, RwRaster *raster) { camera->frameBuffer = raster; return camera; }
RwRaster    *RwCameraGetRaster(const RwCamera *camera) { return camera->frameBuffer; }
RwCamera    *RwCameraSetZRaster(RwCamera *camera, RwRaster *zRaster) { camera->zBuffer = zRaster; return camera; }
RwRaster    *RwCameraGetZRaster(const RwCamera *camera) { return camera->zBuffer; }
RwReal       RwCameraGetNearClipPlane(const RwCamera *camera) { return camera->nearPlane; }
RwReal       RwCameraGetFarClipPlane(const RwCamera *camera) { return camera->farPlane; }
RwCamera    *RwCameraSetFogDistance(RwCamera *camera, RwReal fogDistance) { camera->fogPlane = fogDistance; return camera; }
RwReal       RwCameraGetFogDistance(const RwCamera *camera) { return camera->fogPlane; }
RwCamera    *RwCameraGetCurrentCamera(void) { return rw::engine->currentCamera; }
RwCameraProjection RwCameraGetProjection(const RwCamera *camera);
const RwV2d *RwCameraGetViewWindow(const RwCamera *camera) { return &camera->viewWindow; }
RwMatrix    *RwCameraGetViewMatrix(RwCamera *camera) { return &camera->viewMatrix; }
RwCamera    *RwCameraSetFrame(RwCamera *camera, RwFrame *frame) { camera->setFrame(frame); return camera; }
RwFrame     *RwCameraGetFrame(const RwCamera *camera) { return camera->getFrame(); }





RwImage     *RwImageCreate(RwInt32 width, RwInt32 height, RwInt32 depth) { return Image::create(width, height, depth); }
RwBool       RwImageDestroy(RwImage * image) { image->destroy(); return true; }
RwImage     *RwImageAllocatePixels(RwImage * image) { image->allocate(); return image; }
RwImage     *RwImageFreePixels(RwImage * image) { image->free(); return image; }
RwImage     *RwImageCopy(RwImage * destImage, const RwImage * sourceImage);
RwImage     *RwImageResize(RwImage * image, RwInt32 width, RwInt32 height);
RwImage     *RwImageApplyMask(RwImage * image, const RwImage * mask);
RwImage     *RwImageMakeMask(RwImage * image);
RwImage     *RwImageReadMaskedImage(const RwChar * imageName, const RwChar * maskname);
RwImage     *RwImageRead(const RwChar * imageName);
RwImage     *RwImageWrite(RwImage * image, const RwChar * imageName);
RwChar      *RwImageGetPath(void);
const RwChar *RwImageSetPath(const RwChar * path) { Image::setSearchPath(path); return path; }
RwImage     *RwImageSetStride(RwImage * image, RwInt32 stride) { image->stride = stride; return image; }
RwImage     *RwImageSetPixels(RwImage * image, RwUInt8 * pixels) { image->pixels = pixels; return image; }
RwImage     *RwImageSetPalette(RwImage * image, RwRGBA * palette) { image->palette = (uint8*)palette; return image; }
RwInt32      RwImageGetWidth(const RwImage * image) { return image->width; }
RwInt32      RwImageGetHeight(const RwImage * image) { return image->height; }
RwInt32      RwImageGetDepth(const RwImage * image) { return image->depth; }
RwInt32      RwImageGetStride(const RwImage * image) { return image->stride; }
RwUInt8     *RwImageGetPixels(const RwImage * image) { return image->pixels; }
RwRGBA      *RwImageGetPalette(const RwImage * image) { return (RwRGBA*)image->palette; }
RwUInt32     RwRGBAToPixel(RwRGBA * rgbIn, RwInt32 rasterFormat);
RwRGBA      *RwRGBASetFromPixel(RwRGBA * rgbOut, RwUInt32 pixelValue, RwInt32 rasterFormat);
RwBool       RwImageSetGamma(RwReal gammaValue);
RwReal       RwImageGetGamma(void);
RwImage     *RwImageGammaCorrect(RwImage * image);
RwRGBA      *RwRGBAGammaCorrect(RwRGBA * rgb);
RwInt32      RwImageRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB, RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB);
RwInt32      RwImageGetPluginOffset(RwUInt32 pluginID);
RwBool       RwImageValidatePlugins(const RwImage * image);
//RwBool       RwImageRegisterImageFormat(const RwChar * extension, RwImageCallBackRead imageRead, RwImageCallBackWrite imageWrite);
const RwChar *RwImageFindFileType(const RwChar * imageName);
RwInt32      RwImageStreamGetSize(const RwImage * image);
RwImage     *RwImageStreamRead(RwStream * stream);
const RwImage *RwImageStreamWrite(const RwImage * image, RwStream * stream);

RwImage *RwImageFindRasterFormat(RwImage *ipImage,RwInt32 nRasterType, RwInt32 *npWidth,RwInt32 *npHeight, RwInt32 *npDepth,RwInt32 *npFormat)
{
	return Raster::imageFindRasterFormat(ipImage, nRasterType, npWidth, npHeight, npDepth, npFormat) ? ipImage : nil;
}




RwRaster    *RwRasterCreate(RwInt32 width, RwInt32 height, RwInt32 depth, RwInt32 flags) { return Raster::create(width, height, depth, flags); }
RwBool       RwRasterDestroy(RwRaster * raster) { raster->destroy(); return true; }
RwInt32      RwRasterGetWidth(const RwRaster *raster) { return raster->width; }
RwInt32      RwRasterGetHeight(const RwRaster *raster) { return raster->height; }
RwInt32      RwRasterGetStride(const RwRaster *raster);
RwInt32      RwRasterGetDepth(const RwRaster *raster) { return raster->depth; }
RwInt32      RwRasterGetFormat(const RwRaster *raster);
RwInt32      RwRasterGetType(const RwRaster *raster);
RwRaster    *RwRasterGetParent(const RwRaster *raster) { return raster->parent; }
RwRaster    *RwRasterGetOffset(RwRaster *raster,  RwInt16 *xOffset, RwInt16 *yOffset);
RwInt32      RwRasterGetNumLevels(RwRaster * raster);
RwRaster    *RwRasterSubRaster(RwRaster * subRaster, RwRaster * raster, RwRect * rect);
RwRaster    *RwRasterRenderFast(RwRaster * raster, RwInt32 x, RwInt32 y) { return raster->renderFast(x, y) ? raster : nil; }
RwRaster    *RwRasterRender(RwRaster * raster, RwInt32 x, RwInt32 y);
RwRaster    *RwRasterRenderScaled(RwRaster * raster, RwRect * rect);
RwRaster    *RwRasterPushContext(RwRaster * raster) { return Raster::pushContext(raster); }
RwRaster    *RwRasterPopContext(void) { return Raster::popContext(); }
RwRaster    *RwRasterGetCurrentContext(void) { return Raster::getCurrentContext(); }
RwBool       RwRasterClear(RwInt32 pixelValue);
RwBool       RwRasterClearRect(RwRect * rpRect, RwInt32 pixelValue);
RwRaster    *RwRasterShowRaster(RwRaster * raster, void *dev, RwUInt32 flags);
RwUInt8     *RwRasterLock(RwRaster * raster, RwUInt8 level, RwInt32 lockMode) { return raster->lock(level, lockMode); }
RwRaster    *RwRasterUnlock(RwRaster * raster) { raster->unlock(0); return raster; }
RwUInt8     *RwRasterLockPalette(RwRaster * raster, RwInt32 lockMode);
RwRaster    *RwRasterUnlockPalette(RwRaster * raster);
RwInt32      RwRasterRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB, RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB);
RwInt32      RwRasterGetPluginOffset(RwUInt32 pluginID);
RwBool       RwRasterValidatePlugins(const RwRaster * raster);

RwRaster *RwRasterSetFromImage(RwRaster *raster, RwImage *image) { return raster->setFromImage(image); }




RwTexture *RwTextureCreate(RwRaster * raster) { return Texture::create(raster); }
RwBool RwTextureDestroy(RwTexture * texture) { texture->destroy(); return true; }
RwTexture *RwTextureAddRef(RwTexture *texture) { texture->addRef(); return texture; }
// TODO
RwBool RwTextureSetMipmapping(RwBool enable) { return true; }
RwBool RwTextureGetMipmapping(void);
// TODO
RwBool RwTextureSetAutoMipmapping(RwBool enable) { return true; }
RwBool RwTextureGetAutoMipmapping(void);
RwBool RwTextureSetMipmapGenerationCallBack(RwTextureCallBackMipmapGeneration callback);
RwTextureCallBackMipmapGeneration RwTextureGetMipmapGenerationCallBack(void);
RwBool RwTextureSetMipmapNameCallBack(RwTextureCallBackMipmapName callback);
RwTextureCallBackMipmapName RwTextureGetMipmapNameCallBack(void);
RwBool RwTextureGenerateMipmapName(RwChar * name, RwChar * maskName, RwUInt8 mipLevel, RwInt32 format);
RwBool RwTextureRasterGenerateMipmaps(RwRaster * raster, RwImage * image);
RwTextureCallBackRead RwTextureGetReadCallBack(void);
RwBool RwTextureSetReadCallBack(RwTextureCallBackRead fpCallBack);
RwTexture *RwTextureSetName(RwTexture * texture, const RwChar * name) {
	strncpy(texture->name, name, 32);
	texture->name[31] = '\0';
#ifdef RW_GX
	if(texture->raster)
		rw::gx::texPoolRename(texture->raster, texture->name);
#endif
	return texture;
}
RwTexture *RwTextureSetMaskName(RwTexture * texture, const RwChar * maskName);
RwChar *RwTextureGetName(RwTexture *texture) { return texture->name; }
RwChar *RwTextureGetMaskName(RwTexture *texture);
RwTexture *RwTextureSetRaster(RwTexture * texture, RwRaster * raster) { texture->raster = raster; return texture; }
RwTexture   *RwTextureRead(const RwChar * name, const RwChar * maskName) { return Texture::read(name, maskName); }
RwRaster *RwTextureGetRaster(const RwTexture *texture) { return texture->raster; }
RwInt32 RwTextureRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB, RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB);
RwInt32 RwTextureGetPluginOffset(RwUInt32 pluginID);
RwBool RwTextureValidatePlugins(const RwTexture * texture);

RwTexDictionary *RwTextureGetDictionary(RwTexture *texture);
RwTexture *RwTextureSetFilterMode(RwTexture *texture, RwTextureFilterMode filtering) { texture->setFilter((Texture::FilterMode)filtering); return texture; }
RwTextureFilterMode RwTextureGetFilterMode(const RwTexture *texture);
RwTexture *RwTextureSetAddressing(RwTexture *texture, RwTextureAddressMode addressing) {
	texture->setAddressU((Texture::Addressing)addressing);
	texture->setAddressV((Texture::Addressing)addressing);
	return texture;
}
RwTexture *RwTextureSetAddressingU(RwTexture *texture, RwTextureAddressMode addressing) {
	texture->setAddressU((Texture::Addressing)addressing);
	return texture;
}
RwTexture *RwTextureSetAddressingV(RwTexture *texture, RwTextureAddressMode addressing) {
	texture->setAddressV((Texture::Addressing)addressing);
	return texture;
}
RwTextureAddressMode RwTextureGetAddressing(const RwTexture *texture);
RwTextureAddressMode RwTextureGetAddressingU(const RwTexture *texture);
RwTextureAddressMode RwTextureGetAddressingV(const RwTexture *texture);

// TODO
void _rwD3D8TexDictionaryEnableRasterFormatConversion(bool enable) { }

#ifdef LIBRW
namespace rw {
void debugSetCurrentConvertingTextureName(const char *name);
}
#endif

static bool
fakeShouldLogFocusedConversionResult(const char *name)
{
	if(name == nil || name[0] == '\0')
		return false;

	return strcmp(name, "kbtree3_test") == 0 ||
	       strcmp(name, "kbtree4_test") == 0 ||
	       strcmp(name, "newtreeleaves128") == 0 ||
	       strcmp(name, "newtreeleavesb128") == 0 ||
	       strcmp(name, "foliage256") == 0 ||
	       strcmp(name, "planta256") == 0 ||
	       strcmp(name, "plantb256") == 0 ||
	       strcmp(name, "plantc256") == 0 ||
	       strcmp(name, "fuzzyplant256") == 0 ||
	       strncmp(name, "htl_", 4) == 0 ||
	       strncmp(name, "ht_", 3) == 0 ||
	       strncmp(name, "hot_", 4) == 0 ||
	       strncmp(name, "mob_", 4) == 0 ||
	       strncmp(name, "nt_wall", 7) == 0 ||
	       strncmp(name, "nt_floor", 8) == 0 ||
	       strncmp(name, "nt_woodwall", 11) == 0 ||
	       strstr(name, "hotel") != nil ||
	       strstr(name, "lobby") != nil ||
	       strstr(name, "tree") != nil ||
	       strstr(name, "plant") != nil ||
	       strstr(name, "leaf") != nil;
}


// hack for reading native textures
RwBool rwNativeTextureHackRead(RwStream *stream, RwTexture **tex, RwInt32 size)
{
	*tex = Texture::streamReadNative(stream);
	#if defined(RW_GX) && defined(WII)
	if(*tex == nil && ShouldReadExternalTxdNative()){
		// Texture::streamReadNative restores the stream to the native STRUCT
		// header before rejecting a texture from another platform.
		*tex = d3d8::readNativeTexture((rw::Stream*)stream);
		if(*tex != nil)
			printf("[TXD-NATIVE] D3D8 source accepted txd=%s size=%d texture=%s\n",
			       "splash",
			       (int)size,
			       (*tex)->name);
	}
	#endif
	if(*tex == nil){
		printf("[TXD-FAIL] rwNativeTextureHackRead: streamReadNative failed size=%d\n", (int)size);
		return false;
	}
#ifdef LIBRW
	if((*tex)->raster == nil){
		printf("[TXD-FAIL] rwNativeTextureHackRead: texture has null raster size=%d\n", (int)size);
		(*tex)->destroy();
		*tex = nil;
		return false;
	}
	rw::debugSetCurrentConvertingTextureName((*tex)->name);
	Raster *converted = rw::Raster::convertTexToCurrentPlatform((*tex)->raster);
	rw::debugSetCurrentConvertingTextureName(nil);
	if(converted == nil){
		printf("[TXD-FAIL] rwNativeTextureHackRead: platform conversion failed size=%d\n", (int)size);
		(*tex)->destroy();
		*tex = nil;
		return false;
	}
	(*tex)->raster = converted;
#ifdef RW_GX
	if((*tex)->raster && (*tex)->raster->platform == PLATFORM_GX) {
		rw::gx::syncNativeSamplerFromTexture(*tex, (*tex)->raster);
		rw::gx::texPoolRename((*tex)->raster, (*tex)->name);
		#if 0 // [FAKE-TXD-CONVERT] focused conversion tracing disabled for normal runs.
		if(fakeShouldLogFocusedConversionResult((*tex)->name)) {
			printf("[FAKE-TXD-CONVERT] tex=%s raster=%p plat=%d fmt=0x%X %dx%d depth=%d addr=%u/%u filter=%d\n",
			       (*tex)->name,
			       (void*)(*tex)->raster,
			       (*tex)->raster->platform,
			       (*tex)->raster->format,
			       (*tex)->raster->width,
			       (*tex)->raster->height,
			       (*tex)->raster->depth,
			       (unsigned)(*tex)->getAddressU(),
			       (unsigned)(*tex)->getAddressV(),
			       (int)(*tex)->getFilter());
		}
		#endif
	}
#endif
#endif
	return *tex != nil;
}





RwTexDictionary *RwTexDictionaryCreate(void) { return TexDictionary::create(); }
RwBool RwTexDictionaryDestroy(RwTexDictionary * dict) { dict->destroy(); return true; }
RwTexture *RwTexDictionaryAddTexture(RwTexDictionary * dict, RwTexture * texture) { dict->addFront(texture); return texture; }
//RwTexture *RwTexDictionaryRemoveTexture(RwTexture * texture);
RwTexture *RwTexDictionaryFindNamedTexture(RwTexDictionary * dict, const RwChar * name) { return dict->find(name); }
RwTexDictionary *RwTexDictionaryGetCurrent(void) { return TexDictionary::getCurrent(); }
RwTexDictionary *RwTexDictionarySetCurrent(RwTexDictionary * dict) { TexDictionary::setCurrent(dict); return dict; }
const RwTexDictionary *RwTexDictionaryForAllTextures(const RwTexDictionary * dict, RwTextureCallBack fpCallBack, void *pData) {
	FORLIST(lnk, ((RwTexDictionary*)dict)->textures)
		if(fpCallBack(Texture::fromDict(lnk), pData) == nil)
			break;
	return dict;
}
RwBool RwTexDictionaryForAllTexDictionaries(RwTexDictionaryCallBack fpCallBack, void *pData);
RwInt32 RwTexDictionaryRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB, RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB);
RwInt32 RwTexDictionaryGetPluginOffset(RwUInt32 pluginID);
RwBool RwTexDictionaryValidatePlugins(const RwTexDictionary * dict);
RwUInt32 RwTexDictionaryStreamGetSize(const RwTexDictionary *texDict);
RwTexDictionary *RwTexDictionaryStreamRead(RwStream *stream);
const RwTexDictionary *RwTexDictionaryStreamWrite(const RwTexDictionary *texDict, RwStream *stream) {
	((RwTexDictionary*)texDict)->streamWrite(stream);
	return texDict;
}





RwStream *RwStreamOpen(RwStreamType type, RwStreamAccessType accessType, const void *pData) {
	StreamFile *file;
	StreamMemory *mem;
	RwMemory *memargs;
	const char *mode;

	switch(accessType){
	case rwSTREAMREAD: mode = "rb"; break;
	case rwSTREAMWRITE: mode = "wb"; break;
	case rwSTREAMAPPEND: mode = "ab"; break;
	default: return nil;
	}

	// oh god this is horrible. librw streams really need fixing
	switch(type){
	case rwSTREAMFILENAME:{
		StreamFile fakefile;
		file = rwNewT(StreamFile, 1, 0);
		memcpy(file, &fakefile, sizeof(StreamFile));
#ifndef _WIN32
		char *r = casepath((char*)pData);
		if (r) {
			if (file->open((char*)r, mode)) {
				free(r);
				return file;
			}
			free(r);
		} else
#endif
		{
			if (file->open((char*)pData, mode))
				return file;
		}
		rwFree(file);
		return nil;
	}
	case rwSTREAMMEMORY:{
		StreamMemory fakemem;
		memargs = (RwMemory*)pData;
		mem = rwNewT(StreamMemory, 1, 0);
		memcpy(mem, &fakemem, sizeof(StreamMemory));
		if(mem->open(memargs->start, memargs->length))
			return mem;
		rwFree(mem);
		return nil;
	}
	default:
		assert(0 && "unknown type");
		return nil;
	}
}
RwBool RwStreamClose(RwStream * stream, void *pData) { stream->close(); rwFree(stream); return true; }
RwUInt32 RwStreamRead(RwStream * stream, void *buffer, RwUInt32 length) { return stream->read8(buffer, length); }
RwStream *RwStreamWrite(RwStream * stream, const void *buffer, RwUInt32 length) { stream->write8(buffer, length); return stream; }
RwStream *RwStreamSkip(RwStream * stream, RwUInt32 offset) { stream->seek(offset); return stream; }

RwBool RwStreamFindChunk(RwStream *stream, RwUInt32 type, RwUInt32 *lengthOut, RwUInt32 *versionOut)
	{ return findChunk(stream, type, lengthOut, versionOut); }



void RwIm2DVertexSetCameraX(RwIm2DVertex *vert, RwReal camx) { }
void RwIm2DVertexSetCameraY(RwIm2DVertex *vert, RwReal camy) { }
void RwIm2DVertexSetCameraZ(RwIm2DVertex *vert, RwReal camz) { vert->setCameraZ(camz); }
void RwIm2DVertexSetRecipCameraZ(RwIm2DVertex *vert, RwReal recipz) { vert->setRecipCameraZ(recipz); }
void RwIm2DVertexSetScreenX(RwIm2DVertex *vert, RwReal scrnx) { vert->setScreenX(scrnx); }
void RwIm2DVertexSetScreenY(RwIm2DVertex *vert, RwReal scrny) { vert->setScreenY(scrny); }
void RwIm2DVertexSetScreenZ(RwIm2DVertex *vert, RwReal scrnz) { vert->setScreenZ(scrnz); }
void RwIm2DVertexSetU(RwIm2DVertex *vert, RwReal texU, RwReal recipz) { vert->setU(texU, recipz); }
void RwIm2DVertexSetV(RwIm2DVertex *vert, RwReal texV, RwReal recipz) { vert->setV(texV, recipz); }
void RwIm2DVertexSetIntRGBA(RwIm2DVertex *vert, RwUInt8 red, RwUInt8 green, RwUInt8 blue, RwUInt8 alpha) { vert->setColor(red, green, blue, alpha); }

RwReal RwIm2DGetNearScreenZ(void) { return im2d::GetNearZ(); }
RwReal RwIm2DGetFarScreenZ(void) { return im2d::GetFarZ(); }
RwBool RwIm2DRenderLine(RwIm2DVertex *vertices, RwInt32 numVertices, RwInt32 vert1, RwInt32 vert2)
	{ im2d::RenderLine(vertices, numVertices, vert1, vert2); return true; }
RwBool RwIm2DRenderTriangle(RwIm2DVertex *vertices, RwInt32 numVertices, RwInt32 vert1, RwInt32 vert2, RwInt32 vert3 )
	{ im2d::RenderTriangle(vertices, numVertices, vert1, vert2, vert3); return true; }
RwBool RwIm2DRenderPrimitive(RwPrimitiveType primType, RwIm2DVertex *vertices, RwInt32 numVertices)
	{ im2d::RenderPrimitive((PrimitiveType)primType, vertices, numVertices); return true; }
RwBool RwIm2DRenderIndexedPrimitive(RwPrimitiveType primType, RwIm2DVertex *vertices, RwInt32 numVertices, RwImVertexIndex *indices, RwInt32 numIndices)
	{ im2d::RenderIndexedPrimitive((PrimitiveType)primType, vertices, numVertices, indices, numIndices); return true; }


void RwIm3DVertexSetPos(RwIm3DVertex *vert, RwReal x, RwReal y, RwReal z) {
    vert->setX(x); vert->setY(y); vert->setZ(z);
}
void RwIm3DVertexSetU(RwIm3DVertex *vert, RwReal u) { vert->setU(u); }
void RwIm3DVertexSetV(RwIm3DVertex *vert, RwReal v) { vert->setV(v); }
void RwIm3DVertexSetRGBA(RwIm3DVertex *vert, RwUInt8 r, RwUInt8 g, RwUInt8 b, RwUInt8 a) { vert->setColor(r, g, b, a); }

void  *RwIm3DTransform(RwIm3DVertex *pVerts, RwUInt32 numVerts, RwMatrix *ltm, RwUInt32 flags) { im3d::Transform(pVerts, numVerts, ltm, flags); return pVerts; }
RwBool RwIm3DEnd(void) { im3d::End(); return true; }
RwBool RwIm3DRenderLine(RwInt32 vert1, RwInt32 vert2) {
	RwImVertexIndex indices[2];
	indices[0] = vert1;
	indices[1] = vert2;
	im3d::RenderIndexedPrimitive((PrimitiveType)PRIMTYPELINELIST, indices, 2);
	return true;
}
RwBool RwIm3DRenderTriangle(RwInt32 vert1, RwInt32 vert2, RwInt32 vert3);
RwBool RwIm3DRenderIndexedPrimitive(RwPrimitiveType primType, RwImVertexIndex *indices, RwInt32 numIndices) { im3d::RenderIndexedPrimitive((PrimitiveType)primType, indices, numIndices); return true; }
RwBool RwIm3DRenderPrimitive(RwPrimitiveType primType);





RwBool RwRenderStateGet(RwRenderState state, void *value)
{
	uint32 *uival = (uint32*)value;
	uint32 fog;
	switch(state){
	case rwRENDERSTATETEXTURERASTER: *(void**)value = GetRenderStatePtr(TEXTURERASTER); return true;
	case rwRENDERSTATETEXTUREADDRESS: *uival = GetRenderState(TEXTUREADDRESS); return true;
	case rwRENDERSTATETEXTUREADDRESSU: *uival = GetRenderState(TEXTUREADDRESSU); return true;
	case rwRENDERSTATETEXTUREADDRESSV: *uival = GetRenderState(TEXTUREADDRESSV); return true;
	case rwRENDERSTATETEXTUREPERSPECTIVE: *uival = 1; return true;
	case rwRENDERSTATEZTESTENABLE: *uival = GetRenderState(ZTESTENABLE); return true;
	case rwRENDERSTATESHADEMODE: *uival = rwSHADEMODEGOURAUD; return true;
	case rwRENDERSTATEZWRITEENABLE: *uival = GetRenderState(ZWRITEENABLE); return true;
	case rwRENDERSTATETEXTUREFILTER: *uival = GetRenderState(TEXTUREFILTER); return true;
	case rwRENDERSTATESRCBLEND: *uival = GetRenderState(SRCBLEND); return true;
	case rwRENDERSTATEDESTBLEND: *uival = GetRenderState(DESTBLEND); return true;
	case rwRENDERSTATEVERTEXALPHAENABLE: *uival = GetRenderState(VERTEXALPHA); return true;
	case rwRENDERSTATEBORDERCOLOR: *uival = 0; return true;
	case rwRENDERSTATEFOGENABLE: *uival = GetRenderState(FOGENABLE); return true;
	case rwRENDERSTATEFOGCOLOR:
		// have to swap R and B here
		fog = GetRenderState(FOGCOLOR);
		*uival = (fog>>16)&0xFF;
		*uival |= (fog&0xFF)<<16;
		*uival |= fog&0xFF00;
		*uival |= fog&0xFF000000;
		return true;
	case rwRENDERSTATEFOGTYPE: *uival = rwFOGTYPELINEAR; return true;
	case rwRENDERSTATEFOGDENSITY: *(float*)value = 1.0f; return true;
	case rwRENDERSTATECULLMODE: *uival = GetRenderState(CULLMODE); return true;

	// all unsupported
	case rwRENDERSTATEFOGTABLE:
	case rwRENDERSTATEALPHAPRIMITIVEBUFFER:

	case rwRENDERSTATESTENCILENABLE:
	case rwRENDERSTATESTENCILFAIL:
	case rwRENDERSTATESTENCILZFAIL:
	case rwRENDERSTATESTENCILPASS:
	case rwRENDERSTATESTENCILFUNCTION:
	case rwRENDERSTATESTENCILFUNCTIONREF:
	case rwRENDERSTATESTENCILFUNCTIONMASK:
	case rwRENDERSTATESTENCILFUNCTIONWRITEMASK:
	default:
		return false;
	}
}
RwBool RwRenderStateSet(RwRenderState state, void *value)
{
	uint32 uival = (uintptr)value;
	uint32 fog;
	switch(state){
	case rwRENDERSTATETEXTURERASTER: SetRenderStatePtr(TEXTURERASTER, value); return true;
	case rwRENDERSTATETEXTUREADDRESS: SetRenderState(TEXTUREADDRESS, uival); return true;
	case rwRENDERSTATETEXTUREADDRESSU: SetRenderState(TEXTUREADDRESSU, uival); return true;
	case rwRENDERSTATETEXTUREADDRESSV: SetRenderState(TEXTUREADDRESSV, uival); return true;
	case rwRENDERSTATETEXTUREPERSPECTIVE: return true;
	case rwRENDERSTATEZTESTENABLE: SetRenderState(ZTESTENABLE, uival); return true;
	case rwRENDERSTATESHADEMODE: return true;
	case rwRENDERSTATEZWRITEENABLE: SetRenderState(ZWRITEENABLE, uival); return true;
	case rwRENDERSTATETEXTUREFILTER: SetRenderState(TEXTUREFILTER, uival); return true;
	case rwRENDERSTATESRCBLEND: SetRenderState(SRCBLEND, uival); return true;
	case rwRENDERSTATEDESTBLEND: SetRenderState(DESTBLEND, uival); return true;
	case rwRENDERSTATEVERTEXALPHAENABLE: SetRenderState(VERTEXALPHA, uival); return true;
	case rwRENDERSTATEBORDERCOLOR: return true;
	case rwRENDERSTATEFOGENABLE: SetRenderState(FOGENABLE, uival); return true;
	case rwRENDERSTATEFOGCOLOR:
		// have to swap R and B here
		fog = (uival>>16)&0xFF;
		fog |= (uival&0xFF)<<16;
		fog |= uival&0xFF00;
		fog |= uival&0xFF000000;
		SetRenderState(FOGCOLOR, fog);
		return true;
	case rwRENDERSTATEFOGTYPE: return true;
	case rwRENDERSTATEFOGDENSITY: return true;
	case rwRENDERSTATEFOGTABLE: return true;
	case rwRENDERSTATEALPHAPRIMITIVEBUFFER: return true;
	case rwRENDERSTATECULLMODE: SetRenderState(CULLMODE, uival); return true;

	// all unsupported
	case rwRENDERSTATESTENCILENABLE:
	case rwRENDERSTATESTENCILFAIL:
	case rwRENDERSTATESTENCILZFAIL:
	case rwRENDERSTATESTENCILPASS:
	case rwRENDERSTATESTENCILFUNCTION:
	case rwRENDERSTATESTENCILFUNCTIONREF:
	case rwRENDERSTATESTENCILFUNCTIONMASK:
	case rwRENDERSTATESTENCILFUNCTIONWRITEMASK:
	default:
		return true;
	}
}

static rw::MemoryFunctions gMemfuncs;
static void *(*real_malloc)(size_t size);
static void *(*real_realloc)(void *mem, size_t newSize);
#ifdef WII
static uint32 sRwMem2OverflowCount;
static uint32 sRwMem2UiDirectCount;
#endif
static void *mallocWrap(size_t sz, uint32 hint)
{
	if(sz == 0)
		return nil;
#ifdef WII
	bool triedMem2 = ShouldRouteRwAllocToMem2(sz, hint);
	if(triedMem2){
		void *mem = MemoryMgrMallocMem2Strict(sz, 32);
		if(mem){
			if(IsCriticalUiRwAlloc(sz)){
				sRwMem2UiDirectCount++;
				if(sRwMem2UiDirectCount <= 8 ||
				   (sRwMem2UiDirectCount & (sRwMem2UiDirectCount - 1)) == 0)
					printf("[WII-MEM] rwMalloc UI direct generic MEM2 size=%u hint=0x%08X count=%u\n",
					       (unsigned)sz, (unsigned)hint,
					       (unsigned)sRwMem2UiDirectCount);
			}
			RecordRwResidentPool(mem, hint);
			return mem;
		}
	}
#endif
	void *mem = real_malloc(sz);
#ifdef WII
	if(mem == nil && !triedMem2){
		mem = MemoryMgrMallocMem2Strict(sz, 32);
		if(mem){
			sRwMem2OverflowCount++;
			if(sRwMem2OverflowCount <= 8 ||
			   (sRwMem2OverflowCount & (sRwMem2OverflowCount - 1)) == 0)
				printf("[WII-MEM] rwMalloc overflow used generic MEM2 size=%u hint=0x%08X count=%u\n",
				       (unsigned)sz, (unsigned)hint,
				       (unsigned)sRwMem2OverflowCount);
		}
	}
	RecordRwResidentPool(mem, hint);
#endif
	return mem;
}
static void *reallocWrap(void *p, size_t sz, uint32 hint)
{
#ifdef WII
	if(p == nil)
		return mallocWrap(sz, hint);

	bool ownsMem2 = WiiMemoryOwnsGenericMem2(p);
	bool routeToMem2 = ShouldRouteRwAllocToMem2(sz, hint);
	void *mem = nil;
	if(routeToMem2){
		mem = MemoryMgrReallocMem2Strict(p, sz, 32);
		if(mem == nil && !ownsMem2)
			mem = real_realloc(p, sz);
	}else if(ownsMem2)
		mem = MemoryMgrRealloc(p, sz);
	else
		mem = real_realloc(p, sz);
	RecordRwResidentPool(mem, hint);
	return mem;
#else
	return real_realloc(p, sz);
#endif
}


// WARNING: unused parameters
RwBool RwEngineInit(RwMemoryFunctions *memFuncs, RwUInt32 initFlags, RwUInt32 resArenaSize) {
	if(memFuncs){
		real_malloc = memFuncs->rwmalloc;
		real_realloc = memFuncs->rwrealloc;
		gMemfuncs.rwmalloc = mallocWrap;
		gMemfuncs.rwrealloc = reallocWrap;
		gMemfuncs.rwfree = memFuncs->rwfree;
		Engine::init(&gMemfuncs);
	}else{
		Engine::init(nil);
	}
	return true;
}
// TODO: this is platform dependent
RwBool RwEngineOpen(RwEngineOpenParams *initparams)
{
#if defined(RW_NULL) || (defined(GAMECUBE) && !defined(WII))
    // GameCube 鍜?NULL 璁惧涓嶉渶瑕佺獥鍙ｅ彞鏌勶紝鐩存帴浼?nil 鍚姩寮曟搸
    return rw::Engine::open(nil);
#else
    return rw::Engine::open((rw::EngineOpenParams*)initparams);
#endif
}
RwBool RwEngineStart(void) {
	rw::d3d::isP8supported = false;
	return Engine::start();
}
RwBool RwEngineStop(void) { Engine::stop(); return true; }
RwBool RwEngineClose(void) { Engine::close(); return true; }
RwBool RwEngineTerm(void) { Engine::term(); return true; }
RwInt32 RwEngineRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor initCB, RwPluginObjectDestructor termCB);
RwInt32 RwEngineGetPluginOffset(RwUInt32 pluginID);
RwInt32 RwEngineGetNumSubSystems(void) { return Engine::getNumSubSystems(); }
RwSubSystemInfo *RwEngineGetSubSystemInfo(RwSubSystemInfo *subSystemInfo, RwInt32 subSystemIndex)
	{ return Engine::getSubSystemInfo(subSystemInfo, subSystemIndex); }
RwInt32 RwEngineGetCurrentSubSystem(void) { return Engine::getCurrentSubSystem(); }
RwBool RwEngineSetSubSystem(RwInt32 subSystemIndex) { return Engine::setSubSystem(subSystemIndex); }
RwInt32 RwEngineGetNumVideoModes(void) { return Engine::getNumVideoModes(); }
RwVideoMode *RwEngineGetVideoModeInfo(RwVideoMode *modeinfo, RwInt32 modeIndex)
	{ return Engine::getVideoModeInfo(modeinfo, modeIndex); }
RwInt32 RwEngineGetCurrentVideoMode(void) { return Engine::getCurrentVideoMode(); }
RwBool RwEngineSetVideoMode(RwInt32 modeIndex) { return Engine::setVideoMode(modeIndex); }
RwInt32 RwEngineGetTextureMemorySize(void);
RwInt32 RwEngineGetMaxTextureSize(void);



// TODO
void RwD3D8EngineSetRefreshRate(RwUInt32 refreshRate) {}
RwBool RwD3D8DeviceSupportsDXTTexture(void) { return true; }


void RwD3D8EngineSetMultiSamplingLevels(RwUInt32 level) { Engine::setMultiSamplingLevels(level); }
RwUInt32 RwD3D8EngineGetMaxMultiSamplingLevels(void) { return Engine::getMaxMultiSamplingLevels(); }


RpMaterial *RpMaterialCreate(void) { return Material::create(); }
RwBool RpMaterialDestroy(RpMaterial *material) { material->destroy(); return true; }
//RpMaterial *RpMaterialClone(RpMaterial *material);
RpMaterial *RpMaterialSetTexture(RpMaterial *material, RwTexture *texture) { material->setTexture(texture); return material; }
//RpMaterial *RpMaterialAddRef(RpMaterial *material);
RwTexture *RpMaterialGetTexture(const RpMaterial *material) { return material->texture; }
RpMaterial *RpMaterialSetColor(RpMaterial *material, const RwRGBA *color) { material->color = *color; return material; }
const RwRGBA *RpMaterialGetColor(const RpMaterial *material) { return &material->color; }
RpMaterial *RpMaterialSetSurfaceProperties(RpMaterial *material, const RwSurfaceProperties *surfaceProperties);
const RwSurfaceProperties *RpMaterialGetSurfaceProperties(const RpMaterial *material) { return &material->surfaceProps; }
//RwInt32 RpMaterialRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB, RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB);
//RwInt32 RpMaterialRegisterPluginStream(RwUInt32 pluginID, RwPluginDataChunkReadCallBack readCB, RwPluginDataChunkWriteCallBack writeCB, RwPluginDataChunkGetSizeCallBack getSizeCB);
//RwInt32 RpMaterialSetStreamAlwaysCallBack(RwUInt32 pluginID, RwPluginDataChunkAlwaysCallBack alwaysCB);
//RwInt32 RpMaterialGetPluginOffset(RwUInt32 pluginID);
//RwBool RpMaterialValidatePlugins(const RpMaterial *material);
//RwUInt32 RpMaterialStreamGetSize(const RpMaterial *material);
//RpMaterial *RpMaterialStreamRead(RwStream *stream);
//const RpMaterial *RpMaterialStreamWrite(const RpMaterial *material, RwStream *stream);
//RpMaterialChunkInfo *_rpMaterialChunkInfoRead(RwStream *stream,  RpMaterialChunkInfo *materialChunkInfo, RwInt32 *bytesRead);





RwReal RpLightGetRadius(const RpLight *light) { return light->radius; }
//const RwRGBAReal *RpLightGetColor(const RpLight *light);
RpLight *RpLightSetFrame(RpLight *light, RwFrame *frame) { light->setFrame(frame); return light; }
RwFrame *RpLightGetFrame(const RpLight *light) { return light->getFrame(); }
//RpLightType RpLightGetType(const RpLight *light);
RpLight *RpLightSetFlags(RpLight *light, RwUInt32 flags) { light->setFlags(flags); return light; }
//RwUInt32 RpLightGetFlags(const RpLight *light);
RpLight *RpLightCreate(RwInt32 type) { return rw::Light::create(type); }
RwBool RpLightDestroy(RpLight *light) { light->destroy(); return true; }
RpLight *RpLightSetRadius(RpLight *light, RwReal radius) { light->radius = radius; return light; }
RpLight *RpLightSetColor(RpLight *light, const RwRGBAReal *color) { light->setColor(color->red, color->green, color->blue); return light; }
//RwReal RpLightGetConeAngle(const RpLight *light);
//RpLight *RpLightSetConeAngle(RpLight * ight, RwReal angle);
//RwUInt32 RpLightStreamGetSize(const RpLight *light);
//RpLight *RpLightStreamRead(RwStream *stream);
//const RpLight *RpLightStreamWrite(const RpLight *light, RwStream *stream);
//RpLightChunkInfo *_rpLightChunkInfoRead(RwStream *stream, RpLightChunkInfo *lightChunkInfo, RwInt32 *bytesRead);
//RwInt32 RpLightRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB, RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB);
//RwInt32 RpLightRegisterPluginStream(RwUInt32 pluginID, RwPluginDataChunkReadCallBack readCB, RwPluginDataChunkWriteCallBack writeCB, RwPluginDataChunkGetSizeCallBack getSizeCB);
//RwInt32 RpLightSetStreamAlwaysCallBack(RwUInt32 pluginID, RwPluginDataChunkAlwaysCallBack alwaysCB);
//RwInt32 RpLightGetPluginOffset(RwUInt32 pluginID);
//RwBool RpLightValidatePlugins(const RpLight * light);





RpGeometry  *RpGeometryCreate(RwInt32 numVert, RwInt32 numTriangles, RwUInt32 format) { return Geometry::create(numVert, numTriangles, format); }
RwBool RpGeometryDestroy(RpGeometry *geometry) { geometry->destroy(); return true; }
RpGeometry *_rpGeometryAddRef(RpGeometry *geometry);
RpGeometry  *RpGeometryLock(RpGeometry *geometry, RwInt32 lockMode) { geometry->lock(lockMode); return geometry; }
RpGeometry  *RpGeometryUnlock(RpGeometry *geometry) { geometry->unlock(); return geometry; }
RpGeometry  *RpGeometryTransform(RpGeometry *geometry, const RwMatrix *matrix);
RpGeometry  *RpGeometryCreateSpace(RwReal radius);
RpMorphTarget  *RpMorphTargetSetBoundingSphere(RpMorphTarget *morphTarget, const RwSphere *boundingSphere) { morphTarget->boundingSphere = *boundingSphere; return morphTarget; }
RwSphere  *RpMorphTargetGetBoundingSphere(RpMorphTarget *morphTarget) { return &morphTarget->boundingSphere; }
const RpMorphTarget  *RpMorphTargetCalcBoundingSphere(const RpMorphTarget *morphTarget, RwSphere *boundingSphere) { *boundingSphere = morphTarget->calculateBoundingSphere(); return morphTarget; }
RwInt32 RpGeometryAddMorphTargets(RpGeometry *geometry, RwInt32 mtcount) { RwInt32 n = geometry->numMorphTargets; geometry->addMorphTargets(mtcount); return n; }
RwInt32 RpGeometryAddMorphTarget(RpGeometry *geometry) { return RpGeometryAddMorphTargets(geometry, 1); }
RpGeometry  *RpGeometryRemoveMorphTarget(RpGeometry *geometry, RwInt32 morphTarget);
RwInt32 RpGeometryGetNumMorphTargets(const RpGeometry *geometry);
RpMorphTarget  *RpGeometryGetMorphTarget(const RpGeometry *geometry, RwInt32 morphTarget) { return &geometry->morphTargets[morphTarget]; }
RwRGBA  *RpGeometryGetPreLightColors(const RpGeometry *geometry) { return geometry->colors; }
RwTexCoords  *RpGeometryGetVertexTexCoords(const RpGeometry *geometry, RwTextureCoordinateIndex uvIndex) {
	if(uvIndex == rwNARWTEXTURECOORDINATEINDEX)
		return nil;
	return geometry->texCoords[uvIndex-rwTEXTURECOORDINATEINDEX0];
}
RwInt32 RpGeometryGetNumTexCoordSets(const RpGeometry *geometry) { return geometry->numTexCoordSets; }
RwInt32 RpGeometryGetNumVertices (const RpGeometry *geometry) { return geometry->numVertices; }
RwV3d  *RpMorphTargetGetVertices(const RpMorphTarget *morphTarget) { return morphTarget->vertices; }
RwV3d  *RpMorphTargetGetVertexNormals(const RpMorphTarget *morphTarget) { return morphTarget->normals; }
RpTriangle  *RpGeometryGetTriangles(const RpGeometry *geometry) { return geometry->triangles; }
RwInt32 RpGeometryGetNumTriangles(const RpGeometry *geometry) { return geometry->numTriangles; }
RpMaterial  *RpGeometryGetMaterial(const RpGeometry *geometry, RwInt32 matNum) { return geometry->matList.materials[matNum]; }
const RpGeometry  *RpGeometryTriangleSetVertexIndices(const RpGeometry *geometry, RpTriangle *triangle, RwUInt16 vert1, RwUInt16 vert2, RwUInt16 vert3)
	{ triangle->v[0] = vert1; triangle->v[1] = vert2; triangle->v[2] = vert3; return geometry; }
RpGeometry  *RpGeometryTriangleSetMaterial(RpGeometry *geometry, RpTriangle *triangle, RpMaterial *material) {
	int id = geometry->matList.findIndex(material);
	if(id < 0)
		id = geometry->matList.appendMaterial(material);
	if(id < 0)
		return nil;
	triangle->matId = id;
	return geometry;
}
const RpGeometry  *RpGeometryTriangleGetVertexIndices(const RpGeometry *geometry, const RpTriangle *triangle, RwUInt16 *vert1, RwUInt16 *vert2, RwUInt16 *vert3);
RpMaterial   *RpGeometryTriangleGetMaterial(const RpGeometry *geometry, const RpTriangle *triangle);
RwInt32 RpGeometryGetNumMaterials(const RpGeometry *geometry);
RpGeometry  *RpGeometryForAllMaterials(RpGeometry *geometry, RpMaterialCallBack fpCallBack, void *pData) {
	int i;
	for(i = 0; i < geometry->matList.numMaterials; i++)
		if(fpCallBack(geometry->matList.materials[i], pData) == nil)
			break;
	return geometry;
}
//const RpGeometry  *RpGeometryForAllMeshes(const RpGeometry *geometry, RpMeshCallBack fpCallBack, void *pData);
RwInt32 RpGeometryRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB, RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB);
RwInt32 RpGeometryRegisterPluginStream(RwUInt32 pluginID, RwPluginDataChunkReadCallBack readCB, RwPluginDataChunkWriteCallBack writeCB, RwPluginDataChunkGetSizeCallBack getSizeCB);
RwInt32 RpGeometrySetStreamAlwaysCallBack(RwUInt32 pluginID, RwPluginDataChunkAlwaysCallBack alwaysCB);
RwInt32 RpGeometryGetPluginOffset(RwUInt32 pluginID);
RwBool RpGeometryValidatePlugins(const RpGeometry *geometry);
RwUInt32 RpGeometryStreamGetSize(const RpGeometry *geometry);
const RpGeometry  *RpGeometryStreamWrite(const RpGeometry *geometry, RwStream *stream);
RpGeometry  *RpGeometryStreamRead(RwStream *stream) { return Geometry::streamRead(stream); }
//RpGeometryChunkInfo *_rpGeometryChunkInfoRead(RwStream *stream, RpGeometryChunkInfo *geometryChunkInfo, RwInt32 *bytesRead);
RwUInt32 RpGeometryGetFlags(const RpGeometry *geometry) { return geometry->flags; }
RpGeometry  *RpGeometrySetFlags(RpGeometry *geometry, RwUInt32 flags) { geometry->flags = flags; return geometry; }
const RwSurfaceProperties *_rpGeometryGetSurfaceProperties(const RpGeometry *geometry);
RpGeometry *_rpGeometrySetSurfaceProperties(RpGeometry *geometry, const RwSurfaceProperties *surfaceProperties);





RwFrame *RpClumpGetFrame(const RpClump * clump) { return clump->getFrame(); }
RpClump *RpClumpSetFrame(RpClump * clump, RwFrame * frame) { clump->setFrame(frame); return clump; }
RpClump *RpClumpForAllAtomics(RpClump * clump, RpAtomicCallBack callback, void *pData) {
	FORLIST(lnk, clump->atomics)
		if(callback(Atomic::fromClump(lnk), pData) == nil)
			break;
	return clump;
}
RpClump *RpClumpForAllLights(RpClump * clump, RpLightCallBack callback, void *pData);
RpClump *RpClumpForAllCameras(RpClump * clump, RwCameraCallBack callback, void *pData);
//RpClump *RpClumpCreateSpace(const RwV3d * position, RwReal radius);
RpClump *RpClumpRender(RpClump * clump) { clump->render(); return clump; }
RpClump *RpClumpRemoveAtomic(RpClump * clump, RpAtomic * atomic) { clump->removeAtomic(atomic); return clump; }
RpClump *RpClumpAddAtomic(RpClump * clump, RpAtomic * atomic) { clump->addAtomic(atomic); return clump; }
//RpClump *RpClumpRemoveLight(RpClump * clump, RpLight * light);
//RpClump *RpClumpAddLight(RpClump * clump, RpLight * light);
//RpClump *RpClumpRemoveCamera(RpClump * clump, RwCamera * camera);
//RpClump *RpClumpAddCamera(RpClump * clump, RwCamera * camera);
RwBool RpClumpDestroy(RpClump * clump) { clump->destroy(); return true; }
RpClump *RpClumpCreate(void) { return rw::Clump::create(); }
RpClump *RpClumpClone(RpClump * clump) { return clump->clone(); }
//RpClump *RpClumpSetCallBack(RpClump * clump, RpClumpCallBack callback);
//RpClumpCallBack RpClumpGetCallBack(const RpClump * clump);
RwInt32 RpClumpGetNumAtomics(RpClump * clump) { return clump->countAtomics(); }
//RwInt32 RpClumpGetNumLights(RpClump * clump);
//RwInt32 RpClumpGetNumCameras(RpClump * clump);
RpClump *RpClumpStreamRead(RwStream * stream) { return rw::Clump::streamRead(stream); }
//RpClump *RpClumpStreamWrite(RpClump * clump, RwStream * stream);
RwInt32 RpClumpRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB, RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB)
	{ return Clump::registerPlugin(size, pluginID, constructCB, destructCB, (CopyConstructor)copyCB); }
RwInt32 RpClumpRegisterPluginStream(RwUInt32 pluginID, RwPluginDataChunkReadCallBack  readCB, RwPluginDataChunkWriteCallBack writeCB, RwPluginDataChunkGetSizeCallBack getSizeCB)
	{ return Clump::registerPluginStream(pluginID, readCB, (StreamWrite)writeCB, (StreamGetSize)getSizeCB); }
//RwInt32 RpClumpSetStreamAlwaysCallBack(RwUInt32 pluginID, RwPluginDataChunkAlwaysCallBack alwaysCB);
//RwInt32 RpClumpGetPluginOffset(RwUInt32 pluginID);
//RwBool RpClumpValidatePlugins(const RpClump * clump);



RpAtomic *RpAtomicCreate(void) { return rw::Atomic::create(); }
RwBool RpAtomicDestroy(RpAtomic * atomic) { atomic->destroy(); return true; }
RpAtomic *RpAtomicClone(RpAtomic * atomic) { return atomic->clone(); }
RpAtomic *RpAtomicSetFrame(RpAtomic * atomic, RwFrame * frame) { atomic->setFrame(frame); return atomic; }
RpAtomic *RpAtomicSetGeometry(RpAtomic * atomic, RpGeometry * geometry, RwUInt32 flags) { atomic->setGeometry(geometry, flags); return atomic; }

RwFrame *RpAtomicGetFrame(const RpAtomic * atomic) { return atomic->getFrame(); }
RpAtomic *RpAtomicSetFlags(RpAtomic * atomic, RwUInt32 flags) { atomic->setFlags(flags); return atomic; }
RwUInt32 RpAtomicGetFlags(const RpAtomic * atomic) { return atomic->getFlags(); }
RwSphere *RpAtomicGetBoundingSphere(RpAtomic * atomic) { return &atomic->boundingSphere; }
RpAtomic *RpAtomicRender(RpAtomic * atomic) { atomic->render(); return atomic; }
RpClump *RpAtomicGetClump(const RpAtomic * atomic) { return atomic->clump; }
//RpInterpolator *RpAtomicGetInterpolator(RpAtomic * atomic);
RpGeometry *RpAtomicGetGeometry(const RpAtomic * atomic) { return atomic->geometry; }
// WARNING: illegal cast
void RpAtomicSetRenderCallBack(RpAtomic * atomic, RpAtomicCallBackRender callback) { atomic->setRenderCB((Atomic::RenderCB)callback); }
RpAtomicCallBackRender RpAtomicGetRenderCallBack(const RpAtomic * atomic) { return (RpAtomicCallBackRender)atomic->renderCB; }
//RwBool RpAtomicInstance(RpAtomic *atomic);
//RwUInt32 RpAtomicStreamGetSize(RpAtomic * atomic);
//RpAtomic *RpAtomicStreamRead(RwStream * stream);
//RpAtomic *RpAtomicStreamWrite(RpAtomic * atomic, RwStream * stream);
RwInt32 RpAtomicRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB, RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB)
	{ return Atomic::registerPlugin(size, pluginID, constructCB, destructCB, (CopyConstructor)copyCB); }
//RwInt32 RpAtomicRegisterPluginStream(RwUInt32 pluginID, RwPluginDataChunkReadCallBack readCB, RwPluginDataChunkWriteCallBack writeCB, RwPluginDataChunkGetSizeCallBack getSizeCB);
//RwInt32 RpAtomicSetStreamAlwaysCallBack(RwUInt32 pluginID, RwPluginDataChunkAlwaysCallBack alwaysCB);
//RwInt32 RpAtomicSetStreamRightsCallBack(RwUInt32 pluginID, RwPluginDataChunkRightsCallBack rightsCB);
//RwInt32 RpAtomicGetPluginOffset(RwUInt32 pluginID);
//RwBool RpAtomicValidatePlugins(const RpAtomic * atomic);

static char
fakeLowerAscii(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool
fakeNameContainsNoCase(const char *name, const char *needle)
{
	if(name == nil || needle == nil || needle[0] == '\0')
		return false;

	for(const char *p = name; *p; p++){
		const char *a = p;
		const char *b = needle;
		while(*a && *b && fakeLowerAscii(*a) == fakeLowerAscii(*b)){
			a++;
			b++;
		}
		if(*b == '\0')
			return true;
	}
	return false;
}

static bool
fakeFocusTextureName(const char *name)
{
	return fakeNameContainsNoCase(name, "grass") ||
	       fakeNameContainsNoCase(name, "hedge") ||
	       fakeNameContainsNoCase(name, "leaf") ||
	       fakeNameContainsNoCase(name, "palm") ||
	       fakeNameContainsNoCase(name, "tree") ||
	       fakeNameContainsNoCase(name, "plant") ||
	       fakeNameContainsNoCase(name, "foliage") ||
	       fakeNameContainsNoCase(name, "hotel") ||
	       fakeNameContainsNoCase(name, "lobby") ||
	       strncmp(name, "htl_", 4) == 0 ||
	       strncmp(name, "ht_", 3) == 0 ||
	       strncmp(name, "hot_", 4) == 0 ||
	       strncmp(name, "mob_", 4) == 0 ||
	       strncmp(name, "nt_wall", 7) == 0 ||
	       strncmp(name, "nt_floor", 8) == 0 ||
	       strncmp(name, "nt_woodwall", 11) == 0 ||
	       strcmp(name, "planta256") == 0 ||
	       strcmp(name, "plantb256") == 0 ||
	       strcmp(name, "plantc256") == 0 ||
	       strcmp(name, "fuzzyplant256") == 0 ||
	       strcmp(name, "kbplanter_plants1") == 0 ||
	       strcmp(name, "Bow_church_grass_gen") == 0 ||
	       strcmp(name, "Bow_grass_gryard") == 0;
}

static bool
fakeIsKnownHardAlphaVegetationTexture(const char *name)
{
	if(name == nil || name[0] == '\0')
		return false;

	return strcmp(name, "kbtree4_test") == 0 ||
	       strcmp(name, "foliage256") == 0 ||
	       strcmp(name, "newtreeleaves128") == 0 ||
	       strcmp(name, "newtreeleavesb128") == 0 ||
	       strcmp(name, "planta256") == 0 ||
	       strcmp(name, "plantb256") == 0 ||
	       strcmp(name, "plantc256") == 0 ||
	       strcmp(name, "fuzzyplant256") == 0 ||
	       strcmp(name, "kbplanter_plants1") == 0 ||
	       strcmp(name, "weepalmshadow") == 0 ||
	       strcmp(name, "bigpalmshadow") == 0;
}

static bool
fakeIsVegetationShadowTexture(const char *name)
{
	if(name == nil || name[0] == '\0')
		return false;

	return strcmp(name, "weepalmshadow") == 0 ||
	       strcmp(name, "bigpalmshadow") == 0;
}

static bool
fakeIsKnownSoftBlendVegetationTexture(const char *name)
{
	(void)name;
	return false;
}

static bool
fakeIsKnownEdgeBlendCutoutVegetationTexture(const char *name)
{
	if(name == nil || name[0] == '\0')
		return false;

	return strcmp(name, "kbtree4_test") == 0 ||
	       strcmp(name, "foliage256") == 0 ||
	       strcmp(name, "newtreeleaves128") == 0 ||
	       strcmp(name, "newtreeleavesb128") == 0 ||
	       strcmp(name, "planta256") == 0 ||
	       strcmp(name, "plantb256") == 0 ||
	       strcmp(name, "plantc256") == 0 ||
	       strcmp(name, "fuzzyplant256") == 0 ||
	       strcmp(name, "kbplanter_plants1") == 0;
}

static bool
fakeIsLikelyThinTwoSidedTexture(const char *name)
{
	return fakeNameContainsNoCase(name, "rotor") ||
	       fakeNameContainsNoCase(name, "propell") ||
	       fakeNameContainsNoCase(name, "blade") ||
	       fakeNameContainsNoCase(name, "fan");
}

static bool
fakePreferCutoutTextureAlpha(const char *name)
{
	if(name == nil)
		return false;

	if(fakeIsKnownSoftBlendVegetationTexture(name))
		return false;

	return fakeNameContainsNoCase(name, "fence") ||
	       fakeNameContainsNoCase(name, "mesh") ||
	       fakeNameContainsNoCase(name, "wire") ||
	       fakeNameContainsNoCase(name, "grate") ||
	       fakeNameContainsNoCase(name, "grill") ||
	       fakeNameContainsNoCase(name, "gate") ||
	       fakeNameContainsNoCase(name, "chain") ||
	       fakeNameContainsNoCase(name, "ivy") ||
	       fakeNameContainsNoCase(name, "leaf") ||
	       fakeNameContainsNoCase(name, "bush") ||
	       fakeNameContainsNoCase(name, "tree") ||
	       fakeNameContainsNoCase(name, "palm") ||
	       fakeNameContainsNoCase(name, "plant") ||
	       fakeNameContainsNoCase(name, "grass") ||
	       fakeNameContainsNoCase(name, "hedge") ||
	       fakeNameContainsNoCase(name, "foliage") ||
	       fakeNameContainsNoCase(name, "fern") ||
	       fakeNameContainsNoCase(name, "frond") ||
	       fakeNameContainsNoCase(name, "weed") ||
	       fakeNameContainsNoCase(name, "sign") ||
	       fakeIsLikelyThinTwoSidedTexture(name);
}

static bool32
fakeMeshHasVertexAlpha(rw::Geometry *geo, const uint16 *meshIdx, uint32 numIndices)
{
	if(geo == nil || geo->colors == nil || (geo->flags & rw::Geometry::PRELIT) == 0)
		return 0;

	for(uint32 i = 0; i < numIndices; i++){
		uint16 vi = meshIdx[i];
		if(vi >= geo->numVertices)
			continue;
		if(geo->colors[vi].alpha != 255)
			return 1;
	}
	return 0;
}

static uint8
fakeGxAlphaFuncFromState(int32 f)
{
	switch(f){
	case rw::ALPHAALWAYS:       return GX_ALWAYS;
	case rw::ALPHAGREATEREQUAL: return GX_GEQUAL;
	case rw::ALPHALESS:         return GX_LESS;
	default:                   return GX_ALWAYS;
	}
}

static const char*
fakeFindFocusedAtomicTexture(rw::Atomic *atomic)
{
	if(atomic == nil || atomic->geometry == nil)
		return nil;

	rw::Geometry *geo = atomic->geometry;
	if(geo->meshHeader){
		rw::Mesh *mesh = geo->meshHeader->getMeshes();
		for(uint32 i = 0; i < geo->meshHeader->numMeshes; i++){
			rw::Material *mat = mesh[i].material;
			if(mat && mat->texture && fakeFocusTextureName(mat->texture->name))
				return mat->texture->name;
		}
	}

	for(int32 i = 0; i < geo->matList.numMaterials; i++){
		rw::Material *mat = geo->matList.materials[i];
		if(mat && mat->texture && fakeFocusTextureName(mat->texture->name))
			return mat->texture->name;
	}
	return nil;
}

RpAtomic *AtomicDefaultRenderCallBack(RpAtomic * atomic)
{
#ifdef WII
#if 0 // [ATOMIC-FOCUS]/[ATOMIC-ALPHA] render diagnostics disabled for normal runs.
	static int s_focusAtomicRenderLogCount = 0;
	const char *focusTex = fakeFindFocusedAtomicTexture(atomic);
	if(focusTex && s_focusAtomicRenderLogCount < 160){
		rw::ObjPipeline *pipe = atomic ? atomic->getPipeline() : nil;
		rw::Geometry *geo = atomic ? atomic->geometry : nil;
		rw::Raster *focusRaster = nil;
		rw::Material *focusMaterial = nil;
		rw::Mesh *focusMesh = nil;
		uint32 focusMeshIndex = 0xFFFFFFFFu;
		uint32 focusRasterPlat = 999u;
		uint32 gxFmt = 999u;
		uint32 gxHasAlpha = 999u;
		uint32 gxTexObj = 999u;
		uint32 d3dFmt = 999u;
		uint32 d3dCustom = 999u;
		uint32 d3dHasAlpha = 999u;
		if(geo && geo->meshHeader){
			rw::Mesh *mesh = geo->meshHeader->getMeshes();
			for(uint32 i = 0; i < geo->meshHeader->numMeshes; i++){
				rw::Material *mat = mesh[i].material;
				if(mat && mat->texture && mat->texture->name &&
				   strcmp(mat->texture->name, focusTex) == 0){
					focusMaterial = mat;
					focusMesh = &mesh[i];
					focusMeshIndex = i;
					focusRaster = mat->texture->raster;
					break;
				}
			}
		}
		if(focusRaster == nil && geo){
			for(int32 i = 0; i < geo->matList.numMaterials; i++){
				rw::Material *mat = geo->matList.materials[i];
				if(mat && mat->texture && mat->texture->name &&
				   strcmp(mat->texture->name, focusTex) == 0){
					focusMaterial = mat;
					focusRaster = mat->texture->raster;
					break;
				}
			}
		}
		if(focusRaster){
			focusRasterPlat = (uint32)focusRaster->platform;
#ifdef RW_GX
			if(focusRaster->platform == PLATFORM_GX){
				rw::gx::GxRaster *gxras =
					PLUGINOFFSET(rw::gx::GxRaster, focusRaster, rw::gx::nativeRasterOffset);
				if(gxras){
					gxFmt = gxras->gxFmt;
					gxHasAlpha = gxras->hasAlpha ? 1u : 0u;
					gxTexObj = gxras->texObjValid ? 1u : 0u;
				}
			}
#endif
#ifdef WITH_D3D
			if(focusRaster->platform == PLATFORM_D3D8 ||
			   focusRaster->platform == PLATFORM_D3D9){
				rw::d3d::D3dRaster *d3dras = GETD3DRASTEREXT(focusRaster);
				if(d3dras){
					d3dFmt = d3dras->format;
					d3dCustom = d3dras->customFormat ? 1u : 0u;
					d3dHasAlpha = d3dras->hasAlpha ? 1u : 0u;
				}
			}
#endif
		}
		printf("[ATOMIC-FOCUS] tex=%s atomic=%p pipe=%p pPlat=%u pId=%u pData=%u renderCB=%p pInst=%p pRender=%p pUninst=%p geo=%p gflags=0x%x meshes=%u mats=%d tPlat=%u gxFmt=%u gxA=%u gxObj=%u d3dFmt=0x%08X d3dCustom=%u d3dA=%u\n",
		       focusTex ? focusTex : "none",
		       (void*)atomic,
		       (void*)pipe,
		       pipe ? (unsigned)pipe->platform : 999u,
		       pipe ? (unsigned)pipe->pluginID : 999u,
		       pipe ? (unsigned)pipe->pluginData : 999u,
		       atomic ? (void*)atomic->renderCB : nil,
		       pipe ? (void*)pipe->impl.instance : nil,
		       pipe ? (void*)pipe->impl.render : nil,
		       pipe ? (void*)pipe->impl.uninstance : nil,
		       (void*)geo,
		       geo ? (unsigned)geo->flags : 0u,
		       (geo && geo->meshHeader) ? (unsigned)geo->meshHeader->numMeshes : 0u,
		       geo ? geo->matList.numMaterials : 0,
		       focusRasterPlat,
		       gxFmt,
		       gxHasAlpha,
		       gxTexObj,
		       d3dFmt,
		       d3dCustom,
		       d3dHasAlpha);
		if(focusMaterial){
			uint32 matAlpha = focusMaterial->color.alpha;
			bool forceKnownAlpha = fakeIsKnownHardAlphaVegetationTexture(focusTex);
			bool muteShadowAtlas = fakeIsVegetationShadowTexture(focusTex);
			bool hasMatAlpha = false;
			bool hasTexAlpha = false;
			bool hasVertexColors = geo && geo->colors &&
			                      (geo->flags & rw::Geometry::PRELIT) != 0;
			bool meshVertexAlpha = false;
			bool preferTexCutout = false;
			bool preferEdgeBlendCutout = false;
			bool doBlend = false;
			bool doAlphaTest = false;
			uint32 numIndices = focusMesh ? focusMesh->numIndices : 0u;
			uint8 effectiveAlphaFunc = GX_ALWAYS;
			uint8 effectiveAlphaRef = 0;

			// Mirror gxpipe.cpp setMaterial exactly so this diagnostic matches
			// the real draw. Note: hard-alpha vegetation no longer forces
			// matAlpha=255 -- fade (matA<255) now coexists with the alpha test.
			if(muteShadowAtlas)
				matAlpha = 0;
			hasMatAlpha = matAlpha != 255;

			if(focusRaster){
				if(focusRaster->platform == PLATFORM_GX){
					hasTexAlpha = gxHasAlpha == 1u;
				}else{
					hasTexAlpha = rw::Raster::formatHasAlpha(focusRaster->format);
				}
			}
			if(forceKnownAlpha && !hasTexAlpha)
				hasTexAlpha = true;

			if(focusMesh && geo)
				meshVertexAlpha = fakeMeshHasVertexAlpha(geo, focusMesh->indices,
				                                        focusMesh->numIndices) != 0;

			preferTexCutout = hasTexAlpha && fakePreferCutoutTextureAlpha(focusTex);
			preferEdgeBlendCutout = hasTexAlpha &&
			                        fakeIsKnownEdgeBlendCutoutVegetationTexture(focusTex);
			// Mirror gxpipe.cpp: all draw-alpha sources enable both states.
			bool usesAlpha = hasTexAlpha || forceKnownAlpha ||
			                 hasMatAlpha || meshVertexAlpha;
			doBlend = usesAlpha;
			doAlphaTest = usesAlpha;

			if(doAlphaTest){
				effectiveAlphaFunc = fakeGxAlphaFuncFromState(rw::gx::gxState.alphaTestFunc);
				effectiveAlphaRef = (uint8)rw::gx::gxState.alphaTestRef;
				if(effectiveAlphaRef < 10)
					effectiveAlphaRef = 10;
			}

			printf("[ATOMIC-ALPHA] tex=%s mesh=%u numIdx=%u fmt=0x%02X texObj=%u matA=%u texA=%u vtxA=%u prelit=%u blend=%u alphaTest=%u edgeBlend=%u prefCut=%u forceA=%u shadowMute=%u gAFn=%d gARef=%d effAFn=%u effARef=%u\n",
			       focusTex ? focusTex : "none",
			       focusMeshIndex,
			       numIndices,
			       gxFmt,
			       gxTexObj,
			       matAlpha,
			       hasTexAlpha ? 1u : 0u,
			       meshVertexAlpha ? 1u : 0u,
			       hasVertexColors ? 1u : 0u,
			       doBlend ? 1u : 0u,
			       doAlphaTest ? 1u : 0u,
			       preferEdgeBlendCutout ? 1u : 0u,
			       preferTexCutout ? 1u : 0u,
			       forceKnownAlpha ? 1u : 0u,
			       muteShadowAtlas ? 1u : 0u,
			       (int)rw::gx::gxState.alphaTestFunc,
			       (int)rw::gx::gxState.alphaTestRef,
			       (unsigned)effectiveAlphaFunc,
			       (unsigned)effectiveAlphaRef);
		}
		s_focusAtomicRenderLogCount++;
	}
#endif
#endif
	Atomic::defaultRenderCB(atomic);
	return atomic;
}


// TODO: this is extremely simplified
RpWorld     *RpWorldCreate(RwBBox * boundingBox) { return World::create(); }
RwBool       RpWorldDestroy(RpWorld * world) { world->destroy(); return true; }

RwBool       RpWorldPluginAttach(void) {
	registerMeshPlugin();
	registerNativeDataPlugin();
	registerAtomicRightsPlugin();
	registerMaterialRightsPlugin();

	// not sure if this goes here
	rw::xbox::registerVertexFormatPlugin();
	return true;
}

RpWorld *RpWorldRemoveCamera(RpWorld *world, RwCamera *camera) { world->removeCamera(camera); return world; }
RpWorld *RpWorldAddCamera(RpWorld *world, RwCamera *camera) { world->addCamera(camera); return world; }
RpWorld *RwCameraGetWorld(const RwCamera *camera);
RpWorld *RpWorldRemoveAtomic(RpWorld *world, RpAtomic *atomic);
RpWorld *RpWorldAddAtomic(RpWorld *world, RpAtomic *atomic);
RpWorld *RpAtomicGetWorld(const RpAtomic *atomic);
RpWorld *RpWorldAddClump(RpWorld *world, RpClump *clump);
RpWorld *RpWorldRemoveClump(RpWorld *world, RpClump *clump);
RpWorld *RpClumpGetWorld(const RpClump *clump);
RpWorld *RpWorldAddLight(RpWorld *world, RpLight *light) { world->addLight(light); return world; }
RpWorld *RpWorldRemoveLight(RpWorld *world, RpLight *light) { world->removeLight(light); return world; }
RpWorld *RpLightGetWorld(const RpLight *light);
RwCamera *RwCameraForAllClumpsInFrustum(RwCamera *camera, void *data);
RwCamera *RwCameraForAllClumpsNotInFrustum(RwCamera *camera, RwInt32 numClumps, void *data);




RwBool RpMatFXPluginAttach( void ) { registerMatFXPlugin(); return true; }
RpAtomic *RpMatFXAtomicEnableEffects( RpAtomic *atomic ) { MatFX::enableEffects(atomic); return atomic; }
RpMatFXMaterialFlags RpMatFXMaterialGetEffects( const RpMaterial *material ){ return (RpMatFXMaterialFlags)MatFX::getEffects(material); }
RpMaterial *RpMatFXMaterialSetEffects( RpMaterial *material, RpMatFXMaterialFlags flags ) { MatFX::setEffects(material, (uint32)flags); return material; }
RpMaterial *RpMatFXMaterialSetupEnvMap( RpMaterial *material, RwTexture *texture, RwFrame *frame, RwBool useFrameBufferAlpha, RwReal coef ) {
	MatFX *mfx = MatFX::get(material);
	mfx->setEnvTexture(texture);
	mfx->setEnvFrame(frame);
	mfx->setEnvCoefficient(coef);
	return material;
}
RpMaterial *RpMatFXMaterialSetEnvMapFrame( RpMaterial *material, RwFrame *frame )
{
	MatFX *mfx = MatFX::get(material);
	mfx->setEnvFrame(frame);
	return material;
}
RpMaterial *RpMatFXMaterialSetEnvMapFrameBufferAlpha( RpMaterial *material, RwBool useFrameBufferAlpha )
{
	MatFX *mfx = MatFX::get(material);
	mfx->setEnvFBAlpha(useFrameBufferAlpha);
	return material;
}
RpMaterial *RpMatFXMaterialSetEnvMapCoefficient( RpMaterial *material, RwReal coef )
{
	MatFX *mfx = MatFX::get(material);
	mfx->setEnvCoefficient(coef);
	return material;
}
RwReal RpMatFXMaterialGetEnvMapCoefficient( const RpMaterial *material )
{
	MatFX *mfx = MatFX::get(material);
	return mfx->getEnvCoefficient();
}



RwBool RpHAnimPluginAttach(void) {
	registerHAnimPlugin();
	return true;
}

RwInt32 RpHAnimFrameGetID(RwFrame *frame) { return HAnimData::get(frame)->id; }

RwInt32 RpHAnimIDGetIndex(RpHAnimHierarchy *hierarchy, RwInt32 ID) { return hierarchy->getIndex(ID); }

RwBool RpHAnimFrameSetHierarchy(RwFrame *frame, RpHAnimHierarchy *hierarchy) { HAnimData::get(frame)->hierarchy = hierarchy; return true; }
RpHAnimHierarchy *RpHAnimFrameGetHierarchy(RwFrame *frame) { return HAnimHierarchy::get(frame); }

RpHAnimHierarchy *RpHAnimHierarchySetFlags(RpHAnimHierarchy *hierarchy, RpHAnimHierarchyFlag flags) { hierarchy->flags = flags; return hierarchy; }

RwBool RpHAnimHierarchySetCurrentAnim(RpHAnimHierarchy *hierarchy, RpHAnimAnimation *anim) { hierarchy->interpolator->setCurrentAnim(anim); return true; }
RwBool RpHAnimHierarchyAddAnimTime(RpHAnimHierarchy *hierarchy, RwReal time) { hierarchy->interpolator->addTime(time); return true; }

RwMatrix *RpHAnimHierarchyGetMatrixArray(RpHAnimHierarchy *hierarchy) { return hierarchy->matrices; }
RwBool RpHAnimHierarchyUpdateMatrices(RpHAnimHierarchy *hierarchy) { hierarchy->updateMatrices(); return true; }

RpHAnimAnimation *RpHAnimAnimationCreate(RwInt32 typeID, RwInt32 numFrames, RwInt32 flags, RwReal duration)
	{ return Animation::create(AnimInterpolatorInfo::find(typeID), numFrames, flags, duration); }
RpHAnimAnimation  *RpHAnimAnimationDestroy(RpHAnimAnimation *animation) { animation->destroy(); return animation; }
RpHAnimAnimation  *RpHAnimAnimationStreamRead(RwStream *stream) { return Animation::streamRead(stream); }






RwBool RpSkinPluginAttach(void) {
	registerSkinPlugin();
	return true;
}

RwUInt32 RpSkinGetNumBones( RpSkin *skin ) { return skin->numBones; }
const RwMatrixWeights *RpSkinGetVertexBoneWeights( RpSkin *skin ) { return (RwMatrixWeights*)skin->weights; }
const RwUInt32 *RpSkinGetVertexBoneIndices( RpSkin *skin ) { return (RwUInt32*)skin->indices; }
const RwMatrix *RpSkinGetSkinToBoneMatrices( RpSkin *skin ) { return (const RwMatrix*)skin->inverseMatrices; }

RpSkin *RpSkinGeometryGetSkin( RpGeometry *geometry ) { return Skin::get(geometry); }

RpAtomic *RpSkinAtomicSetHAnimHierarchy( RpAtomic *atomic, RpHAnimHierarchy *hierarchy ) { Skin::setHierarchy(atomic, hierarchy); return atomic; }
RpHAnimHierarchy *RpSkinAtomicGetHAnimHierarchy( const RpAtomic *atomic ) { return Skin::getHierarchy(atomic); }

RwImage *
RtBMPImageWrite(RwImage *image, const RwChar *imageName)
{
#ifndef _WIN32
	char *r = casepath(imageName);
	if (r) {
		rw::writeBMP(image, r);
		free(r);
	} else {
		rw::writeBMP(image, imageName);
	}
	
#else
	rw::writeBMP(image, imageName);
#endif
	return image;
}
RwImage *
RtBMPImageRead(const RwChar *imageName)
{
#ifndef _WIN32
	RwImage *image;
	char *r = casepath(imageName);
	if (r) {
		image = rw::readBMP(r);
		free(r);
	} else {
		image = rw::readBMP(imageName);
	}
	return image;

#else
	return rw::readBMP(imageName);
#endif
}


RwImage *
RtPNGImageWrite(RwImage *image, const RwChar *imageName)
{
#ifndef _WIN32
	char *r = casepath(imageName);
	if (r) {
		rw::writePNG(image, r);
		free(r);
	} else {
		rw::writePNG(image, imageName);
	}
	
#else
	rw::writePNG(image, imageName);
#endif
	return image;
}
RwImage *
RtPNGImageRead(const RwChar *imageName)
{
#ifndef _WIN32
	RwImage *image;
	char *r = casepath(imageName);
	if (r) {
		image = rw::readPNG(r);
		free(r);
	} else {
		image = rw::readPNG(imageName);
	}
	return image;

#else
	return rw::readPNG(imageName);
#endif
}

#include "rtquat.h"

RtQuat *RtQuatRotate(RtQuat * quat, const RwV3d * axis, RwReal angle, RwOpCombineType combineOp) { return (RtQuat*)((rw::Quat*)quat)->rotate(axis, angle/180.0f*3.14159f, (CombineOp)combineOp); }
void RtQuatConvertToMatrix(const RtQuat * const qpQuat, RwMatrix * const mpMatrix) { mpMatrix->rotate(*(rw::Quat*)qpQuat, COMBINEREPLACE); }


#include "rtcharse.h"

RwBool       RtCharsetOpen(void) { return Charset::open(); }
void         RtCharsetClose(void) { return Charset::close(); }
RtCharset   *RtCharsetPrint(RtCharset * charSet, const RwChar * string, RwInt32 x, RwInt32 y) { charSet->print(string, x, y, true); return charSet; }
RtCharset   *RtCharsetPrintBuffered(RtCharset * charSet, const RwChar * string, RwInt32 x, RwInt32 y, RwBool hideSpaces) { charSet->printBuffered(string, x, y, hideSpaces); return charSet; }
RwBool       RtCharsetBufferFlush(void) { Charset::flushBuffer(); return true; }
RtCharset   *RtCharsetSetColors(RtCharset * charSet, const RwRGBA * foreGround, const RwRGBA * backGround) { return charSet->setColors(foreGround, backGround); }
RtCharset   *RtCharsetGetDesc(RtCharset * charset, RtCharsetDesc * desc) { *desc = charset->desc; return charset; }
RtCharset   *RtCharsetCreate(const RwRGBA * foreGround, const RwRGBA * backGround) { return Charset::create(foreGround, backGround); }
RwBool       RtCharsetDestroy(RtCharset * charSet) { if(charSet) charSet->destroy(); return true; }



#include <rpanisot.h>

RwInt8      RpAnisotGetMaxSupportedMaxAnisotropy(void) { return rw::getMaxSupportedMaxAnisotropy(); }
RwTexture    *RpAnisotTextureSetMaxAnisotropy(RwTexture *tex, RwInt8 val) { tex->setMaxAnisotropy(val); return tex; }
RwInt8       RpAnisotTextureGetMaxAnisotropy(RwTexture *tex) { return tex->getMaxAnisotropy(); }
RwBool       RpAnisotPluginAttach(void) { rw::registerAnisotropyPlugin(); return true; }
