// gcm_vfs.c  —  GCM Virtual File System implementation
//
// Architecture:
//   GCM_VFS_Mount()
//     ├─ reads disc header (sector 0) to locate FST
//     ├─ reads FST into g_fst_raw_buf  (memalign'd, kept alive)
//     ├─ sets g_fst / g_strtab pointers into that buffer
//     └─ calls AddDevice(&s_gcm_devoptab)  -> newlib now routes
//        fopen("dvd:/x") through our callbacks
//
//   gcm_open_r   — FST path walk -> fill GCMFileHandle
//   gcm_read_r   — Bounce-buffer DMA read (handles misalignment)
//   gcm_seek_r   — Update current_pos
//   gcm_fstat_r  — Return size for an *open* fd (fstat syscall)
//   gcm_stat_r   — Return size for a *path*   (stat  syscall)
//   gcm_close_r  — No-op (nothing heap-allocated per handle)
//
// Why we keep FST in RAM permanently:
//   Every fopen/stat needs to walk the FST.  At ~34 KB the FST
//   for a typical GCM is small enough to keep resident.

#include "gcm_vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>    /* strcasecmp / strncasecmp */
#include <malloc.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/iosupport.h>   /* devoptab_t, AddDevice, RemoveDevice */

#include <gccore.h>           /* SYS_Report, memalign, u8/u32/s64 … */
#include <ogc/dvd.h>          /* DVD_ReadAbsPrio, dvdcmdblk          */
#include <ogc/mutex.h>
#ifdef WII
#include <di/di.h>
#include <ogc/cache.h>
#include <ogc/lwp_watchdog.h>
#include "../wii/di_compat.h"
#endif

/* ================================================================
 * GCM disc constants
 * ================================================================ */
#define GCM_SECTOR_SIZE    2048u
#define GCM_HDR_FST_OFF    0x0424u   /* u32 BE: GCN byte off / Wii word off */
#define GCM_HDR_FST_SZ     0x0428u   /* u32 BE: FST size in bytes         */
#define FST_ENTRY_SZ       12u       /* bytes per FST entry               */
#define FST_MAX_ENTRIES    100000u   /* sanity ceiling                    */
#define FST_MAX_SIZE       (4u*1024u*1024u)
#define WII_CLUSTER_SIZE   0x8000u
#define WII_CLUSTER_DATA   0x7C00u
#define WII_CLUSTER_HASH   0x0400u
#define WII_LOWMEM_FST_ADDR 0x80000038u
#define WII_LOWMEM_FST_SIZE 0x8000003Cu

/* ================================================================
 * Global VFS state
 * ================================================================ */

/* Original memalign'd block — the only pointer we free() */
static u8         *g_fst_raw_buf       = NULL;

/* Pointer to FST entry[0] inside g_fst_raw_buf (may be offset) */
static u8         *g_fst               = NULL;

/* Pointer to the string table (immediately after all entries)   */
static const char *g_strtab            = NULL;

/* Total number of FST entries (root.next field)                 */
static u32         g_fst_total_entries = 0u;
static u32         g_fst_size_bytes    = 0u;
static u32         g_strtab_size_bytes = 0u;

/* Disc-relative base offset. GameCube stays at 0; Wii uses
 * partition-relative DI_Read() so this remains 0 there too. */
static s64         g_disc_base_offset  = 0;
static bool        g_disc_uses_wii_clusters = false;
static bool        g_disc_wii_offsets_are_words = false;
static u32         g_open_sequence = 1u;
static mutex_t     g_disc_io_mutex = LWP_MUTEX_NULL;
static volatile bool g_vfs_shutting_down = false;

/* ================================================================
 * Open-file handle  (newlib allocates structSize bytes for us)
 * ================================================================ */
typedef struct {
    u32 disc_offset;   /* absolute byte offset of file on disc      */
    u32 file_size;     /* file size in bytes                        */
    u32 current_pos;   /* current read position (0 = start)         */
    u32 open_seq;      /* monotonic sequence for debugging          */
    u8 first_read_logged;
    char debug_path[96];
} GCMFileHandle;

static bool fst_find_file(const char *path, u32 *out_disc_off, u32 *out_file_sz);

/* ================================================================
 * Low-level helpers
 * ================================================================ */

static inline u32 be32(const u8 *p) {
    return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|(u32)p[3];
}

static inline u32 fst_nameoff(const u8 *e) {
    /* name offset is packed in bits [23:0] of bytes [1..3] */
    return ((u32)e[1]<<16)|((u32)e[2]<<8)|(u32)e[3];
}

static bool fst_entry_name(const u8 *e, const char **out_name, size_t *out_name_len)
{
    u32 name_off;
    const char *name;
    const void *name_end;
    size_t max_len;

    if (!g_strtab || g_strtab_size_bytes == 0u)
        return false;

    name_off = fst_nameoff(e);
    if (name_off >= g_strtab_size_bytes)
        return false;

    name = g_strtab + name_off;
    max_len = (size_t)(g_strtab_size_bytes - name_off);
    name_end = memchr(name, '\0', max_len);
    if (!name_end)
        return false;

    *out_name = name;
    if (out_name_len)
        *out_name_len = (const char *)name_end - name;
    return true;
}

