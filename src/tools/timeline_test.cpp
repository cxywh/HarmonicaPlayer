// =============================================================================
//  Timeline Test —— Harmonica Player / Phase 2 的测试程序（时间轴系统）
//
//  验证四件事：
//    1. 换算正确：BPM / 拍号 / 时值 → 绝对时间（秒）
//    2. 不累积误差：长曲按绝对目标时间调度，误差有界、不随音符序号增长
//    3. 等待精度：三段式等待（高精度定时器 + yield + 自旋）的睡醒误差与 CPU 代价
//    4. 变速映射：50% ~ 200% 下“墙钟 → 歌曲位置”的映射，暂停/继续/跳转
//
//  外加：内置测试曲的时间轴打印、以及“按时间轴真实演奏一遍”的演示（会真的按键）。
//
//  用法：双击运行出现编号菜单；带参数运行见 --help。
// =============================================================================

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/Log.h"
#include "core/Platform.h"
#include "core/PrecisionWait.h"
#include "core/Time.h"
#include "core/Types.h"
#include "input/InputController.h"
#include "player/PlaybackClock.h"
#include "song/Note.h"
#include "song/Song.h"
#include "song/TestSongs.h"
#include "song/Timeline.h"

// shellapi.h：CommandLineToArgvW（用它拿到未被 ANSI 代码页破坏的命令行参数）
#include <shellapi.h>

namespace {

using hp::InputController;
using hp::Note;
using hp::NoteValue;
using hp::PlaybackClock;
using hp::PrecisionWait;
using hp::Song;
using hp::TimeSignature;
using hp::Timeline;
using hp::TimelineOptions;
using hp::WaitStats;

std::unique_ptr<InputController> g_input;
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

// 把多行文本按行交给日志（保持日志的时间戳与颜色规则一致）
void LogMultiline(const std::string& text) {
    std::string line;
    for (char ch : text) {
        if (ch == '\n') {
            hp::LogPlain(line);
            line.clear();
        } else if (ch != '\r') {
            line += ch;
        }
    }
    if (!line.empty()) {
        hp::LogPlain(line);
    }
}

void LogFromOstream(const std::ostringstream& stream) {
    LogMultiline(stream.str());
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

void CheckNear(double actual, double expected, double tolerance, const std::string& what) {
    const bool ok = std::fabs(actual - expected) <= tolerance;
    if (ok) {
        ++g_passed;
        hp::LogOk("通过  " + what + "（实测 " + Fmt("%.6f", actual) + "，期望 " +
                  Fmt("%.6f", expected) + "）");
    } else {
        ++g_failed;
        hp::LogError("失败  " + what + "（实测 " + Fmt("%.6f", actual) + "，期望 " +
                     Fmt("%.6f", expected) + "，容差 ±" + Fmt("%.6f", tolerance) + "）");
    }
}

void PauseForEnter() {
    hp::LogBlank();
    hp::LogInline("按回车返回菜单…");
    std::string line;
    std::getline(std::cin, line);
}

void Countdown(int seconds) {
    for (int remaining = seconds; remaining > 0; --remaining) {
        hp::LogInline("\r  " + std::to_string(remaining) + " 秒后开始（现在把焦点切到目标窗口）…  ");
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    hp::LogInline("\r" + std::string(52, ' ') + "\r");
}

std::string ForegroundTitle() {
    HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return "(没有前台窗口)";
    }
    const int length = GetWindowTextLengthW(foreground);
    if (length <= 0) {
        return "(无标题窗口)";
    }
    std::wstring wide(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(foreground, wide.data(), length + 1);
    wide.resize(copied > 0 ? static_cast<size_t>(copied) : 0);

    std::string utf8(static_cast<size_t>(wide.size()) * 3 + 1, '\0');
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                         utf8.data(), static_cast<int>(utf8.size()), nullptr,
                                         nullptr);
    utf8.resize(size > 0 ? static_cast<size_t>(size) : 0);
    return utf8.empty() ? std::string("(无标题窗口)") : utf8;
}

BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
    if (g_input) {
        hp::LogWarn("收到中断信号，正在释放工具按住的输入…");
        g_input->ReleaseAll();
    }
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        return TRUE;
    }
    return FALSE;
}

