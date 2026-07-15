#include "osm_geo_search.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── ICU header: platform-specific ────────────────────────────────── */

#if defined(__ANDROID__)
#include <unicode/ustring.h>
#include <unicode/utypes.h>
#else
#include <unicode/ucasemap.h>
#include <unicode/utypes.h>
#endif

/* ── Binary format constants ──────────────────────────────────────── */

#define HEADER_SIZE   88
#define MAGIC         0x474D534F  /* "OSMG" little-endian */
#define VERSION       1

/* ── Packed structs matching the binary format ─────────────────────── */
#pragma pack(push, 1)

typedef struct {
    uint8_t  magic[4];
    uint16_t version;
    uint32_t record_count;
    uint32_t addr_count;
    uint32_t named_count;
    uint64_t build_timestamp;
    uint8_t  region[46];
    uint32_t string_pool_offset;
    uint32_t named_index_offset;
    uint32_t addr_index_offset;
    uint32_t records_offset;
} Header;

typedef struct {
    uint16_t name_idx;
    uint16_t translit_idx;
    uint8_t  category;
    uint32_t record_idx;
} NamedIndexEntry;

typedef struct {
    uint16_t city_idx;
    uint16_t street_idx;
    uint16_t housenumber_idx;
    uint32_t record_idx;
} AddrIndexEntry;

typedef struct {
    uint8_t  type;
    float    lat;
    float    lon;
} RecordBase;                    /* 9 bytes: type + lat + lon */

typedef struct {
    uint16_t city_idx;
    uint16_t street_idx;
    uint16_t housenumber_idx;
} RecordAddrFields;              /* 6 bytes */

typedef struct {
    uint16_t name_idx;
    uint16_t translit_idx;
    uint8_t  category;
} RecordNamedFields;             /* 5 bytes */

#pragma pack(pop)

/* ── Entry sizes (bytes) ──────────────────────────────────────────── */

#define NAMED_ENTRY_SIZE  sizeof(NamedIndexEntry)   /* 9 */
#define ADDR_ENTRY_SIZE   sizeof(AddrIndexEntry)    /* 10 */
#define ADDR_REC_SIZE     (sizeof(RecordBase) + sizeof(RecordAddrFields))   /* 15 */
#define NAMED_REC_SIZE    (sizeof(RecordBase) + sizeof(RecordNamedFields))  /* 14 */

/* ── Street lookup entry: sorted by folded_prefix for fast search ──── */

typedef struct {
    uint32_t folded_prefix;  /* first 4 bytes of case-folded street, zero-padded */
    uint32_t addr_entry_idx; /* index into Address Index */
} StreetLookup;

/* ── Database struct ──────────────────────────────────────────────── */

struct OsmGeoDB {
    int      fd;
    uint8_t *map;
    size_t   map_size;

    /* Header fields */
    uint32_t record_count;
    uint32_t addr_count;
    uint32_t named_count;
    int64_t  build_timestamp;
    char     region[47];

    /* Section offsets (bytes from file start) */
    uint32_t string_pool_off;
    uint32_t named_index_off;
    uint32_t addr_index_off;
    uint32_t records_off;

    /* Record offset lookup (record_idx → byte offset) */
    uint32_t *record_offsets;

    /* Street lookup: sorted by case-folded prefix → binary search */
    StreetLookup *street_lookup;
    uint32_t      street_lookup_count;

    /* Background index builder */
    pthread_t      index_thread;
    volatile int   index_ready;   /* 0 = building, 1 = done */

#if !defined(__ANDROID__)
    /* ICU case-folding map for UTF-8 case-insensitive comparison */
    UCaseMap *case_map;
#endif
};

/* ── Error state ──────────────────────────────────────────────────── */

static char g_error[256] = {0};

static void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, ap);
    va_end(ap);
}

const char *osm_geo_error(void) {
    return g_error;
}

/* ── Little-endian reader (used for string pool u16 lengths) ───────── */

static inline uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0]       | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ── String pool ──────────────────────────────────────────────────── */

/* String pool layout:
 *   [u32: entry_count]
 *   Entry N: [u16: byte_len] [len bytes of UTF-8]
 *
 * Length-prefixed, no NUL terminators. Index 0 is the first entry.
 */

#define SP_HEADER_SIZE  4

/* Return a malloc'd NUL-terminated copy of string at pool index idx. */
static char *sp_get(const OsmGeoDB *db, uint32_t idx) {
    const uint8_t *p = db->map + db->string_pool_off + SP_HEADER_SIZE;
    const uint8_t *end = db->map + db->map_size;

    for (uint32_t i = 0; i < idx; i++) {
        if (p + 2 > end) return NULL;
        uint16_t len = read_u16(p);
        p += 2 + len;
        if (p > end) return NULL;
    }
    if (p + 2 > end) return NULL;
    uint16_t len = read_u16(p);
    p += 2;
    if (p + len > end) return NULL;
    char *buf = malloc((size_t)len + 1);
    if (!buf) return NULL;
    memcpy(buf, p, len);
    buf[len] = '\0';
    return buf;
}

/* ── Platform-specific case-insensitive helpers ───────────────────── */

#if defined(__ANDROID__)

/* Fold a UTF-8 string to lowercase UChar buffer. Returns UChar length. */
static int32_t utf8_fold(const char *src, int32_t src_len,
                          UChar *dst, int32_t dst_cap, UErrorCode *ec) {
    UChar tmp[512];
    u_strFromUTF8(tmp, (int32_t)sizeof(tmp)/sizeof(UChar), NULL, src, src_len, ec);
    if (U_FAILURE(*ec)) return 0;
    return u_strToLower(dst, dst_cap, tmp, -1, NULL, ec);
}

/* ICU-based case-insensitive comparison of a query against a pool string.
 * Pool string at index idx has known byte length.  Returns <0, 0, >0. */
static int sp_cmp(const OsmGeoDB *db, uint32_t idx, const char *query) {
    const uint8_t *p  = db->map + db->string_pool_off + SP_HEADER_SIZE;
    const uint8_t *end = db->map + db->map_size;
    for (uint32_t i = 0; i < idx; i++) {
        if (p + 2 > end) return 1;
        p += 2 + read_u16(p);
        if (p > end) return 1;
    }
    if (p + 2 > end) return 1;
    uint16_t plen = read_u16(p);
    p += 2;
    if (p + plen > end) return 1;

    UErrorCode ec = U_ZERO_ERROR;
    UChar qbuf[256], pbuf[256];
    int32_t qlen = utf8_fold(query, -1, qbuf, 256, &ec);
    ec = U_ZERO_ERROR;
    int32_t flen = utf8_fold((const char *)p, plen, pbuf, 256, &ec);
    if (U_FAILURE(ec)) return 1;

    int n = qlen < flen ? qlen : flen;
    int diff = u_memcmp(qbuf, pbuf, (int32_t)n);
    if (diff != 0) return diff;
    if (qlen <= flen) return 0;
    return 1;
}

static int sp_is_prefix(const OsmGeoDB *db, uint32_t idx, const char *query) {
    const uint8_t *p  = db->map + db->string_pool_off + SP_HEADER_SIZE;
    const uint8_t *end = db->map + db->map_size;
    for (uint32_t i = 0; i < idx; i++) {
        if (p + 2 > end) return 0;
        p += 2 + read_u16(p);
        if (p > end) return 0;
    }
    if (p + 2 > end) return 0;
    uint16_t plen = read_u16(p);
    p += 2;
    if (p + plen > end) return 0;

    UErrorCode ec = U_ZERO_ERROR;
    UChar qbuf[256], pbuf[256];
    int32_t qlen = utf8_fold(query, -1, qbuf, 256, &ec);
    ec = U_ZERO_ERROR;
    utf8_fold((const char *)p, plen, pbuf, 256, &ec);
    if (U_FAILURE(ec)) return 0;

    return (qlen <= (int32_t)plen && u_memcmp(qbuf, pbuf, (int32_t)qlen) == 0);
}

#else /* !__ANDROID__ — macOS / desktop */