static bool gcm_path_is_focus(const char *path)
{
    return strcasecmp(path, "text/american.gxt") == 0 ||
           strcasecmp(path, "models/hud.txd") == 0 ||
           strcasecmp(path, "models/gta3.img") == 0 ||
           strcasecmp(path, "models/gta3.dir") == 0 ||
           strcasecmp(path, "models/fonts.txd") == 0 ||
           strcasecmp(path, "txd/intro1.txd") == 0;
}

static void gcm_log_focus_lookup(const char *tag, const char *path, u32 disc_off, u32 file_sz)
{
    SYS_Report("[GCM_VFS] %s path='%s' disc=0x%08X size=%u\n",
               tag, path, disc_off, file_sz);
}

static void gcm_log_mount_focus_paths(void)
{
    static const char *const kFocusPaths[] = {
        "text/american.gxt",
        "models/hud.txd",
        "models/gta3.img",
        "models/gta3.dir",
        "models/fonts.txd",
        "txd/intro1.txd",
    };
    size_t i;

    for (i = 0; i < sizeof(kFocusPaths) / sizeof(kFocusPaths[0]); i++) {
        u32 disc_off = 0u;
        u32 file_sz = 0u;
        const char *path = kFocusPaths[i];

        if (fst_find_file(path, &disc_off, &file_sz))
            gcm_log_focus_lookup("mount-probe", path, disc_off, file_sz);
        else
            SYS_Report("[GCM_VFS] mount-probe MISSING path='%s'\n", path);
    }
}

/**
 * Synchronous DVD sector read.
 * buf  : MUST be 32-byte aligned  (use memalign(32,...))
 * len  : rounded up internally to the next 2048-byte boundary
 * off  : disc byte offset  (MUST be 2048-byte aligned)
 */
#ifdef WII
static bool wii_di_read_abs_aligned_chunked(void *buf, u32 len, s64 off)
{
    u8 *dst = (u8 *)buf;
    u32 pos = (u32)off;
    u32 remaining = len;

    while (remaining > 0) {
        u32 chunk = remaining;
        u32 cluster_left = WII_CLUSTER_SIZE - (pos & (WII_CLUSTER_SIZE - 1u));

        if (chunk > cluster_left)
            chunk = cluster_left;

        /* Keep each IOS DVD request sector-aligned and inside one Wii cluster. */
        if ((chunk & (GCM_SECTOR_SIZE - 1u)) != 0)
            chunk &= ~(GCM_SECTOR_SIZE - 1u);
        if (chunk == 0 || chunk > remaining)
            chunk = GCM_SECTOR_SIZE;

        /* IOS/DI writes into main RAM. Invalidate any cached lines first so
         * later CPU reads see the freshly DMA'd bytes instead of stale data. */
        DCInvalidateRange(dst, chunk);
        int ret = DI_Read(dst, chunk, g_disc_wii_offsets_are_words ? (pos >> 2) : pos);
        if (ret != 0) {
            u32 di_err = 0;
            int err_ret = DI_GetError(&di_err);
            SYS_Report("[GCM_VFS] DI chunk fail ret=%d derr=%d/0x%08X pos=0x%08X chunk=%u rem=%u total=%u word=%d\n",
                       ret, err_ret, di_err, pos, chunk, remaining, len,
                       g_disc_wii_offsets_are_words ? 1 : 0);
            return false;
        }
        DCInvalidateRange(dst, chunk);

        dst += chunk;
        pos += chunk;
        remaining -= chunk;
    }

    return true;
}
#endif

static bool dvd_read_abs_aligned(void *buf, u32 len, s64 off) {
#ifdef WII
    if (g_disc_uses_wii_clusters) {
		if ((off & (GCM_SECTOR_SIZE - 1u)) != 0)
			return false;
		return wii_di_read_abs_aligned_chunked(buf, len, off);
    }
#endif
	dvdcmdblk blk;
	u32 alen = (len + (GCM_SECTOR_SIZE - 1u)) & ~(GCM_SECTOR_SIZE - 1u);
	return DVD_ReadAbsPrio(&blk, buf, (s32)alen, off, 2) > 0;
}

static bool dvd_read_abs_exact(void *buf, u32 len, s64 off)
{
    u32 aligned_off = (u32)off & ~(GCM_SECTOR_SIZE - 1u);
    u32 skip = (u32)off - aligned_off;
    u32 alloc_sz = (skip + len + (GCM_SECTOR_SIZE - 1u)) & ~(GCM_SECTOR_SIZE - 1u);

    u8 *tmp = (u8 *)memalign(32u, alloc_sz);
    if (!tmp) {
        SYS_Report("[GCM_VFS] read exact bounce malloc fail off=0x%08X len=%u alloc=%u skip=%u\n",
                   (u32)off, len, alloc_sz, skip);
        return false;
    }

    bool ok = dvd_read_abs_aligned(tmp, alloc_sz, aligned_off);
    if (ok)
        memcpy(buf, tmp + skip, len);

    free(tmp);
    return ok;
}

static bool dvd_read_exact(void *buf, u32 len, s64 off)
{
    return dvd_read_abs_exact(buf, len, g_disc_base_offset + off);
}

static bool dvd_read_aligned(void *buf, u32 len, s64 off)
{
    return dvd_read_abs_aligned(buf, len, g_disc_base_offset + off);
}

