#ifndef _WIN32
#include "common.h"

// ============================================================
// CdStream_posix.cpp — reVC-GC 完整修订版
//
// 修复清单:
//   [F1] #ifdef GTA_PC || GTA_MOBILE 无效预处理语法 → #if defined()
//   [F2] GAMECUBE: CdStreamAddImage 缺少 dvd:/ 前缀 + 小写化
//   [F3] GAMECUBE: GetGTA3ImgSize 使用 fstat 替代 realpath
//   [F4] GetGTA3ImgSize 非 GC 分支补全（原为 ...）
//   [F5] pthread_create 错误判断 == -1 → != 0
//   [F6] ONE_THREAD_PER_CHANNEL: param 内存泄漏 → 立即 free
//   [F7] lseek 返回值未检查 → 检查并标记 STREAM_ERROR
//   [F8] read() 短读检测
//   [F9] CdStreamAddImage 中 assert(false) → ASSERT(0)
//   [F10] re3_sem_open/close 添加缺失的 va_end
//   [F11] CdStreamRemoveImages: gImgNames[i] 置 nil 防悬空
//   [F12] #elifndef → #elif !defined() 兼容旧 GCC
// ============================================================

#if defined(GTA_PC) || defined(GTA_MOBILE) || defined(GAMECUBE)  // [F1]

// ============================================================
// 平台 Include 分流
// ============================================================
#ifdef GAMECUBE
#  include <ogc/semaphore.h>   // sem_t (u32 handle)
#  include <pthread.h>
#  include <fcntl.h>           // O_RDONLY
#  include <errno.h>
#  include <unistd.h>          // open / read / lseek / close / fstat
#  include <sys/stat.h>        // struct stat, stat(), fstat()
#  include <stdlib.h>          // malloc / free / calloc
#  include <string.h>          // strdup / strcpy / strncmp
#  include <stdarg.h>          // va_list
#  include <limits.h>          // PATH_MAX
#  include <ctype.h>           // tolower — [F2] 路径小写化
#  ifndef SEM_FAILED
#    define SEM_FAILED ((sem_t *)NULL)
#  endif
#else
#  include "crossplatform.h"
#  include <signal.h>
#  include <pthread.h>
#  ifndef ANDROID
#    include <semaphore.h>
#  endif
#  include <sys/types.h>
#  include <unistd.h>
#  include <sys/time.h>
#  ifndef ANDROID
#    include <sys/statvfs.h>
#  else
#    include "AndroidMain.h"
#    include <sys/vfs.h>
#    define statvfs statfs
#  endif
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <sys/resource.h>
#  include <stdarg.h>
#  include <limits.h>
#  ifdef __linux__
#    include <sys/syscall.h>
#  endif
#endif  // GAMECUBE

#include "CdStream.h"
#include "MemoryMgr.h"

// ============================================================
// 日志宏
// CDDEBUG : 一般流程信息
// CDTRACE : 错误 / 警告
// ============================================================
// [GC-DEBUG-DISABLED] CdStream logs disabled to reduce OSREPORT noise
#define CDDEBUG(f, ...)  ((void)0)
#define CDTRACE(f, ...)  printf("[CDSTREAM] " f "\n", ## __VA_ARGS__)

#ifdef FLUSHABLE_STREAMING
bool flushStream[MAX_CDCHANNELS];
#endif

// ============================================================
// 信号量抽象层: RE3_SEM_OPEN / RE3_SEM_CLOSE
//
// 优先级链:
//   1. GAMECUBE        → libogc LWP 信号量
//   2. USE_UNNAMED_SEM → POSIX sem_init (匿名)
//   3. !ANDROID        → POSIX sem_open (命名)
//   4. ANDROID         → pthread_mutex (无宏)
// ============================================================

// ── [1] GameCube: libogc LWP 信号量 ──────────────────────────
#ifdef GAMECUBE

static sem_t *
re3_gc_sem_open(int initial, int max_count)
{
    sem_t *sem = (sem_t *)malloc(sizeof(sem_t));
    if (!sem) {
        CDTRACE("re3_gc_sem_open: malloc failed");
        return SEM_FAILED;
    }
    if (LWP_SemInit(sem, (u32)initial, (u32)max_count) != 0) {
        CDTRACE("re3_gc_sem_open: LWP_SemInit failed (init=%d max=%d)",
                initial, max_count);
        free(sem);
        return SEM_FAILED;
    }
    CDDEBUG("re3_gc_sem_open: OK sem=%p handle=0x%08X init=%d max=%d",
            (void *)sem, (unsigned)*sem, initial, max_count);
    return sem;
}

static void
re3_gc_sem_close(sem_t *sem)
{
    if (sem && sem != SEM_FAILED) {
        CDDEBUG("re3_gc_sem_close: sem=%p handle=0x%08X", (void *)sem, (unsigned)*sem);
        LWP_SemDestroy(*sem);
        free(sem);
    }
}

// 全局队列信号量: init=0, max=16(队列深度)
// Done/Start 二值信号量: init=0, max=1
// 宏通过可变参数忽略命名参数（GC 不用命名信号量）
#  define RE3_SEM_OPEN(format, ...)    re3_gc_sem_open(0, 16)
#  define RE3_SEM_CLOSE(sem, ...)      re3_gc_sem_close(sem)

