// 注意：nlohmann/json 必须在 windows.h 之前包含
// （windows.h 的 min/max 宏会破坏 json.hpp 里的 std::numeric_limits<T>::max() 之类写法）
#include <nlohmann/json.hpp>

#include "song/SongSerializer.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "core/Platform.h"
#include "core/Types.h"
#include "song/Note.h"

namespace hp {
namespace {

using Json = nlohmann::ordered_json;   // 保序：文件里的字段顺序就是这里写入的顺序

constexpr int kFormatVersion = 1;

std::string Trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' ||
                           text[begin] == '\n')) {
        ++begin;
    }
    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' ||
            text[end - 1] == '\n')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// UTF-8 → 宽字符（文件路径要能带中文）
std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), size,
                        nullptr, nullptr);
    return utf8;
}

std::string ParentDirectory(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) {
        return std::string();
    }
    return path.substr(0, slash);
}

std::string JoinPath(const std::string& directory, const std::string& leaf) {
    if (directory.empty()) {
        return leaf;
    }
    const char last = directory[directory.size() - 1];
    if (last == '\\' || last == '/') {
        return directory + leaf;
    }
    return directory + "\\" + leaf;
}

// 出错时把用户写的值原样带进消息里（太长就截断），界面上一眼能看出是哪个字段
std::string Shorten(const std::string& text, size_t maxLength = 40) {
    if (text.size() <= maxLength) {
        return text;
    }
    size_t cut = maxLength;
    // 不要把一个 UTF-8 字符切成两半
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return text.substr(0, cut) + "…";
}

// JSON 最大嵌套层数（跳过字符串内部的括号；引号 / 反斜杠转义都照顾到）。
// nlohmann 解析深嵌套是递归的，深到一定程度会栈溢出 —— 那不是异常，try/catch 拦不住，
// 所以要在解析之前先量一下，明显离谱的直接拒。
int MaxJsonNestingDepth(const std::string& text) {
    int depth = 0;
    int maxDepth = 0;
    bool inString = false;
    bool escaped = false;
    for (const char c : text) {
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '{' || c == '[') {
            ++depth;
            if (depth > maxDepth) {
                maxDepth = depth;
            }
        } else if ((c == '}' || c == ']') && depth > 0) {
            --depth;
        }
    }
    return maxDepth;
}

std::string TypeNameOf(const Json& value) {
    if (value.is_null()) {
        return "空值 null";
    }
    if (value.is_boolean()) {
        return "布尔值";
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return "整数";
    }
    if (value.is_number_float()) {
        return "小数";
    }
    if (value.is_string()) {
        return "字符串";
    }
    if (value.is_array()) {
        return "数组";
    }
    if (value.is_object()) {
        return "对象";
    }
    return "未知类型";
}

std::string ValuePreview(const Json& value) {
    return Shorten(value.dump());
}

// 取一个数值字段：不存在/类型不对都写清楚
bool ReadNumber(const Json& object, const char* key, bool required, double fallback, double& out,
                std::string& error, const char* unit) {
    if (!object.contains(key)) {
        if (required) {
            error = std::string("缺少必需字段 \"") + key + "\"（" + unit + "）";
            return false;
        }
        out = fallback;
        return true;
    }
    const Json& value = object[key];
    if (!value.is_number()) {
        error = std::string("字段 \"") + key + "\" 必须是数字，当前是" + TypeNameOf(value) + "：" +
                ValuePreview(value);
        return false;
    }
    out = value.get<double>();
    return true;
}

