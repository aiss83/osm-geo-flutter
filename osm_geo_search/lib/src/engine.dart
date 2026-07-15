import 'dart:ffi';

import 'package:ffi/ffi.dart';

import 'bindings.dart' as bindings;

/// A single search result from the OSM database.
class GeoResult {
  final double lat;
  final double lon;
  final String name;
  final String? translit;
  final String? city;
  final String? street;
  final String? house;
  final int category;
  final ResultType type;

  const GeoResult({
    required this.lat,
    required this.lon,
    required this.name,
    this.translit,
    this.city,
    this.street,
    this.house,
    this.category = 0,
    required this.type,
  });

  @override
  String toString() {
    final addr = [city, street, house].where((e) => e != null && e.isNotEmpty);
    if (addr.isNotEmpty) {
      return 'GeoResult($type, "$name" [${addr.join(', ')}], $lat, $lon)';
    }
    return 'GeoResult($type, "$name", $lat, $lon)';
  }
}

enum ResultType { address, named }

/// Full-text search engine for OSM compact binary format.
///
/// Usage:
/// ```dart
/// final db = OsmGeoDatabase.open('/path/to/data.bin');
/// final results = db.search('Москва', maxResults: 20);
/// for (final r in results) {
///   print('${r.name}: ${r.lat}, ${r.lon}');
/// }
/// db.close();
/// ```
class OsmGeoDatabase {
  final Pointer<bindings.OsmGeoDB> _handle;
  bool _closed = false;

  OsmGeoDatabase._(this._handle);

  /// Open a compact binary database file.
  ///
  /// Throws [OsmGeoException] if the file cannot be opened or has an
  /// invalid format.
  factory OsmGeoDatabase.open(String path) {
    final pathPtr = path.toNativeUtf8();
    try {
      final handle = bindings.osmGeoOpen(pathPtr);
      if (handle == nullptr) {
        final errPtr = bindings.osmGeoError();
        final err = errPtr.toDartString();
        throw OsmGeoException('Failed to open "$path": $err');
      }
      return OsmGeoDatabase._(handle);
    } finally {
      calloc.free(pathPtr);
    }
  }

  /// Open with progress reporting during index construction.
  ///
  /// **Note:** This runs synchronously on the calling thread. The
  /// [onProgress] callback fires during the call, but the UI cannot
  /// update until the call returns (the main isolate is blocked).
  /// For a responsive UI, wrap this call in [Isolate.run] in your
  /// application code.
  ///
  /// [onProgress] receives (done, total, phase):
  ///   - phase "records": building record offset table
  ///   - phase "streets": building street lookup index (slowest)
  ///   - phase "sort":   sorting the street index
  ///
  /// Example:
  /// ```dart
  /// // Fast path (blocks UI briefly — fine for small databases):
  /// final db = OsmGeoDatabase.openWithProgress(
  ///   path,
  ///   onProgress: (done, total, phase) => print('$phase $done/$total'),
  /// );
  /// ```
  factory OsmGeoDatabase.openWithProgress(
    String path, {
    required void Function(int done, int total, String phase) onProgress,
  }) {
    final pathPtr = path.toNativeUtf8();
    final callback = NativeCallable<
        bindings.OsmGeoProgressNative>.listener((done, total, phasePtr) {
      onProgress(done, total, phasePtr.toDartString());
    });
    try {
      final handle = bindings.osmGeoOpenWithProgress(
        pathPtr,
        callback.nativeFunction,
      );
      if (handle == nullptr) {
        final errPtr = bindings.osmGeoError();
        throw OsmGeoException(
            'Failed to open "$path": ${errPtr.toDartString()}');
      }
      return OsmGeoDatabase._(handle);
    } finally {
      callback.close();
      calloc.free(pathPtr);
    }
  }

  /// Search by POI name (prefix match).
  List<GeoResult> searchByName(String query, {int maxResults = 50}) {
    _checkClosed();
    final qPtr = query.toNativeUtf8();
    final outCount = calloc<Int32>();
    try {
      final resultsPtr =
          bindings.osmGeoSearchByName(_handle, qPtr, maxResults, outCount);
      return _collectResults(resultsPtr, outCount.value);
    } finally {
      calloc.free(qPtr);
      calloc.free(outCount);
    }
  }