/* ICU-based case-insensitive comparison via ucasemap. */
static int sp_cmp(const OsmGeoDB *db, uint32_t idx, const char *query) {
    const uint8_t *p  = db->map + db->string_pool_off + SP_HEADER_SIZE;
    const uint8_t *end = db->map + db->map_size;
    for (uint32_t i = 0; i < idx; i++) {
        if (p + 2 > end) return 1;
        p += 2 + read_u16(p);
        if (p > end) return 1;
    }
    if (p + 2 > end) return 1;
    uint16_t plen = read_u16(p);
    p += 2;
    if (p + plen > end) return 1;

    if (!db->case_map) return 0;

    char    qbuf[512], pbuf[512];
    UErrorCode ec = U_ZERO_ERROR;
    int32_t qlen = ucasemap_utf8FoldCase(db->case_map, qbuf, (int32_t)sizeof(qbuf),
                                          query, -1, &ec);
    ec = U_ZERO_ERROR;
    int32_t flen = ucasemap_utf8FoldCase(db->case_map, pbuf, (int32_t)sizeof(pbuf),
                                          (const char *)p, plen, &ec);
    if (U_FAILURE(ec)) return 1;

    int n = qlen < flen ? qlen : flen;
    int diff = strncmp(qbuf, pbuf, (size_t)n);
    if (diff != 0) return diff;
    if (qlen <= flen) return 0;
    return 1;
}

static int sp_is_prefix(const OsmGeoDB *db, uint32_t idx, const char *query) {
    const uint8_t *p  = db->map + db->string_pool_off + SP_HEADER_SIZE;
    const uint8_t *end = db->map + db->map_size;
    for (uint32_t i = 0; i < idx; i++) {
        if (p + 2 > end) return 0;
        p += 2 + read_u16(p);
        if (p > end) return 0;
    }
    if (p + 2 > end) return 0;
    uint16_t plen = read_u16(p);
    p += 2;
    if (p + plen > end) return 0;

    if (!db->case_map) return 0;

    char    qbuf[512], pbuf[512];
    UErrorCode ec = U_ZERO_ERROR;
    int32_t qlen = ucasemap_utf8FoldCase(db->case_map, qbuf, (int32_t)sizeof(qbuf),
                                          query, -1, &ec);
    ec = U_ZERO_ERROR;
    ucasemap_utf8FoldCase(db->case_map, pbuf, (int32_t)sizeof(pbuf),
                          (const char *)p, plen, &ec);
    if (U_FAILURE(ec)) return 0;

    return (qlen <= (int32_t)plen && strncmp(qbuf, pbuf, (size_t)qlen) == 0);
}

#endif /* __ANDROID__ */

/* ── Named Index access ───────────────────────────────────────────── */

static void named_entry_read(const OsmGeoDB *db, uint32_t entry_idx,
                              uint16_t *name_idx, uint16_t *translit_idx,
                              uint8_t *category, uint32_t *record_idx) {
    NamedIndexEntry e;
    memcpy(&e, db->map + db->named_index_off + 4 + entry_idx * NAMED_ENTRY_SIZE,
           sizeof(e));
    *name_idx     = e.name_idx;
    *translit_idx = e.translit_idx;
    *category     = e.category;
    *record_idx   = e.record_idx;
}

static uint16_t named_name_idx(const OsmGeoDB *db, uint32_t entry_idx) {
    NamedIndexEntry e;
    memcpy(&e, db->map + db->named_index_off + 4 + entry_idx * NAMED_ENTRY_SIZE,
           sizeof(e));
    return e.name_idx;
}

/* ── Address Index access ─────────────────────────────────────────── */

static void addr_entry_read(const OsmGeoDB *db, uint32_t entry_idx,
                             uint16_t *city_idx, uint16_t *street_idx,
                             uint16_t *house_idx, uint32_t *record_idx) {
    AddrIndexEntry e;
    memcpy(&e, db->map + db->addr_index_off + 4 + entry_idx * ADDR_ENTRY_SIZE,
           sizeof(e));
    *city_idx   = e.city_idx;
    *street_idx = e.street_idx;
    *house_idx  = e.housenumber_idx;
    *record_idx = e.record_idx;
}

/* ── Record Block access ──────────────────────────────────────────── */

static int record_read(const OsmGeoDB *db, uint32_t record_idx,
                         uint8_t *type, float *lat, float *lon,
                         uint16_t *a, uint16_t *b, uint16_t *c, uint8_t *cat) {
    if (!db->record_offsets || record_idx >= db->record_count) return -1;
    const uint8_t *p = db->map + db->record_offsets[record_idx];

    RecordBase base;
    memcpy(&base, p, sizeof(base));
    *type = base.type;
    *lat  = base.lat;
    *lon  = base.lon;

    if (base.type == 0) {
        RecordAddrFields af;
        memcpy(&af, p + sizeof(RecordBase), sizeof(af));
        *a   = af.city_idx;
        *b   = af.street_idx;
        *c   = af.housenumber_idx;
        *cat = 0;
    } else {
        RecordNamedFields nf;
        memcpy(&nf, p + sizeof(RecordBase), sizeof(nf));
        *a   = nf.name_idx;
        *b   = nf.translit_idx;
        *cat = nf.category;
        *c   = 0;
    }
    return 0;
}

/* ── Forward declarations ──────────────────────────────────────────── */

static int street_lookup_cmp(const void *a, const void *b);

/* ── Background index builder thread ───────────────────────────────── */

static void *build_street_index_thread(void *arg) {
    OsmGeoDB *db = (OsmGeoDB *)arg;

    /* Build into a local buffer — db->street_lookup stays NULL until
     * everything is filled and sorted. This prevents searchFreeform
     * from seeing a partially-built index. */
    StreetLookup *lookup = malloc((size_t)db->addr_count * sizeof(StreetLookup));
    if (!lookup) {
        db->index_ready = 1; /* signal done (failed but won't retry) */
        return NULL;
    }

    for (uint32_t i = 0; i < db->addr_count; i++) {
        uint16_t c_idx, s_idx, h_idx;
        uint32_t rec_off;
        addr_entry_read(db, i, &c_idx, &s_idx, &h_idx, &rec_off);

        char *street = sp_get(db, s_idx);
        uint32_t prefix = 0;
        if (street && street[0]) {
#if defined(__ANDROID__)
            /* Only need first 2 UChar for the lookup key.
             * ICU fills ubuf with a prefix even on overflow. */
            UChar ubuf[2];
            UErrorCode ec = U_ZERO_ERROR;
            int32_t ulen = utf8_fold(street, -1, ubuf, 2, &ec);
            if (ulen > 0) {
                prefix = ((uint32_t)ubuf[0]) << 16;
                if (ulen >= 2) prefix |= (uint32_t)ubuf[1];
            }
#else
            /* Only need first 4 bytes of folded street for the lookup key.
             * ICU fills fbuf with a prefix even on buffer overflow. */
            char fbuf[4] = {0};
            UErrorCode ec = U_ZERO_ERROR;
            (void)ucasemap_utf8FoldCase(db->case_map, fbuf, 4, street, -1, &ec);
            memcpy(&prefix, fbuf, 4);
#endif
        }
        free(street);
        lookup[i].folded_prefix  = prefix;
        lookup[i].addr_entry_idx = i;
    }

    qsort(lookup, db->addr_count, sizeof(StreetLookup), street_lookup_cmp);

    /* Publish the completed index atomically (pointer write is atomic
     * on all supported platforms). Set index_ready AFTER publishing. */
    db->street_lookup_count = db->addr_count;
    db->street_lookup = lookup;
    db->index_ready = 1;
    return NULL;
}

/* ── Street lookup comparator for qsort ────────────────────────────── */

