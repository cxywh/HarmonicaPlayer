// =============================================================================
//  Text Score Test —— Harmonica Player / Phase 9 的测试程序（文本曲谱）
//
//  验证（需求书三十三节 + Phase 9 正文）：
//    1. 需求书原文的例子：Z / X / C / L+V / B / R+N / M / MIDDLE+, 与时值写法 Z 1/4 …
//    2. 宽容写法：大小写、空白、CRLF、BOM、空行、# 与 // 注释、中文/英文/数字写法
//    3. 错误报告：未知键名、未知演奏方式、坏时值、多余内容，都要带行号与中文原因
//    4. 往返：内置曲 → 文本谱 → 曲，逐音符比对；写出"近似"时必须真有不一致（不许虚报精确）
//    5. 端到端：文本谱 → 时间轴 → 干跑播放，量墙钟与需求书算出来的时长是否一致
//
//  用法：
//    dist\text_score_test.exe --all       跑全部五组（不带参数也是跑全部）
//    dist\text_score_test.exe --doc       只跑需求书例子
//    dist\text_score_test.exe --tolerant  只跑宽容写法
//    dist\text_score_test.exe --errors    只跑错误报告
//    dist\text_score_test.exe --roundtrip 只跑往返
//    dist\text_score_test.exe --play      只跑端到端播放
// =============================================================================

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/Log.h"
#include "core/Platform.h"
#include "core/Time.h"
#include "input/InputController.h"
#include "player/SongPlayer.h"
#include "song/BeatMath.h"
#include "song/TestSongs.h"
#include "song/TextScore.h"
#include "song/Timeline.h"
#include "ui/UiFormat.h"

#include <shellapi.h>

using hp::Key;
using hp::LogLevel;
using hp::Note;
using hp::NoteValue;
using hp::PlayMode;
using hp::Song;
using hp::SongPlayer;
using hp::TimeSignature;
using hp::Timeline;

namespace {

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
    if (ok) {
        ++g_passed;
        hp::LogLine(LogLevel::Ok, std::string("通过  ") + what);
    } else {
        ++g_failed;
        hp::LogLine(LogLevel::Error, std::string("错误  ") + what);
    }
}

void Section(const std::string& text) {
    hp::LogLine(LogLevel::Info, "---- " + text + " ----");
}

void Say(const std::string& text) {
    hp::LogLine(LogLevel::Info, text);
}

std::vector<std::string> Utf8Arguments() {
    std::vector<std::string> result;
    int wideCount = 0;
    LPWSTR* wideArgs = CommandLineToArgvW(GetCommandLineW(), &wideCount);
    if (wideArgs == nullptr) {
        return result;
    }
    for (int i = 0; i < wideCount; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wideArgs[i], -1, nullptr, 0, nullptr,
                                             nullptr);
        if (size <= 1) {
            result.push_back(std::string());
            continue;
        }
        std::string utf8(static_cast<size_t>(size - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wideArgs[i], -1, &utf8[0], size, nullptr, nullptr);
        result.push_back(utf8);
    }
    LocalFree(wideArgs);
    return result;
}

Song MakeHeader(const std::string& name, double bpm, NoteValue beatUnit, TimeSignature signature) {
    Song song;
    song.name = name;
    song.bpm = bpm;
    song.beatUnit = beatUnit;
    song.timeSignature = signature;
    return song;
}

bool Near(double a, double b, double tolerance = 1e-9) {
    return std::fabs(a - b) <= tolerance;
}

// 逐音符比对；返回不一致的个数
int CountNoteDifferences(const Song& left, const Song& right) {
    if (left.notes.size() != right.notes.size()) {
        return static_cast<int>(left.notes.size() > right.notes.size() ? left.notes.size()
                                                                      : right.notes.size());
    }
    int differences = 0;
    for (size_t i = 0; i < left.notes.size(); ++i) {
        const Note& a = left.notes[i];
        const Note& b = right.notes[i];
        if (a.key != b.key || a.mode != b.mode || !Near(a.startTime, b.startTime, 1e-9) ||
            !Near(a.duration, b.duration, 1e-9)) {
            ++differences;
        }
    }
    return differences;
}

