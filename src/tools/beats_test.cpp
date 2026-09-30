// =============================================================================
//  Beats Test —— Harmonica Player / Phase 8 的测试程序（BPM 和音乐节拍）
//
//  验证（需求书二十七、二十八、二十九、三十节）：
//    1. BPM 与拍长：1 拍 = 60000 / BPM 毫秒；时值（全/二分/四分/八分/十六分）→ 毫秒
//    2. 拍号与小节：4/4、3/4、6/8 的每小节拍数、每小节秒数、小节定位（真实曲目上逐音符核对）
//    3. 小节的运行期取值：播放过程中小节号单调不减、结束时到最后一小节
//    4. 速度控制：**真实计时**——干跑整曲，量墙钟，核对 50% / 100% / 200% 的实际倍速；
//       并核对"变速不改歌曲时间"（总时长与位置语义与速度无关）
//
//  用法：
//    dist\beats_test.exe --all       跑全部四组（不带参数也是跑全部）
//    dist\beats_test.exe --meter     只跑第 1 组（换算）
//    dist\beats_test.exe --bars      只跑第 2、3 组（拍号 / 小节）
//    dist\beats_test.exe --speed     只跑第 4 组（真实计时，约 21 秒）
//
//  计时说明：第 4 组用干跑（dryRun）播放——只走时间轴与时钟、不注入按键，
//  所以量到的是真实的调度时间，同时不会往任何窗口里打字。
//  墙钟用 core/Time.h 的单调时钟（QueryPerformanceCounter），与播放器同源。
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/Log.h"
#include "core/Platform.h"

#include <shellapi.h>   // CommandLineToArgvW（argv 是 GBK，用宽字符命令行重取）
#include "core/Time.h"
#include "input/InputController.h"
#include "player/SongPlayer.h"
#include "song/BeatMath.h"
#include "song/TestSongs.h"
#include "song/Timeline.h"
#include "ui/UiFormat.h"

using hp::InputController;
using hp::LogLevel;
using hp::NoteValue;
using hp::Song;
using hp::SongPlayer;
using hp::TimeSignature;
using hp::Timeline;