static int street_lookup_cmp(const void *a, const void *b) {
    uint32_t pa = ((const StreetLookup *)a)->folded_prefix;
    uint32_t pb = ((const StreetLookup *)b)->folded_prefix;
    if (pa < pb) return -1;
    if (pa > pb) return 1;
    return 0;
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

OsmGeoDB *osm_geo_open(const char *path) {
    return osm_geo_open_with_progress(path, NULL);
}

OsmGeoDB *osm_geo_open_with_progress(const char *path,
                                      OsmGeoProgress progress) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_error("Cannot open file: %s", path);
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        set_error("Cannot stat file: %s", path);
        close(fd);
        return NULL;
    }

    size_t sz = (size_t)st.st_size;
    if (sz < HEADER_SIZE) {
        set_error("File too small: %zu bytes (min %d)", sz, HEADER_SIZE);
        close(fd);
        return NULL;
    }

    uint8_t *map = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        set_error("mmap failed for %zu bytes", sz);
        close(fd);
        return NULL;
    }

    /* Validate and parse header */
    Header hdr;
    memcpy(&hdr, map, sizeof(hdr));
    if (memcmp(hdr.magic, "OSMG", 4) != 0) {
        uint32_t m;
        memcpy(&m, hdr.magic, 4);
        set_error("Bad magic: expected 0x%08X, got 0x%08X", MAGIC, m);
        munmap(map, sz);
        close(fd);
        return NULL;
    }
    if (hdr.version != VERSION) {
        set_error("Unsupported version: %d (expected %d)", hdr.version, VERSION);
        munmap(map, sz);
        close(fd);
        return NULL;
    }

    OsmGeoDB *db = calloc(1, sizeof(OsmGeoDB));
    if (!db) {
        set_error("Out of memory");
        munmap(map, sz);
        close(fd);
        return NULL;
    }

    db->fd       = fd;
    db->map      = map;
    db->map_size = sz;

    db->record_count    = hdr.record_count;
    db->addr_count      = hdr.addr_count;
    db->named_count     = hdr.named_count;
    db->build_timestamp = (int64_t)hdr.build_timestamp;
    memcpy(db->region, hdr.region, 46);
    db->region[46] = '\0';

    db->string_pool_off = hdr.string_pool_offset;
    db->named_index_off = hdr.named_index_offset;
    db->addr_index_off  = hdr.addr_index_offset;
    db->records_off     = hdr.records_offset;

    /* ── Metadata validation ──────────────────────────────────────── */

    /* All section offsets must be within file */
    if (db->string_pool_off >= sz ||
        db->named_index_off >= sz ||
        db->addr_index_off  >= sz ||
        db->records_off     >= sz) {
        set_error("Corrupt header: offset exceeds file size (%zu)", sz);
        goto fail;
    }

    /* Sections must start after header */
    if (db->string_pool_off < HEADER_SIZE ||
        db->named_index_off < HEADER_SIZE ||
        db->addr_index_off  < HEADER_SIZE ||
        db->records_off     < HEADER_SIZE) {
        set_error("Corrupt header: section offset inside header");
        goto fail;
    }

    /* Sections must be ordered: string_pool < named_index < addr_index < records */
    if (!(db->string_pool_off < db->named_index_off &&
          db->named_index_off < db->addr_index_off &&
          db->addr_index_off  < db->records_off)) {
        set_error("Corrupt header: sections out of order "
                  "(sp=%u ni=%u ai=%u rec=%u)",
                  db->string_pool_off, db->named_index_off,
                  db->addr_index_off, db->records_off);
        goto fail;
    }

    /* Index sizes must fit counts */
    if (db->named_count > 0) {
        size_t named_index_size = db->addr_index_off - db->named_index_off;
        size_t expected = (size_t)db->named_count * NAMED_ENTRY_SIZE;
        if (named_index_size < expected) {
            set_error("Corrupt header: named index too small "
                      "(%zu bytes for %u entries, need >= %zu)",
                      named_index_size, db->named_count, expected);
            goto fail;
        }
    }

    if (db->addr_count > 0) {
        size_t addr_index_size = db->records_off - db->addr_index_off;
        size_t expected = (size_t)db->addr_count * ADDR_ENTRY_SIZE;
        if (addr_index_size < expected) {
            set_error("Corrupt header: address index too small "
                      "(%zu bytes for %u entries, need >= %zu)",
                      addr_index_size, db->addr_count, expected);
            goto fail;
        }
    }

    /* Record count must cover both indices */
    if (db->record_count < db->addr_count + db->named_count) {
        set_error("Corrupt header: record_count (%u) < "
                  "addr_count (%u) + named_count (%u)",
                  db->record_count, db->addr_count, db->named_count);
        goto fail;
    }

    /* Build record offset lookup table */
    {
        const uint8_t *rp = db->map + db->records_off;
        uint32_t rcount = read_u32(rp);
        rp += 4;
        db->record_offsets = malloc((size_t)rcount * sizeof(uint32_t));
        if (!db->record_offsets) {
            set_error("Out of memory building record offsets");
            goto fail;
        }
        if (progress) progress(0, (int)rcount, "records");
        for (uint32_t i = 0; i < rcount; i++) {
            db->record_offsets[i] = (uint32_t)(rp - db->map);
            uint8_t rtype = *rp;
            rp += 1 + 8;
            rp += (rtype == 0) ? 6 : 5;
            if (progress && (i & 0x3FFF) == 0) /* every 16K */
                progress((int)i + 1, (int)rcount, "records");
        }
        if (progress) progress((int)rcount, (int)rcount, "records");
    }

    /* Create ICU case-folding map for UTF-8 case-insensitive comparison */
    {
#if !defined(__ANDROID__)
        UErrorCode ec = U_ZERO_ERROR;
        db->case_map = ucasemap_open(NULL, 0, &ec);
        if (U_FAILURE(ec)) {
            set_error("ICU: ucasemap_open failed");
            goto fail;
        }
#endif
    }

    /* Start background thread to build street lookup index.
     * DB is usable immediately — searchFulltext skips streets until
     * the index is ready. */
    db->index_ready = 0;
    db->street_lookup = NULL;
    db->street_lookup_count = 0;
    if (progress) progress(0, 0, "index");
    if (pthread_create(&db->index_thread, NULL,
                       build_street_index_thread, db) != 0) {
        set_error("Failed to start index builder thread");
        goto fail;
    }

    return db;

 fail:
    if (db) {
#if !defined(__ANDROID__)
        if (db->case_map) ucasemap_close(db->case_map);
#endif
        free(db->street_lookup);
        free(db->record_offsets);
    }
    free(db);
    munmap(map, sz);
    close(fd);
    return NULL;
}

void osm_geo_close(OsmGeoDB *db) {
    if (!db) return;
    /* Wait for background index builder to finish */
    if (!db->index_ready) {
        pthread_join(db->index_thread, NULL);
    }
#if !defined(__ANDROID__)
    if (db->case_map) ucasemap_close(db->case_map);
#endif
    free(db->street_lookup);
    free(db->record_offsets);
    if (db->map && db->map != MAP_FAILED) {
        munmap(db->map, db->map_size);
    }
    if (db->fd >= 0) {
        close(db->fd);
    }
    free(db);
}

/* ── Search: by name ──────────────────────────────────────────────── */

OsmGeoResult *osm_geo_search_by_name(const OsmGeoDB *db,
                                     const char *query,
                                     int max_results,
                                     int *out_count) {
    *out_count = 0;
    if (!db || !query || query[0] == '\0') return NULL;
    if (db->named_count == 0) return NULL;

    /* Binary search for first entry >= query */
    uint32_t lo = 0, hi = db->named_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint16_t mid_ni = named_name_idx(db, mid);
        int cmp = sp_cmp(db, mid_ni, query);
        if (cmp <= 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }

    /* Walk back to catch earlier case-insensitive matches */
    while (lo > 0) {
        uint16_t prev_ni = named_name_idx(db, lo - 1);
        if (!sp_is_prefix(db, prev_ni, query)) break;
        lo--;
    }

    /* Count matching entries */
    uint32_t count = 0;
    uint32_t idx = lo;
    uint32_t limit = (max_results > 0) ? (uint32_t)max_results : UINT32_MAX;
    while (idx < db->named_count && count < limit) {
        uint16_t ni = named_name_idx(db, idx);
        if (!sp_is_prefix(db, ni, query)) break;
        count++;
        idx++;
    }

    if (count == 0) return NULL;

    OsmGeoResult *results = calloc(count, sizeof(OsmGeoResult));
    if (!results) {
        set_error("Out of memory");
        return NULL;
    }

    idx = lo;
    for (uint32_t i = 0; i < count; i++, idx++) {
        uint16_t name_idx, translit_idx;
        uint8_t category;
        uint32_t record_byte_off;
        named_entry_read(db, idx, &name_idx, &translit_idx, &category, &record_byte_off);

        results[i].category = category;
        results[i].type     = 1;  /* Named */

        uint8_t rtype;
        float lat, lon;
        uint16_t a, b, c;
        uint8_t cat;
        if (record_read(db, record_byte_off, &rtype, &lat, &lon, &a, &b, &c, &cat) == 0) {
            results[i].lat = lat;
            results[i].lon = lon;
        }

        results[i].name = sp_get(db, name_idx);
        if (!results[i].name) results[i].name = strdup("");
        results[i].translit = sp_get(db, translit_idx);
    }

    *out_count = (int)count;
    return results;
}