#ifdef WII
static bool wii_partition_header_looks_valid(const u8 *hdr, u32 *out_fst_off, u32 *out_fst_size)
{
    u32 fst_off = be32(hdr + GCM_HDR_FST_OFF);
    u32 fst_size = be32(hdr + GCM_HDR_FST_SZ);

    if (out_fst_off)
        *out_fst_off = fst_off;
    if (out_fst_size)
        *out_fst_size = fst_size;

    return fst_off != 0u && fst_size >= (2u * FST_ENTRY_SZ) && fst_size <= FST_MAX_SIZE;
}

static bool wii_try_mount_fst_candidate(u32 fst_addr, u32 fst_size,
                                        const void *src, const char *tag)
{
    u32 alloc_sz;
    u8 *raw;
    u8 *fst;
    u32 total;
    const u8 *src_bytes = (const u8 *)src;

    DCInvalidateRange((void *)src, fst_size);

    alloc_sz = (fst_size + 31u) & ~31u;
    raw = (u8 *)memalign(32u, alloc_sz);
    if (!raw)
        return false;

    memcpy(raw, src, fst_size);
    fst = raw;
    total = be32(fst + 8);

    if (total < 2u || total > FST_MAX_ENTRIES || total * FST_ENTRY_SZ > fst_size) {
        SYS_Report("[GCM_VFS] lowmem FST %s rejected: total=%u size=%u "
                   "hdr=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
                   "src=%02X %02X %02X %02X\n",
                   tag, total, fst_size,
                   fst[0], fst[1], fst[2], fst[3],
                   fst[4], fst[5], fst[6], fst[7],
                   fst[8], fst[9], fst[10], fst[11],
                   src_bytes[0], src_bytes[1], src_bytes[2], src_bytes[3]);
        free(raw);
        return false;
    }

    g_fst_raw_buf       = raw;
    g_fst               = fst;
    g_strtab            = (const char *)(fst + total * FST_ENTRY_SZ);
    g_fst_total_entries = total;
    g_fst_size_bytes    = fst_size;
    g_strtab_size_bytes = fst_size - total * FST_ENTRY_SZ;

    SYS_Report("[GCM_VFS] Mounted from apploader RAM FST (%s) addr=0x%08X size=%u entries=%u\n",
               tag, fst_addr, fst_size, total);
    return true;
}

static bool wii_try_mount_fst_from_lowmem(void)
{
    u32 fst_addr = be32((const u8 *)WII_LOWMEM_FST_ADDR);
    u32 fst_size = be32((const u8 *)WII_LOWMEM_FST_SIZE);

    SYS_Report("[GCM_VFS] lowmem FST candidate addr=0x%08X size=%u\n",
               fst_addr, fst_size);

    if (fst_addr < 0x80000000u || fst_addr >= 0x94000000u) {
        SYS_Report("[GCM_VFS] lowmem FST addr rejected: 0x%08X\n", fst_addr);
        return false;
    }
    if (fst_size < (2u * FST_ENTRY_SZ) || fst_size > FST_MAX_SIZE) {
        SYS_Report("[GCM_VFS] lowmem FST size rejected: %u\n", fst_size);
        return false;
    }

    if (wii_try_mount_fst_candidate(fst_addr, fst_size, (const void *)fst_addr, "cached"))
        return true;

    if (fst_addr >= 0x80000000u && fst_addr < 0x84000000u) {
        const void *uncached = SYS_VirtualToUncached((const void *)fst_addr);
        if (wii_try_mount_fst_candidate(fst_addr, fst_size, uncached, "uncached"))
            return true;
    }

    return false;
}