// —— 第 1 组：需求书原文的例子 ——
void RunDocTest() {
    Section("第 1 组：需求书三十三节的例子");

    const Song header = MakeHeader("文本谱测试", 120.0, NoteValue::Quarter, TimeSignature{4, 4});

    // 例子一：只有键名（时值默认 1/4）
    const std::string example1 = "Z\nX\nC\nL+V\nB\nR+N\nM\nMIDDLE+,\n";
    const hp::text_score::TextScoreResult row1 = hp::text_score::ParseTextScore(example1, header);
    Say("例子一：" + row1.Summary());
    Check(row1.ok, "例子一解析成功");
    Check(row1.noteLines == 8, "例子一 8 个音符（实际 " + std::to_string(row1.noteLines) + "）");
    if (row1.noteLines == 8) {
        const std::vector<Note>& notes = row1.song.notes;
        Check(notes[0].key == Key::Z && notes[0].mode == PlayMode::Normal, "Z = Normal + Z");
        Check(notes[3].key == Key::V && notes[3].mode == PlayMode::Left, "L+V = Left + V");
        Check(notes[5].key == Key::N && notes[5].mode == PlayMode::Right, "R+N = Right + N");
        Check(notes[6].key == Key::M && notes[6].mode == PlayMode::Normal, "M = Normal + M");
        Check(notes[7].key == Key::Comma && notes[7].mode == PlayMode::Middle,
              "MIDDLE+, = Middle + ,");
        Check(Near(notes[1].startTime, 0.5), "没写时值时默认 1/4：第 2 个音符起音 0.500 秒");
        Check(Near(notes[3].startTime, 1.5), "第 4 个音符起音 1.500 秒");
        bool allQuarter = true;
        for (const Note& note : notes) {
            if (!Near(note.duration, 0.5)) {
                allQuarter = false;
            }
        }
        Check(allQuarter, "8 个音符都是 0.500 秒（1/4 @ 120 BPM）");
    }

    // 例子二：带时值
    const std::string example2 = "Z 1/4\nX 1/4\nC 1/8\nL+V 1/8\nB 1/2\n";
    const hp::text_score::TextScoreResult row2 = hp::text_score::ParseTextScore(example2, header);
    Say("例子二：" + row2.Summary());
    Check(row2.ok, "例子二解析成功");
    Check(row2.noteLines == 5, "例子二 5 个音符");
    if (row2.noteLines == 5) {
        const std::vector<Note>& notes = row2.song.notes;
        const double starts[5] = {0.0, 0.5, 1.0, 1.25, 1.5};
        const double durations[5] = {0.5, 0.5, 0.25, 0.25, 1.0};
        bool startOk = true;
        bool durationOk = true;
        for (int i = 0; i < 5; ++i) {
            if (!Near(notes[i].startTime, starts[i])) {
                startOk = false;
            }
            if (!Near(notes[i].duration, durations[i])) {
                durationOk = false;
            }
        }
        Check(startOk, "起音按时值累加：0 / 0.5 / 1.0 / 1.25 / 1.5 秒");
        Check(durationOk, "时值：0.5 / 0.5 / 0.25 / 0.25 / 1.0 秒");
        Check(Near(row2.totalBeats, 5.0), "共 5 拍（实际 " +
                                              hp::ui::FormatBeats(row2.totalBeats) + "）");
        Check(Near(row2.song.TotalDuration(), 2.5), "整曲 2.500 秒");
    }

    // 例子三：Phase 9 正文里的那一段
    const std::string example3 = "Z\nX\nC\nL+V\nR+N\nMIDDLE+,\n";
    const hp::text_score::TextScoreResult row3 = hp::text_score::ParseTextScore(example3, header);
    Check(row3.ok && row3.noteLines == 6, "Phase 9 正文例子 6 个音符都解析成功");
    if (row3.ok) {
        std::string keys;
        for (const Note& note : row3.song.notes) {
            keys += hp::KeyName(note.key);
        }
        Check(keys == "ZXCVN,", "键序：" + keys);
    }
}