/* ── Address query parsing ────────────────────────────────────────── */

/**
 * Parse address query like "Москва, Тверская, 7" into 3 components.
 * Returns number of components parsed (1–3).
 */
static int parse_addr_query(const char *query, char *city, char *street, char *house,
                             size_t max) {
    const char *p = query;
    size_t len;

    /* City */
    while (*p == ' ') p++;
    const char *start = p;
    while (*p && *p != ',') p++;
    len = (size_t)(p - start);
    if (len >= max) len = max - 1;
    memcpy(city, start, len);
    city[len] = '\0';
    /* Trim trailing space */
    while (len > 0 && city[len - 1] == ' ') city[--len] = '\0';

    if (*p != ',') return 1;
    p++; /* skip comma */

    /* Street */
    while (*p == ' ') p++;
    start = p;
    while (*p && *p != ',') p++;
    len = (size_t)(p - start);
    if (len >= max) len = max - 1;
    memcpy(street, start, len);
    street[len] = '\0';
    while (len > 0 && street[len - 1] == ' ') street[--len] = '\0';

    if (*p != ',') return 2;
    p++;

    /* House number */
    while (*p == ' ') p++;
    start = p;
    while (*p) p++;
    len = (size_t)(p - start);
    if (len >= max) len = max - 1;
    memcpy(house, start, len);
    house[len] = '\0';
    while (len > 0 && house[len - 1] == ' ') house[--len] = '\0';

    return 3;
}

/* ── Search: by address ───────────────────────────────────────────── */

OsmGeoResult *osm_geo_search_by_address(const OsmGeoDB *db,
                                        const char *query,
                                        int max_results,
                                        int *out_count) {
    *out_count = 0;
    if (!db || !query || query[0] == '\0') return NULL;
    if (db->addr_count == 0) return NULL;

    char city[256] = {0}, street[256] = {0}, house[256] = {0};
    int components = parse_addr_query(query, city, street, house, sizeof(city));

    /* Binary search on city_idx (full address string starts with city) */
    uint32_t lo = 0, hi = db->addr_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint16_t city_idx, street_idx, house_idx;
        uint32_t rec_off;
        addr_entry_read(db, mid, &city_idx, &street_idx, &house_idx, &rec_off);
        int cmp = sp_cmp(db, city_idx, city);
        if (cmp <= 0) { hi = mid; } else { lo = mid + 1; }
    }

    /* Walk back for case-insensitive matches */
    while (lo > 0) {
        uint16_t pc_idx, ps_idx, ph_idx;
        uint32_t p_off;
        addr_entry_read(db, lo - 1, &pc_idx, &ps_idx, &ph_idx, &p_off);
        if (!sp_is_prefix(db, pc_idx, city)) break;
        lo--;
    }

    /* Count matching entries */
    uint32_t count = 0;
    uint32_t limit = (max_results > 0) ? (uint32_t)max_results : UINT32_MAX;
    for (uint32_t idx = lo; idx < db->addr_count && count < limit; idx++) {
        uint16_t c_idx, s_idx, h_idx;
        uint32_t rec_off;
        addr_entry_read(db, idx, &c_idx, &s_idx, &h_idx, &rec_off);
        if (!sp_is_prefix(db, c_idx, city)) break;
        if (components >= 2 && street[0]) {
            if (!sp_is_prefix(db, s_idx, street)) continue;
        }
        if (components >= 3 && house[0]) {
            if (!sp_is_prefix(db, h_idx, house)) continue;
        }
        count++;
    }
    if (count == 0) return NULL;

    OsmGeoResult *results = calloc(count, sizeof(OsmGeoResult));
    if (!results) { set_error("Out of memory"); return NULL; }

    /* Fill results */
    uint32_t ri = 0;
    for (uint32_t idx = lo; idx < db->addr_count && ri < count; idx++) {
        uint16_t c_idx, s_idx, h_idx;
        uint32_t rec_off;
        addr_entry_read(db, idx, &c_idx, &s_idx, &h_idx, &rec_off);
        if (!sp_is_prefix(db, c_idx, city)) break;
        if (components >= 2 && street[0]) {
            if (!sp_is_prefix(db, s_idx, street)) continue;
        }
        if (components >= 3 && house[0]) {
            if (!sp_is_prefix(db, h_idx, house)) continue;
        }

        uint8_t rtype;
        float lat, lon;
        uint16_t a, b, c2;
        uint8_t cat;
        if (record_read(db, rec_off, &rtype, &lat, &lon, &a, &b, &c2, &cat) != 0)
            continue;

        results[ri].lat = lat;
        results[ri].lon = lon;
        results[ri].type = 0;
        results[ri].category = 0;

        /* Build full address: "city" | "city, street" | "city, street, house" */
        {
            char *cs = sp_get(db, c_idx);
            char *ss = sp_get(db, s_idx);
            char *hs = sp_get(db, h_idx);
            int csn = (cs && cs[0]) ? 1 : 0;
            int ssn = (ss && ss[0]) ? 1 : 0;
            int hsn = (hs && hs[0]) ? 1 : 0;
            size_t total = (csn ? strlen(cs) : 0) +
                           (ssn ? strlen(ss) + 2 : 0) +
                           (hsn ? strlen(hs) + 2 : 0) + 1;
            results[ri].name = malloc(total);
            if (results[ri].name) {
                results[ri].name[0] = '\0';
                if (csn) strcat(results[ri].name, cs);
                if (ssn) { strcat(results[ri].name, ", "); strcat(results[ri].name, ss); }
                if (hsn) { strcat(results[ri].name, ", "); strcat(results[ri].name, hs); }
            }
            free(cs); free(ss); free(hs);
            if (!results[ri].name) results[ri].name = strdup("");
        }
        ri++;
    }
    *out_count = (int)ri;
    return results;
}

/* ── Combined search ──────────────────────────────────────────────── */

OsmGeoResult *osm_geo_search(const OsmGeoDB *db,
                              const char *query,
                              int max_results,
                              int *out_count) {
    *out_count = 0;

    /* Search both indices and merge */
    int nc = 0, ac = 0;
    OsmGeoResult *named  = osm_geo_search_by_name(db, query, max_results, &nc);
    OsmGeoResult *addr   = osm_geo_search_by_address(db, query, max_results, &ac);

    int total = nc + ac;
    if (total == 0) {
        osm_geo_free_results(named, nc);
        osm_geo_free_results(addr, ac);
        return NULL;
    }

    if (max_results > 0 && total > max_results) total = max_results;

    OsmGeoResult *results = malloc((size_t)total * sizeof(OsmGeoResult));
    if (!results) {
        set_error("Out of memory");
        osm_geo_free_results(named, nc);
        osm_geo_free_results(addr, ac);
        return NULL;
    }

    int ri = 0;
    /* Named results first (higher priority) */
    int n_take = nc;
    if (max_results > 0 && n_take > max_results) n_take = max_results;
    for (int i = 0; i < n_take; i++) {
        results[ri] = named[i];
        /* Transfer ownership: clear source pointers */
        named[i].name = NULL;
        named[i].translit = NULL;
        ri++;
    }

    int remain = max_results > 0 ? (max_results - ri) : ac;
    if (remain < 0) remain = 0;
    if (remain > ac) remain = ac;
    for (int i = 0; i < remain; i++) {
        results[ri] = addr[i];
        addr[i].name = NULL;
        addr[i].translit = NULL;
        ri++;
    }

    osm_geo_free_results(named, nc);
    osm_geo_free_results(addr, ac);

    *out_count = ri;
    return results;
}

/* ── Helper: count UTF-8 codepoints (not bytes) ──────────────────── */

