// =============================================================================
//  Song JSON Test —— Harmonica Player / Phase 4 的测试程序（JSON 曲谱）
//
//  验证（需求书第三十一、三十二节 + Phase 4：Save / Load / 序列化 / 反序列化）：
//    1. 往返一致：内置曲 → JSON 文本 → 再加载 → 逐字段比对（时间容差 1e-9 秒）
//    2. 需求书示例：第三十二节原文那份 JSON 的 beat → 绝对时间 换算是否按公式来
//    3. 错误处理：语法错误、缺字段、类型不对、未知按键、负时间、零时长……都会给中文原因
//    4. 文件读写：保存到 dist\songs\ 再读回，含中文曲名的路径
//    5. 导出全部内置曲：一键把 7 首测试曲写成可编辑的 .json
//    6. 加载真实文件并演奏（可选真注入；默认干跑）
//
//  用法：双击运行出现编号菜单；带参数运行见 --help。
// =============================================================================

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/Log.h"
#include "core/Platform.h"
#include "core/Types.h"
#include "input/InputController.h"
#include "player/SongPlayer.h"
#include "song/Note.h"
#include "song/Song.h"
#include "song/SongSerializer.h"
#include "song/TestSongs.h"
#include "song/Timeline.h"

#include <shellapi.h>

namespace {

using hp::Song;
using hp::SongIoResult;
using hp::SongPlayer;
using hp::Timeline;

std::unique_ptr<hp::InputController> g_input;
int g_passed = 0;
int g_failed = 0;

// -----------------------------------------------------------------------------
// 小工具
// -----------------------------------------------------------------------------
std::string Fmt(const char* format, double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), format, value);
    return std::string(buffer);
}

std::string Trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

void Check(bool ok, const std::string& what) {
    if (ok) {
        ++g_passed;
        hp::LogOk("通过  " + what);
    } else {
        ++g_failed;
        hp::LogError("失败  " + what);
    }
}

