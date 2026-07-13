import 'dart:async';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:osm_geo_search/osm_geo_search.dart';

void main() {
  runApp(const OsmGeoSearchApp());
}

class OsmGeoSearchApp extends StatelessWidget {
  const OsmGeoSearchApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'OSM Geo Search',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorSchemeSeed: Colors.indigo,
        useMaterial3: true,
        brightness: Brightness.light,
      ),
      home: const FilePickerScreen(),
    );
  }
}

// ── Screen 1: File picker ────────────────────────────────────────────

class FilePickerScreen extends StatefulWidget {
  const FilePickerScreen({super.key});

  @override
  State<FilePickerScreen> createState() => _FilePickerScreenState();
}

class _FilePickerScreenState extends State<FilePickerScreen> {
  String? _error;

    Future<void> _pickFile() async {
    setState(() => _error = null);

    final result = await FilePicker.platform.pickFiles(
      type: FileType.any,
      allowMultiple: false,
    );

    if (result == null || result.files.isEmpty) return;

    final path = result.files.single.path;
    if (path == null) {
      setState(() => _error = 'Не удалось получить путь к файлу');
      return;
    }

    // Try to open the database to validate the file
    try {
      OsmGeoDatabase.open(path).close();
    } on OsmGeoException catch (e) {
      setState(() => _error = 'Ошибка открытия файла: ${e.message}');
      return;
    } catch (e) {
      setState(() => _error = 'Не удалось загрузить библиотеку: $e');
      return;
    }

    if (!mounted) return;
    Navigator.pushReplacement(
      context,
      MaterialPageRoute(builder: (_) => SearchScreen(filePath: path)),
    );
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Scaffold(
      body: Center(
        child: Card(
          margin: const EdgeInsets.all(32),
          child: Padding(
            padding: const EdgeInsets.all(40),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                Icon(Icons.map_outlined, size: 72, color: theme.colorScheme.primary),
                const SizedBox(height: 24),
                Text(
                  'OSM Geo Search',
                  style: theme.textTheme.headlineMedium?.copyWith(
                    fontWeight: FontWeight.bold,
                  ),
                ),
                const SizedBox(height: 8),
                Text(
                  'Выберите файл базы данных (.bin)',
                  style: theme.textTheme.bodyLarge?.copyWith(
                    color: theme.colorScheme.onSurfaceVariant,
                  ),
                ),
                const SizedBox(height: 32),
                FilledButton.icon(
                  onPressed: _pickFile,
                  icon: const Icon(Icons.folder_open),
                  label: const Text('Выбрать файл'),
                  style: FilledButton.styleFrom(
                    padding: const EdgeInsets.symmetric(
                      horizontal: 32,
                      vertical: 16,
                    ),
                  ),
                ),
                if (_error != null) ...[
                  const SizedBox(height: 16),
                  Container(
                    padding: const EdgeInsets.all(12),
                    decoration: BoxDecoration(
                      color: theme.colorScheme.errorContainer,
                      borderRadius: BorderRadius.circular(8),
                    ),
                    child: Row(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        Icon(Icons.error_outline,
                            size: 20, color: theme.colorScheme.error),
                        const SizedBox(width: 8),
                        Flexible(
                          child: Text(
                            _error!,
                            style: TextStyle(
                              color: theme.colorScheme.onErrorContainer,
                            ),
                          ),
                        ),
                      ],
                    ),
                  ),
                ],
              ],
            ),
          ),
        ),
      ),
    );
  }
}

// ── Screen 2: Address search ─────────────────────────────────────────

class SearchScreen extends StatefulWidget {
  final String filePath;

  const SearchScreen({super.key, required this.filePath});

  @override
  State<SearchScreen> createState() => _SearchScreenState();
}

class _SearchScreenState extends State<SearchScreen> {
  final _queryController = TextEditingController();
  final _focusNode = FocusNode();
  OsmGeoDatabase? _db;
  List<GeoResult> _suggestions = [];
  GeoResult? _selected;
  Timer? _debounce;
  bool _showSuggestions = false;

  @override
  void initState() {
    super.initState();
    _initDb();
  }

