// ============================================================
// GameCube VFS 璺緞闀滃儚
//
// myfopen() 鏄枃浠朵綔鐢ㄥ煙鐨勯潤鎬佽嚜鐢卞嚱鏁帮紝鏃犳硶璁块棶
// CFileMgr 锟?private 鎴愬憳 ms_dirName锟?
// 鍥犳鍦ㄨ繖閲岀淮鎶や竴涓ā鍧楃骇鍓湰锛岀敱鎴愬憳鍑芥暟
// CFileMgr::ChangeDir / Initialise 璐熻矗鍚屾锟?
// 鏍煎紡锛氬凡瑙勮寖鍖栦负姝ｆ枩鏉狅紝锟?**鏃犲熬閮ㄦ枩锟?*
// 渚嬶細""  /  "DATA"  /  "DATA/MAPS"
// ============================================================
#if defined(WII) || defined(GAMECUBE)
static char s_gx_current_dir[256] = "";
#endif
#define _CRT_SECURE_NO_WARNINGS
#include <fcntl.h>
#ifdef _WIN32
#include <direct.h>
#endif
#if defined(WII)
#include <malloc.h>
#include <ogc/isfs.h>
#endif
#include "common.h"
#include "crossplatform.h"

#include "FileMgr.h"
#if defined(WII)
#include "wii_save.h"
#endif

const char *_psGetUserFilesFolder();

/*
 * Windows FILE is BROKEN for GTA.
 *
 * We need to support mapping between LF and CRLF for text files
 * but we do NOT want to end the file at the first sight of a SUB character.
 * So here is a simple implementation of a FILE interface that works like GTA expects.
 */

struct myFILE
{
	bool isText;
	FILE *file;
#if defined(WII)
	s32 isfsFd;
	int lastError;
	enum Backend {
		BACKEND_NONE,
		BACKEND_STDIO,
		BACKEND_ISFS
	} backend;
#endif
};

#define NUMFILES 20
static myFILE myfiles[NUMFILES];

#if defined(WII)
static size_t
align32(size_t size)
{
	return (size + 31) & ~((size_t)31);
}

