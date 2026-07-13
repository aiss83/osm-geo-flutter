#ifndef OSM_GEO_SEARCH_H
#define OSM_GEO_SEARCH_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Error codes ──────────────────────────────────────────────────── */

#define OSM_OK            0
#define OSM_ERR_OPEN     -1
#define OSM_ERR_MAGIC    -2
#define OSM_ERR_MMAP     -3
#define OSM_ERR_MEM      -4
#define OSM_ERR_RANGE    -5

/* ── Opaque handle ────────────────────────────────────────────────── */

typedef struct OsmGeoDB OsmGeoDB;

/* ── Result entry ─────────────────────────────────────────────────── */

typedef struct {
    double   lat;
    double   lon;
    char    *name;          /* POI name or address string (caller must free) */
    char    *translit;      /* transliterated name (may be NULL) */
    uint8_t  category;      /* category tag for Named; 0 for Address */
    uint8_t  type;          /* 0=Address, 1=Named */
} OsmGeoResult;

/* ── Lifecycle ────────────────────────────────────────────────────── */

/** Open a compact .bin file. Returns NULL on error; call osm_geo_error()
 *  to get the last error message. */
OsmGeoDB *osm_geo_open(const char *path);

/** Close and free all resources. */
void osm_geo_close(OsmGeoDB *db);

/** Last error message (not thread-safe). */
const char *osm_geo_error(void);

/* ── Queries ──────────────────────────────────────────────────────── */

/**
 * Prefix search by name (POI).
 *
 * @param db         Opened database.
 * @param query      UTF-8 search prefix (case-sensitive).
 * @param max_results Maximum results to return (0 = no limit).
 * @param out_count  Output: number of results written.
 * @return           Array of OsmGeoResult; caller must free with osm_geo_free_results().
 */
OsmGeoResult *osm_geo_search_by_name(const OsmGeoDB *db,
                                     const char *query,
                                     int max_results,
                                     int *out_count);

/**
 * Prefix search by address (city, street, housenumber).
 *
 * Query format: "city" or "city, street" or "city, street, house"
 * The search matches prefix in each component.
 *
 * @param db         Opened database.
 * @param query      UTF-8 address query.
 * @param max_results Maximum results to return (0 = no limit).
 * @param out_count  Output: number of results written.
 * @return           Array of OsmGeoResult; caller must free with osm_geo_free_results().
 */
OsmGeoResult *osm_geo_search_by_address(const OsmGeoDB *db,
                                        const char *query,
                                        int max_results,
                                        int *out_count);

/**
 * Full-text search across both name and address indices.
 * Returns combined results sorted by relevance.
 */
OsmGeoResult *osm_geo_search(const OsmGeoDB *db,
                             const char *query,
                             int max_results,
                             int *out_count);

/** Free results returned by any search function. */
void osm_geo_free_results(OsmGeoResult *results, int count);

#ifdef __cplusplus
}
#endif

#endif /* OSM_GEO_SEARCH_H */
