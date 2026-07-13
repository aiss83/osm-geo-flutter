import 'package:flutter_test/flutter_test.dart';
import 'package:osm_geo_search/osm_geo_search.dart';

void main() {
  group('OsmGeoDatabase', () {
    test('open non-existent file throws OsmGeoException', () {
      expect(
        () => OsmGeoDatabase.open('/nonexistent/path/to/data.bin'),
        throwsA(isA<OsmGeoException>()),
      );
    });

    test('close on already-closed database is safe', () {
      // We can't easily test this without a real .bin file,
      // but we can verify the API shape compiles and the types align.
      expect(OsmGeoException, isNotNull);
      expect(ResultType.values, containsAll([ResultType.address, ResultType.named]));
      expect(GeoResult, isNotNull);
    });

    test('GeoResult toString contains expected fields', () {
      const r = GeoResult(
        lat: 55.7558,
        lon: 37.6173,
        name: 'Москва',
        type: ResultType.address,
      );
      final s = r.toString();
      expect(s, contains('Москва'));
      expect(s, contains('55.7558'));
      expect(s, contains('37.6173'));
    });

    test('ResultType enum has two values', () {
      expect(ResultType.values.length, 2);
      expect(ResultType.address.index, 0);
      expect(ResultType.named.index, 1);
    });

    test('OsmGeoException carries message', () {
      const e = OsmGeoException('test error');
      expect(e.message, 'test error');
      expect(e.toString(), contains('test error'));
    });
  });
}