// LWP_SemWait/Post 接受值 (sem_t)，调用处传指针，此处解引用
#  define sem_wait(s)                  LWP_SemWait(*(s))
#  define sem_post(s)                  LWP_SemPost(*(s))

// ── [2] 匿名 POSIX 信号量 (Android / USE_UNNAMED_SEM) ─────────
#elif defined(USE_UNNAMED_SEM)

sem_t *
re3_sem_open(void)
{
    sem_t *sem = (sem_t *)malloc(sizeof(sem_t));
    if (!sem) return SEM_FAILED;
    if (sem_init(sem, 0, 1) == -1) {
        free(sem);
        return SEM_FAILED;
    }
    return sem;
}

void
re3_sem_close(sem_t *sem)
{
    if (sem && sem != SEM_FAILED) {
        sem_destroy(sem);
        free(sem);
    }
}

#  define RE3_SEM_OPEN(name, ...)             re3_sem_open()
#  define RE3_SEM_CLOSE(sem, format, ...)     re3_sem_close(sem)

// ── [3] 命名 POSIX 信号量 (Linux / macOS) ────────────────────
#elif !defined(ANDROID)  // [F12] 原 #elifndef 兼容旧 GCC

sem_t *
re3_sem_open(const char *format, ...)
{
    char semName[21];
    va_list va;
    va_start(va, format);
    vsprintf(semName, format, va);
    va_end(va);  // [F10]
    return sem_open(semName, O_CREAT, 0644, 1);
}

void
re3_sem_close(sem_t *sem, const char *format, ...)
{
    sem_close(sem);
    char semName[21];
    va_list va;
    va_start(va, format);
    vsprintf(semName, format, va);
    va_end(va);  // [F10]
    sem_unlink(semName);
}

#  define RE3_SEM_OPEN   re3_sem_open
#  define RE3_SEM_CLOSE  re3_sem_close

#endif  // 信号量平台选择

// ============================================================
// 通道读取信息结构体
// ============================================================
struct CdReadInfo
{
    uint32 nSectorOffset;
    uint32 nSectorsToRead;
    void  *pBuffer;
    bool   bLocked;
    bool   bReading;
    int32  nStatus;
#ifdef ONE_THREAD_PER_CHANNEL
    int8      nThreadStatus;
    pthread_t pChannelThread;
    sem_t    *pStartSemaphore;
#endif
#ifndef ANDROID
    sem_t          *pDoneSemaphore;
#else
    pthread_mutex_t pDoneSemaphore;
#endif
    int32 hFile;
};

// ============================================================
// 全局状态
// ============================================================
char  gCdImageNames[MAX_CDIMAGES + 1][64];
int32 gNumImages;
int32 gNumChannels;

int32  gImgFiles[MAX_CDIMAGES];  // 0: unused  -1: error  >0: fd+1
char  *gImgNames[MAX_CDIMAGES];

#ifndef ONE_THREAD_PER_CHANNEL
pthread_t _gCdStreamThread;
#  ifndef ANDROID
sem_t          *gCdStreamSema;
#  else
pthread_mutex_t gCdStreamSema;
#  endif
int8  gCdStreamThreadStatus;   // 0: 已创建  1: 优先级已设  2: 请求退出
Queue gChannelRequestQ;
bool  _gbCdStreamOverlapped;
#endif

CdReadInfo *gpReadInfo;
int32       lastPosnRead;
int         _gdwCdStreamFlags;

void *CdStreamThread(void *channelId);

#ifdef GAMECUBE
/* gcm_read_r serialises each logical read against radio I/O. Cap world-stream
 * ownership to 32 KiB at a time so radio can run between large gta3.img
 * transfers. The absolute disc offset may make a chunk cross a cluster. */
static const size_t GC_CDSTREAM_READ_CHUNK_SIZE = 32 * 1024;

static ssize_t
CdStreamReadInterleaved(int fd, void *buffer, size_t size)
{
    u8 *dst = (u8 *)buffer;
    size_t total = 0;

    while (total < size) {
        size_t chunk = size - total;
        if (chunk > GC_CDSTREAM_READ_CHUNK_SIZE)
            chunk = GC_CDSTREAM_READ_CHUNK_SIZE;

        ssize_t got = read(fd, dst + total, chunk);
        if (got < 0)
            return -1;
        if (got == 0)
            break;
        total += (size_t)got;
    }

    return (ssize_t)total;
}
#endif

static int32
CdStreamFindImageByFd(int32 fd)
{
    for (int32 i = 0; i < gNumImages; i++)
        if (gImgFiles[i] == fd + 1)
            return i;
    return -1;
}

static const char *
CdStreamImageNameByFd(int32 fd)
{
    int32 img = CdStreamFindImageByFd(fd);
    if (img >= 0 && gImgNames[img])
        return gImgNames[img];
    return "(unknown)";
}

static unsigned long
CdStreamFileSizeByFd(int32 fd)
{
    struct stat st;
    if (fstat(fd, &st) == 0)
        return (unsigned long)st.st_size;
    return 0;
}