bool ReadNoteObject(const Json& item, size_t index, Song& song, Note& out, std::string& error,
                    std::vector<std::string>& warnings) {
    const std::string where = "第 " + std::to_string(index + 1) + " 个音符";

    if (!item.is_object()) {
        error = where + "不是 { } 对象，而是" + TypeNameOf(item) + "：" + ValuePreview(item);
        return false;
    }

    // ---- key（必需）----
    if (!item.contains("key")) {
        error = where + "缺少 \"key\"（例如 \"Z\"、\"X\"、\",\"）";
        return false;
    }
    if (!item["key"].is_string()) {
        error = where + "的 \"key\" 必须是字符串，当前是" + TypeNameOf(item["key"]);
        return false;
    }
    const std::string keyText = item["key"].get<std::string>();
    Key key = Key::Z;
    if (!ParseKey(keyText, key)) {
        error = where + "的 \"key\" 无法识别：" + ValuePreview(item["key"]) +
                "（可用：Z X C V B N M ,）";
        return false;
    }
    out.key = key;

    // ---- mode（可缺省，缺省 = Normal）----
    if (item.contains("mode")) {
        if (!item["mode"].is_string()) {
            error = where + "的 \"mode\" 必须是字符串，当前是" + TypeNameOf(item["mode"]);
            return false;
        }
        const std::string modeText = item["mode"].get<std::string>();
        PlayMode mode = PlayMode::Normal;
        if (!ParsePlayMode(modeText, mode)) {
            error = where + "的 \"mode\" 无法识别：" + ValuePreview(item["mode"]) +
                    "（可用：Normal Left Middle Right LeftMiddle RightMiddle，或数字 1~6）";
            return false;
        }
        out.mode = mode;
    } else {
        out.mode = PlayMode::Normal;
        warnings.push_back(where + "没有写 \"mode\"，按 Normal 处理");
    }

    // ---- beat（必需，单位是“拍”）----
    double beat = 0.0;
    if (!ReadNumber(item, "beat", true, 0.0, beat, error, "单位是拍，从 0 开始")) {
        return false;
    }
    if (beat < 0.0) {
        error = where + "的 \"beat\" 不能是负数（当前 " + std::to_string(beat) + "）";
        return false;
    }

    // ---- duration（可缺省 = 一拍）----
    double durationBeats = 1.0;
    if (!ReadNumber(item, "duration", false, 1.0, durationBeats, error, "单位是拍，必须为正")) {
        return false;
    }
    if (!item.contains("duration")) {
        warnings.push_back(where + "没有写 \"duration\"，按 1 拍处理");
    }
    if (!(durationBeats > 0.0)) {
        error = where + "的 \"duration\" 必须大于 0（当前 " + std::to_string(durationBeats) + "）";
        return false;
    }

    // ---- 拍 → 绝对时间（这里就是需求书说的那次“转换”）----
    out.startTime = song.BeatsToSeconds(beat);
    out.duration = song.BeatsToSeconds(durationBeats);

    if (!NoteIsValid(out, &error)) {
        error = where + "换算后不合法：" + error;
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// SongIoResult
// ---------------------------------------------------------------------------
void SongIoResult::AddWarning(const std::string& text) {
    warnings.push_back(text);
}

std::string SongIoResult::Message() const {
    if (ok) {
        if (warnings.empty()) {
            return "成功";
        }
        return "成功（有 " + std::to_string(warnings.size()) + " 条提醒）";
    }
    return error.empty() ? "失败" : ("失败：" + error);
}

std::string SongIoResult::DetailText() const {
    std::string text;
    if (!ok) {
        text += "错误：" + (error.empty() ? std::string("未知错误") : error) + "\n";
    }
    for (const std::string& warning : warnings) {
        text += "提醒：" + warning + "\n";
    }
    if (text.empty()) {
        text = "没有问题\n";
    }
    return text;
}

// ---------------------------------------------------------------------------
// 序列化：Song → JSON
// ---------------------------------------------------------------------------
std::string SongToJsonText(const Song& song, int indent) {
    Json root = Json::object();
    root["name"] = song.name;
    root["bpm"] = song.bpm;
    root["timeSignature"] = song.timeSignature.ToString();
    root["beatUnit"] = NoteValueName(song.beatUnit);
    root["version"] = kFormatVersion;

    Json notes = Json::array();
    for (const Note& note : song.notes) {
        Json item = Json::object();
        item["key"] = KeyName(note.key);
        item["mode"] = PlayModeName(note.mode);
        // 绝对时间 → 拍（与加载时的换算严格互逆，保证保存/加载往返无损）
        item["beat"] = song.SecondsToBeats(note.startTime);
        item["duration"] = song.SecondsToBeats(note.duration);
        notes.push_back(std::move(item));
    }
    root["notes"] = std::move(notes);

    // 休止段（可选字段，老文件没有）：只存"第几拍起、停几拍"
    if (!song.rests.empty()) {
        Json rests = Json::array();
        for (const Note& rest : song.rests) {
            Json item = Json::object();
            item["beat"] = song.SecondsToBeats(rest.startTime);
            item["duration"] = song.SecondsToBeats(rest.duration);
            rests.push_back(std::move(item));
        }
        root["rests"] = std::move(rests);
    }

    return root.dump(indent) + "\n";
}

// ---------------------------------------------------------------------------
// 反序列化：JSON → Song
// ---------------------------------------------------------------------------
SongIoResult ParseSongFromJsonTextInner(const std::string& text, Song& out) {
    SongIoResult result;

    std::string body = text;
    // 去掉 UTF-8 BOM（记事本另存为 UTF-8 时会加）
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF &&
        static_cast<unsigned char>(body[1]) == 0xBB && static_cast<unsigned char>(body[2]) == 0xBF) {
        body.erase(0, 3);
    }
    if (Trim(body).empty()) {
        result.error = "内容为空（没有任何 JSON 文本）";
        return result;
    }
    // 体积上限：超大文本先拒掉，别等到解析时才发现（也防内存被撑爆）
    if (body.size() > kMaxSongJsonBytes) {
        result.error = "内容太大（超过 " + std::to_string(kMaxSongJsonBytes / (1024 * 1024)) +
                       " MB 上限），已拒绝加载。";
        return result;
    }
    // 嵌套层数上限：深嵌套会让解析递归到栈溢出（那不是异常，catch 拦不住）
    const int nested = MaxJsonNestingDepth(body);
    if (nested > kMaxSongJsonDepth) {
        result.error = "JSON 嵌套太深（" + std::to_string(nested) + " 层，上限 " +
                       std::to_string(kMaxSongJsonDepth) + " 层），已拒绝加载。";
        return result;
    }

    Json root;
    try {
        root = Json::parse(body);
    } catch (const Json::parse_error& error) {
        result.error = std::string("JSON 语法错误（第 ") + std::to_string(error.byte) +
                       " 字节处）：" + error.what();
        return result;
    } catch (const std::exception& error) {
        result.error = std::string("无法解析 JSON：") + error.what();
        return result;
    }

    if (!root.is_object()) {
        result.error = "最外层必须是 { } 对象（当前是" + TypeNameOf(root) + "）";
        return result;
    }

    Song song;

    // ---- name ----
    if (root.contains("name")) {
        if (!root["name"].is_string()) {
            result.error = "字段 \"name\" 必须是字符串，当前是" + TypeNameOf(root["name"]);
            return result;
        }
        song.name = root["name"].get<std::string>();
        if (Trim(song.name).empty()) {
            song.name = "未命名";
            result.AddWarning("字段 \"name\" 是空字符串，已用“未命名”");
        }
    } else {
        result.AddWarning("没有 \"name\" 字段，已用“未命名”");
    }

    // ---- bpm ----
    if (!root.contains("bpm")) {
        result.error = "缺少必需字段 \"bpm\"";
        return result;
    }
    if (!root["bpm"].is_number()) {
        result.error = "字段 \"bpm\" 必须是数字，当前是" + TypeNameOf(root["bpm"]);
        return result;
    }
    song.bpm = root["bpm"].get<double>();
    if (!(song.bpm > 0.0)) {
        result.error = "字段 \"bpm\" 必须大于 0（当前 " + std::to_string(song.bpm) + "）";
        return result;
    }
    if (song.bpm > 1000.0) {
        result.AddWarning("BPM 偏大：" + std::to_string(song.bpm) + "（上限建议 1000）");
    }

    // ---- timeSignature ----
    TimeSignature signature{4, 4};
    if (root.contains("timeSignature")) {
        if (!root["timeSignature"].is_string()) {
            result.error =
                std::string("字段 \"timeSignature\" 必须是字符串（如 \"4/4\"），当前是") +
                TypeNameOf(root["timeSignature"]);
            return result;
        }
        const std::string signatureText = root["timeSignature"].get<std::string>();
        if (!ParseTimeSignature(signatureText, signature)) {
            result.error = "字段 \"timeSignature\" 无法识别：" + ValuePreview(root["timeSignature"]) +
                           "（应形如 \"4/4\"、\"3/4\"、\"6/8\"）";
            return result;
        }
    } else {
        result.AddWarning("没有 \"timeSignature\" 字段，按 4/4 处理");
    }
    song.timeSignature = signature;

    // ---- beatUnit（可选：缺省按拍号分母推导，这正是“一拍”的定义）----
    NoteValue beatUnit = signature.DenominatorValue();
    if (root.contains("beatUnit")) {
        if (!root["beatUnit"].is_string()) {
            result.AddWarning("字段 \"beatUnit\" 必须是字符串，已忽略（按拍号分母 "
                              "推导）");
        } else {
            NoteValue parsed = NoteValue::Quarter;
            const std::string unitText = root["beatUnit"].get<std::string>();
            if (!ParseNoteValue(unitText, parsed)) {
                result.AddWarning("字段 \"beatUnit\" 无法识别：" + ValuePreview(root["beatUnit"]) +
                                  "，已按拍号分母推导");
            } else if (parsed != beatUnit) {
                result.AddWarning(std::string("字段 \"beatUnit\"（") + NoteValueDisplayName(parsed) +
                                  "）与拍号分母（" + signature.ToString() + "）不一致，已按拍号分母处理");
            }
        }
    }
    song.beatUnit = beatUnit;

    // ---- notes ----
    if (!root.contains("notes")) {
        result.error = "缺少必需字段 \"notes\"（音符数组）";
        return result;
    }
    if (!root["notes"].is_array()) {
        result.error = "字段 \"notes\" 必须是数组，当前是" + TypeNameOf(root["notes"]);
        return result;
    }
    const Json& notes = root["notes"];
    if (notes.empty()) {
        result.AddWarning("音符数组是空的（可以保存/打开，但没什么可弹的）");
    }
    song.notes.reserve(notes.size());
    for (size_t i = 0; i < notes.size(); ++i) {
        Note note;
        std::string error;
        if (!ReadNoteObject(notes[i], i, song, note, error, result.warnings)) {
            result.error = error;
            return result;   // out 保持原样，不留下半成品
        }
        song.notes.push_back(note);
    }

    // ---- rests（休止段，可选；老文件没有这一段，不受影响）----
    if (root.contains("rests")) {
        if (!root["rests"].is_array()) {
            result.AddWarning("字段 \"rests\" 必须是数组，已忽略");
        } else {
            for (const Json& item : root["rests"]) {
                if (!item.is_object()) {
                    result.AddWarning("休止段必须是对象，已跳过一项");
                    continue;
                }
                double beat = 0.0;
                double durationBeats = 1.0;
                std::string error;
                if (!ReadNumber(item, "beat", true, 0.0, beat, error, "单位是拍，从 0 开始")) {
                    result.AddWarning("休止段：" + error);
                    continue;
                }
                if (!ReadNumber(item, "duration", false, 1.0, durationBeats, error,
                                "单位是拍，必须为正")) {
                    result.AddWarning("休止段：" + error);
                    continue;
                }
                if (!(durationBeats > 0.0) || beat < 0.0) {
                    result.AddWarning("休止段的拍位置/时长不合法，已跳过一项");
                    continue;
                }
                Note rest;
                rest.startTime = song.BeatsToSeconds(beat);
                rest.duration = song.BeatsToSeconds(durationBeats);
                song.rests.push_back(rest);
            }
        }
    }

    // 时间轴永远保持按时间排好序（同一时刻的音符保持文件里的先后）
    song.SortNotes();
    for (const std::string& problem : song.Validate()) {
        result.AddWarning("校验：" + problem);
    }

    out = song;
    result.ok = true;
    return result;
}

// 对外入口：把整个解析过程包在 try/catch 里。任何 std::exception（类型错误、内存不足、
// 库内部抛的解析异常……）都在这里变成一条中文错误，绝不让异常穿过 WndProc 把程序带走。
SongIoResult ParseSongFromJsonText(const std::string& text, Song& out) {
    try {
        return ParseSongFromJsonTextInner(text, out);
    } catch (const std::exception& error) {
        SongIoResult failed;
        failed.error = std::string("解析曲谱时发生异常（已拦下，程序不会崩）：") + error.what();
        return failed;
    } catch (...) {
        SongIoResult failed;
        failed.error = "解析曲谱时发生未知异常（已拦下，程序不会崩）。";
        return failed;
    }
}

// ---------------------------------------------------------------------------
// 文件读写
// ---------------------------------------------------------------------------
SongIoResult SaveSongToJson(const Song& song, const std::string& path, int indent) {
    SongIoResult result;
    if (path.empty()) {
        result.error = "保存路径是空的";
        return result;
    }
    if (!(song.bpm > 0.0)) {
        result.error = "BPM 必须大于 0 才能保存（当前 " + std::to_string(song.bpm) + "）";
        return result;
    }
    if (song.notes.empty()) {
        result.AddWarning("歌曲里没有音符，仍然保存");
    }

    const std::string directory = ParentDirectory(path);
    if (!directory.empty() && !EnsureDirectory(directory)) {
        result.error = "无法创建目录：" + directory;
        return result;
    }

    const std::string text = SongToJsonText(song, indent);
    const std::wstring widePath = Utf8ToWide(path);
    if (widePath.empty()) {
        result.error = "保存路径不是合法的 UTF-8 文本：" + path;
        return result;
    }

    std::FILE* file = nullptr;
    if (_wfopen_s(&file, widePath.c_str(), L"wb") != 0 || file == nullptr) {
        result.error = "无法写入文件：" + path;
        return result;
    }
    const size_t written = std::fwrite(text.data(), 1, text.size(), file);
    const bool flushOk = (std::fflush(file) == 0);
    std::fclose(file);
    if (written != text.size() || !flushOk) {
        result.error = "写入不完整（磁盘满或没有权限）：" + path;
        return result;
    }

    result.ok = true;
    return result;
}

SongIoResult LoadSongFromJson(const std::string& path, Song& out) {
    SongIoResult result;
    if (path.empty()) {
        result.error = "文件路径是空的";
        return result;
    }
    const std::wstring widePath = Utf8ToWide(path);
    if (widePath.empty()) {
        result.error = "文件路径不是合法的 UTF-8 文本：" + path;
        return result;
    }

    std::FILE* file = nullptr;
    if (_wfopen_s(&file, widePath.c_str(), L"rb") != 0 || file == nullptr) {
        result.error = "打不开文件（不存在或没有权限）：" + path;
        return result;
    }
    // 体积上限：先看文件到底多大，超了就当场拒掉（不读进来、不解析；也防把内存撑爆）
    long long fileBytes = -1;
    if (_fseeki64(file, 0, SEEK_END) == 0) {
        fileBytes = _ftelli64(file);
        _fseeki64(file, 0, SEEK_SET);
    }
    if (fileBytes > static_cast<long long>(kMaxSongJsonBytes)) {
        std::fclose(file);
        result.error = "文件太大（超过 " + std::to_string(kMaxSongJsonBytes / (1024 * 1024)) +
                       " MB 上限）：" + path + "，已拒绝加载。";
        return result;
    }

    std::string text;
    char buffer[8192];
    size_t read = 0;
    bool tooBig = false;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        text.append(buffer, read);
        if (text.size() > kMaxSongJsonBytes) {
            tooBig = true;   // 拿不到文件大小时（例如特殊文件）在读取途中兜底
            break;
        }
    }
    const bool readError = std::ferror(file) != 0;
    std::fclose(file);
    if (readError) {
        result.error = "读取文件失败：" + path;
        return result;
    }
    if (tooBig) {
        result.error = "文件太大（超过 " + std::to_string(kMaxSongJsonBytes / (1024 * 1024)) +
                       " MB 上限）：" + path + "，已拒绝加载。";
        return result;
    }

    // 整个解析过程再包一层 try/catch：任何异常都在这里变成中文错误，
    // 绝不让它穿出去（这条路径最终会经过窗口过程，异常穿出去等于程序直接崩）。
    try {
        result = ParseSongFromJsonText(text, out);
    } catch (const std::exception& error) {
        result = SongIoResult();
        result.error = std::string("加载曲谱时发生异常（已拦下，程序不会崩）：") + error.what();
    } catch (...) {
        result = SongIoResult();
        result.error = "加载曲谱时发生未知异常（已拦下，程序不会崩）。";
    }
    return result;
}