static bool wii_init_disc_base_offset(void)
{
    u8 *disc_hdr = (u8 *)memalign(32u, GCM_SECTOR_SIZE);
    u8 *part_info = (u8 *)memalign(32u, GCM_SECTOR_SIZE);
    u8 entry[8];
    bool ok = false;

    g_disc_base_offset = 0;
    g_disc_uses_wii_clusters = false;
    g_disc_wii_offsets_are_words = false;

    if (!disc_hdr || !part_info)
        goto done;

    if (!dvd_read_abs_aligned(disc_hdr, GCM_SECTOR_SIZE, 0))
        goto done;

    if (be32(disc_hdr + 0x18) != 0x5D1C9EA3u) {
        ok = true;
        goto done;
    }

    if (!dvd_read_abs_exact(part_info, 0x20u, 0x40000u))
        goto done;

    {
        u32 part_count = be32(part_info + 0x00);
        u32 table_off = be32(part_info + 0x04) << 2;
        u32 part_off;
        u32 fallback_part_off = 0u;
        bool have_fallback = false;
        bool found_game_part = false;
        u32 lowmem_part_type = be32((const u8 *)0x80003194u);
        u32 lowmem_part_off = be32((const u8 *)0x80003198u) << 2;
        u32 i;
        u32 fst_off = 0u;
        u32 fst_size = 0u;
        u32 di_err = 0u;

        if (part_count == 0u)
            goto done;

        for (i = 0u; i < part_count; i++) {
            if (!dvd_read_abs_exact(entry, sizeof(entry), table_off + (s64)i * 8))
                goto done;

            if (be32(entry + 0x04) == 0u) {
                found_game_part = true;
                break;
            }

            if (!have_fallback) {
                fallback_part_off = be32(entry + 0x00);
                have_fallback = true;
            }
        }

        if (found_game_part)
            part_off = be32(entry + 0x00) << 2;
        else if (have_fallback)
            part_off = fallback_part_off << 2;
        else
            goto done;

        SYS_Report("[GCM_VFS] lowmem part type=0x%08X off=0x%08X, table part off=0x%08X\n",
                   lowmem_part_type, lowmem_part_off, part_off);

        if (lowmem_part_off != 0u)
            part_off = lowmem_part_off;

        {
            int di_init = DI_Init();
            SYS_Report("[GCM_VFS] DI_Init -> %d\n", di_init);
            if (di_init < 0)
                goto done;
        }

        {
            int di_read = DI_Read(part_info, GCM_SECTOR_SIZE, 0);
            bool header_ok;
            SYS_Report("[GCM_VFS] DI_Read(current_partition_header) -> %d\n", di_read);
            if (di_read == 0) {
                header_ok = wii_partition_header_looks_valid(part_info, &fst_off, &fst_size);
                SYS_Report("[GCM_VFS] current partition header fst_off=0x%08X fst_size=%u valid=%d\n",
                           fst_off, fst_size, header_ok ? 1 : 0);
                if (header_ok) {
                    g_disc_base_offset = 0;
                    g_disc_uses_wii_clusters = true;
                    g_disc_wii_offsets_are_words = true;
                    SYS_Report("[GCM_VFS] Using already-open Wii game partition\n");
                    ok = true;
                    goto done;
                }
            } else if (DI_GetError(&di_err) == 0) {
                SYS_Report("[GCM_VFS] DI_GetError after current partition read -> 0x%08X\n", di_err);
            }
        }

        {
            int di_part = DI_OpenPartition(part_off);
            SYS_Report("[GCM_VFS] DI_OpenPartition(0x%08X) -> %d\n", part_off, di_part);
            if (di_part != 0) {
                if (DI_GetError(&di_err) == 0)
                    SYS_Report("[GCM_VFS] DI_GetError after open partition -> 0x%08X\n", di_err);
                goto done;
            }
        }

        {
            int di_read = DI_Read(part_info, GCM_SECTOR_SIZE, 0);
            bool header_ok;
            SYS_Report("[GCM_VFS] DI_Read(opened_partition_header) -> %d\n", di_read);
            if (di_read != 0) {
                if (DI_GetError(&di_err) == 0)
                    SYS_Report("[GCM_VFS] DI_GetError after opened partition read -> 0x%08X\n", di_err);
                goto done;
            }

            header_ok = wii_partition_header_looks_valid(part_info, &fst_off, &fst_size);
            SYS_Report("[GCM_VFS] opened partition header fst_off=0x%08X fst_size=%u valid=%d\n",
                       fst_off, fst_size, header_ok ? 1 : 0);
            if (!header_ok)
                goto done;
        }

        g_disc_base_offset = 0;
        g_disc_uses_wii_clusters = true;
        g_disc_wii_offsets_are_words = true;
        SYS_Report("[GCM_VFS] Wii partition opened at 0x%08X\n", part_off);
        ok = true;
    }

done:
    if (!ok) {
        g_disc_base_offset = 0;
        g_disc_uses_wii_clusters = false;
        g_disc_wii_offsets_are_words = false;
    }
    if (disc_hdr) free(disc_hdr);
    if (part_info) free(part_info);
    return ok;
}
#endif

// ════════════════════════════════════════════════════════════
// fst_find_file
//
// g_fst 是 u8* 裸字节指针，GCM FST entry 格式 (12 字节/条目):
//
//   字节 0     : flags  bit0=1 → 目录, bit0=0 → 文件
//   字节 1-3   : name offset (字符串表偏移，用 fst_nameoff() 读取)
//   字节 4-7   : 文件: disc byte offset   / 目录: parent index
//   字节 8-11  : 文件: file length        / 目录: next_offset (目录范围结束)
//
// [NEW-FIX-12] 使用 strncasecmp 大小写不敏感匹配
//   游戏请求 "MODELS/FONTS.TXD"，FST 中存储 "models/fonts.txd"
//   原 strncmp 区分大小写 → 永远匹配不到 → 文件找不到
//
// ⚠ 注意：绝对不能用 g_fst[i].dir.xxx 结构体语法！
//   g_fst 是 u8*，必须用 be32() / fst_nameoff() 按偏移读取
// ════════════════════════════════════════════════════════════
static bool fst_find_file(const char *path,
                           u32        *out_disc_off,
                           u32        *out_file_sz)
{
    if (!g_fst || !path || path[0] == '\0') return false;

    /* ── entry[0] 是根节点 ──────────────────────────────────
     * 根节点 bytes 8-11 = 总 entry 数 (next_offset of root)
     * 用 be32(g_fst + 8) 读取，不是 g_fst[0].dir.next_offset
     * ─────────────────────────────────────────────────────── */
    u32 root_count = g_fst_total_entries;
    u32 dir_start  = 1u;
    u32 dir_end    = root_count;

    const char *seg     = path;
    const char *seg_end;

    while (*seg != '\0') {
        /* 跳过前导斜杠 */
        while (*seg == '/') seg++;
        if (*seg == '\0') break;

        /* 取出当前路径段 */
        seg_end = strchr(seg, '/');
        size_t seg_len;
        bool   is_last;
        if (seg_end == NULL) {
            seg_len = strlen(seg);
            is_last = true;
        } else {
            seg_len = (size_t)(seg_end - seg);
            is_last = false;
        }

        bool found = false;
        for (u32 i = dir_start; i < dir_end; i++) {
            /* ── 取第 i 条 entry 的起始指针 ─────────────────
             * 每条 entry 固定 FST_ENTRY_SZ(12) 字节
             * 绝对不能写 g_fst[i].file.xxx
             * ─────────────────────────────────────────────── */
            const u8   *e          = g_fst + i * FST_ENTRY_SZ;

            /* byte 0 bit0: 1=目录  0=文件 */
            bool        is_dir     = (e[0] & 1u) != 0u;

            /* bytes 1-3: name offset in string table */
            const char *entry_name;
            size_t      entry_name_len;

            if (!fst_entry_name(e, &entry_name, &entry_name_len))
                continue;

            /* [NEW-FIX-12] strncasecmp 大小写不敏感比较 */
            if (entry_name_len == seg_len &&
                strncasecmp(entry_name, seg, seg_len) == 0)
            {
                if (is_dir) {
                    /* 目录: bytes 8-11 = next_offset (目录范围结束 index) */
                    u32 child_end = be32(e + 8);
                    if (child_end < i + 1u || child_end > g_fst_total_entries) {
                        SYS_Report("[GCM_VFS] ERROR: invalid dir range idx=%u next=%u total=%u\n",
                                   i, child_end, g_fst_total_entries);
                        return false;
                    }
                    dir_start = i + 1u;
                    dir_end   = child_end;
                    found     = true;
                    break;
                } else if (is_last) {
                    /* 文件: bytes 4-7 = disc offset, bytes 8-11 = length */
                    *out_disc_off = be32(e + 4);
#ifdef WII
                    if (g_disc_uses_wii_clusters && g_disc_wii_offsets_are_words)
                        *out_disc_off <<= 2;
#endif
                    *out_file_sz  = be32(e + 8);
                    return true;
                }
            }
        }

        if (!found) return false;
        seg = seg_end ? seg_end : seg + seg_len;
    }

    return false;
}

