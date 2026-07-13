import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';

/* ── C type mappings ───────────────────────────────────────────────── */

final class OsmGeoDB extends Opaque {}

final class OsmGeoResult extends Struct {
  @Double()
  external double lat;

  @Double()
  external double lon;

  external Pointer<Utf8> name;

  external Pointer<Utf8> translit;

  @Uint8()
  external int category;

  @Uint8()
  external int type;
}

/* ── Function signatures ───────────────────────────────────────────── */

typedef OsmGeoOpenNative = Pointer<OsmGeoDB> Function(Pointer<Utf8> path);
typedef OsmGeoOpenDart = Pointer<OsmGeoDB> Function(Pointer<Utf8> path);

typedef OsmGeoCloseNative = Void Function(Pointer<OsmGeoDB> db);
typedef OsmGeoCloseDart = void Function(Pointer<OsmGeoDB> db);

typedef OsmGeoErrorNative = Pointer<Utf8> Function();
typedef OsmGeoErrorDart = Pointer<Utf8> Function();

typedef OsmGeoSearchNative = Pointer<OsmGeoResult> Function(
    Pointer<OsmGeoDB> db, Pointer<Utf8> query, Int32 maxResults,
    Pointer<Int32> outCount);
typedef OsmGeoSearchDart = Pointer<OsmGeoResult> Function(
    Pointer<OsmGeoDB> db, Pointer<Utf8> query, int maxResults,
    Pointer<Int32> outCount);

typedef OsmGeoFreeResultsNative = Void Function(
    Pointer<OsmGeoResult> results, Int32 count);
typedef OsmGeoFreeResultsDart = void Function(
    Pointer<OsmGeoResult> results, int count);

/* ── Dynamic library loading ───────────────────────────────────────── */

DynamicLibrary _loadLibrary() {
  if (Platform.isAndroid) {
    return DynamicLibrary.open('libosm_geo_search.so');
  }
  if (Platform.isLinux) {
    return DynamicLibrary.open('libosm_geo_search.so');
  }
  if (Platform.isMacOS) {
    // Static library linked into the binary — symbols available
    // through the process handle.
    return DynamicLibrary.process();
  }
  throw UnsupportedError('Unsupported platform: ${Platform.operatingSystem}');
}

final DynamicLibrary _lib = _loadLibrary();

/* ── Bound functions ───────────────────────────────────────────────── */

final Pointer<OsmGeoDB> Function(Pointer<Utf8> path) osmGeoOpen = _lib
    .lookupFunction<OsmGeoOpenNative, OsmGeoOpenDart>('osm_geo_open');

final void Function(Pointer<OsmGeoDB> db) osmGeoClose = _lib
    .lookupFunction<OsmGeoCloseNative, OsmGeoCloseDart>('osm_geo_close');

final Pointer<Utf8> Function() osmGeoError = _lib
    .lookupFunction<OsmGeoErrorNative, OsmGeoErrorDart>('osm_geo_error');

final Pointer<OsmGeoResult> Function(
        Pointer<OsmGeoDB> db, Pointer<Utf8> query, int maxResults,
        Pointer<Int32> outCount) osmGeoSearchByName = _lib
    .lookupFunction<OsmGeoSearchNative, OsmGeoSearchDart>(
        'osm_geo_search_by_name');

final Pointer<OsmGeoResult> Function(
        Pointer<OsmGeoDB> db, Pointer<Utf8> query, int maxResults,
        Pointer<Int32> outCount) osmGeoSearchByAddress = _lib
    .lookupFunction<OsmGeoSearchNative, OsmGeoSearchDart>(
        'osm_geo_search_by_address');

final Pointer<OsmGeoResult> Function(
        Pointer<OsmGeoDB> db, Pointer<Utf8> query, int maxResults,
        Pointer<Int32> outCount) osmGeoSearch = _lib
    .lookupFunction<OsmGeoSearchNative, OsmGeoSearchDart>('osm_geo_search');

final void Function(Pointer<OsmGeoResult> results, int count) osmGeoFreeResults =
    _lib.lookupFunction<OsmGeoFreeResultsNative, OsmGeoFreeResultsDart>(
        'osm_geo_free_results');
