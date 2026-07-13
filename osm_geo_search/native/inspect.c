#include "osm_geo_search.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* We need internal OsmGeoDB struct for debugging */
struct OsmGeoDB {
    int      fd;
    uint8_t *map;
    size_t   map_size;
    uint32_t record_count;
    uint32_t addr_count;
    uint32_t named_count;
    int64_t  build_timestamp;
    char     region[47];
    uint32_t string_pool_off;
    uint32_t named_index_off;
    uint32_t addr_index_off;
    uint32_t records_off;
};

/* Internal helpers we need */
static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void walk_entry(const uint8_t **pp, const uint8_t *end) {
    const uint8_t *p = *pp;
    while (p < end && *p != '\0') p++;
    if (p >= end) { *pp = end; return; }
    p++;
    if (p >= end) { *pp = end; return; }
    uint8_t tlen = *p;
    p += 1 + tlen;
    *pp = p;
}

static const char *get_name(const uint8_t *pool_start, const uint8_t *end, uint32_t idx) {
    const uint8_t *p = pool_start + 8;  /* skip header */
    for (uint32_t i = 0; i < idx; i++) {
        walk_entry(&p, end);
        if (p >= end) return NULL;
    }
    if (p >= end) return NULL;
    return (const char *)p;
}

int main(void) {
    OsmGeoDB *db_ptr = osm_geo_open("russia-kaliningrad.bin");
    if (!db_ptr) { printf("FAIL open\n"); return 1; }

    struct OsmGeoDB *db = (struct OsmGeoDB *)db_ptr;
    const uint8_t *end = db->map + db->map_size;

    printf("String pool at %u, ni at %u\n", db->string_pool_off, db->named_index_off);

    /* Read first 10 named index entries and resolve names */
    printf("\n=== First 10 named index entries ===\n");
    for (int i = 0; i < 10; i++) {
        const uint8_t *ni = db->map + db->named_index_off + i * 9;
        uint16_t name_idx   = read_u16(ni);
        uint16_t translit_idx = read_u16(ni + 2);
        uint8_t  cat        = ni[4];
        uint32_t rec_off    = (uint32_t)ni[5] | ((uint32_t)ni[6] << 8) |
                              ((uint32_t)ni[7] << 16) | ((uint32_t)ni[8] << 24);

        const uint8_t *sp_start = db->map + db->string_pool_off;
        const char *name = get_name(sp_start, end, name_idx);

        printf("[%d] name_idx=%u translit_idx=%u cat=%u rec=%u name=\"%s\"\n",
               i, name_idx, translit_idx, cat, rec_off,
               name ? name : "NULL");
    }

    /* Print named names at positions 0, 1000, 31406 (last) to verify sorting */
    printf("\n=== Named names at various positions ===\n");
    for (int pos = 0; pos <= (int)db->named_count; pos += (db->named_count > 10 ? db->named_count / 10 : 1)) {
        if (pos > (int)db->named_count) pos = db->named_count;
        const uint8_t *ni = db->map + db->named_index_off + pos * 9;
        uint16_t name_idx = read_u16(ni);
        const uint8_t *sp_start = db->map + db->string_pool_off;
        const char *name = get_name(sp_start, end, name_idx);
        printf("  [%d] name_idx=%u name=\"%s\"\n", pos, name_idx, name ? name : "NULL");
        if (pos == db->named_count) break;
    }

    osm_geo_close(db_ptr);
    return 0;
}