// ============================================================
// CdStreamInitThread
// ============================================================
void
CdStreamInitThread(void)
{
    int status;
    CDDEBUG("CdStreamInitThread: starting (numChannels=%d)", gNumChannels);

#ifndef ONE_THREAD_PER_CHANNEL
    gChannelRequestQ.items = (int32 *)calloc(gNumChannels + 1, sizeof(int32));
    gChannelRequestQ.head  = 0;
    gChannelRequestQ.tail  = 0;
    gChannelRequestQ.size  = gNumChannels + 1;
    ASSERT(gChannelRequestQ.items != nil);
    CDDEBUG("CdStreamInitThread: request queue allocated size=%d ptr=%p",
            gNumChannels + 1, (void *)gChannelRequestQ.items);

#  ifndef ANDROID
    gCdStreamSema = RE3_SEM_OPEN("/semaphore_cd_stream");
    if (gCdStreamSema == SEM_FAILED) {
        CDTRACE("CdStreamInitThread: FAILED to create global stream semaphore");
        ASSERT(0);
        return;
    }
    CDDEBUG("CdStreamInitThread: global stream semaphore OK ptr=%p", (void *)gCdStreamSema);
#  else
    // Android: mutex 方式，此处无需操作
#  endif
#endif  // !ONE_THREAD_PER_CHANNEL

    for (int32 i = 0; i < gNumChannels; i++) {
        CDDEBUG("CdStreamInitThread: initializing channel %d / %d", i, gNumChannels - 1);

#ifndef ANDROID
        gpReadInfo[i].pDoneSemaphore = RE3_SEM_OPEN("/semaphore_done%d", i);
        if (gpReadInfo[i].pDoneSemaphore == SEM_FAILED) {
            CDTRACE("CdStreamInitThread: FAILED to create done semaphore for ch%d", i);
            ASSERT(0);
            return;
        }
        CDDEBUG("CdStreamInitThread: ch%d done semaphore OK ptr=%p",
                i, (void *)gpReadInfo[i].pDoneSemaphore);
#endif

#ifdef ONE_THREAD_PER_CHANNEL
        gpReadInfo[i].pStartSemaphore = RE3_SEM_OPEN("/semaphore_start%d", i);
        if (gpReadInfo[i].pStartSemaphore == SEM_FAILED) {
            CDTRACE("CdStreamInitThread: FAILED to create start semaphore for ch%d", i);
            ASSERT(0);
            return;
        }
        CDDEBUG("CdStreamInitThread: ch%d start semaphore OK ptr=%p",
                i, (void *)gpReadInfo[i].pStartSemaphore);

        gpReadInfo[i].nThreadStatus = 0;

        int *channelI = (int *)malloc(sizeof(int));
        ASSERT(channelI != nil);
        *channelI = i;

        status = pthread_create(&gpReadInfo[i].pChannelThread, NULL,
                                CdStreamThread, (void *)channelI);
        if (status != 0) {  // [F5] 原为 == -1
            CDTRACE("CdStreamInitThread: FAILED to create thread for ch%d err=%d", i, status);
            free(channelI);
            ASSERT(0);
            return;
        }
        CDDEBUG("CdStreamInitThread: ch%d thread created OK", i);
#endif
    }

#ifndef ONE_THREAD_PER_CHANNEL
    debug("[CdStream] Using one streaming thread for all channels\n");
    gCdStreamThreadStatus = 0;
    status = pthread_create(&_gCdStreamThread, NULL, CdStreamThread, nil);
    if (status != 0) {  // [F5]
        CDTRACE("CdStreamInitThread: FAILED to create global stream thread err=%d", status);
        ASSERT(0);
        return;
    }
    CDDEBUG("CdStreamInitThread: global stream thread created OK");
#else
    debug("[CdStream] Using separate streaming threads per channel\n");
#endif

    CDDEBUG("CdStreamInitThread: complete");
}

// ============================================================
// CdStreamInit
// ============================================================
void
CdStreamInit(int32 numChannels)
{
    CDDEBUG("CdStreamInit: numChannels=%d", numChannels);

#ifdef GAMECUBE
    // [GAMECUBE] statvfs 不可用；使用 GC Cache Line 对齐 32 字节
    const uint32 fsBlockSize = 32u;
    CDDEBUG("CdStreamInit: GC mode — fixed block size %u bytes", fsBlockSize);
#else
    struct statvfs fsInfo;
#  if defined(ANDROID)
    char imgPath[MAX_PATH];
    if (StorageRootBuffer == NULL) {
        char pwd[128];
        getcwd(pwd, 128);
        setenv("STORAGE_ROOT", pwd, 1);
        debug("%s\n", pwd);
    }
    debug("FILES %s\n", StorageRootBuffer);
    strcpy(imgPath, StorageRootBuffer);
    strcat(imgPath, "/models/gta3.img");
    debug("%s\n", imgPath);
    if (statvfs(imgPath, &fsInfo) < 0)
#  else
    if (statvfs("models/gta3.img", &fsInfo) < 0)
#  endif
    {
        CDTRACE("CdStreamInit: statvfs failed — can't determine block size");
        ASSERT(0);
        return;
    }
    CDDEBUG("CdStreamInit: fs block size = %lu bytes",
            (unsigned long)fsInfo.f_bsize);
#endif  // GAMECUBE

    // オープンフラグ
#if defined(ANDROID) || defined(GAMECUBE)
    _gdwCdStreamFlags = O_RDONLY;
#elif defined(__linux__)
    _gdwCdStreamFlags = O_RDONLY | O_NOATIME;
#else
    _gdwCdStreamFlags = O_RDONLY;
#endif
    CDDEBUG("CdStreamInit: open flags = 0x%X", _gdwCdStreamFlags);

    // アラインメントテスト用バッファ
#ifdef GAMECUBE
    void *pBuffer = (void *)RwMallocAlign(CDSTREAM_SECTOR_SIZE, fsBlockSize);
#else
    void *pBuffer = (void *)RwMallocAlign(CDSTREAM_SECTOR_SIZE, (RwUInt32)fsInfo.f_bsize);
#endif
    ASSERT(pBuffer != nil);
    CDDEBUG("CdStreamInit: sector align-buffer=%p sectorSize=%d", pBuffer, CDSTREAM_SECTOR_SIZE);

    gNumImages   = 0;
    gNumChannels = numChannels;
    ASSERT(gNumChannels != 0);

    gpReadInfo = (CdReadInfo *)calloc(numChannels, sizeof(CdReadInfo));
    ASSERT(gpReadInfo != nil);
    CDDEBUG("read info %p", gpReadInfo);
    CDDEBUG("CdStreamInit: size of CdReadInfo = %u bytes",
            (unsigned)sizeof(CdReadInfo));

    CdStreamInitThread();

    ASSERT(pBuffer != nil);
    RwFreeAlign(pBuffer);
    CDDEBUG("CdStreamInit: complete");
}