static int utf8_codepoint_count(const char *s) {
    int count = 0;
    while (*s) {
        /* Continuation bytes (0x80–0xBF, i.e. 10xxxxxx) don't start a new codepoint */
        if ((*s & 0xC0) != 0x80) count++;
        s++;
    }
    return count;
}

/* ── Query tokenizer: split on comma and space ─────────────────────── */

#define MAX_TOKENS    8
#define MAX_TOKEN_LEN 256

static int parse_fulltext_query(const char *query,
                                 char tokens[][MAX_TOKEN_LEN],
                                 int max_tokens) {
    int n = 0;
    const char *p = query;

    /* Detect separator: comma = structured, space = word-by-word */
    int has_comma = (strchr(query, ',') != NULL);

    while (*p && n < max_tokens) {
        /* Skip delimiters */
        if (has_comma) {
            while (*p == ' ' || *p == ',') p++;
        } else {
            while (*p == ' ') p++;
        }
        if (!*p) break;

        /* Collect token */
        const char *start = p;
        if (has_comma) {
            /* Comma mode: collect until next comma (spaces are part of token) */
            while (*p && *p != ',') p++;
            /* Trim trailing spaces from token */
            const char *end = p;
            while (end > start && (end[-1] == ' ')) end--;
            size_t len = (size_t)(end - start);
            if (len >= MAX_TOKEN_LEN) len = MAX_TOKEN_LEN - 1;
            memcpy(tokens[n], start, len);
            tokens[n][len] = '\0';
        } else {
            /* Space mode: collect until next space */
            while (*p && *p != ' ') p++;
            size_t len = (size_t)(p - start);
            if (len >= MAX_TOKEN_LEN) len = MAX_TOKEN_LEN - 1;
            memcpy(tokens[n], start, len);
            tokens[n][len] = '\0';
        }
        n++;
    }
    return n;
}

/* ── Pre-folded prefix check (no re-folding of query each call) ────── */

#if defined(__ANDROID__)

static int sp_is_prefix_folded(const OsmGeoDB *db, uint32_t idx,
                                const UChar *fq, int32_t fq_len) {
    const uint8_t *p  = db->map + db->string_pool_off + SP_HEADER_SIZE;
    const uint8_t *end = db->map + db->map_size;
    uint32_t i;
    for (i = 0; i < idx; i++) {
        if (p + 2 > end) return 0;
        p += 2 + read_u16(p);
        if (p > end) return 0;
    }
    if (p + 2 > end) return 0;
    uint16_t plen = read_u16(p);
    p += 2;
    if (p + plen > end) return 0;

    UErrorCode ec = U_ZERO_ERROR;
    UChar pbuf[256];
    int32_t flen = utf8_fold((const char *)p, plen, pbuf, 256, &ec);
    if (U_FAILURE(ec)) return 0;
    return (fq_len <= flen && u_memcmp(fq, pbuf, fq_len) == 0);
}

#else /* !__ANDROID__ */

static int sp_is_prefix_folded(const OsmGeoDB *db, uint32_t idx,
                                const char *fq, int32_t fq_len) {
    const uint8_t *p  = db->map + db->string_pool_off + SP_HEADER_SIZE;
    const uint8_t *end = db->map + db->map_size;
    uint32_t i;
    for (i = 0; i < idx; i++) {
        if (p + 2 > end) return 0;
        p += 2 + read_u16(p);
        if (p > end) return 0;
    }
    if (p + 2 > end) return 0;
    uint16_t plen = read_u16(p);
    p += 2;
    if (p + plen > end) return 0;

    if (!db->case_map) return 0;
    char pbuf[512];
    UErrorCode ec = U_ZERO_ERROR;
    int32_t flen = ucasemap_utf8FoldCase(db->case_map, pbuf, (int32_t)sizeof(pbuf),
                                          (const char *)p, plen, &ec);
    if (U_FAILURE(ec)) return 0;
    return (fq_len <= flen && strncmp(fq, pbuf, (size_t)fq_len) == 0);
}

#endif /* __ANDROID__ */

/* ── Candidate entry for scoring ───────────────────────────────────── */

typedef struct {
    int      score;
    uint8_t  is_named;      /* 1 = named index, 0 = address index */
    uint32_t entry_idx;     /* index into named or addr index */
    uint8_t  match_kind;    /* 0=city, 1=street, 2=name */
    uint32_t house_num;     /* numeric prefix of house (for sorting), 0=none */
} Candidate;

/* ── Free-form search (single token) ────────────────────────────────── */