// —— 第 2 组：宽容写法 ——
void RunTolerantTest() {
    Section("第 2 组：宽容写法（大小写/空白/注释/中英文别名）");

    const Song header = MakeHeader("宽容", 120.0, NoteValue::Quarter, TimeSignature{4, 4});

    const hp::text_score::TextScoreResult lower =
        hp::text_score::ParseTextScore("z\nx\nc\n", header);
    Check(lower.ok && lower.noteLines == 3, "小写键名 z x c 认得");

    const std::string messy = "\xEF\xBB\xBF  Z  \n\r\n# 这是注释\nX 1/4 // 行尾注释\n\t M\t1/8\t\n";
    const hp::text_score::TextScoreResult mixed = hp::text_score::ParseTextScore(messy, header);
    Say("混合文本：" + mixed.Summary());
    Check(mixed.ok && mixed.noteLines == 3, "BOM / CRLF / 空行 / 注释 / Tab 都不影响解析");
    Check(mixed.skippedLines == 2, "跳过 2 行（空行与整行注释），实际 " +
                                       std::to_string(mixed.skippedLines));
    if (mixed.noteLines == 3) {
        Check(Near(mixed.song.notes[2].duration, 0.25), "M 1/8 = 0.250 秒");
    }

    struct AliasCase { const char* text; Key key; PlayMode mode; };
    const AliasCase aliases[] = {
        {"左键+V", Key::V, PlayMode::Left},
        {"中键+,", Key::Comma, PlayMode::Middle},
        {"右键+N", Key::N, PlayMode::Right},
        {"正常+Z", Key::Z, PlayMode::Normal},
        {"L+V", Key::V, PlayMode::Left},
        {"M+N", Key::N, PlayMode::Middle},
        {"MID+V", Key::V, PlayMode::Middle},
        {"R+V", Key::V, PlayMode::Right},
        {"N+V", Key::V, PlayMode::Normal},
        {"2+V", Key::V, PlayMode::Left},
        {"3+V", Key::V, PlayMode::Middle},
        {"4+V", Key::V, PlayMode::Right},
        {"逗号", Key::Comma, PlayMode::Normal},
        {"COMMA", Key::Comma, PlayMode::Normal},
        {",", Key::Comma, PlayMode::Normal},
        // 组合模式（八度 + 半音）：英文名 / 缩写 / 数字 / 中文（含自带 '+' 的写法）
        {"LeftMiddle+V", Key::V, PlayMode::LeftMiddle},
        {"LM+V", Key::V, PlayMode::LeftMiddle},
        {"lm+V", Key::V, PlayMode::LeftMiddle},
        {"5+V", Key::V, PlayMode::LeftMiddle},
        {"左中+V", Key::V, PlayMode::LeftMiddle},
        {"中左+V", Key::V, PlayMode::LeftMiddle},
        {"左键+中键+V", Key::V, PlayMode::LeftMiddle},
        {"RightMiddle+N", Key::N, PlayMode::RightMiddle},
        {"RM+N", Key::N, PlayMode::RightMiddle},
        {"rm+N", Key::N, PlayMode::RightMiddle},
        {"6+N", Key::N, PlayMode::RightMiddle},
        {"右中+N", Key::N, PlayMode::RightMiddle},
        {"中右+N", Key::N, PlayMode::RightMiddle},
        {"右键+中键+N", Key::N, PlayMode::RightMiddle},
    };
    int aliasOk = 0;
    for (const AliasCase& item : aliases) {
        const hp::text_score::TextScoreResult result =
            hp::text_score::ParseTextScore(std::string(item.text) + "\n", header);
        if (result.ok && result.noteLines == 1 && result.song.notes[0].key == item.key &&
            result.song.notes[0].mode == item.mode) {
            ++aliasOk;
        } else {
            Say("  别名没认出来：" + std::string(item.text));
        }
    }
    Check(aliasOk == static_cast<int>(sizeof(aliases) / sizeof(aliases[0])),
          "全部 " + std::to_string(sizeof(aliases) / sizeof(aliases[0])) + " 种演奏方式/键名写法都认（认对 " +
              std::to_string(aliasOk) + " 个）");

    struct ValueCase { const char* text; double seconds; };
    const ValueCase values[] = {
        {"Z 1/1", 2.0},        {"Z 1/2", 1.0},   {"Z 1/4", 0.5},
        {"Z 1/8", 0.25},       {"Z 1/16", 0.125}, {"Z 四分", 0.5},
        {"Z 四分音符", 0.5},   {"Z QUARTER", 0.5}, {"Z 4", 0.5},
        {"Z 八分", 0.25},      {"Z 十六分音符", 0.125},
    };
    int valueOk = 0;
    for (const ValueCase& item : values) {
        const hp::text_score::TextScoreResult result =
            hp::text_score::ParseTextScore(std::string(item.text) + "\n", header);
        if (result.ok && Near(result.song.notes[0].duration, item.seconds, 1e-9)) {
            ++valueOk;
        } else {
            Say("  时值没认出来：" + std::string(item.text));
        }
    }
    Check(valueOk == static_cast<int>(sizeof(values) / sizeof(values[0])),
          "全部 " + std::to_string(sizeof(values) / sizeof(values[0])) + " 种时值写法都认（认对 " +
              std::to_string(valueOk) + " 个）");

    std::string longText;
    for (int i = 0; i < 200; ++i) {
        longText += std::string(hp::KeyName(static_cast<Key>(i % hp::kKeyCount))) + " 1/8\n";
    }
    const hp::text_score::TextScoreResult longRow = hp::text_score::ParseTextScore(longText, header);
    Check(longRow.ok && longRow.noteLines == 200, "200 行长谱解析成功（" +
                                                      std::to_string(longRow.noteLines) + " 音符）");
    Check(Near(longRow.totalBeats, 100.0, 1e-9), "200 个八分音符 = 100 拍");
}