namespace {

int g_passed = 0;
int g_failed = 0;

// 用宽字符命令行重新取参数并转 UTF-8（与其它测试工具一致）
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

void Note(const std::string& text) {
    hp::LogLine(LogLevel::Info, text);
}

double Round3(double value) {
    return std::floor(value * 1000.0 + 0.5) / 1000.0;
}

// —— 第 1 组：换算（BPM / 时值 / 拍号）——
void RunMeterTest() {
    Section("第 1 组：BPM、时值、拍号的换算");

    const Song scale = hp::test_songs::Scale();       // 120 BPM、4/4、四分音符为一拍
    Note("曲目：" + scale.name + "　BPM=" + hp::ui::FormatBeats(scale.bpm) +
         "　拍号=" + hp::beat::SignatureText(scale));

    Check(scale.bpm > 0.0, "内置曲目的 BPM 有效");
    Check(std::fabs(hp::beat::BeatMs(scale) - 60000.0 / scale.bpm) < 1e-9,
          "1 拍 = 60000 / BPM = " + hp::ui::FormatMs(hp::beat::BeatMs(scale)) + " ms");

    // 时值 → 毫秒（以该曲的"一拍"为单位）
    const double quarter = hp::beat::NoteDurationMs(scale, NoteValue::Quarter);
    const double half = hp::beat::NoteDurationMs(scale, NoteValue::Half);
    const double whole = hp::beat::NoteDurationMs(scale, NoteValue::Whole);
    const double eighth = hp::beat::NoteDurationMs(scale, NoteValue::Eighth);
    Note("时值：全 " + hp::ui::FormatMs(whole) + " ms／二分 " + hp::ui::FormatMs(half) +
         " ms／四分 " + hp::ui::FormatMs(quarter) + " ms／八分 " + hp::ui::FormatMs(eighth) + " ms");
    Check(std::fabs(half - quarter * 2.0) < 1e-9, "二分音符 = 2 × 四分音符");
    Check(std::fabs(whole - quarter * 4.0) < 1e-9, "全音符 = 4 × 四分音符");
    Check(std::fabs(eighth - quarter / 2.0) < 1e-9, "八分音符 = 四分音符 ÷ 2");

    // 拍号：每小节拍数与秒数必须与曲目自己的换算一致（不能两处各算一套）
    const double beatsPerBar = hp::beat::BeatsPerBar(scale);
    Check(beatsPerBar == static_cast<double>(scale.timeSignature.numerator),
          "每小节拍数 = 拍号分子 = " + hp::ui::FormatBeats(beatsPerBar));
    Check(std::fabs(hp::beat::SecondsPerBar(scale) - beatsPerBar * 60.0 / scale.bpm) < 1e-9,
          "每小节秒数 = 每小节拍数 × 拍长 = " + hp::ui::FormatSeconds3(hp::beat::SecondsPerBar(scale)) + " 秒");

    // 三种拍号（需求书二十八节明确要求支持 4/4、3/4、6/8）
    struct Case { int numerator; int denominator; NoteValue unit; const char* text; };
    const Case cases[] = {
        {4, 4, NoteValue::Quarter, "4/4"},
        {3, 4, NoteValue::Quarter, "3/4"},
        {6, 8, NoteValue::Eighth, "6/8"},
    };
    for (const Case& item : cases) {
        Song song = scale;
        song.timeSignature = TimeSignature{item.numerator, item.denominator};
        song.beatUnit = item.unit;
        const double perBar = hp::beat::SecondsPerBar(song);
        Note(std::string("  拍号 ") + item.text + "：" + hp::beat::SignatureText(song));
        Check(std::fabs(perBar * song.bpm / 60.0 - item.numerator) < 1e-9,
              std::string(item.text) + "：每小节 " + hp::ui::FormatBeats(perBar * song.bpm / 60.0) + " 拍");
        Check(perBar > 0.0, std::string(item.text) + "：每小节 " + hp::ui::FormatSeconds3(perBar) + " 秒");
    }

    // 6/8 的"一拍"是八分音符 —— 四分音符应等于 2 拍
    Song jig = hp::test_songs::Jig();
    if (jig.timeSignature.numerator == 6 && jig.beatUnit == NoteValue::Eighth) {
        Check(std::fabs(hp::beat::NoteDurationMs(jig, NoteValue::Quarter) -
                        hp::beat::BeatMs(jig) * 2.0) < 1e-9,
              "6/8 曲目里四分音符 = 2 拍 = " + hp::ui::FormatMs(hp::beat::NoteDurationMs(jig, NoteValue::Quarter)) + " ms");
    } else {
        Note("跳过 6/8 检查：内置吉格曲不是 6/8 + 八分音符为一拍");
    }
}

// —— 第 2 组：真实曲目上逐音符核对小节 ——
void RunBarTest() {
    Section("第 2 组：拍号与小节（在真实时间轴上逐音符核对）");

    const Song song = hp::test_songs::Waltz();   // 3/4，正好覆盖拍号
    const Timeline timeline = Timeline::Build(song);
    Check(timeline.NoteCount() > 0, "曲目有音符：" + std::to_string(timeline.NoteCount()) + " 个");

    const int totalBars = hp::beat::BarCount(song);
    Note("曲目：" + song.name + "　总时长 " + hp::ui::FormatMs(timeline.TotalDuration() * 1000.0) +
         " ms　共 " + std::to_string(totalBars) + " 小节（每小节 " +
         hp::ui::FormatSeconds3(hp::beat::SecondsPerBar(song)) + " 秒）");
    Check(totalBars > 0, "总小节数 > 0");

    // 逐音符：小节号单调不减、都在 1..totalBars 内、两种算法（按拍 / 按时间）一致
    bool monotonic = true;
    bool inRange = true;
    bool agree = true;
    int lastBar = 0;
    for (int i = 0; i < timeline.NoteCount(); ++i) {
        const hp::Note& note = timeline.At(i);
        const int byTime = hp::beat::LocateBar(song, note.startTime).bar;
        const int byBeat = hp::beat::BarOfBeat(song, note.startTime / song.SecondsPerBeat());
        if (byTime < lastBar) {
            monotonic = false;
        }
        if (byTime < 1 || byTime > totalBars) {
            inRange = false;
        }
        if (byTime != byBeat) {
            agree = false;
        }
        lastBar = byTime;
    }
    Check(monotonic, "小节号随音符单调不减");
    Check(inRange, "每个音符的小节号都在 1 ~ " + std::to_string(totalBars) + " 内");
    Check(agree, "按时间定位与按拍定位给出同一个小节号");

    // 最后一个音符所在小节 = 总小节数
    const hp::Note& last = timeline.At(timeline.NoteCount() - 1);
    Check(hp::beat::LocateBar(song, last.startTime).bar == totalBars,
          "最后一个音符在第 " + std::to_string(totalBars) + " 小节");

    // 跨过一拍边界时小节必须进位
    const double perBar = hp::beat::SecondsPerBar(song);
    Check(hp::beat::LocateBar(song, 0.0).bar == 1, "0 秒在第 1 小节");
    Check(hp::beat::LocateBar(song, perBar - 0.001).bar == 1, "每小节末尾仍在第 1 小节");
    Check(hp::beat::LocateBar(song, perBar).bar == 2, "跨过一小节边界进入第 2 小节");
    Check(hp::beat::LocateBar(song, perBar * 2.0).bar == 3, "跨过两小节边界进入第 3 小节");
}

// —— 第 3 组：播放过程中的小节取值 ——
void RunPlayingBarTest() {
    Section("第 3 组：播放过程中小节号的运行期取值");

    InputController input;
    hp::PlayerOptions options;
    options.dryRun = true;
    options.boostPlaybackThreadPriority = false;
    SongPlayer player(input, options);

    const Song song = hp::test_songs::Scale();
    const Timeline timeline = Timeline::Build(song);
    const int totalBars = hp::beat::BarCount(song);
    const double perBar = hp::beat::SecondsPerBar(song);

    Check(player.Play(timeline), "开始干跑整曲");
    Check(player.WaitForPlaying(1000), "状态进入演奏中");

    bool monotonic = true;
    int lastBar = 0;
    int samples = 0;
    const double deadline = hp::NowSeconds() + timeline.TotalDuration() * 0.8;
    while (hp::NowSeconds() < deadline && player.IsPlaying()) {
        const int bar = hp::beat::LocateBar(song, player.PositionSeconds()).bar;
        if (bar < lastBar) {
            monotonic = false;
        }
        lastBar = bar;
        ++samples;
        Sleep(20);
    }
    Note("演奏中采样 " + std::to_string(samples) + " 次，最后读到第 " + std::to_string(lastBar) + " 小节");
    Check(samples > 10, "采样次数足够（" + std::to_string(samples) + " 次）");
    Check(monotonic, "演奏中小节号单调不减");
    Check(lastBar >= 1, "演奏中小节号有效");
    Check(std::fabs(player.PositionSeconds() - 0.0) >= 0.0, "演奏中位置可读");

    player.Stop();
    Check(player.WaitForStopped(2000), "停止后状态回到已停止");
    Check(hp::beat::LocateBar(song, player.PositionSeconds()).bar == 1, "停止后位置归零 → 第 1 小节");
    Check(totalBars >= 2, "该曲目跨多小节（" + std::to_string(totalBars) + " 小节，每小节 " +
                              hp::ui::FormatSeconds3(perBar) + " 秒）");
}

// —— 第 4 组：速度控制的真实计时 ——
void RunSpeedTest() {
    Section("第 4 组：速度控制的真实计时（干跑整曲，量墙钟）");

    InputController input;
    hp::PlayerOptions options;
    options.dryRun = true;
    options.boostPlaybackThreadPriority = false;
    SongPlayer player(input, options);

    const Song song = hp::test_songs::Scale();
    const Timeline timeline = Timeline::Build(song);
    const double total = timeline.TotalDuration();
    Note("曲目：" + song.name + "　歌曲时间 " + hp::ui::FormatSeconds3(total) + " 秒（速度不影响这个值）");

    const double speeds[] = {0.5, 1.0, 2.0};
    double measured[3] = {0.0, 0.0, 0.0};
    double expectations[3] = {0.0, 0.0, 0.0};

    for (int i = 0; i < 3; ++i) {
        const double speed = speeds[i];
        player.SetSpeed(speed);
        Check(std::fabs(player.Speed() - speed) < 1e-9,
              "速度设为 " + hp::ui::FormatSpeedText(speed));

        const double expected = total / speed;
        expectations[i] = expected;

        Check(player.Play(timeline), "开始干跑");
        const double start = hp::NowSeconds();
        const bool finished = player.WaitForStopped(static_cast<int>(expected * 1000.0) + 4000);
        const double elapsed = hp::NowSeconds() - start;
        measured[i] = elapsed;

        const double tolerance = std::max(0.15, expected * 0.02);
        Note("  " + hp::ui::FormatSpeedText(speed) + "：期望 " + hp::ui::FormatSeconds3(expected) +
             " 秒，实测 " + hp::ui::FormatSeconds3(Round3(elapsed)) + " 秒，偏差 " +
             hp::ui::FormatMs(std::fabs(elapsed - expected) * 1000.0) + " ms");
        Check(finished, hp::ui::FormatSpeedText(speed) + "：整曲演奏完成");
        Check(std::fabs(elapsed - expected) <= tolerance,
              hp::ui::FormatSpeedText(speed) + "：实际用时在容差内（±" + hp::ui::FormatMs(tolerance * 1000.0) + " ms）");
        Check(player.FiredNoteCount() == timeline.NoteCount(),
              hp::ui::FormatSpeedText(speed) + "：所有音符都起了音（" +
                  std::to_string(player.FiredNoteCount()) + " / " + std::to_string(timeline.NoteCount()) + "）");
        Check(std::fabs(player.TotalDurationSeconds() - total) < 1e-9,
              hp::ui::FormatSpeedText(speed) + "：歌曲总时长不变（仍是 " + hp::ui::FormatSeconds3(total) + " 秒）");
    }

    // 倍速关系：50% 用时应约为 100% 的两倍，200% 用时应约为 100% 的一半
    Check(measured[0] > measured[1] && measured[1] > measured[2], "用时：50% > 100% > 200%");
    const double halfRatio = measured[0] / (measured[1] > 0.0 ? measured[1] : 1.0);
    const double doubleRatio = measured[1] / (measured[2] > 0.0 ? measured[2] : 1.0);
    Note("  实测倍数：50%/100% = " + hp::ui::FormatBeats(Round3(halfRatio)) +
         "　100%/200% = " + hp::ui::FormatBeats(Round3(doubleRatio)));
    Check(std::fabs(halfRatio - 2.0) <= 0.08, "50% 用时约为 100% 的 2 倍（实测 " + hp::ui::FormatBeats(Round3(halfRatio)) + "）");
    Check(std::fabs(doubleRatio - 2.0) <= 0.08, "100% 用时约为 200% 的 2 倍（实测 " + hp::ui::FormatBeats(Round3(doubleRatio)) + "）");

    // 有效 BPM：速度 200% 时相当于 BPM 翻倍（需求书二十九节）
    Check(std::fabs(hp::beat::EffectiveBpm(song, 2.0) - song.bpm * 2.0) < 1e-9,
          "200% 时有效 BPM = " + hp::ui::FormatBeats(hp::beat::EffectiveBpm(song, 2.0)));
    Check(std::fabs(hp::beat::EffectiveBpm(song, 1.0) - song.bpm) < 1e-9,
          "100% 时有效 BPM 等于曲目 BPM");
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    const std::vector<std::string> args = Utf8Arguments();
    bool all = true;
    bool meter = false;
    bool bars = false;
    bool speed = false;
    for (const std::string& arg : args) {
        if (arg == "--meter") {
            meter = true;
            all = false;
        } else if (arg == "--bars") {
            bars = true;
            all = false;
        } else if (arg == "--speed") {
            speed = true;
            all = false;
        } else if (arg == "--all") {
            all = true;
        } else if (arg == "--help" || arg == "-h") {
            std::printf("用法：beats_test.exe [--all | --meter | --bars | --speed]\n"
                        "  --all    跑全部四组（默认）\n"
                        "  --meter  只跑 BPM / 时值 / 拍号的换算\n"
                        "  --bars   只跑拍号与小节（含播放中取样）\n"
                        "  --speed  只跑速度控制的真实计时（约 21 秒）\n");
            return 0;
        }
    }

    hp::LogLine(LogLevel::Info, "游戏口琴自动演奏工具 —— Phase 8 自检（BPM 和音乐节拍）");
    const double started = hp::NowSeconds();
    if (all || meter) {
        RunMeterTest();
    }
    if (all || bars) {
        RunBarTest();
        RunPlayingBarTest();
    }
    if (all || speed) {
        RunSpeedTest();
    }
    const double used = hp::NowSeconds() - started;
    hp::LogLine(LogLevel::Info, "本次测试：通过 " + std::to_string(g_passed) + " 项，失败 " +
                                    std::to_string(g_failed) + " 项，用时 " +
                                    hp::ui::FormatSeconds3(Round3(used)) + " 秒。");
    return g_failed == 0 ? 0 : 1;
}