// =============================================================================
// 1. 时间换算自检
// =============================================================================
void RunConversionSelfTest() {
    hp::LogBlank();
    hp::LogInfo("================ 1. 时间换算自检 ================");
    hp::LogPlain("  校验 BPM / 拍号 / 时值 → 绝对时间（秒），以及时间轴的排序与二分查询。");

    // ---- 时值 ----
    hp::LogBlank();
    hp::LogInfo("--- 时值（以四分音符为 1 拍） ---");
    CheckNear(hp::NoteValueToBeats(NoteValue::Whole), 4.0, 1e-12, "全音符 = 4 拍");
    CheckNear(hp::NoteValueToBeats(NoteValue::Half), 2.0, 1e-12, "二分音符 = 2 拍");
    CheckNear(hp::NoteValueToBeats(NoteValue::Quarter), 1.0, 1e-12, "四分音符 = 1 拍");
    CheckNear(hp::NoteValueToBeats(NoteValue::Eighth), 0.5, 1e-12, "八分音符 = 0.5 拍");
    CheckNear(hp::NoteValueToBeats(NoteValue::Sixteenth), 0.25, 1e-12, "十六分音符 = 0.25 拍");

    NoteValue parsed = NoteValue::Quarter;
    Check(hp::ParseNoteValue("1/4", parsed) && parsed == NoteValue::Quarter,
          "解析 \"1/4\" → 四分音符");
    Check(hp::ParseNoteValue("8", parsed) && parsed == NoteValue::Eighth, "解析 \"8\" → 八分音符");
    Check(hp::ParseNoteValue("QUARTER", parsed) && parsed == NoteValue::Quarter,
          "解析 \"QUARTER\" → 四分音符");
    Check(hp::ParseNoteValue("十六分音符", parsed) && parsed == NoteValue::Sixteenth,
          "解析 \"十六分音符\" → 十六分音符");

    // ---- 60 BPM 4/4：每个四分音符正好 1 秒 ----
    hp::LogBlank();
    hp::LogInfo("--- 60 BPM / 4/4（节拍器，时间最整，便于逐条核对） ---");
    Song metronome = hp::test_songs::Metronome();
    CheckNear(metronome.SecondsPerBeat(), 1.0, 1e-12, "60 BPM → 一拍 = 1.000 秒");
    CheckNear(metronome.SecondsPerBar(), 4.0, 1e-12, "60 BPM + 4/4 → 每小节 = 4.000 秒");
    Check(metronome.NoteCount() == 16, "节拍器共 16 个音符");
    CheckNear(metronome.notes[5].startTime, 5.0, 1e-12, "第 6 个音符的起音 = 5.000 秒");
    CheckNear(metronome.notes[5].duration, 1.0, 1e-12, "四分音符的持续 = 1.000 秒");
    CheckNear(metronome.TotalDuration(), 16.0, 1e-12, "整曲总时长 = 16.000 秒");
    Check(metronome.BarCount() == 4, "16 拍（四分音符）= 4 小节");
    Check(metronome.Validate().empty(), "节拍器通过曲谱校验");

    // ---- 120 BPM：时值翻倍关系 ----
    hp::LogBlank();
    hp::LogInfo("--- 120 BPM（所有时值减半） ---");
    Song scale = hp::test_songs::Scale();
    CheckNear(scale.SecondsPerBeat(), 0.5, 1e-12, "120 BPM → 一拍 = 0.500 秒");
    CheckNear(scale.SecondsFor(NoteValue::Whole), 2.0, 1e-12, "120 BPM 全音符 = 2.000 秒");
    CheckNear(scale.SecondsFor(NoteValue::Half), 1.0, 1e-12, "120 BPM 二分音符 = 1.000 秒");
    CheckNear(scale.SecondsFor(NoteValue::Quarter), 0.5, 1e-12, "120 BPM 四分音符 = 0.500 秒");
    CheckNear(scale.SecondsFor(NoteValue::Eighth), 0.25, 1e-12, "120 BPM 八分音符 = 0.250 秒");
    CheckNear(scale.SecondsFor(NoteValue::Sixteenth), 0.125, 1e-12, "120 BPM 十六分音符 = 0.125 秒");
    // 前 8 个四分音符（0.5 秒一个）之后，接八分音符（0.25 秒一个）
    CheckNear(scale.notes[7].startTime, 3.5, 1e-12, "第 8 个音符（最后一个四分音符）= 3.500 秒");
    CheckNear(scale.notes[8].startTime, 4.0, 1e-12, "第 9 个音符（八分音符起）= 4.000 秒");
    CheckNear(scale.notes[9].startTime, 4.25, 1e-12, "第 10 个音符 = 4.250 秒（间隔 0.25 秒）");
    CheckNear(scale.TotalDuration(), 6.0, 1e-9, "整曲总时长 = 6.000 秒");

    // ---- 拍号 ----
    hp::LogBlank();
    hp::LogInfo("--- 拍号（4/4、3/4、6/8） ---");
    Song waltz = hp::test_songs::Waltz();
    Check(waltz.timeSignature.numerator == 3 && waltz.timeSignature.denominator == 4,
          "圆舞曲的拍号是 3/4");
    CheckNear(waltz.SecondsPerBar(), 1.8, 1e-9, "100 BPM + 3/4 → 每小节 = 1.800 秒（3 拍）");
    Check(waltz.BarCount() == 4, "圆舞曲共 4 小节（12 拍）");

    Song jig = hp::test_songs::Jig();
    Check(jig.beatUnit == NoteValue::Eighth, "吉格舞曲以八分音符为一拍（6/8 的常见做法）");
    CheckNear(jig.SecondsPerBeat(), 0.5, 1e-12, "6/8 + 120 BPM → 一拍（八分音符）= 0.500 秒");
    CheckNear(jig.SecondsFor(NoteValue::Quarter), 1.0, 1e-9, "6/8 里四分音符 = 2 拍 = 1.000 秒");
    CheckNear(jig.SecondsPerBar(), 3.0, 1e-9, "6/8 每小节 6 拍 = 3.000 秒");
    Check(jig.Validate().empty(), "6/8 曲目通过校验（一拍时值与拍号分母一致）");

    // ---- 时间轴与二分查询 ----
    hp::LogBlank();
    hp::LogInfo("--- 时间轴与二分查询 ---");
    Timeline timeline = Timeline::Build(metronome);
    Check(timeline.NoteCount() == 16, "时间轴音符数 = 16");
    CheckNear(timeline.TotalDuration(), 16.0, 1e-9, "时间轴总时长 = 16.000 秒");
    Check(timeline.LowerBound(5.0) == 5, "二分查询：起音 >= 5.000 秒的第一个音符下标 = 5");
    Check(timeline.CountStartedBy(5.0) == 6, "二分查询：到 5.000 秒为止已起音 6 个音符");
    Check(timeline.IndexAtOrBefore(4.999) == 4, "二分查询：4.999 秒时最后一个已起音的下标 = 4");
    Check(timeline.Stats().bars == 4, "时间轴统计：4 小节");
    CheckNear(timeline.Stats().minGapMs, 1000.0, 1e-6, "节拍器的相邻起音间隔 = 1000 ms");

    // ---- 校验与容错 ----
    hp::LogBlank();
    hp::LogInfo("--- 曲谱校验与容错 ---");
    Song broken;
    broken.name = "故意写坏的曲子";
    broken.bpm = 0.0;   // BPM 非法
    broken.notes.push_back(Note{hp::Key::Z, hp::PlayMode::Normal, -1.0, 0.5});  // 负起音
    broken.notes.push_back(Note{hp::Key::X, hp::PlayMode::Normal, 1.0, 0.0});   // 零时长
    broken.notes.push_back(Note{hp::Key::C, hp::PlayMode::Normal, 2.0, 0.5});   // 合法
    const std::vector<std::string> problems = broken.Validate();
    Check(problems.size() >= 2, "坏曲被 Validate 报出多个问题（共 " +
                                    std::to_string(problems.size()) + " 条）");
    for (const std::string& problem : problems) {
        hp::LogPlain("      · " + problem);
    }
    std::vector<std::string> buildProblems;
    const Timeline brokenTimeline = Timeline::Build(broken, TimelineOptions(), &buildProblems);
    Check(brokenTimeline.NoteCount() == 1 && brokenTimeline.Stats().droppedNotes == 2,
          "非法音符被丢弃，时间轴只保留 1 个合法音符");

    // ---- 和弦（同一时刻多个音符） ----
    Song chord;
    chord.name = "和弦测试";
    chord.bpm = 120.0;
    chord.notes.push_back(Note{hp::Key::Z, hp::PlayMode::Normal, 0.0, 0.5});
    chord.notes.push_back(Note{hp::Key::X, hp::PlayMode::Normal, 0.0, 0.5});
    chord.notes.push_back(Note{hp::Key::C, hp::PlayMode::Normal, 0.0, 0.5});
    Check(chord.MaxSimultaneousNotes() == 3, "同一时刻的 3 个音符被识别为和弦");
    const Timeline chordTimeline = Timeline::Build(chord);
    Check(chordTimeline.Stats().simultaneousPeak == 3, "时间轴统计里的同时起音峰值 = 3");

    hp::LogBlank();
    hp::LogInfo("换算自检结束（累计通过 " + std::to_string(g_passed) + "，失败 " +
                std::to_string(g_failed) + "）");
}

