// 本文件由 tools/gen-schema.mjs 从 ts/src/schema/song.ts 生成，**不要手改**。
// 生成时间：构建期（每次 ./build.sh ts 之后）

/// 【模块 6】歌曲模型：字段清单唯一真源在 TypeScript 的 Zod schema。
class Song {
  const Song({
    required this.id,
    required this.source,
    required this.title,
    required this.artist,
    required this.album,
    required this.durationMs,
    required this.quality,
    required this.mid,
  });

  final String id;
  final String source;
  final String title;
  final String artist;
  final String album;
  final int durationMs;
  final String quality;
  final String mid;

  /// 从任意 JSON 对象宽容地取字段：缺字段给默认值，类型不对就当没有。
  static Song? tryParse(Object? node) {
    if (node is! Map) return null;
    final m = <String, Object?>{
      for (final e in node.entries) '${e.key}': e.value,
    };
    final id = _asStr(m['id']);
    if (id.isEmpty) return null;
    return Song(
      id: _asStr(m['id']),
      source: _asStr(m['source']),
      title: _asStr(m['name']),
      artist: _asStr(m['artist']),
      album: _asStr(m['album']),
      durationMs: _asInt(m['durationMs']),
      quality: _asStr(m['quality']),
      mid: _asStr(m['mid']),
    );
  }

  static String _asStr(Object? v) => v is String ? v : (v == null ? '' : '$v');
  static int _asInt(Object? v) {
    if (v is int) return v;
    if (v is num) return v.toInt();
    if (v is String) return int.tryParse(v) ?? 0;
    return 0;
  }

  Map<String, Object?> toJson() => <String, Object?>{
        'id': id,
        'source': source,
        'name': title,
        'artist': artist,
        'album': album,
        'durationMs': durationMs,
        'quality': quality,
        'mid': mid,
      };
}