/* ================================================================
 * devoptab callbacks
 * ================================================================ */

static int gcm_open_r(struct _reent *r,
                      void          *fileStruct,
                      const char    *path,
                      int            flags,
                      int            mode)
{
    (void)mode;
    GCMFileHandle *h = (GCMFileHandle *)fileStruct;

    /* We are read-only */
    if (flags & (O_WRONLY | O_RDWR)) {
        r->_errno = EROFS;
        return -1;
    }
    if (g_vfs_shutting_down || !g_fst) {
        r->_errno = ENODEV;
        return -1;
    }
	if (g_disc_io_mutex == LWP_MUTEX_NULL || LWP_MutexLock(g_disc_io_mutex) != 0) {
		r->_errno = EIO;
		return -1;
	}
	if (g_vfs_shutting_down || !g_fst) {
		LWP_MutexUnlock(g_disc_io_mutex);
		r->_errno = ENODEV;
		return -1;
	}

    /* ── 剥离 "dvd:" 前缀及前导斜杠 ────────────────────────
     * [FIX-VFS] 原代码只剥离 "dvd:"，结果 search_path = "/neo/neo.txd"
     * FST 树形查找分割路径时第一段为空字符串 ""
     * 永远匹配不到任何目录节点 → 所有文件查找全部失败
     * ─────────────────────────────────────────────────────── */
    const char *search_path = path;
    if (strncmp(search_path, "dvd:", 4) == 0) {
        search_path += 4;
    }
    while (*search_path == '/') {   /* [FIX-VFS] 剥离前导 / */
        search_path++;
    }
    /* dvd:/models/coll/peds.col  →  models/coll/peds.col  ✓ */

    /* [OLD-GCM_VFS] Per-open request trace used during VFS bring-up.
    SYS_Report("[GCM_VFS] fopen requested: '%s' -> searching FST for: '%s'\n",
               path, search_path); */

    u32 disc_off = 0u, file_sz = 0u;
    if (!fst_find_file(search_path, &disc_off, &file_sz)) {
        SYS_Report("[GCM_VFS] ERROR: '%s' not found in FST tree!\n", search_path);
        r->_errno = ENOENT;
		LWP_MutexUnlock(g_disc_io_mutex);
        return -1;
    }

    /* [OLD-GCM_VFS] Per-open success trace used during VFS bring-up.
    SYS_Report("[GCM_VFS] fopen OK: '%s' disc_off=0x%lx size=%lu\n",
               search_path, (unsigned long)disc_off, (unsigned long)file_sz); */

    h->disc_offset  = disc_off;
    h->file_size    = file_sz;
    h->current_pos  = 0u;
    h->open_seq     = g_open_sequence++;
    h->first_read_logged = 0u;
    snprintf(h->debug_path, sizeof(h->debug_path), "%s", search_path);

    if (gcm_path_is_focus(search_path)) {
        SYS_Report("[GCM_VFS] open seq=%u path='%s' disc=0x%08X size=%u flags=0x%X\n",
                   h->open_seq, h->debug_path, h->disc_offset, h->file_size, flags);
    }
	LWP_MutexUnlock(g_disc_io_mutex);
    return 0;
}

static int gcm_close_r(struct _reent *r, void *fd) {
    (void)r; (void)fd;
    return 0;
}

