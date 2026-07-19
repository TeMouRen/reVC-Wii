#include "common.h"
#include "crossplatform.h"

#if defined(WII)

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <gccore.h>
#include <ogc/es.h>
#include <ogc/ipc.h>
#include <ogc/isfs.h>

#include "wii_save.h"

namespace {

static bool gWiiSaveInitAttempted = false;
static bool gWiiSaveInitOk = false;
static bool gWiiSaveBannerReady = false;
static u64 gWiiSaveTitleId = 0;
static char gWiiSaveDataDir[ISFS_MAXPATH];
static const s32 kWiiIsfsErrorNoExists = -106;

struct WiiSaveFsStats {
	u32 blockSize;
	u32 freeBlocks;
	u32 occupiedBlocks;
	u32 badBlocks;
	u32 reservedBlocks;
	u32 freeInodes;
	u32 occupiedInodes;
};

struct WiiBannerFile {
	u32 signature;
	u32 flag;
	u16 iconSpeed;
	u8 reserved[22];
	u16 title[32];
	u16 subtitle[32];
	u8 bannerTexture[192 * 64 * 2];
	u8 iconTexture[48 * 48 * 2];
};

static_assert(sizeof(WiiBannerFile) == 0x72A0, "Unexpected Wii save banner size");

static u16
WiiSavePackRgb5A3(uint8 r, uint8 g, uint8 b, uint8 a = 255)
{
	if (a >= 224)
		return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
	return ((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
}

static void
WiiSaveStoreUtf16(const char *text, u16 *dst, size_t capacity)
{
	size_t i;

	for (i = 0; i < capacity; i++)
		dst[i] = 0;
	for (i = 0; i + 1 < capacity && text[i] != '\0'; i++)
		dst[i] = (u8)text[i];
}

static void
WiiSaveSampleBannerPixel(int x, int y, uint8 &r, uint8 &g, uint8 &b)
{
	const int skyTopR = 36;
	const int skyTopG = 131;
	const int skyTopB = 160;
	const int skyBottomR = 255;
	const int skyBottomG = 141;
	const int skyBottomB = 117;
	const int horizon = 44;
	const int waterLine = 50;

	if (y >= waterLine) {
		int mix = (y - waterLine) * 255 / Max(1, 63 - waterLine);
		r = (uint8)((14 * (255 - mix) + 74 * mix) / 255);
		g = (uint8)((56 * (255 - mix) + 22 * mix) / 255);
		b = (uint8)((82 * (255 - mix) + 73 * mix) / 255);
		if (((x / 6) + (y / 2)) & 1) {
			r = Min(255, r + 8);
			g = Min(255, g + 6);
			b = Min(255, b + 6);
		}
		return;
	}

	int mix = y * 255 / Max(1, waterLine - 1);
	r = (uint8)((skyTopR * (255 - mix) + skyBottomR * mix) / 255);
	g = (uint8)((skyTopG * (255 - mix) + skyBottomG * mix) / 255);
	b = (uint8)((skyTopB * (255 - mix) + skyBottomB * mix) / 255);

	int dx = x - 96;
	int dy = y - horizon;
	int dist2 = dx * dx + dy * dy;
	if (dist2 < 28 * 28) {
		int glow = (28 * 28 - dist2) * 160 / (28 * 28);
		r = Min(255, r + glow);
		g = Min(255, g + glow / 2);
	}

	if (x > 132 && y > 10 && y < 60) {
		int stripe = (x - 132) + (y * 2);
		if ((stripe / 6) & 1) {
			r = Min(255, r + 22);
			g = Min(255, g + 8);
			b = Max(0, b - 10);
		}
	}
}

static void
WiiSaveSampleIconPixel(int x, int y, uint8 &r, uint8 &g, uint8 &b)
{
	int cx = x - 24;
	int cy = y - 24;
	int dist2 = cx * cx + cy * cy;

	r = 28;
	g = 86;
	b = 112;

	if (dist2 < 20 * 20) {
		int glow = (20 * 20 - dist2) * 190 / (20 * 20);
		r = Min(255, 220 + glow / 5);
		g = Min(255, 108 + glow / 3);
		b = Min(255, 124 + glow / 6);
	}

	if ((x > 10 && x < 17) || (x > 31 && x < 38)) {
		if (y > 13 && y < 40) {
			r = 16;
			g = 48;
			b = 56;
		}
	}
}

static void
WiiSaveEncodeTextureRGB5A3(uint8 *dst, int width, int height,
	void (*sample)(int x, int y, uint8 &r, uint8 &g, uint8 &b))
{
	u16 *pixels = (u16*)dst;

	for (int blockY = 0; blockY < height; blockY += 4) {
		for (int blockX = 0; blockX < width; blockX += 4) {
			for (int y = 0; y < 4; y++) {
				for (int x = 0; x < 4; x++) {
					uint8 r, g, b;
					sample(blockX + x, blockY + y, r, g, b);
					*pixels++ = WiiSavePackRgb5A3(r, g, b);
				}
			}
		}
	}
}

static void
WiiSaveSetIconSpeed(u16 &iconSpeed, int index, int speed)
{
	const u16 mask = (u16)(3u << (index * 2));
	iconSpeed = (u16)((iconSpeed & ~mask) | ((speed & 3) << (index * 2)));
}

static bool
WiiSavePathExists(const char *path)
{
	u32 count = 0;
	if (ISFS_ReadDir(path, nil, &count) >= 0)
		return true;
	s32 fd = ISFS_Open(path, ISFS_OPEN_READ);
	if (fd < 0)
		return false;
	ISFS_Close(fd);
	return true;
}

static bool
WiiSaveEnsureDirectory(const char *path)
{
	char partial[ISFS_MAXPATH];
	size_t len;

	if (path == nil || path[0] != '/')
		return false;
	if (WiiSavePathExists(path))
		return true;

	len = strlen(path);
	if (len >= sizeof(partial))
		return false;

	memset(partial, 0, sizeof(partial));
	for (size_t i = 1; i < len; i++) {
		if (path[i] != '/')
			continue;
		memcpy(partial, path, i);
		partial[i] = '\0';
		if (partial[0] != '\0' && !WiiSavePathExists(partial))
			ISFS_CreateDir(partial, 0, 3, 3, 3);
	}

	if (!WiiSavePathExists(path))
		ISFS_CreateDir(path, 0, 3, 3, 3);
	return WiiSavePathExists(path);
}

static const char *
WiiSaveGetBaseName(const char *path)
{
	const char *slash = strrchr(path, '/');
	const char *backslash = strrchr(path, '\\');
	const char *base = path;

	if (slash != nil)
		base = slash + 1;
	if (backslash != nil && backslash + 1 > base)
		base = backslash + 1;
	return base;
}

static bool
WiiSaveWriteAll(s32 fd, const void *data, size_t size)
{
	const uint8 *ptr = (const uint8*)data;
	size_t written = 0;
	uint8 *bounce = nil;
	const bool aligned = (((u32)data) & 31) == 0;

	if (!aligned) {
		bounce = (uint8*)memalign(32, 0x2000);
		if (bounce == nil)
			return false;
	}

	while (written < size) {
		size_t chunk = Min((size_t)0x2000, size - written);
		const void *writePtr = ptr + written;
		if (!aligned) {
			memcpy(bounce, writePtr, chunk);
			writePtr = bounce;
		}
		s32 ret = ISFS_Write(fd, writePtr, chunk);
		if (ret <= 0)
		{
			if (bounce != nil)
				free(bounce);
			return false;
		}
		written += ret;
		if ((size_t)ret != chunk) {
			if (bounce != nil)
				free(bounce);
			return false;
		}
	}
	if (bounce != nil)
		free(bounce);
	return true;
}

static bool
WiiSaveCopyFile(const char *srcPath, const char *dstPath)
{
	alignas(32) uint8 buffer[0x2000];
	s32 srcFd;
	s32 dstFd;
	fstats stats;
	u32 remaining;

	srcFd = ISFS_Open(srcPath, ISFS_OPEN_READ);
	if (srcFd < 0) {
		SYS_Report("[reVC-WII] Copy open src failed: path=%s ret=%d\n", srcPath, srcFd);
		return false;
	}

	if (ISFS_GetFileStats(srcFd, &stats) < 0) {
		SYS_Report("[reVC-WII] Copy stat src failed: path=%s\n", srcPath);
		ISFS_Close(srcFd);
		return false;
	}

	s32 createRet = ISFS_CreateFile(dstPath, 0, 3, 3, 3);
	if (createRet < 0) {
		SYS_Report("[reVC-WII] Copy create dst failed: path=%s ret=%d\n", dstPath, createRet);
		ISFS_Close(srcFd);
		return false;
	}

	dstFd = ISFS_Open(dstPath, ISFS_OPEN_RW);
	if (dstFd < 0) {
		SYS_Report("[reVC-WII] Copy open dst failed: path=%s ret=%d\n", dstPath, dstFd);
		ISFS_Close(srcFd);
		return false;
	}

	remaining = stats.file_length;
	while (remaining > 0) {
		u32 chunk = Min((u32)sizeof(buffer), remaining);
		s32 readRet = ISFS_Read(srcFd, buffer, chunk);
		if (readRet < 0) {
			SYS_Report("[reVC-WII] Copy read failed: path=%s ret=%d\n", srcPath, readRet);
			ISFS_Close(dstFd);
			ISFS_Close(srcFd);
			return false;
		}
		if ((u32)readRet != chunk) {
			SYS_Report("[reVC-WII] Copy short read: path=%s got=%d expected=%u\n", srcPath, readRet, chunk);
			ISFS_Close(dstFd);
			ISFS_Close(srcFd);
			return false;
		}

		s32 writeRet = ISFS_Write(dstFd, buffer, chunk);
		if (writeRet < 0) {
			SYS_Report("[reVC-WII] Copy write failed: path=%s ret=%d\n", dstPath, writeRet);
			ISFS_Close(dstFd);
			ISFS_Close(srcFd);
			return false;
		}
		if ((u32)writeRet != chunk) {
			SYS_Report("[reVC-WII] Copy short write: path=%s got=%d expected=%u\n", dstPath, writeRet, chunk);
			ISFS_Close(dstFd);
			ISFS_Close(srcFd);
			return false;
		}

		remaining -= chunk;
	}

	ISFS_Close(dstFd);
	ISFS_Close(srcFd);
	return true;
}

static void
WiiSaveLogUsage(void)
{
	alignas(32) WiiSaveFsStats stats;
	u32 usedBlocks = 0;
	u32 usedInodes = 0;
	s32 usageRet;
	s32 statsRet;

	memset(&stats, 0, sizeof(stats));
	usageRet = ISFS_GetUsage(gWiiSaveDataDir, &usedBlocks, &usedInodes);
	statsRet = ISFS_GetStats(&stats);

	if (usageRet >= 0) {
		SYS_Report("[reVC-WII] Save usage: dir=%s blocks=%u inodes=%u\n",
			gWiiSaveDataDir, usedBlocks, usedInodes);
	} else {
		SYS_Report("[reVC-WII] Save usage query failed: %d\n", usageRet);
	}

	if (statsRet >= 0) {
		SYS_Report("[reVC-WII] NAND stats: blockSize=%u freeBlocks=%u occupiedBlocks=%u freeInodes=%u occupiedInodes=%u\n",
			stats.blockSize,
			stats.freeBlocks,
			stats.occupiedBlocks,
			stats.freeInodes,
			stats.occupiedInodes);
	} else {
		SYS_Report("[reVC-WII] NAND stats query failed: %d\n", statsRet);
	}
}

static bool
WiiSaveBuildBanner(WiiBannerFile &banner)
{
	memset(&banner, 0, sizeof(banner));
	banner.signature = 0x5749424Eu;
	WiiSaveStoreUtf16("Grand Theft Auto: Vice City", banner.title, sizeof(banner.title) / sizeof(banner.title[0]));
	WiiSaveStoreUtf16("Save Data", banner.subtitle, sizeof(banner.subtitle) / sizeof(banner.subtitle[0]));
	WiiSaveEncodeTextureRGB5A3(banner.bannerTexture, 192, 64, WiiSaveSampleBannerPixel);
	WiiSaveEncodeTextureRGB5A3(banner.iconTexture, 48, 48, WiiSaveSampleIconPixel);
	WiiSaveSetIconSpeed(banner.iconSpeed, 0, 2);
	WiiSaveSetIconSpeed(banner.iconSpeed, 1, 0);
	return true;
}

static bool
WiiSaveEnsureBannerInternal(void)
{
	char bannerPath[ISFS_MAXPATH];

	if (gWiiSaveBannerReady)
		return true;
	if (gWiiSaveDataDir[0] == '\0')
		return false;

	snprintf(bannerPath, sizeof(bannerPath), "%s/banner.bin", gWiiSaveDataDir);
	s32 fd = ISFS_Open(bannerPath, ISFS_OPEN_READ);
	if (fd >= 0) {
		alignas(32) fstats stats;
		bool valid = ISFS_GetFileStats(fd, &stats) >= 0 && stats.file_length == sizeof(WiiBannerFile);
		ISFS_Close(fd);
		if (valid) {
			gWiiSaveBannerReady = true;
			return true;
		}
	}

	WiiBannerFile banner;
	if (!WiiSaveBuildBanner(banner))
		return false;

	ISFS_Delete(bannerPath);
	ISFS_CreateFile(bannerPath, 0, 3, 3, 3);
	fd = ISFS_Open(bannerPath, ISFS_OPEN_RW);
	if (fd < 0)
		return false;

	bool ok = WiiSaveWriteAll(fd, &banner, sizeof(banner));
	ISFS_Close(fd);
	if (ok)
		gWiiSaveBannerReady = true;
	return ok;
}

} // namespace

bool
WiiSaveSystemInit(void)
{
	alignas(32) char dataDir[ISFS_MAXPATH];

	if (gWiiSaveInitAttempted) {
		if (gWiiSaveInitOk && !gWiiSaveBannerReady)
			WiiSaveEnsureBannerInternal();
		return gWiiSaveInitOk;
	}

	gWiiSaveInitAttempted = true;
	gWiiSaveInitOk = false;
	gWiiSaveDataDir[0] = '\0';

	s32 ret = __ES_Init();
	if (ret < 0) {
		SYS_Report("[reVC-WII] Save init failed: __ES_Init=%d\n", ret);
		return false;
	}

	ret = ISFS_Initialize();
	if (ret < 0) {
		SYS_Report("[reVC-WII] Save init failed: ISFS_Initialize=%d\n", ret);
		return false;
	}

	ret = ES_GetTitleID(&gWiiSaveTitleId);
	if (ret < 0) {
		SYS_Report("[reVC-WII] Save init failed: ES_GetTitleID=%d\n", ret);
		return false;
	}

	memset(dataDir, 0, sizeof(dataDir));
	ret = ES_GetDataDir(gWiiSaveTitleId, dataDir);
	if (ret < 0) {
		SYS_Report("[reVC-WII] Save init failed: ES_GetDataDir=%d\n", ret);
		return false;
	}

	strncpy(gWiiSaveDataDir, dataDir, sizeof(gWiiSaveDataDir) - 1);
	gWiiSaveDataDir[sizeof(gWiiSaveDataDir) - 1] = '\0';

	if (!WiiSaveEnsureDirectory(gWiiSaveDataDir)) {
		SYS_Report("[reVC-WII] Save init failed: unable to ensure data dir %s\n", gWiiSaveDataDir);
		return false;
	}

	gWiiSaveInitOk = true;

	if (!WiiSaveEnsureBannerInternal()) {
		SYS_Report("[reVC-WII] Save init warning: banner.bin unavailable in %s\n", gWiiSaveDataDir);
	}

	SYS_Report("[reVC-WII] Save data dir: %s (titleId=%08x%08x)\n",
		gWiiSaveDataDir,
		(u32)(gWiiSaveTitleId >> 32),
		(u32)gWiiSaveTitleId);
	WiiSaveLogUsage();

	return true;
}

const char *
WiiSaveGetDataDir(void)
{
	return WiiSaveSystemInit() ? gWiiSaveDataDir : "";
}

bool
WiiSavePathIsManaged(const char *path)
{
	return path != nil &&
		(strncmp(path, "/title/", 7) == 0 ||
		strncmp(path, "\\title\\", 7) == 0 ||
		strncmp(path, "/tmp/", 5) == 0 ||
		strncmp(path, "\\tmp\\", 5) == 0);
}

bool
WiiSaveIsNoExistsError(int ret)
{
	return ret == kWiiIsfsErrorNoExists;
}

bool
WiiSaveBuildTempPath(const char *finalPath, char *tempPath, size_t tempPathSize)
{
	char tempDir[ISFS_MAXPATH];
	const char *baseName;
	int written;

	if (finalPath == nil || tempPath == nil || tempPathSize == 0)
		return false;
	if (!WiiSaveSystemInit())
		return false;

	baseName = WiiSaveGetBaseName(finalPath);
	if (baseName == nil || baseName[0] == '\0')
		return false;
	if (strlen(baseName) > 12) {
		SYS_Report("[reVC-WII] Temp save path rejected: basename too long (%s)\n", baseName);
		return false;
	}

	if (!WiiSaveEnsureDirectory("/tmp") || !WiiSaveEnsureDirectory("/tmp/sys")) {
		SYS_Report("[reVC-WII] Temp save path failed: unable to ensure /tmp/sys\n");
		return false;
	}

	snprintf(tempDir, sizeof(tempDir), "/tmp/sys/%08x", (u32)gWiiSaveTitleId);
	if (!WiiSaveEnsureDirectory(tempDir)) {
		SYS_Report("[reVC-WII] Temp save path failed: unable to ensure %s\n", tempDir);
		return false;
	}

	written = snprintf(tempPath, tempPathSize, "%s/%s", tempDir, baseName);
	if (written < 0 || (size_t)written >= tempPathSize) {
		SYS_Report("[reVC-WII] Temp save path failed: path too long for %s\n", baseName);
		return false;
	}
	return true;
}

bool
WiiSaveDeleteFile(const char *path)
{
	if (!WiiSaveSystemInit())
		return false;
	s32 ret = ISFS_Delete(path);
	if (ret < 0 && !WiiSaveIsNoExistsError(ret))
		SYS_Report("[reVC-WII] ISFS_Delete failed: path=%s ret=%d\n", path, ret);
	return ret >= 0 || WiiSaveIsNoExistsError(ret);
}

bool
WiiSaveCommitTempFile(const char *tempPath, const char *finalPath)
{
	if (!WiiSaveSystemInit())
		return false;
	const bool hadFinal = WiiSavePathExists(finalPath);

	s32 ret = ISFS_Rename(tempPath, finalPath);
	if (ret >= 0)
		return true;
	SYS_Report("[reVC-WII] ISFS_Rename failed (temp->final): from=%s to=%s ret=%d\n",
		tempPath, finalPath, ret);

	if (!hadFinal && WiiSaveCopyFile(tempPath, finalPath)) {
		SYS_Report("[reVC-WII] Save commit fallback copy succeeded: from=%s to=%s\n",
			tempPath, finalPath);
		WiiSaveDeleteFile(tempPath);
		return true;
	}
	if (hadFinal) {
		SYS_Report("[reVC-WII] Save commit fallback skipped to preserve existing save: %s\n",
			finalPath);
	}
	return false;
}

#endif