static OsmGeoResult *search_freeform(const OsmGeoDB *db,
                                      const char *query,
                                      int max_results,
                                      int *out_count) {
    *out_count = 0;
    uint32_t cap = (db->named_count > db->addr_count) ? db->named_count : db->addr_count;
    cap = cap * 2 + 64;
    Candidate *cands = calloc(cap, sizeof(Candidate));
    if (!cands) { set_error("Out of memory"); return NULL; }
    uint32_t n_cands = 0;

#if defined(__ANDROID__)
    UChar fq[256];
    UErrorCode ec = U_ZERO_ERROR;
    int32_t fq_len = utf8_fold(query, -1, fq, 256, &ec);
    if (U_FAILURE(ec)) { free(cands); return NULL; }
#else
    char fq[512];
    UErrorCode ec = U_ZERO_ERROR;
    int32_t fq_len = ucasemap_utf8FoldCase(db->case_map, fq, (int32_t)sizeof(fq),
                                            query, -1, &ec);
    if (U_FAILURE(ec)) { free(cands); return NULL; }
#endif

    /* ── Named index: binary search for prefix matches ────────────── */
    {
        uint32_t lo = 0, hi = db->named_count;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            uint16_t mid_ni = named_name_idx(db, mid);
            int cmp = sp_cmp(db, mid_ni, query);
            if (cmp <= 0) { hi = mid; } else { lo = mid + 1; }
        }
        while (lo > 0) {
            uint16_t prev_ni = named_name_idx(db, lo - 1);
            if (!sp_is_prefix_folded(db, prev_ni, fq, fq_len)) break;
            lo--;
        }
        uint32_t idx = lo;
        while (idx < db->named_count && n_cands < cap) {
            uint16_t ni = named_name_idx(db, idx);
            int pfx = sp_is_prefix_folded(db, ni, fq, fq_len);
            char *dbg_nm = sp_get(db, ni);
            if (!pfx) { free(dbg_nm); break; }
            /* Exact match (same length) → top score, above cities */
            int sc = 3;
            if (dbg_nm && strlen(dbg_nm) == strlen(query)) sc = 6;
            free(dbg_nm);
            cands[n_cands].score      = sc;
            cands[n_cands].is_named   = 1;
            cands[n_cands].entry_idx  = idx;
            cands[n_cands].match_kind = 2;
            n_cands++; idx++;
        }
    }

    /* ── City matches: binary search (index is sorted by city) ──── */
    {
        uint32_t clo = 0, chi = db->addr_count;
        while (clo < chi) {
            uint32_t mid = clo + (chi - clo) / 2;
            uint16_t mid_ci, si, hi2; uint32_t ro;
            addr_entry_read(db, mid, &mid_ci, &si, &hi2, &ro);
            int cmp = sp_cmp(db, mid_ci, query);
            if (cmp <= 0) { chi = mid; } else { clo = mid + 1; }
        }
        while (clo > 0) {
            uint16_t pci, psi, phi; uint32_t po;
            addr_entry_read(db, clo - 1, &pci, &psi, &phi, &po);
            if (!sp_is_prefix_folded(db, pci, fq, fq_len)) break;
            clo--;
        }
        for (uint32_t ai = clo; ai < db->addr_count && n_cands < cap; ai++) {
            uint16_t cci, ssi, hhi; uint32_t rro;
            addr_entry_read(db, ai, &cci, &ssi, &hhi, &rro);
            if (!sp_is_prefix_folded(db, cci, fq, fq_len)) break;
            cands[n_cands].score      = 5;  /* city match — top priority */
            cands[n_cands].is_named   = 0;
            cands[n_cands].entry_idx  = ai;
            cands[n_cands].match_kind = 0;
            n_cands++;
        }
    }

    /* ── Street matches: binary search on street_lookup ──────────── */
    if (db->street_lookup) {
        /* Fold first 4 bytes of query for prefix comparison.
         * Small buffer is fine — ICU fills it with a prefix even on overflow. */
        uint32_t qprefix = 0;
#if defined(__ANDROID__)
        UChar ubuf[2];
        UErrorCode uec = U_ZERO_ERROR;
        int32_t ulen = utf8_fold(query, -1, ubuf, 2, &uec);
        if (ulen > 0) {
            qprefix = ((uint32_t)ubuf[0]) << 16;
            if (ulen >= 2) qprefix |= (uint32_t)ubuf[1];
        }
#else
        {
            char qfbuf[4] = {0};
            UErrorCode uec = U_ZERO_ERROR;
            (void)ucasemap_utf8FoldCase(db->case_map, qfbuf, 4, query, -1, &uec);
            memcpy(&qprefix, qfbuf, 4);
        }
#endif

        /* Binary search for first entry with same high 3 bytes */
        uint32_t qmasked = qprefix & 0xFFFFFF00;
        uint32_t slo = 0, shi = db->street_lookup_count;
        while (slo < shi) {
            uint32_t mid = slo + (shi - slo) / 2;
            if ((db->street_lookup[mid].folded_prefix & 0xFFFFFF00) >= qmasked) {
                shi = mid;
            } else {
                slo = mid + 1;
            }
        }

        /* Walk forward: fold the full street and verify prefix match */
        for (uint32_t si = slo; si < db->street_lookup_count && n_cands < cap; si++) {
            if ((db->street_lookup[si].folded_prefix & 0xFFFFFF00) != qmasked)
                break;

            uint32_t ai = db->street_lookup[si].addr_entry_idx;
            uint16_t c_idx, s_idx, h_idx;
            uint32_t rec_off;
            addr_entry_read(db, ai, &c_idx, &s_idx, &h_idx, &rec_off);

            /* Full ICU prefix verification */
            if (sp_is_prefix_folded(db, s_idx, fq, fq_len)) {
                int sc = (c_idx == 0) ? 2 : 4;
                cands[n_cands].score      = sc;
                cands[n_cands].is_named   = 0;
                cands[n_cands].entry_idx  = ai;
                cands[n_cands].match_kind = 1;
                n_cands++;
            }
        }
    }

    /* ── Sort by score descending (simple insertion for small sets) ─ */
    for (uint32_t i = 1; i < n_cands; i++) {
        Candidate tmp = cands[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && cands[j].score < tmp.score) {
            cands[j + 1] = cands[j];
            j--;
        }
        cands[j + 1] = tmp;
    }

    /* ── Build OsmGeoResult array ─────────────────────────────────── */
    uint32_t limit = n_cands;
    if (max_results > 0 && (uint32_t)max_results < limit) limit = (uint32_t)max_results;

    OsmGeoResult *results = calloc(limit, sizeof(OsmGeoResult));
    if (!results) { free(cands); set_error("Out of memory"); return NULL; }
    uint32_t out = 0;

    for (uint32_t ri = 0; ri < limit && out < limit; ri++) {
        if (cands[ri].is_named) {
            uint16_t name_idx, translit_idx;
            uint8_t category;
            uint32_t record_idx;
            named_entry_read(db, cands[ri].entry_idx,
                             &name_idx, &translit_idx, &category, &record_idx);

            uint8_t rtype; float lat, lon; uint16_t a, b, c2; uint8_t cat;
            if (record_read(db, record_idx, &rtype, &lat, &lon, &a, &b, &c2, &cat) == 0) {
                results[out].lat = lat;
                results[out].lon = lon;
            }
            results[out].category = category;
            results[out].type     = 1;
            results[out].name = sp_get(db, name_idx);
            if (!results[out].name) results[out].name = strdup("");
            results[out].translit = sp_get(db, translit_idx);
            results[out].city    = NULL;
            results[out].street  = NULL;
            results[out].house   = NULL;
            out++;
        } else {
            uint16_t c_idx, s_idx, h_idx;
            uint32_t record_idx;
            addr_entry_read(db, cands[ri].entry_idx,
                            &c_idx, &s_idx, &h_idx, &record_idx);

            uint8_t rtype; float lat, lon; uint16_t a, b, c2; uint8_t cat;
            if (record_read(db, record_idx, &rtype, &lat, &lon, &a, &b, &c2, &cat) == 0) {
                if (lat == 0.0f && lon == 0.0f) continue;
                results[out].lat = lat;
                results[out].lon = lon;
            }
            results[out].type     = 0;
            results[out].category = 0;

            results[out].city   = sp_get(db, c_idx);
            results[out].street = sp_get(db, s_idx);
            results[out].house  = sp_get(db, h_idx);

            /* Build display name: "city, street, house" */
            char *cs = results[out].city;
            char *ss = results[out].street;
            char *hs = results[out].house;
            int csn = (cs && cs[0]) ? 1 : 0;
            int ssn = (ss && ss[0]) ? 1 : 0;
            int hsn = (hs && hs[0]) ? 1 : 0;
            size_t total = (csn ? strlen(cs) : 0) +
                           (ssn ? strlen(ss) + 2 : 0) +
                           (hsn ? strlen(hs) + 2 : 0) + 1;
            results[out].name = malloc(total);
            if (results[out].name) {
                results[out].name[0] = '\0';
                if (csn) strcat(results[out].name, cs);
                if (ssn) { strcat(results[out].name, ", "); strcat(results[out].name, ss); }
                if (hsn) { strcat(results[out].name, ", "); strcat(results[out].name, hs); }
            } else {
                results[out].name = strdup("");
            }
            results[out].translit = NULL;
            out++;
        }
    }

    free(cands);
    *out_count = (int)out;
    return results;
}

/* ── Structured search (2+ tokens: city, [street], [house]) ────────── */