static ssize_t gcm_read_r(struct _reent *r,
                           void          *fd,
                           char          *ptr,
                           size_t         len)
{
    GCMFileHandle *h = (GCMFileHandle *)fd;

    if (g_vfs_shutting_down) {
        r->_errno = ENODEV;
        return -1;
    }
    if (h->current_pos >= h->file_size) return 0; /* EOF */

    /* Clamp to remaining bytes */
    size_t to_read = len;
    if (h->current_pos + to_read > h->file_size)
        to_read = h->file_size - h->current_pos;
    if (to_read == 0u) return 0;
	if (g_disc_io_mutex == LWP_MUTEX_NULL || LWP_MutexLock(g_disc_io_mutex) != 0) {
		r->_errno = EIO;
		return -1;
	}
	if (g_vfs_shutting_down) {
		LWP_MutexUnlock(g_disc_io_mutex);
		r->_errno = ENODEV;
		return -1;
	}


    u32 abs_off     = h->disc_offset + h->current_pos;
    u32 aligned_off = abs_off & ~(GCM_SECTOR_SIZE - 1u);
    u32 skip        = abs_off - aligned_off;

    /* Fast path: buffer + offset + length are all aligned */
    bool ptr_ok = (((uintptr_t)ptr & 31u) == 0u);
    bool off_ok = (skip == 0u);
    bool len_ok = ((to_read & (GCM_SECTOR_SIZE - 1u)) == 0u);
    bool use_fast_path = ptr_ok && off_ok && len_ok;

    if (!h->first_read_logged && h->current_pos == 0u && gcm_path_is_focus(h->debug_path)) {
        SYS_Report("[GCM_VFS] read0-path seq=%u path='%s' mode=%s ptr32=%d skip=%u req=%lu clamp=%lu\n",
                   h->open_seq, h->debug_path, use_fast_path ? "fast" : "bounce",
                   ptr_ok ? 1 : 0, skip, (unsigned long)len, (unsigned long)to_read);
    }

    if (use_fast_path) {
        if (!dvd_read_aligned(ptr, (u32)to_read, (s64)aligned_off)) {
            SYS_Report("[GCM_VFS] read fail aligned disc=0x%08X pos=%u req=%lu clamp=%lu file=%u aligned=0x%08X\n",
                       h->disc_offset, h->current_pos, (unsigned long)len,
                       (unsigned long)to_read, h->file_size, aligned_off);
            r->_errno = EIO;
			LWP_MutexUnlock(g_disc_io_mutex);
            return -1;
        }
        if (!h->first_read_logged && h->current_pos == 0u && gcm_path_is_focus(h->debug_path)) {
            const u8 *b = (const u8 *)ptr;
            u32 dump = (u32)(to_read < 16 ? to_read : 16);
            SYS_Report("[GCM_VFS] read0 seq=%u path='%s' got=%u bytes=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                       h->open_seq, h->debug_path, (u32)to_read,
                       dump > 0 ? b[0] : 0, dump > 1 ? b[1] : 0,
                       dump > 2 ? b[2] : 0, dump > 3 ? b[3] : 0,
                       dump > 4 ? b[4] : 0, dump > 5 ? b[5] : 0,
                       dump > 6 ? b[6] : 0, dump > 7 ? b[7] : 0,
                       dump > 8 ? b[8] : 0, dump > 9 ? b[9] : 0,
                       dump > 10 ? b[10] : 0, dump > 11 ? b[11] : 0,
                       dump > 12 ? b[12] : 0, dump > 13 ? b[13] : 0,
                       dump > 14 ? b[14] : 0, dump > 15 ? b[15] : 0);
            h->first_read_logged = 1u;
        }
        h->current_pos += (u32)to_read;
		LWP_MutexUnlock(g_disc_io_mutex);
        return (ssize_t)to_read;
    }

    /* Bounce buffer — static to avoid memalign/free heap fragmentation.
     * Single-sector reads hit the static buffer; larger reads fall back. */
    u32 alloc_sz = (skip + (u32)to_read + (GCM_SECTOR_SIZE - 1u))
                   & ~(GCM_SECTOR_SIZE - 1u);

    static u8 s_bounce[GCM_SECTOR_SIZE] ATTRIBUTE_ALIGN(32);
    u8 *bounce;
    bool must_free;

    if (alloc_sz <= GCM_SECTOR_SIZE) {
        bounce    = s_bounce;
        must_free = false;
    } else {
        bounce    = (u8 *)memalign(32u, alloc_sz);
        must_free = true;
    }

    if (!bounce) {
        SYS_Report("[GCM_VFS] read bounce malloc fail alloc=%u disc=0x%08X pos=%u req=%lu clamp=%lu file=%u\n",
                   alloc_sz, h->disc_offset, h->current_pos, (unsigned long)len,
                   (unsigned long)to_read, h->file_size);
        r->_errno = ENOMEM;
		LWP_MutexUnlock(g_disc_io_mutex);
        return -1;
    }

    if (!dvd_read_aligned(bounce, alloc_sz, (s64)aligned_off)) {
        if (must_free) free(bounce);
        SYS_Report("[GCM_VFS] read fail bounce disc=0x%08X pos=%u req=%lu clamp=%lu file=%u aligned=0x%08X alloc=%u skip=%u\n",
                   h->disc_offset, h->current_pos, (unsigned long)len,
                   (unsigned long)to_read, h->file_size, aligned_off, alloc_sz, skip);
        r->_errno = EIO;
		LWP_MutexUnlock(g_disc_io_mutex);
        return -1;
    }
    memcpy(ptr, bounce + skip, to_read);
    if (must_free) free(bounce);

    if (!h->first_read_logged && h->current_pos == 0u && gcm_path_is_focus(h->debug_path)) {
        const u8 *b = (const u8 *)ptr;
        u32 dump = (u32)(to_read < 16 ? to_read : 16);
        SYS_Report("[GCM_VFS] read0 seq=%u path='%s' got=%u bytes=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                   h->open_seq, h->debug_path, (u32)to_read,
                   dump > 0 ? b[0] : 0, dump > 1 ? b[1] : 0,
                   dump > 2 ? b[2] : 0, dump > 3 ? b[3] : 0,
                   dump > 4 ? b[4] : 0, dump > 5 ? b[5] : 0,
                   dump > 6 ? b[6] : 0, dump > 7 ? b[7] : 0,
                   dump > 8 ? b[8] : 0, dump > 9 ? b[9] : 0,
                   dump > 10 ? b[10] : 0, dump > 11 ? b[11] : 0,
                   dump > 12 ? b[12] : 0, dump > 13 ? b[13] : 0,
                   dump > 14 ? b[14] : 0, dump > 15 ? b[15] : 0);
        h->first_read_logged = 1u;
    }

    h->current_pos += (u32)to_read;
	LWP_MutexUnlock(g_disc_io_mutex);
    return (ssize_t)to_read;
}