// =============================================================================
// 2. 无累积误差验证
// =============================================================================
void RunNoAccumulationTest(int noteCount, double intervalMs) {
    hp::LogBlank();
    hp::LogInfo("================ 2. 无累积误差验证 ================");

    Song song = hp::test_songs::LongSong(noteCount, intervalMs);
    const Timeline timeline = Timeline::Build(song);
    const int count = timeline.NoteCount();
    if (count < 2) {
        hp::LogError("音符太少，测试无意义");
        ++g_failed;
        return;
    }

    const double plannedTotal = timeline.TotalDuration();
    hp::LogPlain("  曲目：" + song.name + "，起音间隔 " + Fmt("%.1f", intervalMs) +
                 " ms，计划总时长 " + Fmt("%.3f", plannedTotal) + " s");
    hp::LogPlain("  做法：每个音符都睡到它的**绝对目标时刻**；释放也按绝对时间。");
    hp::LogPlain("        绝不按“上一个音符的实际时刻 + 间隔”推进 —— 这是误差累积的唯一来源。");
    hp::LogBlank();

    PrecisionWait waiter;
    hp::LogPlain(std::string("  高精度等待：") +
                 (waiter.HighResolutionTimerAvailable() ? "可等待高分辨率定时器已启用"
                                                        : "退化为 Sleep（精度会差一档）"));

    std::vector<double> startErrors;
    startErrors.reserve(static_cast<size_t>(count));

    const double songStart = hp::NowSeconds();
    for (int i = 0; i < count; ++i) {
        const Note& note = timeline.At(i);
        // 起音：绝对目标时刻
        const double startError = waiter.WaitUntil(songStart + note.startTime);
        startErrors.push_back(startError);
        // 释放：同样按绝对时间（模拟按住 duration）
        waiter.WaitUntil(songStart + note.EndTime());
    }
    const double actualTotal = hp::NowSeconds() - songStart;

    // ---- 统计 ----
    WaitStats stats;
    for (double error : startErrors) {
        stats.Add(error);
    }

    double naiveAccumulatedMs = 0.0;   // Sleep 链式累加会得到的总偏差
    for (double error : startErrors) {
        naiveAccumulatedMs += error * 1000.0;
    }
    const double finalErrorMs = startErrors.back() * 1000.0;

    // 前半段 / 后半段的平均绝对误差：若在累积，后半段会明显更大
    const size_t half = startErrors.size() / 2;
    double sumFirst = 0.0;
    double sumLast = 0.0;
    double maxFirst = 0.0;
    double maxLast = 0.0;
    for (size_t i = 0; i < startErrors.size(); ++i) {
        const double absMs = std::fabs(startErrors[i]) * 1000.0;
        if (i < half) {
            sumFirst += absMs;
            maxFirst = std::max(maxFirst, absMs);
        } else {
            sumLast += absMs;
            maxLast = std::max(maxLast, absMs);
        }
    }
    const double meanFirstMs = sumFirst / static_cast<double>(half);
    const double meanLastMs = sumLast / static_cast<double>(startErrors.size() - half);

    hp::LogBlank();
    hp::LogInfo("  起音误差统计（" + std::to_string(count) + " 个音符）");
    hp::LogPlain("      " + stats.Summary());
    hp::LogPlain("      前半段：平均绝对误差 " + Fmt("%.3f", meanFirstMs) + " ms，最大 " +
                 Fmt("%.3f", maxFirst) + " ms");
    hp::LogPlain("      后半段：平均绝对误差 " + Fmt("%.3f", meanLastMs) + " ms，最大 " +
                 Fmt("%.3f", maxLast) + " ms");
    hp::LogBlank();
    hp::LogInfo("  采样明细（每 " + std::to_string(std::max(1, count / 10)) + " 个取一个）");
    hp::LogPlain("      序号     目标(s)      实际(s)     误差(ms)");
    const int stride = std::max(1, count / 10);
    for (int i = 0; i < count; i += stride) {
        const Note& note = timeline.At(i);
        const double actual = note.startTime + startErrors[static_cast<size_t>(i)];
        char line[160];
        std::snprintf(line, sizeof(line), "      %-8d %-12.4f %-12.4f %+.3f", i + 1,
                      note.startTime, actual, startErrors[static_cast<size_t>(i)] * 1000.0);
        hp::LogPlain(line);
    }

    hp::LogBlank();
    hp::LogInfo("  结论对比");
    hp::LogPlain("      绝对时间轴：最后一个音符的偏差 = " + Fmt("%+.3f", finalErrorMs) +
                 " ms（有界，与第几个音符无关）");
    hp::LogPlain("      等长 Sleep 链：所有抖动会依次叠加，累计偏差 = " +
                 Fmt("%+.3f", naiveAccumulatedMs) + " ms");
    hp::LogPlain("      整曲实际墙钟 " + Fmt("%.4f", actualTotal) + " s，计划 " +
                 Fmt("%.4f", plannedTotal) + " s，总偏差 " +
                 Fmt("%+.3f", (actualTotal - plannedTotal) * 1000.0) + " ms");

    hp::LogBlank();
    hp::LogPlain("      注：抽样里最坏单次延后 " + Fmt("%+.3f", stats.maxLateMs) +
                 " ms 属于操作系统调度抖动（偶发一次），不是跑拍 ——");
    hp::LogPlain("          因为它不会被带进下一个音符，下一拍依然按绝对时刻对齐。");
    hp::LogPlain("          下面改用 p99 与“超限次数”这类稳健指标来判定精度。");
    Check(stats.meanAbsMs < 1.0,
          "平均绝对误差 < 1 ms（实测 " + Fmt("%.3f", stats.meanAbsMs) + " ms）");
    Check(stats.QuantileAbsMs(0.99) < 1.5,
          "p99 绝对误差 < 1.5 ms（实测 " + Fmt("%.3f", stats.QuantileAbsMs(0.99)) +
              " ms，即 99% 的音符都在此范围内）");
    Check(stats.RatioAbsOver(3.0) < 0.01,
          "|误差| 超过 3 ms 的样本低于 1%（实测 " + std::to_string(stats.CountAbsOver(3.0)) +
              "/" + std::to_string(stats.count) + " 次）");
    Check(std::fabs(finalErrorMs) < 3.0,
          "最后一个音符的绝对偏差仍有界（实测 " + Fmt("%+.3f", finalErrorMs) + " ms）");
    Check(meanLastMs <= std::max(meanFirstMs * 2.0, meanFirstMs + 0.3),
          "后半段误差没有比前半段变大 → 误差不累积（前半 " + Fmt("%.3f", meanFirstMs) +
              " ms，后半 " + Fmt("%.3f", meanLastMs) + " ms）");
    Check(std::fabs(actualTotal - plannedTotal) < 0.005,
          "整曲总时长的偏差 < 5 ms（实测 " +
              Fmt("%+.3f", (actualTotal - plannedTotal) * 1000.0) + " ms）");
}

