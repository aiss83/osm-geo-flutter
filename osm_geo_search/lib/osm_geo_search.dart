/// Full-text search engine for OSM compact binary format (.bin).
///
/// This plugin provides memory-mapped, zero-copy search over compact
/// OpenStreetMap data files, optimized for mobile devices.
///
/// ## Features
///
/// - **Zero-copy access** via `mmap` — the OS pages in only what's needed
/// - **Binary search** on sorted indices — O(log N) lookup
/// - **Prefix matching** for incremental search / autocomplete
/// - **Combined search** across POI names and addresses
///
/// ## Usage
///
/// ```dart
/// import 'package:osm_geo_search/osm_geo_search.dart';
///
/// final db = OsmGeoDatabase.open('path/to/data.bin');
///
/// // Search POI by name prefix
/// final pois = db.searchByName('кафе');
///
/// // Search address by prefix
/// final addrs = db.searchByAddress('Москва, Тверская');
///
/// // Combined search
/// final all = db.search('москва', maxResults: 20);
///
/// for (final r in all) {
///   print('${r.name}: (${r.lat}, ${r.lon})');
/// }
///
/// db.close();
/// ```
library osm_geo_search;

export 'src/engine.dart';