// —— 第 3 组：错误报告 ——
void RunErrorTest() {
    Section("第 3 组：错误报告（行号 + 中文原因）");

    const Song header = MakeHeader("错误", 120.0, NoteValue::Quarter, TimeSignature{4, 4});

    struct ErrorCase { const char* text; int line; const char* keyword; };
    const ErrorCase cases[] = {
        {"Q\n", 1, "键名"},
        {"Z\nX\nXX+V\n", 3, "演奏方式"},
        {"L+\n", 1, "两边"},
        {"Z 1/3\n", 1, "时值"},
        {"Z 1/4 多余\n", 1, "只能写"},
    };
    for (const ErrorCase& item : cases) {
        const hp::text_score::TextScoreResult result =
            hp::text_score::ParseTextScore(item.text, header);
        const bool lineOk = !result.issues.empty() && result.issues.front().line == item.line;
        const bool keywordOk =
            !result.issues.empty() &&
            result.issues.front().message.find(item.keyword) != std::string::npos;
        Check(!result.ok && lineOk && keywordOk,
              std::string("「") + item.text + "」被拒：行号 " + std::to_string(item.line) +
                  "，原因含「" + item.keyword + "」（实际：" +
                  (result.issues.empty() ? std::string("无") : result.issues.front().message) + "）");
    }

    const hp::text_score::TextScoreResult empty = hp::text_score::ParseTextScore("", header);
    Check(!empty.ok && !empty.issues.empty(), "空文本被拒：" + empty.Summary());

    const hp::text_score::TextScoreResult onlyComments =
        hp::text_score::ParseTextScore("# 只有注释\n// 再来一行\n\n", header);
    Check(!onlyComments.ok && !onlyComments.issues.empty(),
          "只有注释与空行时被拒：" + onlyComments.Summary());

    std::string manyErrors;
    for (int i = 0; i < 40; ++i) {
        manyErrors += "Q\n";
    }
    const hp::text_score::TextScoreResult many =
        hp::text_score::ParseTextScore(manyErrors, header);
    Check(!many.ok && many.issues.size() == 24 && many.moreIssues == 16,
          "40 行错误：列出 24 条、另有 16 条（实际 " + std::to_string(many.issues.size()) + " + " +
              std::to_string(many.moreIssues) + "）");
    Check(many.IssueText().find("另有 16 处") != std::string::npos,
          "问题详情里写明了未列出的条数");

    const hp::text_score::TextScoreResult badBpm =
        hp::text_score::ParseTextScore("Z\n", MakeHeader("坏 BPM", 0.0, NoteValue::Quarter,
                                                         TimeSignature{4, 4}));
    Check(!badBpm.ok, "曲头 BPM 为 0 时直接拒绝：" + badBpm.Summary());
}