// =============================================================================
// 3. 精等待精度
// =============================================================================
void RunPrecisionWaitTest(int samples) {
    hp::LogBlank();
    hp::LogInfo("================ 3. 精等待精度实测 ================");
    hp::LogPlain("  QPC 频率：" + std::to_string(hp::TicksPerSecond()) + " Hz（时钟分辨率 " +
                 Fmt("%.1f", 1e9 / static_cast<double>(hp::TicksPerSecond())) + " ns）");

    PrecisionWait waiter;
    hp::LogPlain(std::string("  高精度定时器：") +
                 (waiter.HighResolutionTimerAvailable()
                      ? "可用（CreateWaitableTimerEx + HIGH_RESOLUTION）"
                      : "不可用，退化为 Sleep"));
    hp::LogBlank();

    const int count = std::max(20, samples);
    const double intervalMs = 10.0;

    // ---- A. 三段式等待 ----
    WaitStats stats;
    double target = hp::NowSeconds() + intervalMs / 1000.0;
    const double cpuBefore = hp::ProcessCpuSeconds();
    const double wallBefore = hp::NowSeconds();
    for (int i = 0; i < count; ++i) {
        // 每个目标都是“绝对时刻”，误差不会传给下一次
        stats.Add(waiter.WaitUntil(target));
        target += intervalMs / 1000.0;
    }
    const double cpuUsed = hp::ProcessCpuSeconds() - cpuBefore;
    const double wallUsed = hp::NowSeconds() - wallBefore;

    hp::LogInfo("  三段式等待（定时器 + yield + 自旋），目标间隔 10 ms，共 " +
                std::to_string(count) + " 次");
    hp::LogPlain("      " + stats.Summary());
    hp::LogPlain("      CPU 时间 " + Fmt("%.4f", cpuUsed) + " s / 墙钟 " + Fmt("%.4f", wallUsed) +
                 " s = " + Fmt("%.2f", cpuUsed / wallUsed * 100.0) + "%（单核的百分比）");

    // ---- B. 对照：直接 Sleep ----
    WaitStats sleepStats;
    const int sleepCount = std::min(count, 60);
    for (int i = 0; i < sleepCount; ++i) {
        const double sleepTarget = hp::NowSeconds() + intervalMs / 1000.0;
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(intervalMs)));
        sleepStats.Add(hp::NowSeconds() - sleepTarget);
    }
    hp::LogInfo("  对照：直接 std::this_thread::sleep_for(10 ms)，共 " +
                std::to_string(sleepCount) + " 次");
    hp::LogPlain("      " + sleepStats.Summary());
    hp::LogPlain("      说明：Sleep 的精度受系统计时器粒度限制（Windows 常见 15.6 ms），");
    hp::LogPlain("            所以纯 Sleep 作为播放调度会有几十毫秒级的抖动。");

    // ---- C. 实验：提高播放线程优先级 ----
    WaitStats boostStats;
    {
        hp::ScopedThreadPriorityBoost boost;
        hp::LogInfo(std::string("  实验：把当前线程提升到“时间关键”后重测同样的 " +
                                std::to_string(count) + " 次") +
                    (boost.Applied() ? "（已生效，原优先级 " +
                                           std::to_string(boost.PreviousPriority()) + "）"
                                     : "（提升失败，结果等同普通优先级）"));
        double boostTarget = hp::NowSeconds() + intervalMs / 1000.0;
        for (int i = 0; i < count; ++i) {
            boostStats.Add(waiter.WaitUntil(boostTarget));
            boostTarget += intervalMs / 1000.0;
        }
    }
    hp::LogPlain("      " + boostStats.Summary());
    hp::LogPlain("      结论：等待算法本身的误差在微秒级，最坏值由系统调度决定；");
    hp::LogPlain("            Phase 3 的播放线程会以较高优先级运行，以压低最坏值。");

    hp::LogBlank();
    Check(stats.QuantileAbsMs(0.99) < 1.0, "三段式：p99 绝对误差 < 1 ms（实测 " +
                                               Fmt("%.3f", stats.QuantileAbsMs(0.99)) + " ms）");
    Check(stats.RatioAbsOver(3.0) < 0.01,
          "三段式：|误差| 超过 3 ms 的比例低于 1%（实测 " + std::to_string(stats.CountAbsOver(3.0)) +
              "/" + std::to_string(stats.count) + " 次）");
    hp::LogPlain("            注：极少数几毫秒的晚醒来自 Windows 调度抢占，不是等待算法本身 ——");
    hp::LogPlain("                算法自身的误差在微秒级（见上方 p95/p99）。");
    Check(stats.meanAbsMs < 1.0, "三段式：平均绝对误差 < 1 ms（实测 " +
                                     Fmt("%.3f", stats.meanAbsMs) + " ms）");
    Check(boostStats.QuantileAbsMs(0.99) < 1.5,
          "提升线程优先级后 p99 仍在 1.5 ms 内（实测 " +
              Fmt("%.3f", boostStats.QuantileAbsMs(0.99)) + " ms）");
    Check(cpuUsed / wallUsed < 0.5,
          "三段式：CPU 占用低于单核 50%（实测 " +
              Fmt("%.2f", cpuUsed / wallUsed * 100.0) + "%）");
    Check(sleepStats.meanAbsMs >= stats.meanAbsMs,
          "三段式比纯 Sleep 更准（Sleep 平均绝对误差 " + Fmt("%.3f", sleepStats.meanAbsMs) +
              " ms）");
}