// ============================================================
// GetGTA3ImgSize
// ============================================================
uint32
GetGTA3ImgSize(void)
{
    CDDEBUG("GetGTA3ImgSize: gImgFiles[0]=%d gImgNames[0]=%s",
            gImgFiles[0], gImgNames[0] ? gImgNames[0] : "(null)");
    ASSERT(gImgFiles[0] > 0);

    struct stat statbuf;

#ifdef GAMECUBE
    // [F3] realpath unreliable with GC devoptab paths
    //      open 済みの fd から fstat する方が確実
    int fd = gImgFiles[0] - 1;
    CDDEBUG("GetGTA3ImgSize: fstat fd=%d", fd);
    if (fstat(fd, &statbuf) == -1) {
        CDTRACE("GetGTA3ImgSize: fstat FAILED fd=%d", fd);
        ASSERT(0);
        return 0;
    }
    CDDEBUG("GetGTA3ImgSize: size=%u bytes (%u KB)",
            (uint32)statbuf.st_size,
            (uint32)(statbuf.st_size >> 10));
#else
    // [F4] 原は ... のみ — 正規実装に復元
    char path[PATH_MAX];
    realpath(gImgNames[0], path);
    CDDEBUG("GetGTA3ImgSize: stat path='%s'", path);

    if (stat(path, &statbuf) == -1) {
        CDDEBUG("GetGTA3ImgSize: stat failed, trying casepath fallback");
        char *real = casepath(gImgNames[0], false);
        if (real) {
            realpath(real, path);
            free(real);
            CDDEBUG("GetGTA3ImgSize: casepath resolved='%s'", path);
            if (stat(path, &statbuf) != -1)
                goto got_size;
        }
        CDTRACE("GetGTA3ImgSize: FAILED — cannot stat '%s'", gImgNames[0]);
        ASSERT(0);
        return 0;
    }
got_size:
    CDDEBUG("GetGTA3ImgSize: size=%u bytes (%u KB)",
            (uint32)statbuf.st_size,
            (uint32)(statbuf.st_size >> 10));
#endif  // GAMECUBE

    return (uint32)statbuf.st_size;
}

// ============================================================
// CdStreamShutdown
// ============================================================
void
CdStreamShutdown(void)
{
    CDDEBUG("CdStreamShutdown: requesting exit");

#ifndef ONE_THREAD_PER_CHANNEL
    gCdStreamThreadStatus = 2;
#  ifndef ANDROID
    sem_post(gCdStreamSema);
#  else
    pthread_mutex_unlock(&gCdStreamSema);
#  endif
    CDDEBUG("CdStreamShutdown: waiting for global stream thread...");
    pthread_join(_gCdStreamThread, nil);
    CDDEBUG("CdStreamShutdown: global stream thread exited");
#else
    for (int32 i = 0; i < gNumChannels; i++) {
        CDDEBUG("CdStreamShutdown: signaling ch%d to exit", i);
        gpReadInfo[i].nThreadStatus = 2;
        sem_post(gpReadInfo[i].pStartSemaphore);
        pthread_join(gpReadInfo[i].pChannelThread, nil);
        CDDEBUG("CdStreamShutdown: ch%d exited", i);
    }
#endif

    CDDEBUG("CdStreamShutdown: complete");
}

