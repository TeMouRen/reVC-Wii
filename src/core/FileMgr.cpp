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
#include "common.h"
#include "crossplatform.h"

#include "FileMgr.h"

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
};

#define NUMFILES 20
static myFILE myfiles[NUMFILES];


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
		if(myfiles[fd].file == nil)
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
    setvbuf(myfiles[fd].file, NULL, _IOFBF, 4096);  // Force 4KB buffering
    return fd;
}

static int
myfclose(int fd)
{
	int ret;
	assert(fd < NUMFILES);
	if(myfiles[fd].file){
		ret = fclose(myfiles[fd].file);
		myfiles[fd].file = nil;
		return ret;
	}
	return EOF;
}

static int
myfgetc(int fd)
{
	int c;
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
	return fseek(myfiles[fd].file, offset, whence);
}

static int
myfeof(int fd)
{
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
	mychdir(_psGetUserFilesFolder());
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