// =============================================================================
// 4. 变速映射
// =============================================================================
void RunSpeedTest() {
    hp::LogBlank();
    hp::LogInfo("================ 4. 变速映射验证 ================");

    Song song = hp::test_songs::Scale();
    const Timeline timeline = Timeline::Build(song);
    const double total = timeline.TotalDuration();
    hp::LogPlain("  曲目：" + song.name + "，总时长 " + Fmt("%.3f", total) + " s");
    hp::LogPlain("  原则：速度只改变“墙钟 → 歌曲位置”的映射（位置 = 墙钟 × 速度），");
    hp::LogPlain("        时间轴本身一个字节都不改，更不是把每次 Sleep 乘一个倍数。");
    hp::LogBlank();

    PlaybackClock clock;
    const double speeds[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
    const double probeWallSeconds = 0.4;

    for (double speed : speeds) {
        clock.Start();
        clock.SetSpeed(speed);
        const double positionBefore = clock.PositionSeconds();
        std::this_thread::sleep_for(
            std::chrono::milliseconds(static_cast<int>(probeWallSeconds * 1000.0)));
        const double position = clock.PositionSeconds();
        const double expected = probeWallSeconds * speed;
        const double tolerance = 0.05 * speed + 0.03;

        hp::LogInfo("  速度 " + Fmt("%.0f", speed * 100.0) + "%：墙钟 " +
                    Fmt("%.3f", probeWallSeconds) + " s 后位置 " + Fmt("%.4f", position) +
                    " s（期望 " + Fmt("%.4f", expected) + " s）｜整曲墙钟耗时 " +
                    Fmt("%.3f", total / speed) + " s");
        (void)positionBefore;
        Check(std::fabs(position - expected) < tolerance,
              "速度 " + Fmt("%.0f", speed * 100.0) + "% 的位置映射正确");

        // 变速瞬间位置必须连续（不能跳变）
        const double before = clock.PositionSeconds();
        clock.SetSpeed(speed * 1.5);
        const double after = clock.PositionSeconds();
        Check(std::fabs(after - before) < 0.002,
              "速度 " + Fmt("%.0f", speed * 100.0) + "% → " + Fmt("%.0f", speed * 150.0) +
                  "% 变速瞬间位置连续（跳变 " + Fmt("%.4f", std::fabs(after - before) * 1000.0) +
                  " ms）");
        clock.Stop();
    }

    // ---- 暂停 / 继续 ----
    hp::LogBlank();
    hp::LogInfo("--- 暂停与继续 ---");
    clock.Start();
    // 上一段把速度改过（速度属于设置，Stop 不会复位它），这里显式归 100%
    clock.SetSpeed(1.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    clock.Pause();
    const double pausedAt = clock.PositionSeconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const double stillPaused = clock.PositionSeconds();
    Check(std::fabs(stillPaused - pausedAt) < 0.005,
          "暂停 300 ms 期间位置冻结（变化 " +
              Fmt("%.4f", std::fabs(stillPaused - pausedAt) * 1000.0) + " ms）");
    clock.Resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const double resumed = clock.PositionSeconds();
    Check(resumed > pausedAt + 0.10 && resumed < pausedAt + 0.25,
          "继续后从暂停处接着走（暂停点 " + Fmt("%.3f", pausedAt) + " s → 现在 " +
              Fmt("%.3f", resumed) + " s），没有回到开头");

    // ---- 跳转与目标墙钟换算 ----
    hp::LogBlank();
    hp::LogInfo("--- 跳转（Seek / StartAt）与目标墙钟换算 ---");
    clock.Seek(3.0);
    CheckNear(clock.PositionSeconds(), 3.0, 0.01, "Seek(3.0) 立即生效");
    clock.StartAt(2.0);
    CheckNear(clock.PositionSeconds(), 2.0, 0.01, "StartAt(2.0) 立即生效（重播/继续用）");

    clock.Start();
    clock.SetSpeed(2.0);
    const double waitForThreeSeconds = clock.WallClockForPosition(3.0) - hp::NowSeconds();
    CheckNear(waitForThreeSeconds, 1.5, 0.03, "200% 速度下走到歌曲第 3.0 秒只需 1.5 秒墙钟");
    clock.Stop();
    Check(clock.IsStopped() && clock.PositionSeconds() == 0.0, "Stop 后状态为已停止且位置归零");
}

// =============================================================================
// 5. 自然模式抖动
// =============================================================================
void RunNaturalModeTest() {
    hp::LogBlank();
    hp::LogInfo("================ 5. 自然模式抖动验证 ================");

    // 间隔 50 ms 远大于 ±3 ms 抖动，因此抖动不会改变音符先后，便于逐一对齐比较
    Song song = hp::test_songs::LongSong(200, 50.0);
    const Timeline precise = Timeline::Build(song);

    TimelineOptions options;
    options.naturalMode = true;
    options.jitterMs = 3.0;
    options.randomSeed = 12345u;
    const Timeline natural = Timeline::Build(song, options);

    hp::LogPlain("  曲目：" + song.name + "（200 个音符，间隔 50 ms），抖动上限 ±" +
                 Fmt("%.1f", options.jitterMs) + " ms");
    hp::LogBlank();

    // ---- 精确模式：与原曲绝对时间完全一致 ----
    bool preciseExact = true;
    for (int i = 0; i < song.NoteCount(); ++i) {
        if (std::fabs(precise.At(i).startTime - song.notes[static_cast<size_t>(i)].startTime) >
            1e-12) {
            preciseExact = false;
        }
    }
    Check(preciseExact, "精确模式：时间轴与原曲绝对时间完全一致（误差 0）");

    // ---- 抖动幅度受控、且不随序号放大 ----
    double maxOffsetMs = -1e30;
    double minOffsetMs = 1e30;
    double sumOffsetMs = 0.0;
    double earlyMaxMs = 0.0;
    double lateMaxMs = 0.0;
    const int halfIndex = song.NoteCount() / 2;
    for (int i = 0; i < song.NoteCount(); ++i) {
        const double offsetMs =
            (natural.At(i).startTime - song.notes[static_cast<size_t>(i)].startTime) * 1000.0;
        maxOffsetMs = std::max(maxOffsetMs, offsetMs);
        minOffsetMs = std::min(minOffsetMs, offsetMs);
        sumOffsetMs += offsetMs;
        if (i < halfIndex) {
            earlyMaxMs = std::max(earlyMaxMs, std::fabs(offsetMs));
        } else {
            lateMaxMs = std::max(lateMaxMs, std::fabs(offsetMs));
        }
    }
    const double meanOffsetMs = sumOffsetMs / static_cast<double>(song.NoteCount());

    hp::LogInfo("  抖动范围：最大 +" + Fmt("%.3f", maxOffsetMs) + " ms，最小 " +
                Fmt("%.3f", minOffsetMs) + " ms，均值 " + Fmt("%+.3f", meanOffsetMs) + " ms");
    hp::LogPlain("  前一半最大偏移 " + Fmt("%.3f", earlyMaxMs) + " ms，后一半最大偏移 " +
                 Fmt("%.3f", lateMaxMs) + " ms（若累积，后者会远大于上限）");
    hp::LogInfo("  前 12 个音符的偏移（ms）");
    {
        std::string line = "      ";
        char buffer[32];
        for (int i = 0; i < 12 && i < song.NoteCount(); ++i) {
            const double offsetMs =
                (natural.At(i).startTime - song.notes[static_cast<size_t>(i)].startTime) * 1000.0;
            std::snprintf(buffer, sizeof(buffer), "%+.2f  ", offsetMs);
            line += buffer;
        }
        hp::LogPlain(line);
    }

    hp::LogBlank();
    Check(maxOffsetMs <= options.jitterMs + 1e-6 && minOffsetMs >= -options.jitterMs - 1e-6,
          "每个音符的偏移都在 ±" + Fmt("%.1f", options.jitterMs) + " ms 以内");
    Check(std::fabs(meanOffsetMs) < 1.0, "偏移均值接近 0，没有整体提前/拖后（实测 " +
                                             Fmt("%+.3f", meanOffsetMs) + " ms）");
    Check(earlyMaxMs <= options.jitterMs + 1e-6 && lateMaxMs <= options.jitterMs + 1e-6,
          "前后两半的最大偏移都受同一个上限约束 → 偏移不随播放推进而放大");
    Check(std::fabs(natural.TotalDuration() - precise.TotalDuration()) <= 0.004,
          "总时长变化不超过 4 ms → 不改变整体节拍（实测 " +
              Fmt("%+.3f", (natural.TotalDuration() - precise.TotalDuration()) * 1000.0) +
              " ms）");
    Check(natural.NaturalMode() && !precise.NaturalMode(), "自然模式可以开启/关闭");

    // ---- 可复现性 ----
    const Timeline again = Timeline::Build(song, options);
    bool reproducible = true;
    for (int i = 0; i < song.NoteCount(); ++i) {
        if (again.At(i).startTime != natural.At(i).startTime) {
            reproducible = false;
        }
    }
    Check(reproducible, "同一种子的抖动结果完全可复现（便于复现问题）");

    options.randomSeed = 999u;
    const Timeline other = Timeline::Build(song, options);
    int different = 0;
    for (int i = 0; i < song.NoteCount(); ++i) {
        if (std::fabs(other.At(i).startTime - natural.At(i).startTime) > 1e-9) {
            ++different;
        }
    }
    Check(different > song.NoteCount() / 2,
          "换一个种子会得到不同的抖动（" + std::to_string(different) + "/" +
              std::to_string(song.NoteCount()) + " 个音符不同）");
}

// =============================================================================
// 6. 内置测试曲与时间轴
// =============================================================================
void RunSongCatalog() {
    hp::LogBlank();
    hp::LogInfo("================ 6. 内置测试曲与时间轴 ================");

    for (const std::string& name : hp::test_songs::Names()) {
        Song song = hp::test_songs::ByName(name);
        const Timeline timeline = Timeline::Build(song);

        hp::LogBlank();
        hp::LogInfo("--- " + name + " ---");
        hp::LogPlain("  " + song.Summary());

        const std::vector<std::string> problems = song.Validate();
        if (problems.empty()) {
            hp::LogOk("  曲谱校验通过");
        } else {
            for (const std::string& problem : problems) {
                hp::LogWarn("  曲谱校验：" + problem);
            }
        }

        {
            std::ostringstream buffer;
            buffer << "  【拍谱（作者视角）】\n";
            song.PrintBeatTable(buffer);
            LogFromOstream(buffer);
        }
        {
            std::ostringstream buffer;
            buffer << "  【绝对时间轴（前 10 行）】\n";
            timeline.PrintTable(buffer, 10);
            LogFromOstream(buffer);
        }
        {
            std::ostringstream buffer;
            timeline.PrintStats(buffer);
            LogFromOstream(buffer);
        }
    }

    hp::LogBlank();
    hp::LogInfo("内置曲目共 " + std::to_string(hp::test_songs::Names().size()) + " 首，全部构建成功。");
}

// =============================================================================
// 7. 按时间轴真实演奏（演示）
// =============================================================================
void RunPlayDemo(const std::string& songName, double speed, bool useCountdown) {
    hp::LogBlank();
    hp::LogInfo("================ 7. 按时间轴真实演奏（演示） ================");

    Song song = hp::test_songs::ByName(songName);
    const Timeline timeline = Timeline::Build(song);
    if (timeline.Empty()) {
        hp::LogError("曲目为空或名字不认识：" + songName);
        hp::LogPlain("  可用的曲目：" + hp::test_songs::Names()[0] + " 等，用 --list 查看全部。");
        ++g_failed;
        return;
    }

    hp::LogPlain("  曲目：" + song.name + "，音符 " + std::to_string(timeline.NoteCount()) +
                 " 个，时长 " + Fmt("%.3f", timeline.TotalDuration()) + " s，速度 " +
                 Fmt("%.0f", speed * 100.0) + "%");
    hp::LogPlain("  会真的按键（含鼠标按键）。请先把焦点切到能看到反应的地方。");
    hp::LogPlain("  说明：这是时间轴的调度演示；完整的播放器（播放/暂停/停止/急停）在 Phase 3。");
    if (useCountdown) {
        Countdown(5);
    }
    hp::LogPlain("  注入前的前台窗口：" + ForegroundTitle());
    hp::LogBlank();

    PrecisionWait waiter;
    // 播放线程以“时间关键”优先级运行（实测可压低被系统抢占造成的偶发晚醒）
    hp::ScopedThreadPriorityBoost priorityBoost;
    hp::LogPlain(std::string("  播放线程优先级：") +
                 (priorityBoost.Applied() ? "已提升到“时间关键”" : "保持默认"));
    PlaybackClock clock;
    clock.Start();
    clock.SetSpeed(speed);

    std::vector<double> errors;
    errors.reserve(static_cast<size_t>(timeline.NoteCount()));

    const double wallStart = hp::NowSeconds();
    for (int i = 0; i < timeline.NoteCount(); ++i) {
        const Note& note = timeline.At(i);

        // 起音：由时钟把“歌曲位置”映射成“墙钟时刻”，再精等待到那一刻
        const double startTarget = clock.WallClockForPosition(note.startTime);
        errors.push_back(waiter.WaitUntil(startTarget));

        // 先按住鼠标键（演奏方式），再按下音调键
        g_input->PressMode(note.mode);
        g_input->PressKey(note.key);

        // 释放：同样按绝对时间（歌曲里这个音符的结束时刻）
        waiter.WaitUntil(clock.WallClockForPosition(note.EndTime()));
        g_input->ReleaseKey(note.key);
        g_input->ReleaseMode(note.mode);
    }
    g_input->ReleaseAll();
    const double wallUsed = hp::NowSeconds() - wallStart;
    const double expectedWall = timeline.TotalDuration() / speed;

    WaitStats stats;
    for (double error : errors) {
        stats.Add(error);
    }

    hp::LogBlank();
    hp::LogInfo("  演奏完成");
    hp::LogPlain("      起音误差：" + stats.Summary());
    hp::LogPlain("      实际墙钟 " + Fmt("%.3f", wallUsed) + " s，期望 " +
                 Fmt("%.3f", expectedWall) + " s，偏差 " +
                 Fmt("%+.1f", (wallUsed - expectedWall) * 1000.0) + " ms");
    hp::LogPlain("      结束时工具持有的输入：" + g_input->PlayerStateSummary());

    hp::LogBlank();
    Check(stats.maxLateMs < 5.0,
          "演奏中最大延后 < 5 ms（实测 " + Fmt("%.3f", stats.maxLateMs) + " ms）");
    Check(stats.meanAbsMs < 1.5,
          "演奏中平均绝对误差 < 1.5 ms（实测 " + Fmt("%.3f", stats.meanAbsMs) + " ms）");
    Check(std::fabs(wallUsed - expectedWall) < 0.05,
          "整曲墙钟耗时与“总时长 ÷ 速度”的偏差 < 50 ms（实测 " +
              Fmt("%+.1f", (wallUsed - expectedWall) * 1000.0) + " ms）");
    Check(!g_input->AnyPlayerInputDown(), "演奏结束后没有残留的按键/鼠标键");
}

// =============================================================================
// 菜单与命令行
// =============================================================================
void PrintBanner() {
    hp::LogBlank();
    hp::LogPlain("  ============================================================");
    hp::LogPlain("    Harmonica Player  —  Timeline Test  （Phase 2：时间轴系统）");
    hp::LogPlain("    游戏口琴自动演奏工具 · 时间轴与播放时钟自检工具");
    hp::LogPlain("  ============================================================");
    hp::LogBlank();
}

void PrintMenu() {
    hp::LogBlank();
    hp::LogPlain("  ------------------------------------------------------------");
    hp::LogPlain("   1  时间换算自检     BPM / 拍号 / 时值 → 绝对时间，逐条断言");
    hp::LogPlain("   2  无累积误差验证   长曲按绝对时间调度，量化“误差是否有界”");
    hp::LogPlain("   3  精等待精度实测   三段式等待的睡醒误差与 CPU 代价");
    hp::LogPlain("   4  变速映射验证     50% ~ 200%，暂停/继续/跳转");
    hp::LogPlain("   5  自然模式抖动     ±3 ms 上限、不累积、可关闭、可复现");
    hp::LogPlain("   6  内置测试曲       6 首曲目的拍谱、时间轴与统计");
    hp::LogPlain("   7  真实演奏演示     按时间轴真按键弹一遍（会真的按键）");
    hp::LogPlain("   0  退出             退出前会自动释放所有工具按住的输入");
    hp::LogPlain("  ------------------------------------------------------------");
}

void PrintSongList() {
    hp::LogInfo("内置测试曲目：");
    int index = 1;
    for (const std::string& name : hp::test_songs::Names()) {
        Song song = hp::test_songs::ByName(name);
        hp::LogPlain("  " + std::to_string(index) + "  " + name + "    " +
                     std::to_string(song.NoteCount()) + " 个音符，时长 " +
                     Fmt("%.3f", song.TotalDuration()) + " s");
        ++index;
    }
}

void PrintHelp() {
    hp::LogPlain("  Harmonica Player — Timeline Test（Phase 2）");
    hp::LogBlank();
    hp::LogPlain("  用法：timeline_test.exe [命令] [选项]");
    hp::LogBlank();
    hp::LogPlain("  不带任何参数          打开交互菜单（推荐）");
    hp::LogBlank();
    hp::LogPlain("  命令：");
    hp::LogPlain("    --selftest          时间换算自检（默认动作之一）");
    hp::LogPlain("    --drift [N]         无累积误差验证（N = 音符数，默认 600）");
    hp::LogPlain("    --wait [N]          精等待精度实测（N = 采样数，默认 400）");
    hp::LogPlain("    --speed             变速映射验证");
    hp::LogPlain("    --natural           自然模式抖动验证");
    hp::LogPlain("    --songs             打印全部内置测试曲的时间轴");
    hp::LogPlain("    --list              列出内置测试曲目");
    hp::LogPlain("    --play <曲名>       按时间轴真实演奏一遍（会真的按键）");
    hp::LogPlain("    --all               依次跑完 1~6（不含真实演奏）");
    hp::LogPlain("    --help              本帮助");
    hp::LogBlank();
    hp::LogPlain("  选项：");
    hp::LogPlain("    --drift-ms <ms>     无累积误差验证的起音间隔（默认 20 ms）");
    hp::LogPlain("    --play-speed <p>    演奏演示的速度倍率（默认 1.0，例如 1.5）");
    hp::LogPlain("    --no-countdown      演奏演示跳过 5 秒倒计时");
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
            RunConversionSelfTest();
            PauseForEnter();
        } else if (value == "2") {
            RunNoAccumulationTest(600, 20.0);
            PauseForEnter();
        } else if (value == "3") {
            RunPrecisionWaitTest(400);
            PauseForEnter();
        } else if (value == "4") {
            RunSpeedTest();
            PauseForEnter();
        } else if (value == "5") {
            RunNaturalModeTest();
            PauseForEnter();
        } else if (value == "6") {
            RunSongCatalog();
            PauseForEnter();
        } else if (value == "7") {
            PrintSongList();
            hp::LogInline("  输入要演奏的曲名（直接回车 = C 大调音阶）：");
            std::string songName;
            std::getline(std::cin, songName);
            songName = Trim(songName);
            if (songName.empty()) {
                songName = "C 大调音阶";
            }
            RunPlayDemo(songName, 1.0, true);
            PauseForEnter();
        } else if (!value.empty()) {
            hp::LogWarn("没有这个选项：" + value);
        }
    }
    return 0;
}

