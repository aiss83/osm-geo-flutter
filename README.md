# osm_geo_search

Full-text search plugin for OpenStreetMap compact binary format (`.bin`).
Optimized for mobile devices with **mmap** zero-copy access and **binary search**
on sorted indices. Supports prefix matching for incremental search / autocomplete.

[![pub version](https://img.shields.io/badge/pub-0.1.0-blue)](https://pub.dev)
[![license](https://img.shields.io/badge/license-MIT-green)](LICENSE)

## Features

- **Zero-copy access** via `mmap` — the OS pages in only what's needed
- **Binary search** on sorted indices — O(log N) lookup
- **Prefix matching** for incremental search / autocomplete
- **Case-insensitive** search — handles ASCII, Cyrillic, and mixed case via ICU
- **Combined search** across POI names and addresses
- **MacOS** · **Android** · **Linux** support

## Installation

```yaml
dependencies:
  osm_geo_search: ^0.1.0
```

## Usage

```dart
import 'package:osm_geo_search/osm_geo_search.dart';

// Open a compact binary database (.bin)
final db = OsmGeoDatabase.open('path/to/data.bin');

// Search POI by name prefix
final pois = db.searchByName('кафе');
for (final r in pois) {
  print('${r.name}: (${r.lat}, ${r.lon})');
}

// Search address by prefix
final addrs = db.searchByAddress('Калининград, Тверская');
for (final r in addrs) {
  print('${r.name}: (${r.lat}, ${r.lon})');
}

// Combined search (POI names + addresses)
final all = db.search('кафе', maxResults: 20);

db.close();
```

## API Reference

### `OsmGeoDatabase`

| Method | Description |
|--------|-------------|
| `OsmGeoDatabase.open(String path)` | Open a `.bin` database file. Throws `OsmGeoException` on failure. |
| `searchByName(String query, {int maxResults = 50})` | Prefix search by POI name. |
| `searchByAddress(String query, {int maxResults = 50})` | Prefix search by address. Query format: `"city"` or `"city, street"` or `"city, street, house"`. |
| `search(String query, {int maxResults = 50})` | Combined search across POI names and addresses. Named results come first. |
| `close()` | Close the database and release all resources. |

### `GeoResult`

| Field | Type | Description |
|-------|------|-------------|
| `lat` | `double` | Latitude |
| `lon` | `double` | Longitude |
| `name` | `String` | POI name or full address string |
| `translit` | `String?` | Transliterated name (may be null) |
| `category` | `int` | Category tag for Named results |
| `type` | `ResultType` | `ResultType.named` or `ResultType.address` |

### `ResultType`

- `ResultType.named` — POI (point of interest) result
- `ResultType.address` — Address result

### `OsmGeoException`

Thrown by `OsmGeoDatabase.open()` when the file cannot be opened or has an invalid format. Carries a human-readable `message`.

## Binary Format

The plugin works with a compact binary format (`.bin`) structured as:

```
┌──────────────────┐
│  Header (88 B)   │  magic, version, record counts, section offsets
├──────────────────┤
│  String Pool     │  Length-prefixed UTF-8 strings (city, street, name, etc.)
├──────────────────┤
│  Named Index     │  Sorted by name → record mapping for POI search
├──────────────────┤
│  Address Index   │  Sorted by city → street → house → record mapping
├──────────────────┤
│  Records Block   │  Typed records (Address: 15 B, Named: 14 B)
│                   │  Each contains lat/lon + string pool indices
└──────────────────┘
```

## Platform-Specific Notes

### macOS

The plugin bundles a pre-built `libosm_geo_search.dylib` in the app's
`Frameworks/` directory. ICU is **statically linked** — no external
dependencies at runtime.

### Android

- **minSdkVersion**: 31 (Android 12+)
- ICU is provided by the Android NDK (`libicu.so`) — no additional dependencies
- The native library is compiled via CMake with NDK headers (ICU 76.1)
- Case-insensitive search uses `u_strToLower` / `u_memcmp` from ICU

### Linux

Links against system `libosm_geo_search.so`. ICU must be installed
separately (e.g., `libicu-dev` on Debian/Ubuntu).

## Building the Native Library

### macOS

```bash
cc -std=c11 -O2 -fPIC -shared osm_geo_search.c \
  -I/opt/homebrew/opt/icu4c@78/include \
  /opt/homebrew/opt/icu4c@78/lib/libicuuc.a \
  /opt/homebrew/opt/icu4c@78/lib/libicudata.a \
  -lc++ -o libosm_geo_search.dylib
```

### Android

The native library is built automatically by Gradle via CMake
(`android/CMakeLists.txt`). Requires Android NDK and homebrew ICU headers.

### Linux

```bash
cc -std=c11 -O2 -fPIC -shared osm_geo_search.c \
  -licuuc -licudata -o libosm_geo_search.so
```

## Running Tests

```bash
# C tests
cd osm_geo_search/native
cc -std=c11 -O2 osm_geo_search.c test_main.c \
  -I/opt/homebrew/opt/icu4c@78/include \
  /opt/homebrew/opt/icu4c@78/lib/libicuuc.a \
  /opt/homebrew/opt/icu4c@78/lib/libicudata.a \
  -lc++ -o test_search
./test_search

# Dart tests
cd osm_geo_search
dart test
```

## License

MIT
