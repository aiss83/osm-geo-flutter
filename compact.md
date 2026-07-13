## 9. Компактный бинарный формат (`.bin`)

Альтернативный формат без SQLite. Предназначен для production-использования на мобильных устройствах.

### 9.1. Сборка

```bash
osm-geo build --input russia/central-fed-district --format compact --output cfd.bin
```

### 9.2. Структура файла

| Секция | Размер | Описание |
|--------|--------|----------|
| Header | 88 B | magic, version, счётчики, timestamp, регион, offsets (см. 9.3) |
| String pool | ~10 MB | Все уникальные строки (города, улицы, названия). Индекс 0 — пустая строка. |
| Named Index | ~8 MB | Сортирован по name → бинарный поиск для префиксного поиска |
| Address Index | ~14 MB | Сортирован по (city, street, housenumber) |
| Record Block | ~30 MB | Сортирован по (lat, lon) → бинарный поиск для пространственных запросов |

### 9.3. Заголовок (Header, 88 байт, little-endian)

| Смещение | Размер | Поле | Описание |
|----------|--------|------|----------|
| 0 | 4 B | `magic` | `OSMG` (0x4F 0x53 0x4D 0x47) |
| 4 | 2 B | `version` | Версия формата (1) |
| 6 | 4 B | `record_count` | Общее количество объектов |
| 10 | 4 B | `addr_count` | Количество адресов |
| 14 | 4 B | `named_count` | Количество POI |
| 18 | 8 B | `build_timestamp` | Unix timestamp времени сборки |
| 26 | 46 B | `region` | Код региона, UTF-8, zero-padded (напр. `RU-CFD`) |
| 72 | 4 B | `string_pool_offset` | Смещение до String Pool |
| 76 | 4 B | `named_index_offset` | Смещение до Named Index |
| 80 | 4 B | `addr_index_offset` | Смещение до Address Index |
| 84 | 4 B | `records_offset` | Смещение до Record Block |

### 9.4. Записи

**Record Block** (сортирован по lat, lon):
```
type:     u8      — 0=Address, 1=Named
lat:      f32
lon:      f32
Address:  city_idx:u16, street_idx:u16, housenumber_idx:u16   (9 байт)
Named:    name_idx:u16, translit_idx:u16, category:u8         (6 байт)
```

**Named Index** (сортирован по строке имени):
```
name_idx:      u16
translit_idx:  u16
category:      u8    — тег категории (1=amenity, 2=tourism, …)
record_idx:    u32   — индекс в Record Block
```

**Address Index** (сортирован по city, street, housenumber):
```
city_idx:         u16
street_idx:       u16
housenumber_idx:  u16
record_idx:       u32
```

### 9.5. Поиск

Потребитель выполняет бинарный поиск по сортированным массивам:

- **По имени**: бинарный поиск в Named Index → получение record_idx → чтение координат из Record Block
- **По адресу**: бинарный поиск в Address Index → record_idx → координаты
- **По координатам**: бинарный поиск в Record Block по lat