static OsmGeoResult *search_structured(const OsmGeoDB *db,
                                        char tokens[][MAX_TOKEN_LEN],
                                        int n_tokens,
                                        int max_results,
                                        int *out_count) {
    *out_count = 0;
    const char *city_q = tokens[0];
    char street_joined[MAX_TOKEN_LEN * 2];
    street_joined[0] = '\0';
    const char *street_q = NULL;
    const char *house_q  = NULL;

    if (n_tokens == 2) {
        street_q = tokens[1];
    } else if (n_tokens == 3) {
        street_q = tokens[1];
        house_q  = tokens[2];
    } else if (n_tokens >= 4) {
        /* Join tokens[1..n-2] as street, last token is house */
        int pos = 0;
        for (int t = 1; t < n_tokens - 1; t++) {
            if (t > 1) street_joined[pos++] = ' ';
            const char *tok = tokens[t];
            while (*tok && pos < (int)sizeof(street_joined) - 1)
                street_joined[pos++] = *tok++;
        }
        street_joined[pos] = '\0';
        street_q = street_joined;
        house_q  = tokens[n_tokens - 1];
    }

    /* Binary search for city prefix */
    uint32_t lo = 0, hi = db->addr_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint16_t mid_ci, si, hi2; uint32_t ro;
        addr_entry_read(db, mid, &mid_ci, &si, &hi2, &ro);
        int cmp = sp_cmp(db, mid_ci, city_q);
        if (cmp <= 0) { hi = mid; } else { lo = mid + 1; }
    }
    while (lo > 0) {
        uint16_t pci, psi, phi; uint32_t po;
        addr_entry_read(db, lo - 1, &pci, &psi, &phi, &po);
        if (!sp_is_prefix(db, pci, city_q)) break;
        lo--;
    }

    /* Determine city range end */
    uint32_t city_end = lo;
    while (city_end < db->addr_count) {
        uint16_t cei, sei, hei; uint32_t ro;
        addr_entry_read(db, city_end, &cei, &sei, &hei, &ro);
        if (!sp_is_prefix(db, cei, city_q)) break;
        city_end++;
    }

    /* Cap candidates */
    uint32_t cap = city_end - lo;
    if (street_q) cap += db->named_count;  /* room for named matches too */
    cap += 64;

    Candidate *cands = calloc(cap, sizeof(Candidate));
    if (!cands) { set_error("Out of memory"); return NULL; }
    uint32_t n_cands = 0;

    /* ── Collect address matches within city range ──────────────────── */
    for (uint32_t ai = lo; ai < city_end && n_cands < cap; ai++) {
        uint16_t c_idx, s_idx, h_idx;
        uint32_t rec_off;
        addr_entry_read(db, ai, &c_idx, &s_idx, &h_idx, &rec_off);

        if (street_q && !sp_is_prefix(db, s_idx, street_q)) continue;
        if (house_q && !sp_is_prefix(db, h_idx, house_q)) continue;

        int score = 6;
        if (house_q && street_q)      score = 6;  /* city + street + house */
        else if (street_q)            score = 5;  /* city + street */
        else                          score = 3;  /* city only */

        cands[n_cands].score      = score;
        cands[n_cands].is_named   = 0;
        cands[n_cands].entry_idx  = ai;
        cands[n_cands].match_kind = 1;
        n_cands++;
    }

    /* ── If street query: also search Named Index (city + POI) ──────── */
    if (street_q && db->named_count > 0 && n_cands < cap) {
        uint32_t nlo = 0, nhi = db->named_count;
        while (nlo < nhi) {
            uint32_t mid = nlo + (nhi - nlo) / 2;
            uint16_t mid_ni = named_name_idx(db, mid);
            int cmp = sp_cmp(db, mid_ni, street_q);
            if (cmp <= 0) { nhi = mid; } else { nlo = mid + 1; }
        }
        while (nlo > 0) {
            uint16_t prev_ni = named_name_idx(db, nlo - 1);
            if (!sp_is_prefix(db, prev_ni, street_q)) break;
            nlo--;
        }
        for (uint32_t ni = nlo; ni < db->named_count && n_cands < cap; ni++) {
            uint16_t name_ni = named_name_idx(db, ni);
            if (!sp_is_prefix(db, name_ni, street_q)) break;
            cands[n_cands].score      = 4;  /* city + POI */
            cands[n_cands].is_named   = 1;
            cands[n_cands].entry_idx  = ni;
            cands[n_cands].match_kind = 2;
            n_cands++;
        }
    }

    /* ── Sort by score descending ─────────────────────────────────── */
    for (uint32_t i = 1; i < n_cands; i++) {
        Candidate tmp = cands[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && cands[j].score < tmp.score) {
            cands[j + 1] = cands[j];
            j--;
        }
        cands[j + 1] = tmp;
    }

    /* ── Build OsmGeoResult array ─────────────────────────────────── */
    uint32_t limit = n_cands;
    if (max_results > 0 && (uint32_t)max_results < limit) limit = (uint32_t)max_results;

    OsmGeoResult *results = calloc(limit, sizeof(OsmGeoResult));
    if (!results) { free(cands); set_error("Out of memory"); return NULL; }
    uint32_t out = 0;

    for (uint32_t ri = 0; ri < limit && out < limit; ri++) {
        if (cands[ri].is_named) {
            uint16_t name_idx, translit_idx;
            uint8_t category;
            uint32_t record_idx;
            named_entry_read(db, cands[ri].entry_idx,
                             &name_idx, &translit_idx, &category, &record_idx);

            uint8_t rtype; float lat, lon; uint16_t a, b, c2; uint8_t cat;
            if (record_read(db, record_idx, &rtype, &lat, &lon, &a, &b, &c2, &cat) == 0) {
                results[out].lat = lat;
                results[out].lon = lon;
            }
            results[out].category = category;
            results[out].type     = 1;
            results[out].name = sp_get(db, name_idx);
            if (!results[out].name) results[out].name = strdup("");
            results[out].translit = sp_get(db, translit_idx);
            results[out].city    = NULL;
            results[out].street  = NULL;
            results[out].house   = NULL;
            out++;
        } else {
            uint16_t c_idx, s_idx, h_idx;
            uint32_t record_idx;
            addr_entry_read(db, cands[ri].entry_idx,
                            &c_idx, &s_idx, &h_idx, &record_idx);

            uint8_t rtype; float lat, lon; uint16_t a, b, c2; uint8_t cat;
            if (record_read(db, record_idx, &rtype, &lat, &lon, &a, &b, &c2, &cat) == 0) {
                results[out].lat = lat;
                if (lat == 0.0f && lon == 0.0f) continue;
                results[out].lon = lon;
            }
            results[out].type     = 0;
            results[out].category = 0;

            results[out].city   = sp_get(db, c_idx);
            results[out].street = sp_get(db, s_idx);
            results[out].house  = sp_get(db, h_idx);

            char *cs = results[out].city;
            char *ss = results[out].street;
            char *hs = results[out].house;
            int csn = (cs && cs[0]) ? 1 : 0;
            int ssn = (ss && ss[0]) ? 1 : 0;
            int hsn = (hs && hs[0]) ? 1 : 0;
            size_t total = (csn ? strlen(cs) : 0) +
                           (ssn ? strlen(ss) + 2 : 0) +
                           (hsn ? strlen(hs) + 2 : 0) + 1;
            results[out].name = malloc(total);
            if (results[out].name) {
                results[out].name[0] = '\0';
                if (csn) strcat(results[out].name, cs);
                if (ssn) { strcat(results[out].name, ", "); strcat(results[out].name, ss); }
                if (hsn) { strcat(results[out].name, ", "); strcat(results[out].name, hs); }
            } else {
                results[out].name = strdup("");
            }
            results[out].translit = NULL;
            out++;
        }
    }

    free(cands);
    *out_count = (int)out;
    return results;
}

/* ── Natural house number comparison ─────────────────────────────────── */

/* Parse numeric prefix of a house string: "196"→196, "184А"→184, ""→0 */
static uint32_t parse_house_num(const char *s) {
    if (!s || !s[0]) return 0;
    uint32_t n = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (uint32_t)(*s - '0');
        s++;
    }
    return n;
}

static int house_cmp(const void *a, const void *b) {
    const Candidate *ca = (const Candidate *)a;
    const Candidate *cb = (const Candidate *)b;
    if (ca->score != cb->score) return cb->score - ca->score; /* desc */
    if (ca->house_num != cb->house_num)
        return (ca->house_num > cb->house_num) ? 1 : -1;      /* asc */
    return 0;
}

/* ── Build results from scored candidates (shared helper) ────────────── */

static OsmGeoResult *_build_candidate_results(const OsmGeoDB *db,
                                               Candidate *cands,
                                               uint32_t n_cands,
                                               int max_results,
                                               int *out_count) {
    *out_count = 0;
    if (n_cands == 0) return NULL;
    if (!cands) return NULL;

    uint32_t limit = n_cands;
    if (max_results > 0 && (uint32_t)max_results < limit) limit = (uint32_t)max_results;

    OsmGeoResult *results = calloc(limit, sizeof(OsmGeoResult));
    if (!results) return NULL;

    uint32_t out = 0;
    for (uint32_t ri = 0; ri < limit && out < limit; ri++) {
        if (cands[ri].is_named) {
            uint16_t name_idx, translit_idx;
            uint8_t category;
            uint32_t record_idx;
            named_entry_read(db, cands[ri].entry_idx,
                             &name_idx, &translit_idx, &category, &record_idx);
            uint8_t rtype; float lat, lon; uint16_t a, b, c2; uint8_t cat;
            if (record_read(db, record_idx, &rtype, &lat, &lon, &a, &b, &c2, &cat) == 0) {
                results[out].lat = lat;
                results[out].lon = lon;
            }
            results[out].category = category;
            results[out].type     = 1;
            results[out].name = sp_get(db, name_idx);
            if (!results[out].name) results[out].name = strdup("");
            results[out].translit = sp_get(db, translit_idx);
            results[out].city    = NULL;
            results[out].street  = NULL;
            results[out].house   = NULL;
            out++;
        } else {
            uint16_t c_idx, s_idx, h_idx;
            uint32_t record_idx;
            addr_entry_read(db, cands[ri].entry_idx,
                            &c_idx, &s_idx, &h_idx, &record_idx);
            uint8_t rtype; float lat, lon; uint16_t a, b, c2; uint8_t cat;
            if (record_read(db, record_idx, &rtype, &lat, &lon, &a, &b, &c2, &cat) == 0) {
                if (lat == 0.0f && lon == 0.0f) continue;
                results[out].lat = lat;
                results[out].lon = lon;
            }
            results[out].type     = 0;
            results[out].category = 0;
            results[out].city   = sp_get(db, c_idx);
            results[out].street = sp_get(db, s_idx);
            results[out].house  = sp_get(db, h_idx);
            char *cs = results[out].city;
            char *ss = results[out].street;
            char *hs = results[out].house;
            int csn = (cs && cs[0]) ? 1 : 0;
            int ssn = (ss && ss[0]) ? 1 : 0;
            int hsn = (hs && hs[0]) ? 1 : 0;
            size_t total = (csn ? strlen(cs) : 0) +
                           (ssn ? strlen(ss) + 2 : 0) +
                           (hsn ? strlen(hs) + 2 : 0) + 1;
            results[out].name = malloc(total);
            if (results[out].name) {
                results[out].name[0] = '\0';
                if (csn) strcat(results[out].name, cs);
                if (ssn) { strcat(results[out].name, ", "); strcat(results[out].name, ss); }
                if (hsn) { strcat(results[out].name, ", "); strcat(results[out].name, hs); }
            } else {
                results[out].name = strdup("");
            }
            results[out].translit = NULL;
            out++;
        }
    }
    *out_count = (int)out;
    return results;
}