void PauseForEnter() {
    hp::LogBlank();
    hp::LogInline("按回车返回菜单…");
    std::string line;
    std::getline(std::cin, line);
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

std::string JoinWarnings(const SongIoResult& result) {
    std::string text;
    for (const std::string& warning : result.warnings) {
        text += (text.empty() ? "" : " ｜ ") + warning;
    }
    return text;
}

// -----------------------------------------------------------------------------
// 1. 往返一致（内存里：Song → JSON 文本 → Song）
// -----------------------------------------------------------------------------
void RunRoundTripTest() {
    hp::LogBlank();
    hp::LogInfo("================ 1. 序列化往返一致（内置曲） ================");

    double worstErrorMs = 0.0;
    int totalNotes = 0;
    for (const std::string& name : hp::test_songs::Names()) {
        const Song song = hp::test_songs::ByName(name);
        const std::string json = hp::SongToJsonText(song);

        Song back;
        const SongIoResult result = hp::ParseSongFromJsonText(json, back);

        Check(result.ok, "「" + name + "」能加载回来（" + std::to_string(json.size()) +
                             " 字节 JSON）");
        if (!result.ok) {
            hp::LogError("      " + result.Message());
            continue;
        }

        const SongIoResult shape = hp::CheckSongJsonShape(json);
        Check(shape.ok, "「" + name + "」写出的 JSON 必需字段齐全、类型正确");

        bool same = (song.name == back.name) && (std::fabs(song.bpm - back.bpm) < 1e-9) &&
                    (song.timeSignature.ToString() == back.timeSignature.ToString()) &&
                    (song.beatUnit == back.beatUnit) &&
                    (song.notes.size() == back.notes.size());
        double worstMs = 0.0;
        if (same) {
            for (size_t i = 0; i < song.notes.size(); ++i) {
                const hp::Note& a = song.notes[i];
                const hp::Note& b = back.notes[i];
                if (a.key != b.key || a.mode != b.mode) {
                    same = false;
                    break;
                }
                worstMs = std::max(worstMs, std::fabs(a.startTime - b.startTime) * 1000.0);
                worstMs = std::max(worstMs, std::fabs(a.duration - b.duration) * 1000.0);
            }
        }
        worstErrorMs = std::max(worstErrorMs, worstMs);
        totalNotes += static_cast<int>(back.notes.size());

        Check(same && worstMs < 1e-6,
              "「" + name + "」往返后 BPM/拍号/一拍时值/音符全部一致（最大时间误差 " +
                  Fmt("%.9f", worstMs) + " ms）");
        Check(result.warnings.empty(), "「" + name + "」往返没有产生提醒（" +
                                           (result.warnings.empty() ? "无" : JoinWarnings(result)) +
                                           "）");
    }

    hp::LogBlank();
    hp::LogPlain("  合计 " + std::to_string(hp::test_songs::Names().size()) + " 首 / " +
                 std::to_string(totalNotes) + " 个音符，往返最大时间误差 " +
                 Fmt("%.9f", worstErrorMs) + " ms");
}

// -----------------------------------------------------------------------------
// 2. 需求书第三十二节的示例（beat → 绝对时间）
// -----------------------------------------------------------------------------
void RunRequirementExampleTest() {
    hp::LogBlank();
    hp::LogInfo("========== 2. 需求书第三十二节示例的换算核对 ==========");

    const std::string json = hp::RequirementExampleJson();
    hp::LogPlain("  输入就是需求书第三十二节里那份 JSON（一字未改）：");
    hp::LogPlain("      name=Example Song  bpm=120  timeSignature=4/4  3 个音符");

    Song song;
    const SongIoResult result = hp::ParseSongFromJsonText(json, song);
    Check(result.ok, "需求书示例能加载：" + result.Message());
    if (!result.ok) {
        hp::LogError("      " + result.DetailText());
        return;
    }

    Check(song.name == "Example Song", "曲名 = Example Song");
    Check(std::fabs(song.bpm - 120.0) < 1e-9, "BPM = 120");
    Check(song.timeSignature.ToString() == "4/4", "拍号 = 4/4");
    Check(song.notes.size() == 3, "音符 3 个");

    // 120 BPM、4/4 → 一拍 0.5 秒；文件里的 beat 与 duration 都是“拍”
    const double perBeat = 0.5;
    const double expectedStart[3] = {0.0 * perBeat, 0.25 * perBeat, 0.5 * perBeat};
    const hp::Key expectedKey[3] = {hp::Key::Z, hp::Key::X, hp::Key::C};
    const hp::PlayMode expectedMode[3] = {hp::PlayMode::Normal, hp::PlayMode::Normal,
                                         hp::PlayMode::Left};
    for (size_t i = 0; i < song.notes.size() && i < 3; ++i) {
        const hp::Note& note = song.notes[i];
        const bool ok = (note.key == expectedKey[i]) && (note.mode == expectedMode[i]) &&
                        (std::fabs(note.startTime - expectedStart[i]) < 1e-9) &&
                        (std::fabs(note.duration - 0.25 * perBeat) < 1e-9);
        Check(ok, "第 " + std::to_string(i + 1) + " 个音符：" +
                      std::string(hp::KeyName(note.key)) + "/" +
                      std::string(hp::PlayModeName(note.mode)) + " 起音 " +
                      Fmt("%.4f", note.startTime) + " s，持续 " + Fmt("%.4f", note.duration) +
                      " s（期望 " + Fmt("%.4f", expectedStart[i]) + " / " +
                      Fmt("%.4f", 0.25 * perBeat) + "）");
    }

    Check(std::fabs(song.TotalDuration() - 0.375) < 1e-9,
          "总时长 = 0.375 s（最后一个音符 0.25 起音 + 0.125 持续）");
    hp::LogBlank();
    hp::LogPlain("  也就是说：文件里写的是“拍”，加载时按 60/BPM 换算成秒，");
    hp::LogPlain("  换算完播放器只看到绝对时间轴（需求书第三十二节原文要求）。");
}

// -----------------------------------------------------------------------------
// 3. 错误处理电池
// -----------------------------------------------------------------------------
struct ErrorCase {
    const char* name;        // 用例名
    std::string json;        // 输入
    bool expectOk;           // 期望加载成功吗
    const char* expectText;  // 错误或提醒里必须出现的关键词
    bool expectInError;      // true = 关键词在错误里，false = 在提醒里
};

void RunErrorHandlingTest() {
    hp::LogBlank();
    hp::LogInfo("================ 3. 错误处理与宽容度 ================");

    const std::vector<ErrorCase> cases = {
        {"JSON 语法错误", "{\n  \"name\": \"坏歌\",\n  \"bpm\": 120,\n  \"notes\": [\n}",
         false, "JSON 语法错误", true},
        {"最外层不是对象", "[1, 2, 3]", false, "最外层", true},
        {"内容是空的", "   \n\t  ", false, "内容为空", true},
        {"缺 notes", "{\"name\":\"x\",\"bpm\":120,\"timeSignature\":\"4/4\"}", false, "notes",
         true},
        {"notes 不是数组", "{\"bpm\":120,\"notes\":{}}", false, "\"notes\" 必须是数组", true},
        {"缺 bpm", "{\"name\":\"x\",\"timeSignature\":\"4/4\",\"notes\":[]}", false, "bpm", true},
        {"bpm 是字符串", "{\"bpm\":\"快\",\"notes\":[]}", false, "必须是数字", true},
        {"bpm 为 0", "{\"bpm\":0,\"notes\":[]}", false, "大于 0", true},
        {"bpm 为负", "{\"bpm\":-60,\"notes\":[]}", false, "大于 0", true},
        {"缺拍号（宽容）", "{\"bpm\":120,\"notes\":[{\"key\":\"Z\",\"beat\":0}]}", true, "4/4",
         false},
        {"拍号分母不是 2 的幂", "{\"bpm\":120,\"timeSignature\":\"4/5\",\"notes\":[]}", false,
         "无法识别", true},
        {"拍号夹字母", "{\"bpm\":120,\"timeSignature\":\"4/4abc\",\"notes\":[]}", false, "无法识别",
         true},
        {"未知按键", "{\"bpm\":120,\"notes\":[{\"key\":\"Q\",\"beat\":0}]}", false, "无法识别",
         true},
        {"未知演奏方式", "{\"bpm\":120,\"notes\":[{\"key\":\"Z\",\"mode\":\"Up\",\"beat\":0}]}",
         false, "无法识别", true},
        {"缺 mode（宽容）",
         "{\"bpm\":120,\"notes\":[{\"key\":\"Z\",\"beat\":0,\"duration\":1}]}", true, "Normal",
         false},
        {"beat 为负", "{\"bpm\":120,\"notes\":[{\"key\":\"Z\",\"beat\":-1}]}", false, "负数", true},
        {"duration 为 0", "{\"bpm\":120,\"notes\":[{\"key\":\"Z\",\"beat\":0,\"duration\":0}]}",
         false, "大于 0", true},
        {"缺 duration（宽容）", "{\"bpm\":120,\"notes\":[{\"key\":\"Z\",\"beat\":0}]}", true,
         "duration", false},
        {"音符数组是空的（宽容）", "{\"bpm\":120,\"notes\":[]}", true, "空", false},
        {"第 3 个音符缺 key",
         "{\"bpm\":120,\"notes\":[{\"key\":\"Z\",\"beat\":0},{\"key\":\"X\",\"beat\":1},"
         "{\"beat\":2}]}",
         false, "第 3 个音符", true},
        {"beat 是字符串", "{\"bpm\":120,\"notes\":[{\"key\":\"Z\",\"beat\":\"一\"}]}", false,
         "必须是数字", true},
        {"beatUnit 与拍号不一致（宽容）",
         "{\"bpm\":120,\"timeSignature\":\"4/4\",\"beatUnit\":\"Eighth\","
         "\"notes\":[{\"key\":\"Z\",\"beat\":0}]}",
         true, "不一致", false},
        {"带 BOM 与中文曲名",
         "\xEF\xBB\xBF{\"name\":\"中文歌名·测试\",\"bpm\":100,\"timeSignature\":\"4/4\","
         "\"notes\":[{\"key\":\",\",\"mode\":\"Middle\",\"beat\":0,\"duration\":2}]}",
         true, "中文歌名", false},
    };

    for (const ErrorCase& item : cases) {
        Song song;
        const SongIoResult result = hp::ParseSongFromJsonText(item.json, song);
        const bool okMatches = (result.ok == item.expectOk);
        const std::string haystack =
            item.expectInError ? result.error : JoinWarnings(result) + song.name;
        const bool textMatches = Contains(haystack, item.expectText);

        if (!okMatches || !textMatches) {
            hp::LogError("      " + std::string(item.name) + "：" +
                         (okMatches ? "" : (result.ok ? "本该失败却成功了  " : "本该成功却失败了  ")) +
                         (textMatches ? "" : ("消息里没有「" + std::string(item.expectText) + "」  ")) +
                         "实际：" + result.Message());
        }
        Check(okMatches && textMatches,
              std::string(item.name) + "｜" + (item.expectOk ? "应加载成功" : "应加载失败") +
                  "，消息含「" + item.expectText + "」");
    }

    // 失败时不能留下半成品
    Song untouched = hp::test_songs::Metronome();
    const std::string before = untouched.Summary();
    hp::ParseSongFromJsonText("{\"bpm\":0,\"notes\":[]}", untouched);
    Check(untouched.Summary() == before, "加载失败时不会破坏调用方原来的 Song（不留半成品）");

    // ---- 组合模式（八度 + 半音）：新名字要认，旧名字的含义一个字都不能变 ----
    hp::LogBlank();
    hp::LogInfo("  —— 演奏方式：新组合模式与旧的 4 个名字 ——");
    struct ModeCase {
        const char* text;
        hp::PlayMode mode;
    };
    const ModeCase modeCases[] = {
        // 新增：低八度 + 升半音
        {"LeftMiddle", hp::PlayMode::LeftMiddle},
        {"leftmiddle", hp::PlayMode::LeftMiddle},
        {"LM", hp::PlayMode::LeftMiddle},
        {"5", hp::PlayMode::LeftMiddle},
        {"左中", hp::PlayMode::LeftMiddle},
        {"中左", hp::PlayMode::LeftMiddle},
        {"左键+中键", hp::PlayMode::LeftMiddle},
        // 新增：高八度 + 升半音
        {"RightMiddle", hp::PlayMode::RightMiddle},
        {"rightmiddle", hp::PlayMode::RightMiddle},
        {"RM", hp::PlayMode::RightMiddle},
        {"6", hp::PlayMode::RightMiddle},
        {"右中", hp::PlayMode::RightMiddle},
        {"中右", hp::PlayMode::RightMiddle},
        {"右键+中键", hp::PlayMode::RightMiddle},
        // 旧名字（含编号与中文）语义不变
        {"Normal", hp::PlayMode::Normal},   {"1", hp::PlayMode::Normal},
        {"正常", hp::PlayMode::Normal},     {"Left", hp::PlayMode::Left},
        {"2", hp::PlayMode::Left},          {"左键", hp::PlayMode::Left},
        {"Middle", hp::PlayMode::Middle},   {"3", hp::PlayMode::Middle},
        {"中键", hp::PlayMode::Middle},     {"Right", hp::PlayMode::Right},
        {"4", hp::PlayMode::Right},         {"右键", hp::PlayMode::Right},
    };
    const int modeTotal = static_cast<int>(sizeof(modeCases) / sizeof(modeCases[0]));
    int modeParsed = 0;
    for (const ModeCase& item : modeCases) {
        const std::string json =
            std::string("{\"bpm\":120,\"version\":1,\"notes\":[{\"key\":\"Z\",\"mode\":\"") +
            item.text + "\",\"beat\":0,\"duration\":1}]}";
        Song song;
        const SongIoResult result = hp::ParseSongFromJsonText(json, song);
        const bool good = result.ok && song.notes.size() == 1 && song.notes[0].mode == item.mode;
        if (!good) {
            hp::LogError(std::string("      mode 「") + item.text + "」应解析成 " +
                         hp::PlayModeName(item.mode) + "，实际：" + result.Message());
        }
        modeParsed += good ? 1 : 0;
    }
    Check(modeParsed == modeTotal,
          "全部 " + std::to_string(modeTotal) + " 种 mode 写法都解析正确（认对 " +
              std::to_string(modeParsed) + " 个：新组合模式 14 种 + 旧名字 12 种）");

    // 写出 → 读回：组合模式用英文名落盘，version 仍然是 1（版本号语义没变）
    {
        Song combo = hp::test_songs::Metronome();   // 只要一个合法的曲头
        combo.notes.clear();
        combo.notes.push_back(hp::Note{hp::Key::Z, hp::PlayMode::LeftMiddle, 0.0, 0.5});
        combo.notes.push_back(hp::Note{hp::Key::X, hp::PlayMode::RightMiddle, 0.5, 0.5});
        const std::string text = hp::SongToJsonText(combo);
        Check(Contains(text, "\"LeftMiddle\"") && Contains(text, "\"RightMiddle\""),
              "写出的 JSON 里就是 LeftMiddle / RightMiddle 这两个名字");
        Check(Contains(text, "\"version\": 1"), "文件版本号仍然是 1（没有改 version 的语义）");

        Song back;
        const SongIoResult roundTrip = hp::ParseSongFromJsonText(text, back);
        Check(roundTrip.ok && back.notes.size() == 2 &&
                  back.notes[0].mode == hp::PlayMode::LeftMiddle &&
                  back.notes[1].mode == hp::PlayMode::RightMiddle,
              "组合模式 JSON 往返一致（" + roundTrip.Message() + "）");
    }

    // 旧曲谱（version 1、只有旧 4 种模式）照样能加载
    {
        const std::string legacy =
            "{\n  \"name\": \"旧曲谱\",\n  \"bpm\": 120,\n  \"timeSignature\": \"4/4\",\n"
            "  \"version\": 1,\n  \"notes\": [\n"
            "    { \"key\": \"Z\", \"mode\": \"Normal\", \"beat\": 0, \"duration\": 1 },\n"
            "    { \"key\": \"X\", \"mode\": \"Left\", \"beat\": 1, \"duration\": 1 },\n"
            "    { \"key\": \"C\", \"mode\": \"Middle\", \"beat\": 2, \"duration\": 1 },\n"
            "    { \"key\": \"V\", \"mode\": \"Right\", \"beat\": 3, \"duration\": 1 }\n  ]\n}\n";
        Song song;
        const SongIoResult result = hp::ParseSongFromJsonText(legacy, song);
        Check(result.ok && song.notes.size() == 4 && song.notes[0].mode == hp::PlayMode::Normal &&
                  song.notes[1].mode == hp::PlayMode::Left &&
                  song.notes[2].mode == hp::PlayMode::Middle &&
                  song.notes[3].mode == hp::PlayMode::Right,
              "旧曲谱（version 1、只有 4 种旧模式）照常加载，四个模式含义没变");
    }

    // 路径与文件名工具
    const std::string directory = hp::DefaultSongDirectory();
    Check(!directory.empty() && Contains(directory, "songs"), "默认曲谱目录：" + directory);
    Check(hp::SanitizeFileName("a/b:c*?d") == "a_b_c__d",
          "非法字符会被替换：a/b:c*?d → " + hp::SanitizeFileName("a/b:c*?d"));
    Check(hp::SanitizeFileName("  ..  ") == "未命名", "全是点和空格的名字会退回“未命名”");
    Check(hp::SanitizeFileName("C 大调音阶") == "C 大调音阶", "中文曲名原样保留");
    Check(Contains(hp::SongFilePathFor("C 大调音阶"), "C 大调音阶.json"),
          "曲名 → 路径：" + hp::SongFilePathFor("C 大调音阶"));

    SongIoResult missing = hp::LoadSongFromJson(
        hp::SongFilePathFor("这个文件一定不存在-" + std::to_string(GetTickCount64())), untouched);
    Check(!missing.ok && Contains(missing.error, "打不开文件"),
          "加载不存在的文件会给出中文原因：" + missing.error);
}

// -----------------------------------------------------------------------------
// 4. 保存到文件再读回
// -----------------------------------------------------------------------------
void RunFileRoundTripTest(const std::string& songName) {
    hp::LogBlank();
    hp::LogInfo("================ 4. 保存到文件再读回 ================");

    const Song song = hp::test_songs::ByName(songName);
    const std::string path = hp::SongFilePathFor(song.name);
    hp::LogPlain("  曲目：" + song.name);
    hp::LogPlain("  目标文件：" + path);

    const SongIoResult saved = hp::SaveSongToJson(song, path);
    Check(saved.ok, "保存成功：" + saved.Message());
    if (!saved.ok) {
        hp::LogError("      " + saved.error);
        return;
    }

    Song back;
    const SongIoResult loaded = hp::LoadSongFromJson(path, back);
    Check(loaded.ok, "读回成功：" + loaded.Message());
    if (!loaded.ok) {
        hp::LogError("      " + loaded.error);
        return;
    }

    double worstMs = 0.0;
    bool same = (back.name == song.name) && (std::fabs(back.bpm - song.bpm) < 1e-9) &&
                (back.notes.size() == song.notes.size());
    if (same) {
        for (size_t i = 0; i < song.notes.size(); ++i) {
            worstMs = std::max(worstMs, std::fabs(song.notes[i].startTime - back.notes[i].startTime) * 1000.0);
            worstMs = std::max(worstMs, std::fabs(song.notes[i].duration - back.notes[i].duration) * 1000.0);
            if (song.notes[i].key != back.notes[i].key || song.notes[i].mode != back.notes[i].mode) {
                same = false;
            }
        }
    }
    Check(same && worstMs < 1e-6,
          "文件往返后完全一致（" + std::to_string(back.notes.size()) + " 个音符，最大时间误差 " +
              Fmt("%.9f", worstMs) + " ms）");

    // 文件里的时间轴可以直接交给播放器（Phase 3）
    const Timeline timeline = Timeline::Build(back);
    Check(timeline.NoteCount() == static_cast<int>(song.notes.size()),
          "读回的曲目能直接建出时间轴（" + std::to_string(timeline.NoteCount()) + " 个音符，" +
              Fmt("%.3f", timeline.TotalDuration()) + " s）");
}

// -----------------------------------------------------------------------------
// 5. 导出全部内置曲
// -----------------------------------------------------------------------------
void RunExportAllTest() {
    hp::LogBlank();
    hp::LogInfo("================ 5. 导出全部内置曲 ================");

    const std::string directory = hp::DefaultSongDirectory();
    hp::LogPlain("  目录：" + directory);
    Check(hp::EnsureDirectory(directory), "曲谱目录已就绪");

    int exported = 0;
    for (const std::string& name : hp::test_songs::Names()) {
        const Song song = hp::test_songs::ByName(name);
        const std::string path = hp::SongFilePathFor(song.name);
        const SongIoResult saved = hp::SaveSongToJson(song, path);
        if (!saved.ok) {
            Check(false, "导出「" + name + "」失败：" + saved.error);
            continue;
        }
        Song back;
        const SongIoResult loaded = hp::LoadSongFromJson(path, back);
        const bool same = loaded.ok && back.notes.size() == song.notes.size();
        Check(same, "导出并读回「" + song.name + "」（" + std::to_string(song.NoteCount()) +
                        " 个音符 → " + hp::SanitizeFileName(song.name) + ".json）");
        ++exported;
    }

    const std::vector<std::string> files = hp::ListSongFiles(directory);
    hp::LogBlank();
    hp::LogInfo("  目录里现在有 " + std::to_string(files.size()) + " 个 .json 文件：");
    for (const std::string& file : files) {
        hp::LogPlain("      " + file);
    }
    Check(exported == static_cast<int>(hp::test_songs::Names().size()),
          "全部 " + std::to_string(exported) + " 首内置曲都导出成功");
    Check(static_cast<int>(files.size()) >= exported,
          "目录列表能列出这些文件（" + std::to_string(files.size()) + " 个）");
}

// -----------------------------------------------------------------------------
// 5b. 加固自检：超大 / 深嵌套 JSON 必须被拒且不崩（Phase 13 修；不注入任何按键）
//     内存路径 + 真实文件路径都走一遍，顺带确认正常大小的曲谱没被误伤。
// -----------------------------------------------------------------------------
void RunRobustnessTest() {
    hp::LogBlank();
    hp::LogInfo("================ 5b. 加固自检（超大 / 深嵌套 JSON）================");

    Check(hp::kMaxSongJsonBytes == 4u * 1024u * 1024u, "体积上限是 4 MB");
    Check(hp::kMaxSongJsonDepth == 100, "嵌套层数上限是 100");

    // 1) 超大文本（内存路径）：当场被拒，且给中文原因
    std::string big = "{\"bpm\":120,\"notes\":[]}";
    big.resize(hp::kMaxSongJsonBytes + 4096, ' ');   // 末尾用空格撑过上限
    {
        Song song;
        const SongIoResult result = hp::ParseSongFromJsonText(big, song);
        Check(!result.ok && Contains(result.error, "太大"),
              "超大文本被拒（" + result.error + "）");
    }

    // 2) 超大文件（真实文件路径）：按文件大小先拒掉，不读进来、不解析
    {
        const std::string directory = hp::DefaultSongDirectory();
        Check(hp::EnsureDirectory(directory), "临时曲谱目录已就绪（放超大测试文件）");
        const std::string path = directory + "\\hp_robustness_big.json";
        bool wrote = false;
        std::FILE* file = nullptr;
        if (fopen_s(&file, path.c_str(), "wb") == 0 && file != nullptr) {
            wrote = std::fwrite(big.data(), 1, big.size(), file) == big.size();
            std::fclose(file);
        }
        Check(wrote, "临时超大文件写入成功（" + std::to_string(big.size()) + " 字节）");

        Song song;
        const SongIoResult result = hp::LoadSongFromJson(path, song);
        Check(!result.ok && Contains(result.error, "太大"),
              "超大文件被拒（" + result.error + "）");
        Check(std::remove(path.c_str()) == 0, "临时超大文件已清理（不给别人留垃圾）");
    }

    // 3) 深嵌套文本：超上限被拒（而不是让解析递归到栈溢出把程序带走）
    {
        std::string nested(static_cast<size_t>(hp::kMaxSongJsonDepth) + 50, '[');
        nested += "]";
        Song song;
        const SongIoResult result = hp::ParseSongFromJsonText(nested, song);
        Check(!result.ok && Contains(result.error, "嵌套太深"),
              "深嵌套文本被拒（" + result.error + "）");
    }

    // 4) 上限之内的正常曲谱照常能读（证明加固没有误伤正常文件）
    {
        const std::string normal =
            "{\"name\":\"加固自检\",\"bpm\":120,\"notes\":"
            "[{\"key\":\"Z\",\"beat\":0.0,\"duration\":0.25}]}";
        Song song;
        const SongIoResult result = hp::ParseSongFromJsonText(normal, song);
        Check(result.ok && song.NoteCount() == 1, "正常大小的曲谱照常能读（不被加固误伤）");
    }
}

// -----------------------------------------------------------------------------
// 6. 打印 JSON（人工核对）
// -----------------------------------------------------------------------------
void RunDumpTest(const std::string& songName) {
    hp::LogBlank();
    hp::LogInfo("================ 6. 打印 JSON 与拍表对照 ================");

    const Song song = hp::test_songs::ByName(songName);
    hp::LogPlain("  " + song.Summary());
    hp::LogBlank();
    hp::LogPlain("  —— 写出的 JSON ——");
    hp::LogPlain(hp::SongToJsonText(song, 4));
    hp::LogPlain("  —— 加载后的绝对时间轴 ——");
    song.PrintBeatTable(std::cout);

    Song back;
    const SongIoResult result = hp::ParseSongFromJsonText(hp::SongToJsonText(song), back);
    Check(result.ok && back.notes.size() == song.notes.size(),
          "JSON 与拍表描述的是同一首曲子（" + std::to_string(back.notes.size()) + " 个音符）");
}

// -----------------------------------------------------------------------------
// 7. 加载真实文件（可选演奏）
// -----------------------------------------------------------------------------
void RunLoadFileTest(const std::string& path, bool injectInput) {
    hp::LogBlank();
    hp::LogInfo("================ 7. 加载文件" +
                std::string(injectInput ? "并真实演奏" : "并干跑") + " ================");

    hp::LogPlain("  文件：" + path);
    Song song;
    const SongIoResult result = hp::LoadSongFromJson(path, song);
    if (!result.ok) {
        Check(false, "加载失败：" + result.error);
        return;
    }
    Check(true, "加载成功：" + result.Message());
    if (!result.warnings.empty()) {
        for (const std::string& warning : result.warnings) {
            hp::LogWarn("      提醒：" + warning);
        }
    }
    hp::LogPlain("      " + song.Summary());

    const Timeline timeline = Timeline::Build(song);
    if (timeline.Empty()) {
        Check(false, "曲目是空的，没法演奏");
        return;
    }

    hp::PlayerOptions options;
    options.dryRun = !injectInput;
    SongPlayer player(*g_input, options);

    double worstMs = 0.0;
    int fired = 0;
    player.SetNoteObserver([&worstMs, &fired](int, const hp::Note&, double, double lateness) {
        worstMs = std::max(worstMs, std::fabs(lateness) * 1000.0);
        ++fired;
    });

    if (injectInput) {
        hp::LogPlain("  3 秒后开始真实注入（请把焦点切到目标窗口）…");
        for (int i = 3; i > 0; --i) {
            hp::LogInline("\r  " + std::to_string(i) + " …  ");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
        hp::LogInline("\r" + std::string(24, ' ') + "\r");
    }

    player.Play(timeline);
    player.WaitForStopped(static_cast<int>(timeline.TotalDuration() * 1000.0) + 5000);

    hp::LogBlank();
    hp::LogPlain("      " + player.StatusText());
    hp::LogPlain("      " + player.ProgressText());
    Check(fired == timeline.NoteCount(),
          "从文件读出的曲目完整演奏了一遍（" + std::to_string(fired) + "/" +
              std::to_string(timeline.NoteCount()) + " 个音符，最大误差 " + Fmt("%.3f", worstMs) +
              " ms）");
    Check(player.LastStopReason() == hp::StopReason::Finished, "结束原因：歌曲播放完成");
    Check(!g_input->AnyPlayerInputDown(), "演奏结束后没有残留按键");
}

// -----------------------------------------------------------------------------
// 菜单与命令行
// -----------------------------------------------------------------------------
void PrintBanner() {
    hp::LogBlank();
    hp::LogPlain("  ============================================================");
    hp::LogPlain("    Harmonica Player  —  Song JSON Test  （Phase 4：JSON 曲谱）");
    hp::LogPlain("    游戏口琴自动演奏工具 · 曲谱读写自检工具");
    hp::LogPlain("  ============================================================");
    hp::LogBlank();
    hp::LogPlain("  格式（需求书第三十二节）：name / bpm / timeSignature / notes[key,mode,beat,duration]");
    hp::LogPlain("  文件里存“拍”，加载时换算成绝对时间；播放器只看到时间轴。");
}

void PrintSongList() {
    hp::LogInfo("内置测试曲目：");
    int index = 1;
    for (const std::string& name : hp::test_songs::Names()) {
        const Song song = hp::test_songs::ByName(name);
        hp::LogPlain("  " + std::to_string(index) + "  " + song.name + "    " +
                     std::to_string(song.NoteCount()) + " 个音符  |  " + song.timeSignature.ToString() +
                     "  |  " + Fmt("%.0f", song.bpm) + " BPM  |  " +
                     Fmt("%.3f", song.TotalDuration()) + " s");
        ++index;
    }
}

void PrintMenu() {
    hp::LogBlank();
    hp::LogPlain("  ------------------------------------------------------------");
    hp::LogPlain("   1  往返一致         内置曲 → JSON → 再加载，逐字段比对");
    hp::LogPlain("   2  需求书示例       第三十二节那份 JSON 的 beat → 秒 换算核对");
    hp::LogPlain("   3  错误处理         23 个坏输入/宽容用例，检查中文错误消息");
    hp::LogPlain("   4  保存到文件       写进 dist\\songs\\ 再读回比对");
    hp::LogPlain("   5  导出全部内置曲   一键生成 7 个可编辑的 .json");
    hp::LogPlain("   6  打印 JSON        某首曲子的 JSON 与拍表对照");
    hp::LogPlain("   7  加载文件         读 dist\\songs\\ 里的 .json（可选真实演奏）");
    hp::LogPlain("   0  退出");
    hp::LogPlain("  ------------------------------------------------------------");
    hp::LogPlain("   1/2/3/4/5/6 都不会注入任何按键；只有 7 里选“真实演奏”才会。");
}

void PrintHelp() {
    hp::LogPlain("  Harmonica Player — Song JSON Test（Phase 4）");
    hp::LogBlank();
    hp::LogPlain("  用法：song_json_test.exe [命令] [选项]");
    hp::LogBlank();
    hp::LogPlain("  不带任何参数          打开交互菜单（推荐）");
    hp::LogBlank();
    hp::LogPlain("  命令（都不注入按键）：");
    hp::LogPlain("    --roundtrip         内存往返一致（7 首内置曲）");
    hp::LogPlain("    --example           需求书示例的 beat → 秒 换算核对");
    hp::LogPlain("    --selftest          错误处理电池（23 个用例）");
    hp::LogPlain("    --save [曲名]        保存到 dist\\songs\\ 再读回（默认 C 大调音阶）");
    hp::LogPlain("    --export-all        导出全部内置曲到 dist\\songs\\");
    hp::LogPlain("    --robustness        超大 / 深嵌套 JSON 必须被拒且不崩（不注入）");
    hp::LogPlain("    --dump [曲名]        打印 JSON 与拍表");
    hp::LogPlain("    --list              列出 dist\\songs\\ 里的 .json");
    hp::LogPlain("    --all               依次跑 roundtrip + example + selftest + save + export-all + robustness");
    hp::LogBlank();
    hp::LogPlain("  命令（可能注入按键）：");
    hp::LogPlain("    --load <文件>        加载指定 .json 并干跑一遍");
    hp::LogPlain("    --inject            给 --load 用：真的按键演奏（3 秒倒计时）");
    hp::LogPlain("    --help              本帮助");
}

std::vector<std::string> Utf8Arguments() {
    std::vector<std::string> result;
    int wideCount = 0;
    LPWSTR* wideArgs = CommandLineToArgvW(GetCommandLineW(), &wideCount);
    if (wideArgs == nullptr) {
        return result;
    }
    for (int i = 0; i < wideCount; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wideArgs[i], -1, nullptr, 0, nullptr, nullptr);
        if (size <= 1) {
            result.push_back(std::string());
            continue;
        }
        std::string utf8(static_cast<size_t>(size - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wideArgs[i], -1, utf8.data(), size, nullptr, nullptr);
        result.push_back(utf8);
    }
    LocalFree(wideArgs);
    return result;
}

bool HasFlag(const std::vector<std::string>& args, const std::string& flag) {
    for (const std::string& arg : args) {
        if (arg == flag) {
            return true;
        }
    }
    return false;
}

std::string StringOption(const std::vector<std::string>& args, const std::string& name,
                         const std::string& fallback) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == name) {
            return args[i + 1];
        }
    }
    return fallback;
}

