#pragma once

// JSON 曲谱：序列化 / 反序列化（需求书第三十一、三十二节 + Phase 4）。
//
// 文件格式（与需求书第三十二节的示例一致）：
// {
//     "name": "Example Song",
//     "bpm": 120,
//     "timeSignature": "4/4",
//     "notes": [
//         { "key": "Z", "mode": "Normal", "beat": 0.0,  "duration": 0.25 },
//         { "key": "X", "mode": "Normal", "beat": 0.25, "duration": 0.25 },
//         { "key": "C", "mode": "Left",   "beat": 0.5,  "duration": 0.25 }
//     ]
// }
//
// 核心约定（需求书原文：「加载后 beat ↓ 转换 ↓ absoluteTime，播放器只处理时间轴」）：
//   - 文件里存的是**拍**（beat），是从作者视角写的谱面；
//   - 加载时一次性换算成**绝对时间**（秒）存进 Song::notes；
//   - 换算完就再没有“拍”的概念，播放器永远只看到绝对时间轴。
//
// 另外写了三个可选字段（缺了也能加载，便于以后扩展）：
//   "version"  ：格式版本（当前 1）
//   "beatUnit" ：一拍对应哪个时值（4/4 → "Quarter"、6/8 → "Eighth"）；缺省按拍号分母推导
//   "comment"  ：留给作者的自由备注

#include <string>
#include <vector>

#include "song/Song.h"

namespace hp {

// ---------------------------------------------------------------------------
// 读写结果：失败给出中文原因；能救回来的问题进 warnings（不阻断加载）
// ---------------------------------------------------------------------------
struct SongIoResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;

    void AddWarning(const std::string& text);
    bool HasWarnings() const { return !warnings.empty(); }
    // 一行摘要，可直接显示在界面上
    std::string Message() const;
    // 多行详情（错误 + 每条警告各一行）
    std::string DetailText() const;
};

// ---------------------------------------------------------------------------
// 序列化：Song → JSON 文本
// ---------------------------------------------------------------------------
std::string SongToJsonText(const Song& song, int indent = 4);

// ---------------------------------------------------------------------------
// 反序列化：JSON 文本 → Song（做 beat → 秒 换算）
// 失败时 out 保持原样（不产生半成品）
// ---------------------------------------------------------------------------
SongIoResult ParseSongFromJsonText(const std::string& text, Song& out);

// ---------------------------------------------------------------------------
// 文件读写（UTF-8，无 BOM；写入时自动创建上级目录）
// ---------------------------------------------------------------------------
SongIoResult SaveSongToJson(const Song& song, const std::string& path, int indent = 4);
SongIoResult LoadSongFromJson(const std::string& path, Song& out);

// ---------------------------------------------------------------------------
// 路径工具（下一步的 GUI 会直接用它）
// ---------------------------------------------------------------------------
std::string ExeDirectory();                     // exe 所在目录，带尾部分隔符
std::string DefaultSongDirectory();             // <exe 目录>\songs
std::string SanitizeFileName(const std::string& name);   // 曲名 → 安全文件名（不含扩展名）
std::string SongFilePathFor(const std::string& name);    // <songs 目录>\<安全名>.json
std::vector<std::string> ListSongFiles(const std::string& directory);   // 已排序的 .json 全路径
bool EnsureDirectory(const std::string& directory);

// ---------------------------------------------------------------------------
// 格式自检：检查一段 JSON 文本是否满足“必需字段 + 类型”的要求
// （用于测试与将来的“导入前校验”）
// ---------------------------------------------------------------------------
SongIoResult CheckSongJsonShape(const std::string& text);

// 需求书第三十二节里的那份示例（测试会用它核对 beat → 秒 的换算）
const char* RequirementExampleJson();

}  // namespace hp
