#include "osm_geo_search.h"
#include <stdio.h>
#include <string.h>

static int fail;

static void check_eq(int a, int b, const char *msg) {
    if (a != b) { printf("FAIL: %s (%d != %d)\n", msg, a, b); fail = 1; }
}

int main(void) {
    OsmGeoDB *db = osm_geo_open("russia-kaliningrad.bin");
    if (!db) { printf("FAIL: open\n"); return 1; }
    int c1, c2;
    OsmGeoResult *r1, *r2;

    printf("Test 1: ASCII case-insensitive\n");
    r1 = osm_geo_search_by_name(db, "balt", 10, &c1);
    r2 = osm_geo_search_by_name(db, "BALT", 10, &c2);
    check_eq(c1, c2, "balt vs BALT count");
    for (int i = 0; i < c1 && i < c2; i++)
        check_eq(strcmp(r1[i].name, r2[i].name), 0, "balt vs BALT name");
    osm_geo_free_results(r1, c1);
    osm_geo_free_results(r2, c2);

    printf("Test 2: Cyrillic case-insensitive\n");
    r1 = osm_geo_search_by_name(db, "кафе", 5, &c1);
    r2 = osm_geo_search_by_name(db, "КАФЕ", 5, &c2);
    check_eq(c1, c2, "кафе vs КАФЕ count");
    osm_geo_free_results(r1, c1);
    osm_geo_free_results(r2, c2);

    printf("Test 3: Mixed case\n");
    r1 = osm_geo_search_by_name(db, "VoKsEl", 5, &c1);
    r2 = osm_geo_search_by_name(db, "voksel", 5, &c2);
    check_eq(c1, c2, "VoKsEl vs voksel count");
    osm_geo_free_results(r1, c1);
    osm_geo_free_results(r2, c2);

    printf("Test 4: Combined search\n");
    r1 = osm_geo_search(db, "Калин", 10, &c1);
    r2 = osm_geo_search(db, "калин", 10, &c2);
    check_eq(c1, c2, "Калин vs калин count");
    osm_geo_free_results(r1, c1);
    osm_geo_free_results(r2, c2);

    printf("Test 5: Non-existent name\n");
    r1 = osm_geo_search_by_name(db, "ZZZNOTEXIST", 3, &c1);
    check_eq(c1, 0, "non-existent name");
    osm_geo_free_results(r1, c1);

    /* ── Address search tests ─────────────────────────────────────── */

    printf("\nTest 6: Address search - city only\n");
    r1 = osm_geo_search_by_address(db, "Калининград", 5, &c1);
    printf("  city='Калининград' -> %d results\n", c1);
    for (int i = 0; i < c1 && i < 5; i++)
        printf("    [%d] name=\"%s\" lat=%.6f lon=%.6f\n", i, r1[i].name, r1[i].lat, r1[i].lon);
    check_eq(c1 > 0, 1, "city search returns results");
    osm_geo_free_results(r1, c1);

    printf("\nTest 7: Address search - city + street\n");
    r1 = osm_geo_search_by_address(db, "Калининград, Твер", 10, &c1);
    printf("  city='Калининград' street='Твер' -> %d results\n", c1);
    for (int i = 0; i < c1 && i < 5; i++)
        printf("    [%d] name=\"%s\" lat=%.6f lon=%.6f\n", i, r1[i].name, r1[i].lat, r1[i].lon);
    osm_geo_free_results(r1, c1);

    printf("\nTest 8: Address search - city + street + house\n");
    r1 = osm_geo_search_by_address(db, "Калининград, Тверская, 1", 10, &c1);
    printf("  city='Калининград' street='Тверская' house='1' -> %d results\n", c1);
    for (int i = 0; i < c1 && i < 5; i++)
        printf("    [%d] name=\"%s\" lat=%.6f lon=%.6f\n", i, r1[i].name, r1[i].lat, r1[i].lon);
    osm_geo_free_results(r1, c1);

    printf("\nTest 9: Address search - case insensitive\n");
    r1 = osm_geo_search_by_address(db, "калининград, тверская", 5, &c1);
    r2 = osm_geo_search_by_address(db, "КАЛИНИНГРАД, ТВЕРСКАЯ", 5, &c2);
    printf("  lowercase: %d results, UPPERCASE: %d results\n", c1, c2);
    check_eq(c1, c2, "address case insensitive count");
    osm_geo_free_results(r1, c1);
    osm_geo_free_results(r2, c2);

    printf("\nTest 10: Non-existent address\n");
    r1 = osm_geo_search_by_address(db, "ZZZNOTEXIST", 3, &c1);
    check_eq(c1, 0, "non-existent address");
    osm_geo_free_results(r1, c1);

    printf("\nTest 11: Mixed case — lowercase city, UPPERCASE street\n");
    r1 = osm_geo_search_by_address(db, "калининград, ТВЕРСКАЯ", 5, &c1);
    r2 = osm_geo_search_by_address(db, "Калининград, Тверская", 5, &c2);
    check_eq(c1, c2, "lo-city UP-street vs normal count");
    for (int i = 0; i < c1 && i < c2; i++)
        check_eq(strcmp(r1[i].name, r2[i].name), 0, "lo-city UP-street vs normal name");
    osm_geo_free_results(r1, c1);
    osm_geo_free_results(r2, c2);

    printf("\nTest 12: Mixed case — UPPERCASE city, lowercase street\n");
    r1 = osm_geo_search_by_address(db, "КАЛИНИНГРАД, тверская", 5, &c1);
    r2 = osm_geo_search_by_address(db, "Калининград, Тверская", 5, &c2);
    check_eq(c1, c2, "UP-city lo-street vs normal count");
    for (int i = 0; i < c1 && i < c2; i++)
        check_eq(strcmp(r1[i].name, r2[i].name), 0, "UP-city lo-street vs normal name");
    osm_geo_free_results(r1, c1);
    osm_geo_free_results(r2, c2);

    printf("\nTest 13: Random case salad — city + street\n");
    r1 = osm_geo_search_by_address(db, "КаЛиНиНгРаД, ТвЕрСкАя", 5, &c1);
    r2 = osm_geo_search_by_address(db, "Калининград, Тверская", 5, &c2);
    check_eq(c1, c2, "salad vs normal count");
    for (int i = 0; i < c1 && i < c2; i++)
        check_eq(strcmp(r1[i].name, r2[i].name), 0, "salad vs normal name");
    osm_geo_free_results(r1, c1);
    osm_geo_free_results(r2, c2);

    printf("\nTest 14: Random case salad — city + street + house\n");
    r1 = osm_geo_search_by_address(db, "КаЛиНиНгРаД, ТвЕрСкАя УлИцА, 10", 5, &c1);
    r2 = osm_geo_search_by_address(db, "Калининград, Тверская улица, 10", 5, &c2);
    check_eq(c1, c2, "salad house vs normal count");
    for (int i = 0; i < c1 && i < c2; i++)
        check_eq(strcmp(r1[i].name, r2[i].name), 0, "salad house vs normal name");
    osm_geo_free_results(r1, c1);
    osm_geo_free_results(r2, c2);

    printf("\nTest 15: City-only — different cases\n");
    r1 = osm_geo_search_by_address(db, "калининград", 5, &c1);
    r2 = osm_geo_search_by_address(db, "КАЛИНИНГРАД", 5, &c2);
    check_eq(c1, c2, "city-only lo vs UP count");
    for (int i = 0; i < c1 && i < c2; i++)
        check_eq(strcmp(r1[i].name, r2[i].name), 0, "city-only lo vs UP name");
    osm_geo_free_results(r1, c1);
    osm_geo_free_results(r2, c2);

    osm_geo_close(db);
    if (fail) { printf("\n*** FAILED ***\n"); return 1; }
    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