int RunInteractiveMenu() {
    for (;;) {
        PrintMenu();
        hp::LogInline("  请选择 [0-7]：");
        std::string choice;
        if (!std::getline(std::cin, choice)) {
            break;
        }
        const std::string value = Trim(choice);

        if (value == "0" || value == "q" || value == "Q") {
            break;
        } else if (value == "1") {
            RunRoundTripTest();
            PauseForEnter();
        } else if (value == "2") {
            RunRequirementExampleTest();
            PauseForEnter();
        } else if (value == "3") {
            RunErrorHandlingTest();
            PauseForEnter();
        } else if (value == "4") {
            PrintSongList();
            hp::LogInline("  输入要保存的曲名（直接回车 = C 大调音阶）：");
            std::string songName;
            std::getline(std::cin, songName);
            songName = Trim(songName);
            RunFileRoundTripTest(songName.empty() ? "C 大调音阶" : songName);
            PauseForEnter();
        } else if (value == "5") {
            RunExportAllTest();
            PauseForEnter();
        } else if (value == "6") {
            PrintSongList();
            hp::LogInline("  输入要打印的曲名（直接回车 = C 大调音阶）：");
            std::string songName;
            std::getline(std::cin, songName);
            songName = Trim(songName);
            RunDumpTest(songName.empty() ? "C 大调音阶" : songName);
            PauseForEnter();
        } else if (value == "7") {
            const std::vector<std::string> files = hp::ListSongFiles(hp::DefaultSongDirectory());
            if (files.empty()) {
                hp::LogWarn("  dist\\songs\\ 里还没有 .json，先跑一次 5（导出全部内置曲）。");
                PauseForEnter();
                continue;
            }
            hp::LogInfo("  dist\\songs\\ 里的文件：");
            for (size_t i = 0; i < files.size(); ++i) {
                hp::LogPlain("  " + std::to_string(i + 1) + "  " + files[i]);
            }
            hp::LogInline("  输入编号或完整路径（回车取消）：");
            std::string answer;
            std::getline(std::cin, answer);
            answer = Trim(answer);
            if (answer.empty()) {
                continue;
            }
            std::string path = answer;
            const int index = std::atoi(answer.c_str());
            if (index >= 1 && index <= static_cast<int>(files.size())) {
                path = files[static_cast<size_t>(index - 1)];
            }
            hp::LogInline("  要真的按键演奏吗？输入 y 真注入，直接回车 = 干跑：");
            std::string inject;
            std::getline(std::cin, inject);
            const std::string trimmedInject = Trim(inject);
            RunLoadFileTest(path, trimmedInject == "y" || trimmedInject == "Y");
            PauseForEnter();
        } else if (!value.empty()) {
            hp::LogWarn("没有这个选项：" + value);
        }
    }
    return 0;
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    const std::vector<std::string> args = Utf8Arguments();

    g_input = std::make_unique<hp::InputController>(hp::InputSendOptions());

    PrintBanner();

    int exitCode = 0;
    if (args.size() <= 1) {
        exitCode = RunInteractiveMenu();
    } else if (HasFlag(args, "--help") || HasFlag(args, "-h")) {
        PrintHelp();
    } else if (HasFlag(args, "--roundtrip")) {
        RunRoundTripTest();
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--example")) {
        RunRequirementExampleTest();
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--selftest")) {
        RunErrorHandlingTest();
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--save")) {
        RunFileRoundTripTest(StringOption(args, "--save", "C 大调音阶"));
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--export-all")) {
        RunExportAllTest();
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--robustness")) {
        RunRobustnessTest();
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--dump")) {
        RunDumpTest(StringOption(args, "--dump", "C 大调音阶"));
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--list")) {
        const std::vector<std::string> files = hp::ListSongFiles(hp::DefaultSongDirectory());
        hp::LogInfo("曲谱目录：" + hp::DefaultSongDirectory());
        if (files.empty()) {
            hp::LogWarn("  目录里没有 .json（先跑 --export-all）");
        }
        for (const std::string& file : files) {
            hp::LogPlain("  " + file);
        }
    } else if (HasFlag(args, "--all")) {
        RunRoundTripTest();
        RunRequirementExampleTest();
        RunErrorHandlingTest();
        RunFileRoundTripTest("C 大调音阶");
        RunExportAllTest();
        RunRobustnessTest();
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--load")) {
        const std::string path = StringOption(args, "--load", "");
        if (path.empty()) {
            hp::LogError("--load 需要一个文件路径");
            PrintHelp();
            exitCode = 1;
        } else {
            RunLoadFileTest(path, HasFlag(args, "--inject"));
            exitCode = (g_failed == 0) ? 0 : 1;
        }
    } else {
        hp::LogWarn("无法识别的参数，已显示帮助。");
        PrintHelp();
    }

    hp::LogInfo("正在退出并释放工具按住的输入…");
    g_input->ReleaseAll();
    g_input.reset();
    hp::LogOk("已退出。本次测试：通过 " + std::to_string(g_passed) + " 项，失败 " +
              std::to_string(g_failed) + " 项。");
    return exitCode;
}