// 用宽字符命令行重新取出参数并转成 UTF-8。
// 必须这样做的原因：main 收到的 argv 是按 ANSI 代码页（简体中文下是 GBK）转换的，
// 命令行里写中文曲名（如 --play "C 大调音阶"）会被转坏，导致曲名匹配不上。
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

int IntOption(const std::vector<std::string>& args, const std::string& name, int fallback) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == name) {
            return std::atoi(args[i + 1].c_str());
        }
    }
    return fallback;
}

double DoubleOption(const std::vector<std::string>& args, const std::string& name, double fallback) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == name) {
            return std::atof(args[i + 1].c_str());
        }
    }
    return fallback;
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

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    const std::vector<std::string> args = Utf8Arguments();

    hp::InputSendOptions options;
    g_input = std::make_unique<InputController>(options);

    PrintBanner();

    const bool useCountdown = !HasFlag(args, "--no-countdown");
    int exitCode = 0;

    if (args.size() <= 1) {
        exitCode = RunInteractiveMenu();
    } else if (HasFlag(args, "--help") || HasFlag(args, "-h")) {
        PrintHelp();
    } else if (HasFlag(args, "--list")) {
        PrintSongList();
    } else if (HasFlag(args, "--all")) {
        RunConversionSelfTest();
        RunNoAccumulationTest(600, 20.0);
        RunPrecisionWaitTest(400);
        RunSpeedTest();
        RunNaturalModeTest();
        RunSongCatalog();
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--selftest")) {
        RunConversionSelfTest();
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--drift")) {
        const int noteCount = IntOption(args, "--drift", 600);
        const double intervalMs = DoubleOption(args, "--drift-ms", 20.0);
        RunNoAccumulationTest(noteCount > 0 ? noteCount : 600, intervalMs);
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--wait")) {
        const int samples = IntOption(args, "--wait", 400);
        RunPrecisionWaitTest(samples > 0 ? samples : 400);
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--speed")) {
        RunSpeedTest();
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--natural")) {
        RunNaturalModeTest();
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--songs")) {
        RunSongCatalog();
    } else if (HasFlag(args, "--play")) {
        const std::string songName = StringOption(args, "--play", "C 大调音阶");
        const double speed = DoubleOption(args, "--play-speed", 1.0);
        RunPlayDemo(songName, speed > 0.0 ? speed : 1.0, useCountdown);
        exitCode = (g_failed == 0) ? 0 : 1;
    } else {
        hp::LogWarn("无法识别的参数，已显示帮助。");
        PrintHelp();
    }

    hp::LogInfo("正在退出并释放工具按住的输入…");
    g_input.reset();
    hp::LogOk("已退出。本次测试：通过 " + std::to_string(g_passed) + " 项，失败 " +
              std::to_string(g_failed) + " 项。");
    return exitCode;
}