// ---------------------------------------------------------------------------
// 路径工具
// ---------------------------------------------------------------------------
std::string ExeDirectory() {
    wchar_t buffer[MAX_PATH * 4] = {0};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0) {
        return std::string();
    }
    std::wstring wide(buffer, length);
    const size_t slash = wide.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return std::string();
    }
    wide.resize(slash + 1);
    return WideToUtf8(wide);
}

std::string DefaultSongDirectory() {
    return JoinPath(ExeDirectory(), "songs");
}

bool EnsureDirectory(const std::string& directory) {
    if (directory.empty()) {
        return false;
    }
    const std::wstring wide = Utf8ToWide(directory);
    if (wide.empty()) {
        return false;
    }
    if (CreateDirectoryW(wide.c_str(), nullptr) != FALSE) {
        return true;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return true;
    }
    // 上级目录可能还不存在：逐级建
    const std::string parent = ParentDirectory(directory);
    if (parent.empty()) {
        return false;
    }
    if (!EnsureDirectory(parent)) {
        return false;
    }
    CreateDirectoryW(wide.c_str(), nullptr);
    return GetLastError() == ERROR_ALREADY_EXISTS || GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string SanitizeFileName(const std::string& name) {
    std::string safe;
    safe.reserve(name.size());
    for (char c : name) {
        switch (c) {
            case '<': case '>': case ':': case '"': case '/': case '\\': case '|': case '?':
            case '*':
                safe.push_back('_');
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    safe.push_back('_');
                } else {
                    safe.push_back(c);
                }
                break;
        }
    }
    // 去掉首尾空白与点（Windows 不允许文件名以点或空格结尾）
    while (!safe.empty() &&
           (safe.back() == ' ' || safe.back() == '.' || safe.back() == '\r' || safe.back() == '\n')) {
        safe.pop_back();
    }
    size_t begin = 0;
    while (begin < safe.size() && (safe[begin] == ' ' || safe[begin] == '.')) {
        ++begin;
    }
    safe = safe.substr(begin);
    // 限长（按字节，不切坏 UTF-8 字符）
    constexpr size_t kMaxBytes = 80;
    if (safe.size() > kMaxBytes) {
        size_t cut = kMaxBytes;
        while (cut > 0 && (static_cast<unsigned char>(safe[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        safe.resize(cut);
    }
    if (safe.empty()) {
        safe = "未命名";
    }
    return safe;
}

std::string SongFilePathFor(const std::string& name) {
    return JoinPath(DefaultSongDirectory(), SanitizeFileName(name) + ".json");
}

std::vector<std::string> ListSongFiles(const std::string& directory) {
    std::vector<std::string> files;
    if (directory.empty()) {
        return files;
    }
    const std::wstring pattern = Utf8ToWide(JoinPath(directory, "*.json"));
    if (pattern.empty()) {
        return files;
    }
    WIN32_FIND_DATAW data{};
    HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) {
        return files;
    }
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        files.push_back(JoinPath(directory, WideToUtf8(data.cFileName)));
    } while (FindNextFileW(handle, &data) != FALSE);
    FindClose(handle);
    std::sort(files.begin(), files.end());
    return files;
}

// ---------------------------------------------------------------------------
// 形状自检（只看必需字段与类型，不做数值校验、不做换算）
// ---------------------------------------------------------------------------
SongIoResult CheckSongJsonShape(const std::string& text) {
    SongIoResult result;
    std::string body = text;
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF &&
        static_cast<unsigned char>(body[1]) == 0xBB && static_cast<unsigned char>(body[2]) == 0xBF) {
        body.erase(0, 3);
    }
    // 与加载路径同一条体积 / 嵌套上限（这个函数也吃原始文本，同样别被撑爆 / 栈溢出）
    if (body.size() > kMaxSongJsonBytes) {
        result.error = "内容太大（超过 " + std::to_string(kMaxSongJsonBytes / (1024 * 1024)) +
                       " MB 上限），已拒绝检查。";
        return result;
    }
    if (MaxJsonNestingDepth(body) > kMaxSongJsonDepth) {
        result.error = "JSON 嵌套太深（上限 " + std::to_string(kMaxSongJsonDepth) + " 层），已拒绝检查。";
        return result;
    }

    Json root;
    try {
        root = Json::parse(body);
    } catch (const std::exception& error) {
        result.error = std::string("JSON 语法错误：") + error.what();
        return result;
    }
    if (!root.is_object()) {
        result.error = "最外层必须是 { } 对象";
        return result;
    }

    struct Field {
        const char* name;
        enum Kind { KindNumber, KindString, KindArray } kind;
        bool required;
    };
    const Field fields[] = {
        {"name", Field::KindString, false},
        {"bpm", Field::KindNumber, true},
        {"timeSignature", Field::KindString, false},
        {"beatUnit", Field::KindString, false},
        {"version", Field::KindNumber, false},
        {"notes", Field::KindArray, true},
    };
    for (const Field& field : fields) {
        if (!root.contains(field.name)) {
            if (field.required) {
                result.error = std::string("缺少必需字段 \"") + field.name + "\"";
                return result;
            }
            result.AddWarning(std::string("缺少可选字段 \"") + field.name + "\"");
            continue;
        }
        const Json& value = root[field.name];
        bool typeOk = false;
        switch (field.kind) {
            case Field::KindNumber:
                typeOk = value.is_number();
                break;
            case Field::KindString:
                typeOk = value.is_string();
                break;
            case Field::KindArray:
                typeOk = value.is_array();
                break;
        }
        if (!typeOk) {
            result.error = std::string("字段 \"") + field.name + "\" 的类型不对：" + TypeNameOf(value);
            return result;
        }
    }

    if (!root["notes"].is_array()) {
        result.error = "\"notes\" 必须是数组";
        return result;
    }
    const Json& notes = root["notes"];
    for (size_t i = 0; i < notes.size(); ++i) {
        const Json& item = notes[i];
        if (!item.is_object()) {
            result.error = "第 " + std::to_string(i + 1) + " 个音符不是对象";
            return result;
        }
        if (!item.contains("key") || !item["key"].is_string()) {
            result.error = "第 " + std::to_string(i + 1) + " 个音符缺少字符串 \"key\"";
            return result;
        }
        if (!item.contains("beat") || !item["beat"].is_number()) {
            result.error = "第 " + std::to_string(i + 1) + " 个音符缺少数字 \"beat\"";
            return result;
        }
        if (item.contains("mode") && !item["mode"].is_string()) {
            result.error = "第 " + std::to_string(i + 1) + " 个音符的 \"mode\" 不是字符串";
            return result;
        }
        if (item.contains("duration") && !item["duration"].is_number()) {
            result.error = "第 " + std::to_string(i + 1) + " 个音符的 \"duration\" 不是数字";
            return result;
        }
    }

    result.ok = true;
    return result;
}

const char* RequirementExampleJson() {
    // 需求书第三十二节原文示例，一个字都没有改（测试用它核对 beat → 秒 的换算）
    return R"({
    "name": "Example Song",
    "bpm": 120,
    "timeSignature": "4/4",

    "notes": [
        {
            "key": "Z",
            "mode": "Normal",
            "beat": 0.0,
            "duration": 0.25
        },
        {
            "key": "X",
            "mode": "Normal",
            "beat": 0.25,
            "duration": 0.25
        },
        {
            "key": "C",
            "mode": "Left",
            "beat": 0.5,
            "duration": 0.25
        }
    ]
}
)";
}

}  // namespace hp