static off_t gcm_seek_r(struct _reent *r, void *fd, off_t pos, int dir) {
    GCMFileHandle *h = (GCMFileHandle *)fd;
    off_t new_pos;

    switch (dir) {
        case SEEK_SET: new_pos = pos;                              break;
        case SEEK_CUR: new_pos = (off_t)h->current_pos + pos;     break;
        case SEEK_END: new_pos = (off_t)h->file_size   + pos;     break;
        default:
            r->_errno = EINVAL;
            return (off_t)-1;
    }

    if (new_pos < 0) {
        SYS_Report("[GCM_VFS] seek below start seq=%u path='%s' dir=%d req=%ld file=%u disc=0x%08X cur=%u\n",
                   h->open_seq, h->debug_path, dir, (long)new_pos,
                   h->file_size, h->disc_offset, h->current_pos);
        new_pos = 0;
    }
    if (new_pos > (off_t)h->file_size) {
        SYS_Report("[GCM_VFS] seek past EOF seq=%u path='%s' dir=%d req=%ld file=%u disc=0x%08X cur=%u\n",
                   h->open_seq, h->debug_path, dir, (long)new_pos,
                   h->file_size, h->disc_offset, h->current_pos);
        new_pos = (off_t)h->file_size;
    }

    h->current_pos = (u32)new_pos;
    return new_pos;
}

/* fstat_r: called when code does fstat(fd, &st) on an open file */
static int gcm_fstat_r(struct _reent *r, void *fd, struct stat *st) {
    (void)r;
    GCMFileHandle *h = (GCMFileHandle *)fd;
    memset(st, 0, sizeof(struct stat));
    st->st_size    = (off_t)h->file_size;
    st->st_blksize = 4096;   /* critical: non-zero → newlib enables FILE buffering */
    st->st_mode    = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
    st->st_nlink   = 1;
    return 0;
}

/* stat_r 也需要剥离前缀 */
static int gcm_stat_r(struct _reent *r, const char *path, struct stat *st) {
	if (g_vfs_shutting_down || !g_fst || g_disc_io_mutex == LWP_MUTEX_NULL || LWP_MutexLock(g_disc_io_mutex) != 0) {
		r->_errno = ENODEV;
		return -1;
	}
	if (g_vfs_shutting_down || !g_fst) {
		LWP_MutexUnlock(g_disc_io_mutex);
		r->_errno = ENODEV;
		return -1;
	}
    const char *search_path = path;
    if (strncmp(search_path, "dvd:", 4) == 0) {
        search_path += 4;
    }
    while (*search_path == '/') {
        search_path++;
    }

    u32 disc_off = 0u, file_sz = 0u;
    if (!fst_find_file(search_path, &disc_off, &file_sz)) {
        r->_errno = ENOENT;
		LWP_MutexUnlock(g_disc_io_mutex);
        return -1;
    }
    memset(st, 0, sizeof(struct stat));
    st->st_size  = (off_t)file_sz;
    st->st_mode  = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
    st->st_nlink = 1;
	LWP_MutexUnlock(g_disc_io_mutex);
    return 0;
}

/* ================================================================
 * Device table
 * ================================================================ */
static const devoptab_t s_gcm_devoptab = {
    .name        = "dvd",
    .structSize  = sizeof(GCMFileHandle),
    .open_r      = gcm_open_r,
    .close_r     = gcm_close_r,
    .write_r     = NULL,
    .read_r      = gcm_read_r,
    .seek_r      = gcm_seek_r,
    .fstat_r     = gcm_fstat_r,
    .stat_r      = gcm_stat_r,
};

/* ================================================================
 * Public API
 * ================================================================ */