// —— 第 4 组：往返 ——
void RunRoundTripTest() {
    Section("第 4 组：内置曲 → 文本谱 → 曲（逐音符比对）");

    struct Case { Song song; const char* label; };
    std::vector<Case> cases;
    cases.push_back(Case{hp::test_songs::Metronome(), "节拍器"});
    cases.push_back(Case{hp::test_songs::Scale(), "C 大调音阶"});
    cases.push_back(Case{hp::test_songs::ScaleFast(), "快速音阶"});
    cases.push_back(Case{hp::test_songs::Waltz(), "圆舞曲"});
    cases.push_back(Case{hp::test_songs::Jig(), "吉格（6/8）"});
    cases.push_back(Case{hp::test_songs::AllModes(), "全部演奏方式"});
    cases.push_back(Case{hp::test_songs::LongSong(200), "长曲 200 音符"});

    int exactCount = 0;
    int inexactCount = 0;
    for (const Case& item : cases) {
        std::string reason;
        const std::string text = hp::text_score::SongToTextScore(item.song, &reason);
        const hp::text_score::TextScoreResult back =
            hp::text_score::ParseTextScore(text, item.song);
        const int differences = CountNoteDifferences(item.song, back.song);

        if (reason.empty()) {
            Check(back.ok && differences == 0 && back.noteLines == item.song.NoteCount(),
                  std::string(item.label) + "：往返完全一致（" +
                      std::to_string(item.song.NoteCount()) + " 个音符）");
            if (back.ok && differences == 0) {
                ++exactCount;
                const std::string again = hp::text_score::SongToTextScore(back.song, nullptr);
                Check(again == text, std::string(item.label) + "：再写一遍文本完全一样（稳定）");
            }
        } else {
            ++inexactCount;
            Say("  " + std::string(item.label) + " 写不出完全等价的文本谱：" + reason);
            Check(back.ok, std::string(item.label) + "：虽然近似，但写出的文本仍能解析");
            Check(differences > 0,
                  std::string(item.label) + "：声明了「近似」就确实有 " +
                      std::to_string(differences) + " 个音符不一致（不是虚报）");
        }
    }
    Say("  完全一致 " + std::to_string(exactCount) + " 首，近似 " + std::to_string(inexactCount) + " 首");
    Check(exactCount >= 1, "至少有一首内置曲能完全往返");

    // 手写文本谱 → 曲 → 文本谱，必须逐字节一致
    // （组合模式写成 LM+/RM+；万一输出用的缩写和解析用的缩写不一致，这一条会立刻炸）
    const std::string handwritten =
        "Z 1/4\nX 1/4\nL+C 1/8\nMIDDLE+V 1/8\nLM+Z 1/4\nRM+X 1/4\nR+B 1/2\n, 1/4\n";
    const Song header = MakeHeader("手写", 120.0, NoteValue::Quarter, TimeSignature{4, 4});
    const hp::text_score::TextScoreResult parsed =
        hp::text_score::ParseTextScore(handwritten, header);
    Check(parsed.ok, "手写文本谱解析成功");
    if (parsed.ok) {
        const std::string again = hp::text_score::SongToTextScore(parsed.song, nullptr);
        Check(again == handwritten, "写回去与手写原文一致：" + again.substr(0, 24) + "…");
    }
}