  void _initDb() {
    try {
      _db = OsmGeoDatabase.open(widget.filePath);
      // Auto-focus the search field
      WidgetsBinding.instance.addPostFrameCallback((_) {
        _focusNode.requestFocus();
      });
    } on OsmGeoException catch (e) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text('Ошибка: ${e.message}'),
            backgroundColor: Theme.of(context).colorScheme.error,
          ),
        );
      });
    }
  }

  void _onQueryChanged(String query) {
    _debounce?.cancel();
    _selected = null;

    if (query.length < 2) {
      setState(() {
        _suggestions = [];
        _showSuggestions = false;
      });
      return;
    }

    _debounce = Timer(const Duration(milliseconds: 250), () {
      _search(query);
    });
  }

  void _search(String query) {
    if (_db == null) return;

    try {
      final results = _db!.searchByAddress(query, maxResults: 10);
      setState(() {
        _suggestions = results;
        _showSuggestions = true;
      });
    } on OsmGeoException {
      setState(() {
        _suggestions = [];
        _showSuggestions = false;
      });
    }
  }

  void _selectResult(GeoResult result) {
    setState(() {
      _selected = result;
      _showSuggestions = false;
      _queryController.text = result.name;
    });
  }

  @override
  void dispose() {
    _debounce?.cancel();
    _db?.close();
    _queryController.dispose();
    _focusNode.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final fileName = widget.filePath.split('/').last;

    return Scaffold(
      appBar: AppBar(
        title: Text(fileName),
        leading: IconButton(
          icon: const Icon(Icons.arrow_back),
          onPressed: () {
            _db?.close();
            Navigator.pushReplacement(
              context,
              MaterialPageRoute(builder: (_) => const FilePickerScreen()),
            );
          },
        ),
      ),
      body: Column(
        children: [
          // Search bar
          Padding(
            padding: const EdgeInsets.all(16),
            child: TextField(
              controller: _queryController,
              focusNode: _focusNode,
              onChanged: _onQueryChanged,
              decoration: InputDecoration(
                hintText: 'Введите адрес (город, улица, дом)',
                prefixIcon: const Icon(Icons.search),
                suffixIcon: _queryController.text.isNotEmpty
                    ? IconButton(
                        icon: const Icon(Icons.clear),
                        onPressed: () {
                          _queryController.clear();
                          setState(() {
                            _suggestions = [];
                            _showSuggestions = false;
                            _selected = null;
                          });
                        },
                      )
                    : null,
                border: const OutlineInputBorder(),
                filled: true,
                fillColor: theme.colorScheme.surfaceContainerHighest
                    .withValues(alpha: 0.3),
              ),
            ),
          ),

          // Suggestions dropdown
          if (_showSuggestions && _suggestions.isNotEmpty)
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16),
              child: Card(
                elevation: 4,
                margin: EdgeInsets.zero,
                child: ConstrainedBox(
                  constraints: const BoxConstraints(maxHeight: 400),
                  child: ListView.separated(
                    shrinkWrap: true,
                    itemCount: _suggestions.length,
                    separatorBuilder: (_, __) => const Divider(height: 1),
                    itemBuilder: (context, index) {
                      final r = _suggestions[index];
                      return ListTile(
                        leading: const Icon(Icons.location_on_outlined),
                        title: Text(r.name),
                        subtitle: Text(
                          '${r.lat.toStringAsFixed(5)}, ${r.lon.toStringAsFixed(5)}',
                          style: theme.textTheme.bodySmall,
                        ),
                        onTap: () => _selectResult(r),
                      );
                    },
                  ),
                ),
              ),
            ),

          // "Not found" state
          if (_showSuggestions &&
              _suggestions.isEmpty &&
              _queryController.text.length >= 2)
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16),
              child: Card(
                elevation: 2,
                margin: EdgeInsets.zero,
                color: theme.colorScheme.surfaceContainerHighest,
                child: const Padding(
                  padding: EdgeInsets.all(24),
                  child: Center(
                    child: Text(
                      'Не найден',
                      style: TextStyle(fontSize: 18),
                    ),
                  ),
                ),
              ),
            ),

          // Selected result — coordinates in center
          if (_selected != null)
            Expanded(
              child: Center(
                child: Card(
                  margin: const EdgeInsets.all(32),
                  child: Padding(
                    padding: const EdgeInsets.all(48),
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        Icon(Icons.location_on,
                            size: 64, color: theme.colorScheme.primary),
                        const SizedBox(height: 16),
                        Text(
                          _selected!.name,
                          style: theme.textTheme.headlineSmall?.copyWith(
                            fontWeight: FontWeight.bold,
                          ),
                          textAlign: TextAlign.center,
                        ),
                        const SizedBox(height: 24),
                        _CoordinateRow(
                          label: 'Широта',
                          value: _selected!.lat.toStringAsFixed(7),
                        ),
                        const SizedBox(height: 8),
                        _CoordinateRow(
                          label: 'Долгота',
                          value: _selected!.lon.toStringAsFixed(7),
                        ),
                      ],
                    ),
                  ),
                ),
              ),
            )
          else
            const Expanded(
              child: Center(
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Icon(Icons.search_off, size: 48, color: Colors.grey),
                    SizedBox(height: 16),
                    Text(
                      'Начните вводить адрес для поиска',
                      style: TextStyle(color: Colors.grey, fontSize: 16),
                    ),
                  ],
                ),
              ),
            ),
        ],
      ),
    );
  }
}

class _CoordinateRow extends StatelessWidget {
  final String label;
  final String value;

  const _CoordinateRow({required this.label, required this.value});

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        SizedBox(
          width: 72,
          child: Text(
            label,
            style: theme.textTheme.bodyMedium?.copyWith(
              color: theme.colorScheme.onSurfaceVariant,
            ),
          ),
        ),
        Container(
          padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
          decoration: BoxDecoration(
            color: theme.colorScheme.secondaryContainer,
            borderRadius: BorderRadius.circular(6),
          ),
          child: Text(
            value,
            style: theme.textTheme.titleMedium?.copyWith(
              fontWeight: FontWeight.w600,
              fontFamily: 'monospace',
            ),
          ),
        ),
      ],
    );
  }
}
