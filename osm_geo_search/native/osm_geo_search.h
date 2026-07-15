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
    char    *name;          /* POI name or full address string (caller must free) */
    char    *translit;      /* transliterated name (may be NULL) */
    char    *city;          /* city / settlement name (may be NULL for POI) */
    char    *street;        /* street name (may be NULL) */
    char    *house;         /* house number (may be NULL) */
    uint8_t  category;      /* category tag for Named; 0 for Address */
    uint8_t  type;          /* 0=Address, 1=Named */
} OsmGeoResult;

/* ── Progress callback ────────────────────────────────────────────── */

/**
 * Called during osm_geo_open_with_progress() to report build progress.
 *
 * @param done   Items processed so far.
 * @param total  Total items to process.
 * @param phase  Current phase (e.g. "records", "streets", "sort").
 */
typedef void (*OsmGeoProgress)(int done, int total, const char *phase);

/* ── Lifecycle ────────────────────────────────────────────────────── */

/** Open a compact .bin file. Returns NULL on error; call osm_geo_error()
 *  to get the last error message. */
OsmGeoDB *osm_geo_open(const char *path);

/**
 * Open with progress reporting.
 *
 * @param path      Path to .bin file.
 * @param progress  Callback for build progress (may be NULL).
 *                  Called from the calling thread during construction.
 * @return          Database handle, or NULL on error.
 */
OsmGeoDB *osm_geo_open_with_progress(const char *path,
                                      OsmGeoProgress progress);

/** Close and free all resources. */
void osm_geo_close(OsmGeoDB *db);

/** Last error message (not thread-safe). */
const char *osm_geo_error(void);

/* ── Queries ──────────────────────────────────────────────────────── */

/**
 * Prefix search by name (POI).
 *
 * @param db         Opened database.
 * @param query      UTF-8 search prefix (case-insensitive).
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

/**
 * Intelligent full-text search with automatic mode detection.
 *
 * If query contains no separator (comma / two+ spaces), it searches
 * as a prefix simultaneously across POI names, cities, and streets.
 *
 * If query has separators, it's treated as structured address:
 *   "city street" or "city, street" or "city, street, house"
 *
 * Results include structured fields (city, street, house) for address
 * matches and are ranked by relevance:
 *   - Named (POI) matches come first
 *   - Then street matches, then city matches
 *
 * Minimum query length: 3 UTF-8 codepoints.
 *
 * @param db         Opened database.
 * @param query      UTF-8 search query.
 * @param max_results Maximum results to return (0 = no limit).
 * @param out_count  Output: number of results written.
 * @return           Array of OsmGeoResult; caller must free with osm_geo_free_results().
 */
OsmGeoResult *osm_geo_search_fulltext(const OsmGeoDB *db,
                                       const char *query,
                                       int max_results,
                                       int *out_count);

/**
 * Check whether the background street index is fully built.
 * Returns 1 when ready, 0 while still building.
 * DB is usable immediately after open(); street search results
 * gradually improve as the index is built in a background thread.
 */
int osm_geo_is_index_ready(const OsmGeoDB *db);

/** Returns a build identifier string to verify which dylib is loaded. */
const char *osm_geo_version(void);

/** Free results returned by any search function. */
void osm_geo_free_results(OsmGeoResult *results, int count);

#ifdef __cplusplus
}
#endif

#endif /* OSM_GEO_SEARCH_H */