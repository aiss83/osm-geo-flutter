#include "osm_geo_search.h"
#include <stdio.h>
int main(){
    OsmGeoDB *db=osm_geo_open("russia-kaliningrad.bin");
    int c; OsmGeoResult *r;
    r=osm_geo_search_by_name(db,"balt",5,&c);
    printf("balt=%d\n",c);
    for(int i=0;i<c;i++)printf("  %s\n",r[i].name);
    osm_geo_free_results(r,c);
    r=osm_geo_search_by_name(db,"BALT",5,&c);
    printf("BALT=%d\n",c);
    for(int i=0;i<c;i++)printf("  %s\n",r[i].name);
    osm_geo_free_results(r,c);
    osm_geo_close(db);
}