// —— 第 5 组：端到端播放 ——
void RunPlayTest() {
    Section("第 5 组：文本谱 → 时间轴 → 干跑播放");

    const Song header = MakeHeader("端到端", 120.0, NoteValue::Quarter, TimeSignature{4, 4});

    // 8 个四分 + 8 个八分 = 12 拍 = 6.000 秒（与 Phase 8 的基准曲一致）
    std::string text;
    const char* keys = "ZXCVBNM,";
    for (int i = 0; i < 8; ++i) {
        text += std::string(1, keys[i]) + " 1/4\n";
    }
    for (int i = 0; i < 8; ++i) {
        text += std::string(1, keys[i]) + " 1/8\n";
    }

    const hp::text_score::TextScoreResult result = hp::text_score::ParseTextScore(text, header);
    Check(result.ok && result.noteLines == 16, "16 行文本谱解析成功");
    if (!result.ok) {
        return;
    }
    Check(Near(result.song.TotalDuration(), 6.0, 1e-9),
          "整曲 6.000 秒（实际 " + hp::ui::FormatSeconds3(result.song.TotalDuration()) + " 秒）");
    Check(hp::beat::BarCount(result.song) == 3, "12 拍 + 4/4 = 3 小节");
    Check(result.song.Validate().empty(), "解析出来的曲目通过 Song::Validate()");

    const Timeline timeline = Timeline::Build(result.song);
    Check(timeline.NoteCount() == 16, "时间轴收下 16 个音符（丢弃 " +
                                          std::to_string(timeline.Stats().droppedNotes) + " 个）");

    hp::InputController input;
    hp::PlayerOptions options;
    options.dryRun = true;
    options.boostPlaybackThreadPriority = false;
    SongPlayer player(input, options);
    Check(player.Play(timeline), "开始干跑演奏");
    const double start = hp::NowSeconds();
    const bool finished = player.WaitForStopped(10000);
    const double elapsed = hp::NowSeconds() - start;
    Say("  干跑整曲：" + hp::ui::FormatSeconds3(std::fabs(floor(elapsed * 1000.0 + 0.5) / 1000.0)) +
         " 秒，起音 " + std::to_string(player.FiredNoteCount()) + " / " +
         std::to_string(timeline.NoteCount()));
    Check(finished, "演奏完成");
    Check(std::fabs(elapsed - 6.0) <= 0.15, "墙钟用时在 6.000 秒 ±150 ms 内");
    Check(player.FiredNoteCount() == 16, "16 个音符都起了音");

    // 同一段文本配 6/8 曲头：一拍变八分，四分音符就成了 2 拍
    const Song jigHeader =
        MakeHeader("端到端 6/8", 120.0, NoteValue::Eighth, TimeSignature{6, 8});
    const hp::text_score::TextScoreResult jig =
        hp::text_score::ParseTextScore("Z 1/4\nX 1/4\n", jigHeader);
    Check(jig.ok && Near(jig.song.notes[0].duration, 1.0, 1e-9),
          "6/8 曲头下 1/4 等于 2 拍 = 1.000 秒");
    Check(jig.ok && Near(jig.song.notes[1].startTime, 1.0, 1e-9),
          "6/8 曲头下第二个音符起音 1.000 秒");
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    const std::vector<std::string> args = Utf8Arguments();
    bool all = true;
    bool doc = false;
    bool tolerant = false;
    bool errors = false;
    bool roundTrip = false;
    bool play = false;
    for (const std::string& arg : args) {
        if (arg == "--doc") {
            doc = true;
            all = false;
        } else if (arg == "--tolerant") {
            tolerant = true;
            all = false;
        } else if (arg == "--errors") {
            errors = true;
            all = false;
        } else if (arg == "--roundtrip") {
            roundTrip = true;
            all = false;
        } else if (arg == "--play") {
            play = true;
            all = false;
        } else if (arg == "--all") {
            all = true;
        } else if (arg == "--help" || arg == "-h") {
            std::printf("用法：text_score_test.exe [--all | --doc | --tolerant | --errors | --roundtrip | --play]\n"
                        "  --all       跑全部五组（默认）\n"
                        "  --doc       需求书三十三节与 Phase 9 的例子\n"
                        "  --tolerant  大小写/空白/注释/中英文别名\n"
                        "  --errors    错误报告（行号 + 中文原因）\n"
                        "  --roundtrip 内置曲 → 文本谱 → 曲\n"
                        "  --play      文本谱 → 时间轴 → 干跑播放\n");
            return 0;
        }
    }

    hp::LogLine(LogLevel::Info, "游戏口琴自动演奏工具 —— Phase 9 自检（文本曲谱）");
    const double started = hp::NowSeconds();
    if (all || doc) {
        RunDocTest();
    }
    if (all || tolerant) {
        RunTolerantTest();
    }
    if (all || errors) {
        RunErrorTest();
    }
    if (all || roundTrip) {
        RunRoundTripTest();
    }
    if (all || play) {
        RunPlayTest();
    }
    const double used = hp::NowSeconds() - started;
    hp::LogLine(LogLevel::Info, "本次测试：通过 " + std::to_string(g_passed) + " 项，失败 " +
                                    std::to_string(g_failed) + " 项，用时 " +
                                    hp::ui::FormatSeconds3(std::floor(used * 1000.0 + 0.5) / 1000.0) + " 秒。");
    return g_failed == 0 ? 0 : 1;
}
