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

/* ── Lazy library loading with fallback chain ──────────────────────── */

DynamicLibrary? _cachedLib;
String? _lastError;

DynamicLibrary _loadLibrary() {
  if (_cachedLib != null) return _cachedLib!;

  const macosPaths = [
    // 1. Static symbols (SPM-linked).
    null,
    // 2. Bundled dylib in app Frameworks/.
    '@executable_path/../Frameworks/libosm_geo_search.dylib',
    // 3. Relative to cwd (dev mode).
    'Frameworks/libosm_geo_search.dylib',
    // 4. Bare name (DYLD_LIBRARY_PATH).
    'libosm_geo_search.dylib',
  ];

  final paths = Platform.isMacOS
      ? macosPaths
      : [if (Platform.isAndroid || Platform.isLinux) 'libosm_geo_search.so'];

  final buf = StringBuffer();
  buf.writeln('[osm_geo_search] Loading (${Platform.operatingSystem})');
  buf.writeln('[osm_geo_search] Exe: ${Platform.resolvedExecutable}');

  final errors = <String>[];
  var attempt = 0;
  for (final path in paths) {
    attempt++;
    final label = path ?? 'process';
    try {
      final lib = path == null ? DynamicLibrary.process() : DynamicLibrary.open(path);
      // Verify that the library actually has our symbols.
      lib.lookupFunction<OsmGeoOpenNative, OsmGeoOpenDart>('osm_geo_open');
      _cachedLib = lib;
      buf.writeln('[osm_geo_search]   #$attempt $label … OK');
      // ignore: avoid_print
      print(buf.toString());
      return lib;
    } catch (e) {
      buf.writeln('[osm_geo_search]   #$attempt $label … FAIL: $e');
      errors.add('  $label: $e');
    }
  }

  buf.writeln('[osm_geo_search] All paths exhausted.');
  _lastError = buf.toString();
  // ignore: avoid_print
  print(_lastError!);
  throw UnsupportedError(_lastError!);
}

/// Returns the last library loading error, if any.
String? get lastLoadError => _lastError;

/* ── Bound functions (lazy, via getters) ───────────────────────────── */

Pointer<OsmGeoDB> Function(Pointer<Utf8> path) get osmGeoOpen =>
    _loadLibrary()
        .lookupFunction<OsmGeoOpenNative, OsmGeoOpenDart>('osm_geo_open');

void Function(Pointer<OsmGeoDB> db) get osmGeoClose =>
    _loadLibrary()
        .lookupFunction<OsmGeoCloseNative, OsmGeoCloseDart>('osm_geo_close');

Pointer<Utf8> Function() get osmGeoError =>
    _loadLibrary()
        .lookupFunction<OsmGeoErrorNative, OsmGeoErrorDart>('osm_geo_error');

Pointer<OsmGeoResult> Function(
        Pointer<OsmGeoDB> db, Pointer<Utf8> query, int maxResults,
        Pointer<Int32> outCount) get osmGeoSearchByName =>
    _loadLibrary().lookupFunction<OsmGeoSearchNative, OsmGeoSearchDart>(
        'osm_geo_search_by_name');

Pointer<OsmGeoResult> Function(
        Pointer<OsmGeoDB> db, Pointer<Utf8> query, int maxResults,
        Pointer<Int32> outCount) get osmGeoSearchByAddress =>
    _loadLibrary().lookupFunction<OsmGeoSearchNative, OsmGeoSearchDart>(
        'osm_geo_search_by_address');

Pointer<OsmGeoResult> Function(
        Pointer<OsmGeoDB> db, Pointer<Utf8> query, int maxResults,
        Pointer<Int32> outCount) get osmGeoSearch =>
    _loadLibrary().lookupFunction<OsmGeoSearchNative, OsmGeoSearchDart>(
        'osm_geo_search');

void Function(Pointer<OsmGeoResult> results, int count)
    get osmGeoFreeResults =>
        _loadLibrary().lookupFunction<OsmGeoFreeResultsNative,
            OsmGeoFreeResultsDart>('osm_geo_free_results');