/* ── Full-text search: public API ────────────────────────────────────── */

OsmGeoResult *osm_geo_search_fulltext(const OsmGeoDB *db,
                                       const char *query,
                                       int max_results,
                                       int *out_count) {
    *out_count = 0;
    if (!db || !query || !query[0]) return NULL;

    /* Minimum 3 UTF-8 codepoints */
    if (utf8_codepoint_count(query) < 3) {
        set_error("Query too short: minimum 3 characters required");
        return NULL;
    }

    /* Tokenize */
    char tokens[MAX_TOKENS][MAX_TOKEN_LEN];
    int n_tokens = parse_fulltext_query(query, tokens, MAX_TOKENS);

    if (n_tokens <= 0) return NULL;


    /* 1 token → free-form search */
    if (n_tokens == 1) {
        return search_freeform(db, tokens[0], max_results, out_count);
    }

    /* 2+ tokens → try structured (city-based) search */
    /* Validate street/house tokens have minimum length if present */
    if (n_tokens >= 2 && utf8_codepoint_count(tokens[1]) < 1) {
        /* Join all tokens and search as free-form */
        char joined[512];
        int pos = 0;
        for (int t = 0; t < n_tokens && pos < (int)sizeof(joined) - 1; t++) {
            if (t > 0) joined[pos++] = ' ';
            const char *tok = tokens[t];
            while (*tok && pos < (int)sizeof(joined) - 1) joined[pos++] = *tok++;
        }
        joined[pos] = '\0';
        return search_freeform(db, joined, max_results, out_count);
    }

    /* Check if city actually exists in the data */
    {
        uint32_t lo = 0, hi = db->addr_count;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            uint16_t mid_ci, si, hi2; uint32_t ro;
            addr_entry_read(db, mid, &mid_ci, &si, &hi2, &ro);
            int cmp = sp_cmp(db, mid_ci, tokens[0]);
            if (cmp <= 0) { hi = mid; } else { lo = mid + 1; }
        }
        int city_found = 0;
        if (lo < db->addr_count) {
            uint16_t cci, ssi, hhi; uint32_t rro;
            addr_entry_read(db, lo, &cci, &ssi, &hhi, &rro);
            city_found = sp_is_prefix(db, cci, tokens[0]);
            char *cname = sp_get(db, cci);
            free(cname);
        }
        if (!city_found) {
            /* City not found. Try street+house across all cities:
             * - Comma mode with 2+ tokens: "Московский проспект, 1"
             * - Space mode with 3+ tokens: "Московский проспект 1"
             *   (last token is house, rest is street).
             * Otherwise fall back to free-form. */
            int try_street_house = (strchr(query, ',') != NULL && n_tokens >= 2)
                                || (n_tokens >= 3);
            if (try_street_house && db->street_lookup) {
                /* Street + house: search across all cities */
                char street_q[512], house_q[256];
                int pos = 0;
                for (int t = 0; t < n_tokens - 1; t++) {
                    if (t > 0) street_q[pos++] = ' ';
                    const char *tok = tokens[t];
                    while (*tok && pos < (int)sizeof(street_q) - 1)
                        street_q[pos++] = *tok++;
                }
                street_q[pos] = '\0';
                const char *hq = tokens[n_tokens - 1];
                /* Copy house query */
                pos = 0;
                while (hq[pos] && pos < (int)sizeof(house_q) - 1) {
                    house_q[pos] = hq[pos]; pos++;
                }
                house_q[pos] = '\0';

                /* Fold street query for street_lookup binary search */
                uint32_t qprefix = 0;
                {
                    char fbuf[4] = {0};
                    UErrorCode ec = U_ZERO_ERROR;
                    (void)ucasemap_utf8FoldCase(db->case_map, fbuf, 4,
                                                 street_q, -1, &ec);
                    memcpy(&qprefix, fbuf, 4);
                }
                uint32_t qmasked = qprefix & 0xFFFFFF00;

                /* Binary search street_lookup */
                uint32_t slo = 0, shi = db->street_lookup_count;
                while (slo < shi) {
                    uint32_t mid = slo + (shi - slo) / 2;
                    if ((db->street_lookup[mid].folded_prefix & 0xFFFFFF00)
                        >= qmasked) {
                        shi = mid;
                    } else {
                        slo = mid + 1;
                    }
                }

                /* Collect candidates with house filter */
                uint32_t cap = db->addr_count + 64;
                Candidate *cands = calloc(cap, sizeof(Candidate));
                uint32_t n = 0;
                if (cands) {
                    for (uint32_t si = slo;
                         si < db->street_lookup_count && n < cap; si++) {
                        if ((db->street_lookup[si].folded_prefix & 0xFFFFFF00)
                            != qmasked) break;
                        uint32_t ai = db->street_lookup[si].addr_entry_idx;
                        uint16_t c_idx, s_idx, h_idx;
                        uint32_t rec_off;
                        addr_entry_read(db, ai, &c_idx, &s_idx, &h_idx, &rec_off);
                        /* Full street prefix check + house filter */
                        if (sp_is_prefix(db, s_idx, street_q) &&
                            sp_is_prefix(db, h_idx, house_q)) {
                            cands[n].score = (c_idx == 0) ? 2 : 4;
                            cands[n].is_named = 0;
                            cands[n].entry_idx = ai;
                            cands[n].match_kind = 1;
                            /* Parse house number for natural sorting */
                            char *hs = sp_get(db, h_idx);
                            cands[n].house_num = parse_house_num(hs);
                            free(hs);
                            n++;
                        }
                    }
                    /* Sort by score desc, then house number asc */
                    qsort(cands, n, sizeof(Candidate), house_cmp);
                    /* Build and return results */
                    OsmGeoResult *results = _build_candidate_results(
                        db, cands, n, max_results, out_count);
                    if (results) { free(cands); return results; }
                    free(cands);
                }
            }

            /* Fall back to free-form with full query */
            char joined[512];
            int pos = 0;
            for (int t = 0; t < n_tokens && pos < (int)sizeof(joined) - 1; t++) {
                if (t > 0) joined[pos++] = ' ';
                const char *tok = tokens[t];
                while (*tok && pos < (int)sizeof(joined) - 1) joined[pos++] = *tok++;
            }
            joined[pos] = '\0';
            return search_freeform(db, joined, max_results, out_count);
        }
    }

    return search_structured(db, tokens, n_tokens, max_results, out_count);
}

/* ── Index ready check ────────────────────────────────────────────── */

int osm_geo_is_index_ready(const OsmGeoDB *db) {
    return (db && db->index_ready) ? 1 : 0;
}

/* ── Version ─────────────────────────────────────────────────────── */

const char *osm_geo_version(void) {
    return "osm_geo_search 0.2.0 (2026-07-15T12:00 fixed dylib)";
}

/* ── Free results ─────────────────────────────────────────────────── */

void osm_geo_free_results(OsmGeoResult *results, int count) {
    if (!results) return;
    for (int i = 0; i < count; i++) {
        free(results[i].name);
        free(results[i].translit);
        free(results[i].city);
        free(results[i].street);
        free(results[i].house);
    }
    free(results);
}