// ============================================================
// CdStreamRead
// ============================================================
int32
CdStreamRead(int32 channel, void *buffer, uint32 offset, uint32 size)
{
    ASSERT(channel < gNumChannels);
    ASSERT(buffer != nil);

    lastPosnRead = size + offset;

    uint32 imgIndex = _GET_INDEX(offset);
    uint32 sectorOffset = _GET_OFFSET(offset);
    if (imgIndex >= MAX_CDIMAGES) {
        CDTRACE("CdStreamRead rejected: ch=%d invalid image index=%u offset=0x%08X sectors=%u",
                channel, imgIndex, offset, size);
        return STREAM_ERROR;
    }

    int32 hImage = gImgFiles[imgIndex];
    if (hImage <= 0) {
        CDTRACE("CdStreamRead rejected: ch=%d image=%u not open handle=%d offset=0x%08X sectors=%u",
                channel, imgIndex, hImage, offset, size);
        return STREAM_ERROR_OPENCD;
    }

    CDDEBUG("CdStreamRead: ch=%d buf=%p offset=0x%08X imgIdx=%d size=%u",
            channel, buffer, offset, imgIndex, size);

    ASSERT(hImage > 0);

    CdReadInfo *pChannel = &gpReadInfo[channel];
    ASSERT(pChannel != nil);

    if (pChannel->nSectorsToRead != 0 || pChannel->bReading) {
        if (pChannel->hFile          == hImage - 1           &&
            pChannel->nSectorOffset  == sectorOffset  &&
            pChannel->nSectorsToRead >= size) {
            CDDEBUG("CdStreamRead: ch=%d already reading same range — early SUCCESS", channel);
            return STREAM_SUCCESS;
        }
#ifdef FLUSHABLE_STREAMING
        CDDEBUG("CdStreamRead: ch=%d conflict (sectors=%u reading=%d) — flushing",
                channel, pChannel->nSectorsToRead, (int)pChannel->bReading);
        flushStream[channel] = 1;
        CdStreamSync(channel);
#else
        CDDEBUG("CdStreamRead: ch=%d busy (sectors=%u reading=%d) — STREAM_NONE",
                channel, pChannel->nSectorsToRead, (int)pChannel->bReading);
        return STREAM_NONE;
#endif
    }

    pChannel->hFile          = hImage - 1;
    pChannel->nStatus        = STREAM_NONE;
    pChannel->nSectorOffset  = sectorOffset;
    pChannel->nSectorsToRead = size;
    pChannel->pBuffer        = buffer;
    pChannel->bLocked        = 0;

    CDDEBUG("CdStreamRead: ch=%d enqueued fd=%d sectorOff=%u sectors=%u buf=%p",
            channel, pChannel->hFile, pChannel->nSectorOffset, size, buffer);

#ifndef ONE_THREAD_PER_CHANNEL
    AddToQueue(&gChannelRequestQ, channel);
#  if defined(ANDROID)
    if (pthread_mutex_unlock(&gCdStreamSema) != 0)
#  else
    if (sem_post(gCdStreamSema) != 0)
#  endif
    {
        CDTRACE("CdStreamRead: ch=%d WARNING sem_post failed on stream sema", channel);
    }
#else
    if (sem_post(pChannel->pStartSemaphore) != 0) {
        CDTRACE("CdStreamRead: ch=%d WARNING sem_post failed on start sema", channel);
    }
#endif

    return STREAM_SUCCESS;
}

// ============================================================
// CdStreamGetStatus
// ============================================================
int32
CdStreamGetStatus(int32 channel)
{
    ASSERT(channel < gNumChannels);
    CdReadInfo *pChannel = &gpReadInfo[channel];
    ASSERT(pChannel != nil);

#ifdef ONE_THREAD_PER_CHANNEL
    if (pChannel->nThreadStatus == 2)
        return STREAM_NONE;
#else
    if (gCdStreamThreadStatus == 2)
        return STREAM_NONE;
#endif

    if (pChannel->bReading)
        return STREAM_READING;

    if (pChannel->nSectorsToRead != 0)
        return STREAM_WAITING;

    if (pChannel->nStatus != STREAM_NONE) {
        int32 st          = pChannel->nStatus;
        pChannel->nStatus = STREAM_NONE;
        CDTRACE("status consumed: ch=%d status=0x%02X fd=%d img=%d sector=%u sectors=%u name='%s'",
                channel, st & 0xFF, pChannel->hFile,
                CdStreamFindImageByFd(pChannel->hFile),
                pChannel->nSectorOffset, pChannel->nSectorsToRead,
                CdStreamImageNameByFd(pChannel->hFile));
        CDDEBUG("CdStreamGetStatus: ch=%d consuming status=%d", channel, st);
        return st;
    }

    return STREAM_NONE;
}

// ============================================================
// CdStreamGetLastPosn
// ============================================================
int32
CdStreamGetLastPosn(void)
{
    return lastPosnRead;
}