  /// Search by address (prefix match on city, street, housenumber).
  ///
  /// Query format: "city" or "city, street" or "city, street, house".
  List<GeoResult> searchByAddress(String query, {int maxResults = 50}) {
    _checkClosed();
    final qPtr = query.toNativeUtf8();
    final outCount = calloc<Int32>();
    try {
      final resultsPtr =
          bindings.osmGeoSearchByAddress(_handle, qPtr, maxResults, outCount);
      return _collectResults(resultsPtr, outCount.value);
    } finally {
      calloc.free(qPtr);
      calloc.free(outCount);
    }
  }

  /// Combined search across both POI names and addresses.
  ///
  /// Named (POI) results come first, followed by address results.
  List<GeoResult> search(String query, {int maxResults = 50}) {
    _checkClosed();
    final qPtr = query.toNativeUtf8();
    final outCount = calloc<Int32>();
    try {
      final resultsPtr =
          bindings.osmGeoSearch(_handle, qPtr, maxResults, outCount);
      return _collectResults(resultsPtr, outCount.value);
    } finally {
      calloc.free(qPtr);
      calloc.free(outCount);
    }
  }

  /// Intelligent full-text search with automatic mode detection.
  ///
  /// Single-token queries (no comma/space separators) search as a prefix
  /// simultaneously across POI names, cities, and streets.
  /// Results are ranked: named POI first, then streets, then cities.
  ///
  /// Multi-token queries (separated by comma or space) are treated as
  /// structured address: "city street" or "city, street" or
  /// "city, street, house". Also supports "city, POI-name" pattern.
  ///
  /// Minimum query length is 3 characters. Results include structured
  /// address fields ([GeoResult.city], [GeoResult.street],
  /// [GeoResult.house]) when available.
  ///
  /// Example:
  /// ```dart
  /// // Free-form: finds streets and POIs starting with "Твер"
  /// db.searchFulltext('Твер');
  /// // => "Калининград, Тверская улица", "Тверской бульвар", ...
  ///
  /// // Structured: city + street
  /// db.searchFulltext('Калининград, Тверская');
  ///
  /// // Structured: city + POI
  /// db.searchFulltext('Калининград, кафе');
  /// ```
  List<GeoResult> searchFulltext(String query, {int maxResults = 50}) {
    _checkClosed();
    final qPtr = query.toNativeUtf8();
    final outCount = calloc<Int32>();
    try {
      final resultsPtr =
          bindings.osmGeoSearchFulltext(_handle, qPtr, maxResults, outCount);
      return _collectResults(resultsPtr, outCount.value);
    } finally {
      calloc.free(qPtr);
      calloc.free(outCount);
    }
  }

  List<GeoResult> _collectResults(
      Pointer<bindings.OsmGeoResult> resultsPtr, int count) {
    if (resultsPtr == nullptr || count <= 0) return [];

    final results = <GeoResult>[];
    for (var i = 0; i < count; i++) {
      final entry = (resultsPtr + i).ref;
      results.add(GeoResult(
        lat: entry.lat,
        lon: entry.lon,
        name: entry.name.toDartString(),
        translit:
            entry.translit == nullptr ? null : entry.translit.toDartString(),
        city: entry.city == nullptr ? null : entry.city.toDartString(),
        street: entry.street == nullptr ? null : entry.street.toDartString(),
        house: entry.house == nullptr ? null : entry.house.toDartString(),
        category: entry.category,
        type: entry.type == 1 ? ResultType.named : ResultType.address,
      ));
    }

    bindings.osmGeoFreeResults(resultsPtr, count);
    return results;
  }

  void _checkClosed() {
    if (_closed) throw StateError('Database is closed');
  }

  /// Whether the background street index has finished building.
  /// [searchFulltext] works immediately, but street results are
  /// limited until this returns true.
  bool get isIndexReady => bindings.osmGeoIsIndexReady(_handle) != 0;

  /// Close the database and release all resources.
  void close() {
    if (_closed) return;
    _closed = true;
    bindings.osmGeoClose(_handle);
  }
}

/// Exception thrown by [OsmGeoDatabase] operations.
class OsmGeoException implements Exception {
  final String message;
  const OsmGeoException(this.message);

  @override
  String toString() => 'OsmGeoException: $message';
}