static bool
isAbsoluteFsPath(const char *path)
{
	return path != nil &&
		(path[0] == '/' ||
		path[0] == '\\' ||
		(((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':'));
}

static bool
buildManagedPathFromCurrentDir(const char *filename, char *outPath, size_t outPathSize)
{
	int written;

	if (filename == nil || outPath == nil || outPathSize == 0)
		return false;
	if (s_gx_current_dir[0] == '\0' || !WiiSavePathIsManaged(s_gx_current_dir))
		return false;
	if (WiiSavePathIsManaged(filename) || isAbsoluteFsPath(filename))
		return false;

	written = snprintf(outPath, outPathSize, "%s/%s", s_gx_current_dir, filename);
	if (written < 0 || (size_t)written >= outPathSize) {
		SYS_Report("[reVC-WII] Managed path build failed: dir=%s file=%s\n",
			s_gx_current_dir, filename);
		return false;
	}

	for (size_t i = 0; outPath[i] != '\0'; i++) {
		if (outPath[i] == '\\')
			outPath[i] = '/';
	}

	return WiiSavePathIsManaged(outPath);
}
#endif

static size_t myfread(void *buf, size_t elt, size_t n, int fd);
static size_t myfwrite(void *buf, size_t elt, size_t n, int fd);


#if !defined(_WIN32)
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#define _getcwd getcwd

// Case-insensitivity on linux (from https://github.com/OneSadCookie/fcaseopen)
void mychdir(char const *path)
{
#if GX_CONSOLE
    // GameCube 锟?VFS 灞備娇鐢ㄧ粷瀵硅矾寰勶紝涓嶉渶瑕佺郴缁熺骇鍒殑 chdir锛岀洿鎺ヨ繑鍥炲嵆锟?
    return;
#elif defined(ANDROID)
	if(!path) {
        return;
    }
#endif
#if !GX_CONSOLE
	char* r = casepath(path, false);
    if (r) {
#if defined(ANDROID)
		char path[MAX_PATH];
		strcpy(path, CFileMgr::GetRootDirName());
		strcat(path, r);
        chdir(path);
#else
        chdir(r);
#endif
		free(r);
    } else {
        errno = ENOENT;
    }
#endif
}
#else
#define mychdir chdir
#endif

/* Force file to open as binary but remember if it was text mode */
static int
myfopen(const char *filename, const char *mode)
{
	int fd;
	char realmode[10], *p;

	for(fd = 1; fd < NUMFILES; fd++)
#if defined(WII)
		if(myfiles[fd].backend == myFILE::BACKEND_NONE)
#else
		if(myfiles[fd].file == nil)
#endif
			goto found;
	return 0;	// no free fd
found:
#if GX_CONSOLE
	myfiles[fd].isText = false;  // GX consoles never use byte-by-byte text mode
#else
	myfiles[fd].isText = strchr(mode, 'b') == nil;
#endif
	p = realmode;
	while(*mode)
		if(*mode != 't' && *mode != 'b')
			*p++ = *mode++;
		else
			mode++;
	*p++ = 'b';
	*p = '\0';

#if defined(WII)
	char managedPath[ISFS_MAXPATH];
	const char *effectiveFilename = filename;
	const bool shouldUseManagedCurrentDir =
		s_gx_current_dir[0] != '\0' &&
		WiiSavePathIsManaged(s_gx_current_dir) &&
		!WiiSavePathIsManaged(filename) &&
		!isAbsoluteFsPath(filename);
	if (buildManagedPathFromCurrentDir(filename, managedPath, sizeof(managedPath)))
		effectiveFilename = managedPath;
	else if (shouldUseManagedCurrentDir)
		return 0;

	if (WiiSavePathIsManaged(effectiveFilename)) {
		if (!WiiSaveSystemInit())
			return 0;

		myfiles[fd].file = nil;
		myfiles[fd].isfsFd = -1;
		myfiles[fd].lastError = 0;
		myfiles[fd].backend = myFILE::BACKEND_ISFS;
		myfiles[fd].isText = false;

		const bool wantsRead = strchr(realmode, 'r') != nil;
		const bool wantsWrite = strchr(realmode, 'w') != nil;
		const bool wantsAppend = strchr(realmode, 'a') != nil;
		const bool wantsPlus = strchr(realmode, '+') != nil;

		if (wantsWrite) {
			WiiSaveDeleteFile(effectiveFilename);
			s32 createRet = ISFS_CreateFile(effectiveFilename, 0, 3, 3, 3);
			if (createRet < 0) {
				SYS_Report("[reVC-WII] ISFS_CreateFile failed: path=%s ret=%d\n", effectiveFilename, createRet);
			}
			myfiles[fd].isfsFd = ISFS_Open(effectiveFilename, ISFS_OPEN_RW);
			if (myfiles[fd].isfsFd < 0) {
				SYS_Report("[reVC-WII] ISFS_Open failed for write: path=%s ret=%d\n", effectiveFilename, myfiles[fd].isfsFd);
				myfiles[fd].backend = myFILE::BACKEND_NONE;
				return 0;
			}
			s32 seekRet = ISFS_Seek(myfiles[fd].isfsFd, 0, SEEK_SET);
			if (seekRet < 0) {
				SYS_Report("[reVC-WII] ISFS_Seek failed after write-open: path=%s ret=%d\n", effectiveFilename, seekRet);
			}
			return fd;
		}

		if (wantsAppend) {
			myfiles[fd].isfsFd = ISFS_Open(effectiveFilename, ISFS_OPEN_RW);
			if (myfiles[fd].isfsFd < 0) {
				s32 createRet = ISFS_CreateFile(effectiveFilename, 0, 3, 3, 3);
				if (createRet < 0) {
					SYS_Report("[reVC-WII] ISFS_CreateFile failed for append: path=%s ret=%d\n", effectiveFilename, createRet);
				}
				myfiles[fd].isfsFd = ISFS_Open(effectiveFilename, ISFS_OPEN_RW);
			}
			if (myfiles[fd].isfsFd < 0) {
				SYS_Report("[reVC-WII] ISFS_Open failed for append: path=%s ret=%d\n", effectiveFilename, myfiles[fd].isfsFd);
				myfiles[fd].backend = myFILE::BACKEND_NONE;
				return 0;
			}
			if (ISFS_Seek(myfiles[fd].isfsFd, 0, SEEK_END) < 0) {
				SYS_Report("[reVC-WII] ISFS_Seek failed for append: path=%s\n", effectiveFilename);
				ISFS_Close(myfiles[fd].isfsFd);
				myfiles[fd].isfsFd = -1;
				myfiles[fd].backend = myFILE::BACKEND_NONE;
				return 0;
			}
			return fd;
		}

		u8 openMode = wantsRead && wantsPlus ? ISFS_OPEN_RW : (wantsRead ? ISFS_OPEN_READ : ISFS_OPEN_RW);
		myfiles[fd].isfsFd = ISFS_Open(effectiveFilename, openMode);
		if (myfiles[fd].isfsFd < 0) {
			if (!WiiSaveIsNoExistsError(myfiles[fd].isfsFd))
				SYS_Report("[reVC-WII] ISFS_Open failed: path=%s mode=%u ret=%d\n", effectiveFilename, openMode, myfiles[fd].isfsFd);
			myfiles[fd].backend = myFILE::BACKEND_NONE;
			return 0;
		}
		return fd;
	}
#endif

#if GX_CONSOLE
    // ----------------------------------------------------------
    // GameCube: 鎵嬪姩鎷兼帴 dvd:/ 缁濆璺緞
    //
    // s_gx_current_dir 锟?ChangeDir 缁存姢锛屽凡瑙勮寖鍖栵紝鏃犲熬閮ㄦ枩锟?
    // 绀轰緥锟?
    //   s_gx_current_dir = "DATA"
    //   filename         = "MAIN.SCM"  (锟?"maps\\main.ipl")
    //   锟?full            = "dvd:/DATA/MAIN.SCM"
    // ----------------------------------------------------------
    char full[512];

    if (s_gx_current_dir[0] != '\0') {
        snprintf(full, sizeof(full), "dvd:/%s/%s",
                 s_gx_current_dir, filename);
    } else {
        snprintf(full, sizeof(full), "dvd:/%s", filename);
    }

    // 瑙勮寖锟?filename 鑷韩鍙兘鎼哄甫鐨勫弽鏂滄潬锛堝 "models\gta3.img"锟?
    for (int i = 0; full[i] != '\0'; i++) {
        if (full[i] == '\\') full[i] = '/';
    }

    ((void)0); // [GX-DEBUG-DISABLED]

    // FST 灞傚凡澶у皬鍐欎笉鏁忔劅锛岀洿鎺ヨ皟鏍囧噯 fopen 鍗冲彲
    myfiles[fd].file = fopen(full, realmode);

#else
    myfiles[fd].file = fcaseopen(filename, realmode);
#endif

    if(myfiles[fd].file == nil)
        return 0;
#if defined(WII)
	myfiles[fd].backend = myFILE::BACKEND_STDIO;
	myfiles[fd].isfsFd = -1;
	myfiles[fd].lastError = 0;
#endif
    setvbuf(myfiles[fd].file, NULL, _IOFBF, 4096);  // Force 4KB buffering
    return fd;
}

static int
myfclose(int fd)
{
	int ret;
	assert(fd < NUMFILES);
#if defined(WII)
	if(myfiles[fd].backend == myFILE::BACKEND_ISFS && myfiles[fd].isfsFd >= 0){
		ret = ISFS_Close(myfiles[fd].isfsFd);
		if(ret < 0)
			SYS_Report("[reVC-WII] ISFS_Close failed: fd=%d ret=%d\n", myfiles[fd].isfsFd, ret);
		myfiles[fd].isfsFd = -1;
		myfiles[fd].backend = myFILE::BACKEND_NONE;
		myfiles[fd].lastError = 0;
		return ret;
	}
#endif
	if(myfiles[fd].file){
		ret = fclose(myfiles[fd].file);
		myfiles[fd].file = nil;
#if defined(WII)
		myfiles[fd].backend = myFILE::BACKEND_NONE;
		myfiles[fd].lastError = 0;
#endif
		return ret;
	}
	return EOF;
}

static int
myfgetc(int fd)
{
	int c;
#if defined(WII)
	if (myfiles[fd].backend == myFILE::BACKEND_ISFS) {
		unsigned char ch;
		if (myfread(&ch, 1, 1, fd) != 1)
			return EOF;
		return ch;
	}
#endif
	c = fgetc(myfiles[fd].file);
	if(myfiles[fd].isText && c == 015){
		/* translate CRLF to LF */
		c = fgetc(myfiles[fd].file);
		if(c == 012)
			return c;
		ungetc(c, myfiles[fd].file);
		return 015;
	}
	return c;
}

static int
myfputc(int c, int fd)
{
	/* translate LF to CRLF */
#if defined(WII)
	if (myfiles[fd].backend == myFILE::BACKEND_ISFS) {
		unsigned char ch = (unsigned char)c;
		return myfwrite(&ch, 1, 1, fd) == 1 ? ch : EOF;
	}
#endif
	if(myfiles[fd].isText && c == 012)
		fputc(015, myfiles[fd].file);
	return fputc(c, myfiles[fd].file);
}

static char*
myfgets(char *buf, int len, int fd)
{
	int c;
	char *p;

	p = buf;
	len--;	// NUL byte
	while(len--){
		c = myfgetc(fd);
		if(c == EOF){
			if(p == buf)
				return nil;
			break;
		}
		*p++ = c;
		if(c == '\n')
			break;
	}
	*p = '\0';
	return buf;
}

static size_t
myfread(void *buf, size_t elt, size_t n, int fd)
{
#if defined(WII)
	if(myfiles[fd].backend == myFILE::BACKEND_ISFS){
		size_t total, done;
		uint8 *dst;
		uint8 *bounce;
		const bool directRead = (((u32)buf) & 31) == 0;

		if(myfiles[fd].isText){
			unsigned char *p;
			size_t i;
			int c;

			n *= elt;
			p = (unsigned char*)buf;
			for(i = 0; i < n; i++){
				c = myfgetc(fd);
				if(c == EOF)
					break;
				*p++ = (unsigned char)c;
			}
			return i / elt;
		}

		total = elt * n;
		if(total == 0)
			return 0;

		myfiles[fd].lastError = 0;
		bounce = nil;
		if(!directRead){
			bounce = (uint8*)memalign(32, align32(Min((size_t)0x2000, total)));
			if(bounce == nil){
				myfiles[fd].lastError = ISFS_ENOMEM;
				return 0;
			}
		}

		dst = (uint8*)buf;
		done = 0;
		while(done < total){
			size_t chunk = Min((size_t)0x2000, total - done);
			void *readPtr = directRead ? (dst + done) : bounce;
			s32 res = ISFS_Read(myfiles[fd].isfsFd, readPtr, chunk);
			if(res < 0){
				myfiles[fd].lastError = res;
				break;
			}
			if(res == 0){
				myfiles[fd].lastError = 1;
				break;
			}
			if(!directRead)
				memcpy(dst + done, bounce, res);
			done += res;
			if((size_t)res != chunk){
				myfiles[fd].lastError = 1;
				break;
			}
		}
		if(bounce != nil)
			free(bounce);
		return done / elt;
	}
#endif
	if(myfiles[fd].isText){
		unsigned char *p;
		size_t i;
		int c;

		n *= elt;
		p = (unsigned char*)buf;
		for(i = 0; i < n; i++){
			c = myfgetc(fd);
			if(c == EOF)
				break;
			*p++ = (unsigned char)c;
		}
		return i / elt;
	}
	return fread(buf, elt, n, myfiles[fd].file);
}

static size_t
myfwrite(void *buf, size_t elt, size_t n, int fd)
{
#if defined(WII)
	if(myfiles[fd].backend == myFILE::BACKEND_ISFS){
		size_t total, done;
		const uint8 *src;
		uint8 *bounce;
		const bool directWrite = (((u32)buf) & 31) == 0;

		if(myfiles[fd].isText){
			unsigned char *p;
			size_t i;
			int c;

			n *= elt;
			p = (unsigned char*)buf;
			for(i = 0; i < n; i++){
				c = *p++;
				myfputc(c, fd);
				if(myfiles[fd].lastError)
					break;
			}
			return i / elt;
		}

		total = elt * n;
		if(total == 0)
			return 0;

		src = (const uint8*)buf;
		done = 0;
		myfiles[fd].lastError = 0;
		bounce = nil;
		if(!directWrite){
			bounce = (uint8*)memalign(32, align32(Min((size_t)0x2000, total)));
			if(bounce == nil){
				myfiles[fd].lastError = ISFS_ENOMEM;
				return 0;
			}
		}
		while(done < total){
			size_t chunk = Min((size_t)0x2000, total - done);
			const void *writePtr = src + done;
			if(!directWrite){
				memcpy(bounce, src + done, chunk);
				writePtr = bounce;
			}
			s32 res = ISFS_Write(myfiles[fd].isfsFd, writePtr, chunk);
			if(res < 0){
				SYS_Report("[reVC-WII] ISFS_Write failed: fd=%d done=%u total=%u ret=%d\n",
					myfiles[fd].isfsFd, (u32)done, (u32)total, res);
				myfiles[fd].lastError = res;
				break;
			}
			if(res == 0){
				SYS_Report("[reVC-WII] ISFS_Write returned 0: fd=%d done=%u total=%u\n",
					myfiles[fd].isfsFd, (u32)done, (u32)total);
				myfiles[fd].lastError = 1;
				break;
			}
			done += res;
			if((size_t)res != chunk){
				myfiles[fd].lastError = 1;
				break;
			}
		}
		if(bounce != nil)
			free(bounce);
		return done / elt;
	}
#endif
	if(myfiles[fd].isText){
		unsigned char *p;
		size_t i;
		int c;

		n *= elt;
		p = (unsigned char*)buf;
		for(i = 0; i < n; i++){
			c = *p++;
			myfputc(c, fd);
			if(feof(myfiles[fd].file))	// is this right?
				break;
		}
		return i / elt;
	}
	return fwrite(buf, elt, n, myfiles[fd].file);
}

static int
myfseek(int fd, long offset, int whence)
{
#if defined(WII)
	if(myfiles[fd].backend == myFILE::BACKEND_ISFS){
		s32 res = ISFS_Seek(myfiles[fd].isfsFd, offset, whence);
		if(res < 0){
			myfiles[fd].lastError = res;
			return -1;
		}
		myfiles[fd].lastError = 0;
		return 0;
	}
#endif
	return fseek(myfiles[fd].file, offset, whence);
}

static int
myfeof(int fd)
{
#if defined(WII)
	if(myfiles[fd].backend == myFILE::BACKEND_ISFS)
		return myfiles[fd].lastError != 0;
#endif
	return feof(myfiles[fd].file);
//	return ferror(myfiles[fd].file);
}


char CFileMgr::ms_rootDirName[128] = {'\0'};
char CFileMgr::ms_dirName[128];

void
CFileMgr::Initialise(void)
{
#if GX_CONSOLE
    // GameCube: 鏍圭洰褰曠疆绌哄嵆鍙紝鍏ㄩ儴渚濊禆浜庝唬鐮佹嫾鎺ュ嚭锟?dvd:/ 缁濆璺緞
    strcpy(ms_rootDirName, "");
    strcpy(ms_dirName, "");
	s_gx_current_dir[0] = '\0';   // 锟?鏂板锛氬悓姝ラ暅鍍忓彉锟?
#elif defined(ANDROID)
	if(getenv("STORAGE_ROOT") != NULL) {
		strcpy(ms_rootDirName, getenv("STORAGE_ROOT"));
		strcat(ms_rootDirName, "/");
        debug("Android: Root Dir: %s\n", ms_rootDirName);
	}
#else
    _getcwd(ms_rootDirName, 128);
	strcat(ms_rootDirName, "\\");
#endif
}

void
CFileMgr::ChangeDir(const char *dir)
{
	if(*dir == '\\'){
		strcpy(ms_dirName, ms_rootDirName);
		dir++;
	}
	if(*dir != '\0'){
		strcat(ms_dirName, dir);
#ifndef ANDROID
        // BUG in the game it seems, it's off by one
		if(dir[strlen(dir)-1] != '\\')
			strcat(ms_dirName, "\\");
#endif
	}
	debug("CFileMgr::ChangeDir: %s", ms_dirName);
	mychdir(ms_dirName);
#if GX_CONSOLE
    // --------------------------------------------------------
    // 鍚屾 s_gx_current_dir锛堜緵 myfopen 浣跨敤锟?
    // ms_dirName 姝ゆ椂鍙兘锟?"DATA\" 锟?"" 锟?Windows 椋庢牸璺緞
    // 鎴戜滑闇€瑕侊細
    //   1. 杞崲 \ -> /
    //   2. 鍘婚櫎灏鹃儴鏂滄潬锛坢yfopen 鎷兼帴鏃惰嚜宸卞姞 /锟?
    // --------------------------------------------------------
    strncpy(s_gx_current_dir, ms_dirName, sizeof(s_gx_current_dir) - 1);
    s_gx_current_dir[sizeof(s_gx_current_dir) - 1] = '\0';

    // 姝ラ 1锛氬弽鏂滄潬 -> 姝ｆ枩锟?
    for (int i = 0; s_gx_current_dir[i] != '\0'; i++) {
        if (s_gx_current_dir[i] == '\\') s_gx_current_dir[i] = '/';
    }

    // 姝ラ 2锛氬幓闄ゅ熬閮ㄦ枩锟?
    int gc_len = (int)strlen(s_gx_current_dir);
    if (gc_len > 0 && s_gx_current_dir[gc_len - 1] == '/')
        s_gx_current_dir[gc_len - 1] = '\0';

    debug("[GC] s_gx_current_dir = \"%s\"", s_gx_current_dir);
#endif
}

void
CFileMgr::SetDir(const char *dir)
{
	strcpy(ms_dirName, ms_rootDirName);
	if(*dir != '\0'){
		strcat(ms_dirName, dir);
#ifndef ANDROID
        // BUG in the game it seems, it's off by one
		if(dir[strlen(dir)-1] != '\\')
			strcat(ms_dirName, "\\");
#endif
	}
	//debug("CFileMgr::SetDir: %s", ms_dirName);
	mychdir(ms_dirName);
#if GX_CONSOLE
	strncpy(s_gx_current_dir, ms_dirName, sizeof(s_gx_current_dir) - 1);
	s_gx_current_dir[sizeof(s_gx_current_dir) - 1] = '\0';
	for(int i = 0; s_gx_current_dir[i] != '\0'; i++)
		if(s_gx_current_dir[i] == '\\')
			s_gx_current_dir[i] = '/';
	int gc_len = (int)strlen(s_gx_current_dir);
	if(gc_len > 0 && s_gx_current_dir[gc_len - 1] == '/')
		s_gx_current_dir[gc_len - 1] = '\0';
#endif
}

void
CFileMgr::SetDirMyDocuments(void)
{
	SetDir("");	// better start at the root if user directory is relative
#if defined(WII)
	const char *path = _psGetUserFilesFolder();
	if (path == nil)
		return;
	strncpy(s_gx_current_dir, path, sizeof(s_gx_current_dir) - 1);
	s_gx_current_dir[sizeof(s_gx_current_dir) - 1] = '\0';
	for (int i = 0; s_gx_current_dir[i] != '\0'; i++)
		if (s_gx_current_dir[i] == '\\')
			s_gx_current_dir[i] = '/';
	int gc_len = (int)strlen(s_gx_current_dir);
	if (gc_len > 1 && s_gx_current_dir[gc_len - 1] == '/')
		s_gx_current_dir[gc_len - 1] = '\0';
#else
	mychdir(_psGetUserFilesFolder());
#endif
}

ssize_t
CFileMgr::LoadFile(const char *file, uint8 *buf, int maxlen, const char *mode)
{
	int fd;
	ssize_t n, len;

	fd = myfopen(file, mode);
	if(fd == 0)
		return -1;
#if GX_CONSOLE && 0
	// GC: single-shot read 锟?avoid fread loop buffering issues
	len = 0;
	do{
		n = fread(buf + len, 1, Min(0x4000, maxlen - 1 - (int)len), myfiles[fd].file);
		if(n < 0){
			myfclose(fd);
			return -1;
		}
		len += n;
		if(len >= maxlen - 1)
			break;
	}while(n != 0);
	buf[len] = 0;
#else
	len = 0;
	do{
		n = myfread(buf + len, 1, 0x4000, fd);
#ifndef FIX_BUGS
		if (n < 0)
			return -1;
#endif
		len += n;
		assert(len < maxlen);
	}while(n == 0x4000);
	buf[len] = 0;
#endif
	myfclose(fd);
	return len;
}

int
CFileMgr::OpenFile(const char *file, const char *mode)
{
	//debug("CFileMgr::OpenFile: %s", file);
	return myfopen(file, mode);
}

int
CFileMgr::OpenFileForWriting(const char *file)
{
	//debug("CFileMgr::OpenFileForWriting: %s", file);
	return OpenFile(file, "wb");
}

size_t
CFileMgr::Read(int fd, char *buf, ssize_t len)
{
	return myfread((void*)buf, 1, len, fd);
}

size_t
CFileMgr::Write(int fd, const char *buf, ssize_t len)
{
	return myfwrite((void*)buf, 1, len, fd);
}

bool
CFileMgr::Seek(int fd, int offset, int whence)
{
	return !!myfseek(fd, offset, whence);
}

long
CFileMgr::Tell(int fd)
{
#if defined(WII)
	if(myfiles[fd].backend == myFILE::BACKEND_ISFS){
		alignas(32) fstats stats;
		s32 res = ISFS_GetFileStats(myfiles[fd].isfsFd, &stats);
		if(res < 0){
			myfiles[fd].lastError = res;
			return -1;
		}
		myfiles[fd].lastError = 0;
		return stats.file_pos;
	}
#endif
	return ftell(myfiles[fd].file);
}

bool
CFileMgr::ReadLine(int fd, char *buf, int len)
{
	return myfgets(buf, len, fd) != nil;
}

int
CFileMgr::CloseFile(int fd)
{
	return myfclose(fd);
}

int
CFileMgr::GetErrorReadWrite(int fd)
{
	return myfeof(fd);
}