// ============================================================
// CdStreamSync
// ============================================================
int32
CdStreamSync(int32 channel)
{
    ASSERT(channel < gNumChannels);
    CdReadInfo *pChannel = &gpReadInfo[channel];
    ASSERT(pChannel != nil);

    CDDEBUG("CdStreamSync: ch=%d (sectors=%u reading=%d locked=%d)",
            channel, pChannel->nSectorsToRead,
            (int)pChannel->bReading, (int)pChannel->bLocked);

#ifdef FLUSHABLE_STREAMING
    if (flushStream[channel]) {
        CDDEBUG("CdStreamSync: ch=%d flush path", channel);
        pChannel->nSectorsToRead = 0;

#  ifdef ONE_THREAD_PER_CHANNEL
        // [GAMECUBE] libogc は POSIX シグナル非対応 → pthread_kill スキップ
        // スレッドは次回 sem_wait 後に nSectorsToRead==0 を自然検出
#    ifndef GAMECUBE
        pthread_kill(pChannel->pChannelThread, SIGUSR1);
#    endif
        if (pChannel->bReading) {
            pChannel->bLocked = true;
#  else
        if (pChannel->bReading) {
            pChannel->bLocked = true;
#    ifndef GAMECUBE
            pthread_kill(_gCdStreamThread, SIGUSR1);
#    endif
#  endif
            CDDEBUG("CdStreamSync: ch=%d waiting for flush done...", channel);
            while (pChannel->bLocked) {
#  ifndef ANDROID
                sem_wait(pChannel->pDoneSemaphore);
#  else
                pthread_mutex_lock(&pChannel->pDoneSemaphore);
#  endif
            }
            CDDEBUG("CdStreamSync: ch=%d flush wait complete", channel);
        }

        pChannel->bReading   = false;
        flushStream[channel] = false;
        return STREAM_NONE;
    }
#endif  // FLUSHABLE_STREAMING

    if (pChannel->nSectorsToRead != 0) {
        pChannel->bLocked = true;
        CDDEBUG("CdStreamSync: ch=%d waiting for read done...", channel);
        while (pChannel->bLocked && pChannel->nSectorsToRead != 0) {
#ifndef ANDROID
            sem_wait(pChannel->pDoneSemaphore);
#else
            pthread_mutex_lock(&pChannel->pDoneSemaphore);
#endif
        }
        pChannel->bLocked = false;
        CDDEBUG("CdStreamSync: ch=%d read done, status=%d", channel, pChannel->nStatus);
    }

    pChannel->bReading = false;
    return pChannel->nStatus;
}

// ============================================================
// キュー操作
// ============================================================
void
AddToQueue(Queue *queue, int32 item)
{
    ASSERT(queue != nil);
    ASSERT(queue->items != nil);

    queue->items[queue->tail] = item;
    queue->tail = (queue->tail + 1) % queue->size;

    if (queue->head == queue->tail)
        CDTRACE("AddToQueue: WARNING queue FULL (size=%d)", queue->size);
}

int32
GetFirstInQueue(Queue *queue)
{
    ASSERT(queue != nil);
    if (queue->head == queue->tail)
        return -1;
    ASSERT(queue->items != nil);
    return queue->items[queue->head];
}

void
RemoveFirstInQueue(Queue *queue)
{
    ASSERT(queue != nil);
    if (queue->head == queue->tail) {
        CDTRACE("RemoveFirstInQueue: WARNING queue already EMPTY");
        return;
    }
    queue->head = (queue->head + 1) % queue->size;
}

// ============================================================
// CdStreamThread
// ============================================================
void *
CdStreamThread(void *param)
{
#ifdef ONE_THREAD_PER_CHANNEL
    int channel = *((int *)param);
    free(param);   // [F6] 即座に解放してリークを防ぐ
    CDDEBUG("CdStreamThread: ch=%d thread started", channel);
#else
    CDDEBUG("CdStreamThread: global stream thread started");
#endif

#ifndef ONE_THREAD_PER_CHANNEL
    while (gCdStreamThreadStatus != 2) {
#  ifndef ANDROID
        sem_wait(gCdStreamSema);
#  else
        pthread_mutex_lock(&gCdStreamSema);
#  endif

        int32 channel = GetFirstInQueue(&gChannelRequestQ);
        if (channel == -1) {
            CDDEBUG("CdStreamThread: spurious wake, no pending channel");
            continue;
        }
        CDDEBUG("CdStreamThread: dequeued channel=%d", channel);
#else
    while (gpReadInfo[channel].nThreadStatus != 2) {
        sem_wait(gpReadInfo[channel].pStartSemaphore);
        CDDEBUG("CdStreamThread: ch=%d woken from start sema", channel);
#endif

        CdReadInfo *pChannel = &gpReadInfo[channel];
        ASSERT(pChannel != nil);

        if (pChannel->nSectorsToRead == 0) {
            CDDEBUG("CdStreamThread: ch=%d spurious / post-flush zero, skipping", channel);
#ifndef ONE_THREAD_PER_CHANNEL
            RemoveFirstInQueue(&gChannelRequestQ);
#endif
            continue;
        }

        pChannel->bReading = true;

        // Linux スレッド優先度調整（GC は非対応 → 自然にスキップ）
#ifdef __linux__
#  ifdef ONE_THREAD_PER_CHANNEL
        if (gpReadInfo[channel].nThreadStatus == 0) {
            gpReadInfo[channel].nThreadStatus = 1;
#  else
        if (gCdStreamThreadStatus == 0) {
            gCdStreamThreadStatus = 1;
#  endif
            pid_t tid = syscall(SYS_gettid);
            int ret = setpriority(PRIO_PROCESS, tid,
                                  getpriority(PRIO_PROCESS, getpid()) + 1);
            (void)ret;
        }
#endif

        if (pChannel->nStatus == STREAM_NONE) {
            ASSERT(pChannel->hFile >= 0);
            ASSERT(pChannel->pBuffer != nil);

            off_t  seekPos  = (off_t)pChannel->nSectorOffset  * (off_t)CDSTREAM_SECTOR_SIZE;
            size_t readSize = (size_t)pChannel->nSectorsToRead * (size_t)CDSTREAM_SECTOR_SIZE;
            int32 imageIndex = CdStreamFindImageByFd(pChannel->hFile);
            const char *imageName = CdStreamImageNameByFd(pChannel->hFile);
            unsigned long fileSize = CdStreamFileSizeByFd(pChannel->hFile);

            CDDEBUG("CdStreamThread: ch=%d READ fd=%d sectorOff=%u seekBytes=%lu readBytes=%lu",
                    channel, pChannel->hFile,
                    pChannel->nSectorOffset,
                    (unsigned long)seekPos,
                    (unsigned long)readSize);

            // [F7] lseek 戻り値チェック
            off_t seekResult = lseek(pChannel->hFile, seekPos, SEEK_SET);
            if (seekResult == (off_t)-1) {
                CDTRACE("lseek failed: ch=%d fd=%d img=%d name='%s' seek=%lu sectors=%u fileSize=%lu errno=%d",
                        channel, pChannel->hFile, imageIndex, imageName,
                        (unsigned long)seekPos, pChannel->nSectorsToRead,
                        fileSize, errno);
                pChannel->nStatus = STREAM_ERROR;
            } else {
                // [F8] read 戻り値 + 短読み出しチェック
#ifdef GAMECUBE
                ssize_t bytesRead = CdStreamReadInterleaved(pChannel->hFile, pChannel->pBuffer, readSize);
#else
                ssize_t bytesRead = read(pChannel->hFile, pChannel->pBuffer, readSize);
#endif
                if (bytesRead == -1) {
                    CDTRACE("read failed: ch=%d fd=%d img=%d name='%s' sector=%u seek=%lu expect=%lu fileSize=%lu errno=%d",
                            channel, pChannel->hFile, imageIndex, imageName,
                            pChannel->nSectorOffset, (unsigned long)seekPos,
                            (unsigned long)readSize, fileSize, errno);
                    pChannel->nStatus = (pChannel->nSectorsToRead == 0)
                                        ? STREAM_WAITING : STREAM_ERROR;
                } else if ((size_t)bytesRead < readSize) {
                    CDTRACE("short read: ch=%d fd=%d img=%d name='%s' sector=%u seek=%lu got=%ld expect=%lu fileSize=%lu eofDelta=%ld",
                            channel, pChannel->hFile, imageIndex, imageName,
                            pChannel->nSectorOffset, (unsigned long)seekPos,
                            (long)bytesRead, (unsigned long)readSize, fileSize,
                            (long)fileSize - (long)((unsigned long)seekPos + (unsigned long)readSize));
                    pChannel->nStatus = STREAM_ERROR;
                } else {
                    CDDEBUG("CdStreamThread: ch=%d read OK %ld bytes", channel, (long)bytesRead);
                    pChannel->nStatus = STREAM_NONE;
                }
            }
        } else {
            CDDEBUG("CdStreamThread: ch=%d skip read (status=%d != NONE)", channel, pChannel->nStatus);
        }

#ifndef ONE_THREAD_PER_CHANNEL
        RemoveFirstInQueue(&gChannelRequestQ);
#endif

        pChannel->nSectorsToRead = 0;
        if (pChannel->bLocked) {
            CDDEBUG("CdStreamThread: ch=%d signaling done sema", channel);
            pChannel->bLocked = 0;
#ifndef ANDROID
            sem_post(pChannel->pDoneSemaphore);
#else
            pthread_mutex_unlock(&pChannel->pDoneSemaphore);
#endif
        }
        pChannel->bReading = false;
    }

    // ── スレッド終了クリーンアップ ──────────────────────────
    CDDEBUG("CdStreamThread: thread exiting — cleanup start");

#ifndef ONE_THREAD_PER_CHANNEL
    for (int32 i = 0; i < gNumChannels; i++) {
        CDDEBUG("CdStreamThread: destroying done sema for ch%d", i);
#  ifndef ANDROID
        RE3_SEM_CLOSE(gpReadInfo[i].pDoneSemaphore, "/semaphore_done%d", i);
#  else
        pthread_mutex_destroy(&gpReadInfo[i].pDoneSemaphore);
#  endif
    }
#  ifndef ANDROID
    CDDEBUG("CdStreamThread: destroying global stream sema");
    RE3_SEM_CLOSE(gCdStreamSema, "/semaphore_cd_stream");
#  else
    pthread_mutex_destroy(&gCdStreamSema);
#  endif

    if (gChannelRequestQ.items) {
        free(gChannelRequestQ.items);
        gChannelRequestQ.items = nil;
        CDDEBUG("CdStreamThread: request queue freed");
    }
#else
    CDDEBUG("CdStreamThread: ch=%d destroying start/done semas", channel);
    RE3_SEM_CLOSE(gpReadInfo[channel].pStartSemaphore, "/semaphore_start%d", channel);
    RE3_SEM_CLOSE(gpReadInfo[channel].pDoneSemaphore,  "/semaphore_done%d",  channel);
#endif  // ONE_THREAD_PER_CHANNEL

    if (gpReadInfo) {
        free(gpReadInfo);
        gpReadInfo = nil;
        CDDEBUG("CdStreamThread: gpReadInfo freed");
    }

    CDDEBUG("CdStreamThread: pthread_exit");
    pthread_exit(nil);
    return nil;
}

// ============================================================
// CdStreamAddImage
// ============================================================
bool
CdStreamAddImage(char const *path)
{
    ASSERT(path != nil);
    ASSERT(gNumImages < MAX_CDIMAGES);

    CDDEBUG("CdStreamAddImage: [slot %d] path='%s'", gNumImages, path);

#ifdef GAMECUBE
    // [F2] open() hits devoptab directly, needs dvd:/ prefix
    //      same path transform as myfopen shim, done manually here
    //      also lowercase path to match FST entries

    char gcPath[256];

    if (strncmp(path, "dvd:/", 5) != 0 && strncmp(path, "sd:/", 4) != 0) {
        int written = snprintf(gcPath, sizeof(gcPath), "dvd:/%s", path);
        if (written < 0 || written >= (int)sizeof(gcPath)) {
            CDTRACE("CdStreamAddImage: path too long to prepend dvd:/ '%s'", path);
            ASSERT(0);
            return false;
        }
        // normalize after dvd:/: backslash -> slash + lowercase
        for (char *p = gcPath + 5; *p; p++) {
            if (*p == '\\') *p = '/';
            else *p = (char)tolower((unsigned char)*p);
        }
    } else {
        strncpy(gcPath, path, sizeof(gcPath) - 1);
        gcPath[sizeof(gcPath) - 1] = '\0';
    }

    CDDEBUG("CdStreamAddImage: GC resolved path='%s'", gcPath);

    int fd = open(gcPath, _gdwCdStreamFlags);
    if (fd == -1) {
        CDTRACE("CdStreamAddImage: FAILED open('%s') — check dvd:/ mount and FST", gcPath);
        ASSERT(0);  // [F9] lowercase assert -> ASSERT
        return false;
    }
    CDDEBUG("CdStreamAddImage: open OK fd=%d", fd);

    gImgFiles[gNumImages] = fd + 1;            // +1: convention, 0=unused
    gImgNames[gNumImages] = strdup(gcPath);    // save dvd:/ path for fstat
    CDTRACE("image open: slot=%d fd=%d path='%s' size=%lu",
            gNumImages, fd, gcPath, CdStreamFileSizeByFd(fd));

#else  // ── 非 GameCube ──────────────────────────────────────

    gImgFiles[gNumImages] = open(path, _gdwCdStreamFlags);
    if (gImgFiles[gNumImages] == -1) {
        CDDEBUG("CdStreamAddImage: initial open failed, trying casepath for '%s'", path);
        char *real = casepath(path, false);
        if (real) {
            gImgFiles[gNumImages] = open(real, _gdwCdStreamFlags);
            CDDEBUG("CdStreamAddImage: casepath='%s' fd=%d", real, gImgFiles[gNumImages]);
            free(real);
        }
    }
    if (gImgFiles[gNumImages] == -1) {
        CDTRACE("CdStreamAddImage: FAILED — all open attempts failed for '%s'", path);
        ASSERT(0);  // [F9]
        return false;
    }
    CDDEBUG("CdStreamAddImage: open OK raw_fd=%d", gImgFiles[gNumImages]);

    gImgNames[gNumImages] = strdup(path);
    gImgFiles[gNumImages]++;  // +1: 0=unused
    CDTRACE("image open: slot=%d fd=%d path='%s' size=%lu",
            gNumImages, gImgFiles[gNumImages] - 1, path,
            CdStreamFileSizeByFd(gImgFiles[gNumImages] - 1));

#endif  // GAMECUBE

    strcpy(gCdImageNames[gNumImages], path);
    gNumImages++;

    CDDEBUG("CdStreamAddImage: slot[%d] mounted gImgFiles[%d]=%d name='%s' total=%d",
            gNumImages - 1, gNumImages - 1,
            gImgFiles[gNumImages - 1],
            gImgNames[gNumImages - 1] ? gImgNames[gNumImages - 1] : "(null)",
            gNumImages);
    return true;
}

// ============================================================
// CdStreamGetImageName
// ============================================================
char *
CdStreamGetImageName(int32 cd)
{
    ASSERT(cd < MAX_CDIMAGES);
    if (gImgFiles[cd] > 0)
        return gCdImageNames[cd];
    return nil;
}

// ============================================================
// CdStreamRemoveImages
// ============================================================
void
CdStreamRemoveImages(void)
{
    CDDEBUG("CdStreamRemoveImages: flushing %d channels then closing %d images",
            gNumChannels, gNumImages);

    for (int32 i = 0; i < gNumChannels; i++) {
#ifdef FLUSHABLE_STREAMING
        flushStream[i] = 1;
#endif
        CdStreamSync(i);
    }

    for (int32 i = 0; i < gNumImages; i++) {
        int rawFd = gImgFiles[i] - 1;
        CDDEBUG("CdStreamRemoveImages: closing slot[%d] fd=%d name='%s'",
                i, rawFd, gImgNames[i] ? gImgNames[i] : "(null)");
        close(rawFd);
        free(gImgNames[i]);
        gImgNames[i] = nil;  // [F11] 解放後に nil でダングリング防止
        gImgFiles[i] = 0;
    }

    gNumImages = 0;
    CDDEBUG("CdStreamRemoveImages: complete");
}

// ============================================================
// CdStreamGetNumImages
// ============================================================
int32
CdStreamGetNumImages(void)
{
    return gNumImages;
}

#endif  // defined(GTA_PC) || defined(GTA_MOBILE) || defined(GAMECUBE)
#endif  // !_WIN32