bool GCM_VFS_Mount(void) {
    if (g_fst_raw_buf) return true;   /* idempotent */
	if (g_disc_io_mutex == LWP_MUTEX_NULL && LWP_MutexInit(&g_disc_io_mutex, false) != 0) {
		SYS_Report("[GCM_VFS] ERROR: cannot create disc I/O mutex\n");
		return false;
	}
	g_vfs_shutting_down = false;

    /* [OLD-GCM_VFS] One-time mount trace used during VFS bring-up.
    SYS_Report("[GCM_VFS] Mounting disc VFS...\n"); */

#ifdef WII
    {
        if (wii_try_mount_fst_from_lowmem()) {
            SYS_Report("[GCM_VFS] Mounted OK from lowmem. Total FST entries: %u, strtab=%u bytes\n",
                       g_fst_total_entries, g_strtab_size_bytes);
            gcm_log_mount_focus_paths();
            goto register_device;
        }

        if (!wii_init_disc_base_offset()) {
            SYS_Report("[GCM_VFS] ERROR: failed to open Wii game partition\n");
            return false;
        }
    }
#endif

    /* ---- 1. Read disc header (sector 0) ---- */
    u8 *hdr = (u8 *)memalign(32u, GCM_SECTOR_SIZE);
    if (!hdr) return false;

    if (!dvd_read_aligned(hdr, GCM_SECTOR_SIZE, 0)) {
        free(hdr);
        SYS_Report("[GCM_VFS] ERROR: cannot read disc header\n");
        return false;
    }

    u32 fst_off  = be32(hdr + GCM_HDR_FST_OFF);
    u32 fst_size = be32(hdr + GCM_HDR_FST_SZ);
    free(hdr);

#ifdef WII
    if (g_disc_uses_wii_clusters && g_disc_wii_offsets_are_words) {
        SYS_Report("[GCM_VFS] Wii header raw fst_off(words)=0x%08X fst_size(words)=%u\n",
                   fst_off, fst_size);
        fst_off <<= 2;
        fst_size <<= 2;
    }
#endif

    if (!fst_off || !fst_size || fst_size > FST_MAX_SIZE) {
        SYS_Report("[GCM_VFS] ERROR: bad FST header "
                   "(off=0x%08X size=%u)\n", fst_off, fst_size);
        return false;
    }
    /* [OLD-GCM_VFS] FST header trace used during VFS bring-up.
    SYS_Report("[GCM_VFS] FST disc_off=0x%08X size=%u\n",
               fst_off, fst_size); */

    /* ---- 2. Read FST ---- */
    s64 fst_sec_off = (s64)fst_off & ~(s64)(GCM_SECTOR_SIZE - 1u);
    u32 fst_skip    = fst_off - (u32)fst_sec_off;
    u32 fst_alloc   = (fst_skip + fst_size + GCM_SECTOR_SIZE - 1u)
                      & ~(GCM_SECTOR_SIZE - 1u);

    u8 *raw = (u8 *)memalign(32u, fst_alloc);
    if (!raw) return false;

    if (!dvd_read_aligned(raw, fst_alloc, fst_sec_off)) {
        free(raw);
        SYS_Report("[GCM_VFS] ERROR: cannot read FST\n");
        return false;
    }

    /* ---- 3. Validate and set up pointers ---- */
    u8  *fst   = raw + fst_skip;
    u32  total = be32(fst + 8);   /* root entry bytes 8-11 = total count */

    SYS_Report("[GCM_VFS] FST bytes @0x%08X size=%u hdr=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
               fst_off, fst_size,
               fst[0], fst[1], fst[2], fst[3],
               fst[4], fst[5], fst[6], fst[7],
               fst[8], fst[9], fst[10], fst[11]);

    if (total < 2u || total > FST_MAX_ENTRIES || total * FST_ENTRY_SZ > fst_size) {
        free(raw);
        SYS_Report("[GCM_VFS] ERROR: FST entry count invalid (%u, size=%u)\n",
                   total, fst_size);
        return false;
    }

    /* ---- 4. Commit state ---- */
    g_fst_raw_buf       = raw;
    g_fst               = fst;
    g_strtab            = (const char *)(fst + total * FST_ENTRY_SZ);
    g_fst_total_entries = total;
    g_fst_size_bytes    = fst_size;
    g_strtab_size_bytes = fst_size - total * FST_ENTRY_SZ;
    SYS_Report("[GCM_VFS] Mounted OK. Total FST entries: %u, strtab=%u bytes\n",
               total, g_strtab_size_bytes);
    gcm_log_mount_focus_paths();

register_device:
    /* ---- 5. Register with newlib ---- */
    AddDevice(&s_gcm_devoptab);

    /* [OLD-GCM_VFS] One-time successful mount trace used during VFS bring-up.
    SYS_Report("[GCM_VFS] Mounted OK. Total FST entries: %u\n", total);
    SYS_Report("[GCM_VFS] Use fopen(\"dvd:/path/to/file\", \"rb\")\n"); */
    return true;
}

void GCM_VFS_Unmount(void) {
	if (!g_fst_raw_buf) return;

	g_vfs_shutting_down = true;
	bool io_locked = g_disc_io_mutex != LWP_MUTEX_NULL && LWP_MutexLock(g_disc_io_mutex) == 0;

    RemoveDevice("dvd");

    free(g_fst_raw_buf);
    g_fst_raw_buf       = NULL;
    g_fst               = NULL;
    g_strtab            = NULL;
    g_fst_total_entries = 0u;
    g_fst_size_bytes    = 0u;
    g_strtab_size_bytes = 0u;
    g_disc_base_offset  = 0;
    g_disc_uses_wii_clusters = false;
    g_disc_wii_offsets_are_words = false;
	if (io_locked)
		LWP_MutexUnlock(g_disc_io_mutex);

    /* [OLD-GCM_VFS] Unmount trace used during VFS bring-up.
    SYS_Report("[GCM_VFS] Unmounted.\n"); */
}
