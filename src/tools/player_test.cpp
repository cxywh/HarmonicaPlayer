// =============================================================================
//  Player Test —— Harmonica Player / Phase 3 的测试程序（SongPlayer）
//
//  验证：
//    1. 状态机：Play / Pause / Resume / Stop / Restart + 自然播放结束
//    2. 暂停语义：暂停期间位置冻结、释放当前输入；继续时从暂停位置接着弹（不回开头）
//    3. UI 不阻塞：演奏中调用各个命令都在毫秒级返回
//    4. 急停：随机时刻急停，之后不再产生新的自动输入、不留残留按键、响应延迟量化
//    5. 高精度调度：整曲每个音符的起音误差分布（干跑，用音符观察者量出来）
//    6. 真实演奏：倒计时后真按键演奏，可选“中途急停”演练
//    7. 停止原因演示：需求书第二十六节要求的 UI 文案
//
//  用法：双击运行出现编号菜单；带参数运行见 --help。
// =============================================================================

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
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
#include "player/PlaybackTypes.h"
#include "player/SongPlayer.h"
#include "song/Note.h"
#include "song/Song.h"
#include "song/TestSongs.h"
#include "song/Timeline.h"

// shellapi.h：CommandLineToArgvW（避免命令行里的中文被 ANSI 代码页破坏）
#include <shellapi.h>

namespace {

using hp::InputController;
using hp::Note;
using hp::PlaybackState;
using hp::PlayerOptions;
using hp::Song;
using hp::SongPlayer;
using hp::StopReason;
using hp::Timeline;
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

void Check(bool ok, const std::string& what) {
    if (ok) {
        ++g_passed;
        hp::LogOk("通过  " + what);
    } else {
        ++g_failed;
        hp::LogError("失败  " + what);
    }
}

void SleepMs(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
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
        SleepMs(1000);
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

// 音符观察者：记录每个音符的起音迟到量（秒）。回调在播放线程上，这里只做加锁的 push。
class NoteRecorder {
public:
    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        latenessMs_.clear();
        indices_.clear();
    }
    void Add(int index, double latenessSeconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        indices_.push_back(index);
        latenessMs_.push_back(latenessSeconds * 1000.0);
    }
    size_t Count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return latenessMs_.size();
    }
    std::vector<double> LatenessMs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return latenessMs_;
    }
    std::vector<int> Indices() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return indices_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<double> latenessMs_;
    std::vector<int> indices_;
};

// -----------------------------------------------------------------------------
// 1. 状态机自检（干跑：不注入按键，纯状态与位置）
// -----------------------------------------------------------------------------
void RunStateMachineTest() {
    hp::LogBlank();
    hp::LogInfo("================ 1. 状态机自检（干跑） ================");

    PlayerOptions options;
    options.dryRun = true;
    SongPlayer player(*g_input, options);

    Song song = hp::test_songs::Scale();           // 16 个音符，6.000 秒
    const Timeline timeline = Timeline::Build(song);
    hp::LogPlain("  曲目：" + song.name + "，音符 " + std::to_string(timeline.NoteCount()) +
                 " 个，时长 " + Fmt("%.3f", timeline.TotalDuration()) + " s");
    hp::LogPlain("  干跑模式：只走时间轴与时钟，不注入任何按键。");
    hp::LogBlank();

    // ---- Play ----
    Check(player.IsStopped(), "初始状态：已停止");
    Check(player.Play(timeline), "Play 被接受");
    Check(player.WaitForPlaying(1000), "Play 之后状态变成演奏中");
    SleepMs(200);
    const double afterPlay = player.PositionSeconds();
    Check(afterPlay > 0.1 && afterPlay < 0.5,
          "演奏中位置在推进（0.200 秒后位置 " + Fmt("%.3f", afterPlay) + " s）");
    Check(player.PlaybackLoopActive(), "播放循环正在运行");
    Check(player.PlaybackThreadId() != 0 && player.PlaybackThreadId() != GetCurrentThreadId(),
          "演奏发生在另一个线程上（播放线程 id " + std::to_string(player.PlaybackThreadId()) +
              "，本线程 id " + std::to_string(GetCurrentThreadId()) + "）");

    // ---- Pause ----
    SleepMs(200);
    Check(player.Pause(), "Pause 被接受");
    Check(player.WaitForState(PlaybackState::Paused, 500), "Pause 之后状态变成已暂停");
    const double paused1 = player.PositionSeconds();
    SleepMs(300);
    const double paused2 = player.PositionSeconds();
    Check(std::fabs(paused2 - paused1) < 0.005,
          "暂停期间位置冻结（300 ms 内变化 " + Fmt("%.3f", (paused2 - paused1) * 1000.0) + " ms）");
    Check(!g_input->AnyPlayerInputDown(), "暂停后没有残留的按下输入");
    Check(!player.IsPlaying() && player.IsPaused(), "暂停后不再处于演奏中");

    // ---- Resume ----
    Check(player.Resume(), "Resume 被接受");
    Check(player.WaitForPlaying(500), "Resume 之后回到演奏中");
    SleepMs(300);
    const double afterResume = player.PositionSeconds();
    Check(afterResume > paused2 + 0.2,
          "继续后位置从暂停点往后走（暂停点 " + Fmt("%.3f", paused2) + " s → 现在 " +
              Fmt("%.3f", afterResume) + " s）");
    Check(afterResume < paused2 + 0.6, "继续没有跳变（不是从别处接上）");

    // ---- Stop ----
    Check(player.Stop(StopReason::UserStop), "Stop 被接受");
    Check(player.WaitForStopped(500), "Stop 之后状态变成已停止");
    Check(player.PositionSeconds() == 0.0, "Stop 之后播放位置归零");
    Check(player.LastStopReason() == StopReason::UserStop, "停止原因：用户手动停止");
    Check(!g_input->AnyPlayerInputDown(), "Stop 之后没有残留的按下输入");

    // ---- Restart ----
    Check(player.Restart(), "Restart 被接受");
    Check(player.WaitForPlaying(500), "Restart 之后开始演奏");
    Check(player.PositionSeconds() < 0.35,
          "Restart 从 0 开始（位置 " + Fmt("%.3f", player.PositionSeconds()) + " s）");
    Check(player.FiredNoteCount() <= 2, "Restart 后已起音音符数从头算起");

    // ---- 歌曲自然播放结束 ----
    player.Stop(StopReason::UserStop);
    Check(player.Play(timeline), "重新 Play 用于“自然结束”验证");
    const double waitStart = hp::NowSeconds();
    const bool finished = player.WaitForStopped(9000);
    const double wall = hp::NowSeconds() - waitStart;
    Check(finished, "歌曲自然播放结束后状态变成已停止（用时 " + Fmt("%.3f", wall) + " s）");
    Check(player.LastStopReason() == StopReason::Finished, "停止原因：歌曲播放完成");
    Check(player.FiredNoteCount() == timeline.NoteCount(),
          "全部音符都已起音（" + std::to_string(player.FiredNoteCount()) + "/" +
              std::to_string(timeline.NoteCount()) + "）");
    Check(player.HeldInputCount() == 0, "歌曲结束后没有已按下的输入");
    Check(!g_input->AnyPlayerInputDown(), "歌曲结束后没有残留按键（最后一个音符没有被一直按着）");
    Check(std::fabs(player.PositionSeconds()) < 0.001, "结束后播放位置归零");
    Check(std::fabs(wall - timeline.TotalDuration()) < 0.08,
          "整曲墙钟与曲长一致（实测 " + Fmt("%.3f", wall) + " s，曲长 " +
              Fmt("%.3f", timeline.TotalDuration()) + " s）");
    hp::LogPlain("  最终状态：" + player.StatusText());
}

// -----------------------------------------------------------------------------
// 2. 暂停位置与继续位置（需求书第二十二节）
// -----------------------------------------------------------------------------
void RunPausePositionTest() {
    hp::LogBlank();
    hp::LogInfo("================ 2. 暂停位置与继续（干跑） ================");

    PlayerOptions options;
    options.dryRun = true;
    SongPlayer player(*g_input, options);

    Song song = hp::test_songs::Metronome();   // 60 BPM，每秒一个音符，便于核对位置
    const Timeline timeline = Timeline::Build(song);
    hp::LogPlain("  曲目：" + song.name + "（60 BPM，每 1.000 秒一个音符，共 " +
                 Fmt("%.0f", timeline.TotalDuration()) + " s）");

    Check(player.Play(timeline), "Play 被接受");
    Check(player.WaitForFiredNotes(2, 4000), "已经起音 2 个音符（约 1 秒处）");
    Check(player.Pause(), "Pause 被接受");
    const double paused = player.PositionSeconds();
    const int firedAtPause = player.FiredNoteCount();
    hp::LogPlain("  暂停点：位置 " + Fmt("%.3f", paused) + " s，已起音 " + std::to_string(firedAtPause) +
                 " 个音符");

    SleepMs(500);
    Check(std::fabs(player.PositionSeconds() - paused) < 0.005, "暂停 500 ms 后位置仍然冻结");

    Check(player.Resume(), "Resume 被接受");
    Check(player.WaitForPlaying(500), "继续后回到演奏中");
    const double justResumed = player.PositionSeconds();
    const int firedJustResumed = player.FiredNoteCount();
    Check(std::fabs(justResumed - paused) < 0.05,
          "继续位置 = 暂停位置（差 " + Fmt("%.1f", (justResumed - paused) * 1000.0) + " ms，不是从头）");
    Check(firedJustResumed >= firedAtPause,
          "继续后的游标不会倒退（暂停时 " + std::to_string(firedAtPause) + " → 现在 " +
              std::to_string(firedJustResumed) + "）");
    SleepMs(600);
    Check(player.PositionSeconds() > paused + 0.4, "继续后位置继续推进");
    Check(player.LastStopReason() != StopReason::Finished, "继续后没有把整曲判成已结束");

    player.Stop(StopReason::UserStop);
    hp::LogBlank();
    hp::LogPlain("  说明：暂停会把当前按住的音符释放掉；继续时从暂停位置之后的第一个音符接着弹，");
    hp::LogPlain("        暂停瞬间正在按下的那个音符不会补按（见 README「已知问题」）。");
}

// -----------------------------------------------------------------------------
// 3. UI 线程不被阻塞（需求书第三十八节）
// -----------------------------------------------------------------------------
void RunNonBlockingTest() {
    hp::LogBlank();
    hp::LogInfo("================ 3. UI 线程不阻塞验证（干跑） ================");

    PlayerOptions options;
    options.dryRun = true;
    SongPlayer player(*g_input, options);

    Song song = hp::test_songs::LongSong(1200, 25.0);   // 30 秒的长曲
    const Timeline timeline = Timeline::Build(song);
    hp::LogPlain("  曲目：" + song.name + "（" + std::to_string(timeline.NoteCount()) +
                 " 个音符，" + Fmt("%.1f", timeline.TotalDuration()) + " s，演奏中调命令）");
    hp::LogBlank();

    Check(player.Play(timeline), "Play 被接受");
    Check(player.WaitForPlaying(500), "开始演奏");
    SleepMs(300);

    struct Step {
        const char* name;
        std::function<bool()> call;
    };
    const std::vector<Step> steps = {
        {"Pause", [&] { return player.Pause(); }},
        {"Resume", [&] { return player.Resume(); }},
        {"SetSpeed(150%)", [&] { player.SetSpeed(1.5); return true; }},
        {"SetSpeed(100%)", [&] { player.SetSpeed(1.0); return true; }},
        {"Restart", [&] { return player.Restart(); }},
        {"Stop", [&] { return player.Stop(StopReason::UserStop); }},
        {"Play", [&] { return player.Play(timeline); }},
        {"EmergencyStop", [&] { return player.EmergencyStop("测试"); }},
    };

    double worstMs = 0.0;
    for (const Step& step : steps) {
        SleepMs(150);
        const double before = hp::NowSeconds();
        const bool accepted = step.call();
        const double latencyMs = (hp::NowSeconds() - before) * 1000.0;
        worstMs = std::max(worstMs, latencyMs);
        hp::LogInfo("  " + std::string(step.name) + "：" + (accepted ? "已接受" : "被拒绝") +
                    "，调用耗时 " + Fmt("%.3f", latencyMs) + " ms（状态 " +
                    std::string(hp::PlaybackStateName(player.State())) + "）");
    }
    Check(worstMs < 5.0, "所有命令都在 5 ms 内返回（最坏 " + Fmt("%.3f", worstMs) + " ms）");
    Check(!g_input->AnyPlayerInputDown(), "命令风暴之后没有残留输入");
    player.Stop(StopReason::UserStop);
}

// -----------------------------------------------------------------------------
// 4. 急停压力测试（需求书第四十一节）
// -----------------------------------------------------------------------------
void RunEmergencyStressTest(int rounds, bool dry) {
    hp::LogBlank();
    hp::LogInfo("================ 4. 急停压力测试 ================");

    PlayerOptions options;
    options.dryRun = dry;
    SongPlayer player(*g_input, options);

    Song song = hp::test_songs::LongSong(400, 40.0);   // 400 个音符，16 秒，密度较高
    const Timeline timeline = Timeline::Build(song);
    hp::LogPlain(std::string("  模式：") + (dry ? "干跑（不注入按键）" : "真实注入（会真的按键）"));
    hp::LogPlain("  做法：每轮开始演奏，随机等待 20~300 ms 后调用 EmergencyStop，");
    hp::LogPlain("        然后检查：①之后不再有新的音符被起音 ②没有残留按键 ③原因正确 ④调用耗时");
    hp::LogBlank();

    std::mt19937 rng(20260925u);
    std::uniform_int_distribution<int> delayDistribution(20, 300);

    int violations = 0;
    double worstLatencyMs = 0.0;
    double sumLatencyMs = 0.0;
    int firedTotal = 0;

    for (int round = 0; round < rounds; ++round) {
        if (!player.Play(timeline)) {
            ++violations;
            break;
        }
        if (!player.WaitForPlaying(500)) {
            ++violations;
        }
        SleepMs(delayDistribution(rng));

        const double before = hp::NowSeconds();
        player.EmergencyStop("Space（压力测试模拟）");
        const double latencyMs = (hp::NowSeconds() - before) * 1000.0;
        worstLatencyMs = std::max(worstLatencyMs, latencyMs);
        sumLatencyMs += latencyMs;

        const int firedRightAfter = player.FiredNoteCount();
        firedTotal += firedRightAfter;

        // 急停之后不允许再出现新的自动输入
        SleepMs(60);
        const int firedLater = player.FiredNoteCount();
        if (firedLater != firedRightAfter) {
            ++violations;
            hp::LogError("  第 " + std::to_string(round + 1) + " 轮：急停后又有音符被起音（" +
                         std::to_string(firedRightAfter) + " → " + std::to_string(firedLater) + "）");
        }
        if (g_input->AnyPlayerInputDown()) {
            ++violations;
            hp::LogError("  第 " + std::to_string(round + 1) + " 轮：急停后仍有按住的输入（" +
                         g_input->PlayerStateSummary() + "）");
        }
        if (!player.IsStopped() || player.LastStopReason() != StopReason::UserInput) {
            ++violations;
            hp::LogError("  第 " + std::to_string(round + 1) + " 轮：状态或原因不对（" +
                         player.StatusText() + "）");
        }
        if (player.PositionSeconds() != 0.0) {
            ++violations;
            hp::LogError("  第 " + std::to_string(round + 1) + " 轮：位置没有归零");
        }
    }

    hp::LogBlank();
    hp::LogInfo("  " + std::to_string(rounds) + " 轮急停完成");
    hp::LogPlain("      急停调用耗时：最坏 " + Fmt("%.3f", worstLatencyMs) + " ms，平均 " +
                 Fmt("%.3f", sumLatencyMs / std::max(1, rounds)) + " ms");
    hp::LogPlain("      每轮平均已起音 " + std::to_string(firedTotal / std::max(1, rounds)) +
                 " 个音符后被急停");
    hp::LogPlain("      违规次数：" + std::to_string(violations) + "（残留输入 / 急停后新增起音 / 状态原因错误）");
    hp::LogBlank();
    Check(violations == 0, "急停后没有残留输入、没有新的自动输入、状态与原因都正确（" +
                               std::to_string(rounds) + " 轮）");
    Check(worstLatencyMs < 2.0,
          "急停调用最坏耗时 < 2 ms（实测 " + Fmt("%.3f", worstLatencyMs) +
              " ms，调用即释放输入，不等播放线程）");
    Check(!g_input->AnyPlayerInputDown(), "全部测试结束后没有任何残留输入");
}

// -----------------------------------------------------------------------------
// 5. 高精度调度实测（干跑 + 音符观察者）
// -----------------------------------------------------------------------------
void RunScheduleAccuracyTest(int noteCount, double intervalMs) {
    hp::LogBlank();
    hp::LogInfo("================ 5. 高精度调度实测（干跑） ================");

    PlayerOptions options;
    options.dryRun = true;
    SongPlayer player(*g_input, options);

    Song song = hp::test_songs::LongSong(noteCount, intervalMs);
    const Timeline timeline = Timeline::Build(song);
    NoteRecorder recorder;
    player.SetNoteObserver([&recorder](int index, const Note&, double, double lateness) {
        recorder.Add(index, lateness);
    });

    hp::LogPlain("  曲目：" + song.name + "（" + std::to_string(timeline.NoteCount()) + " 个音符，间隔 " +
                 Fmt("%.1f", intervalMs) + " ms，时长 " + Fmt("%.3f", timeline.TotalDuration()) + " s）");
    hp::LogBlank();

    const double wallStart = hp::NowSeconds();
    Check(player.Play(timeline), "Play 被接受");
    const bool finished = player.WaitForFiredNotes(timeline.NoteCount(), 90000);
    Check(finished || player.WaitForStopped(90000), "整曲演奏完成");
    player.WaitForStopped(5000);
    const double wall = hp::NowSeconds() - wallStart;

    const std::vector<double> lateness = recorder.LatenessMs();
    WaitStats stats;
    for (double value : lateness) {
        stats.Add(value / 1000.0);
    }

    double sumLateness = 0.0;
    for (double value : lateness) {
        sumLateness += value;
    }

    hp::LogInfo("  起音误差（正值 = 比目标晚）：");
    hp::LogPlain("      " + stats.Summary());
    hp::LogPlain("      起音次数 " + std::to_string(lateness.size()) + "/" +
                 std::to_string(timeline.NoteCount()) + "，累计迟到 " + Fmt("%+.3f", sumLateness) +
                 " ms（按 Sleep 链累加的话就是这个量级）");
    hp::LogPlain("      整曲墙钟 " + Fmt("%.3f", wall) + " s，曲长 " +
                 Fmt("%.3f", timeline.TotalDuration()) + " s");
    hp::LogBlank();

    Check(lateness.size() == static_cast<size_t>(timeline.NoteCount()),
          "每个音符都起音了一次（" + std::to_string(lateness.size()) + " 个）");
    Check(stats.meanAbsMs < 1.0, "平均绝对误差 < 1 ms（实测 " + Fmt("%.3f", stats.meanAbsMs) + " ms）");
    Check(stats.QuantileAbsMs(0.99) < 1.5,
          "p99 绝对误差 < 1.5 ms（实测 " + Fmt("%.3f", stats.QuantileAbsMs(0.99)) + " ms）");
    Check(stats.RatioAbsOver(3.0) < 0.01,
          "误差超过 3 ms 的比例低于 1%（实测 " + std::to_string(stats.CountAbsOver(3.0)) + "/" +
              std::to_string(stats.count) + " 次）");
    Check(std::fabs(player.PositionSeconds()) < 0.001, "结束后位置归零");
    Check(player.LastStopReason() == StopReason::Finished, "停止原因：歌曲播放完成");
}

// -----------------------------------------------------------------------------
// 6. 真实演奏（会真的按键）
// -----------------------------------------------------------------------------
void RunRealPlayTest(const std::string& songName, int stopAfterMs, bool useCountdown) {
    hp::LogBlank();
    hp::LogInfo("================ 6. 真实演奏（会真的按键） ================");

    Song song = hp::test_songs::ByName(songName);
    const Timeline timeline = Timeline::Build(song);
    if (timeline.Empty()) {
        hp::LogError("曲目为空或名字不认识：" + songName);
        ++g_failed;
        return;
    }

    PlayerOptions options;   // dryRun = false：真的按键
    SongPlayer player(*g_input, options);
    NoteRecorder recorder;
    player.SetNoteObserver([&recorder](int index, const Note&, double, double lateness) {
        recorder.Add(index, lateness);
    });

    hp::LogPlain("  曲目：" + song.name + "，音符 " + std::to_string(timeline.NoteCount()) +
                 " 个，时长 " + Fmt("%.3f", timeline.TotalDuration()) + " s");
    if (stopAfterMs > 0) {
        hp::LogPlain("  计划：" + std::to_string(stopAfterMs) + " ms 后急停（演练急停路径）");
    } else {
        hp::LogPlain("  计划：完整演奏到底");
    }
    hp::LogPlain("  请先把焦点切到能看到反应的地方（记事本或游戏）。");
    if (useCountdown) {
        Countdown(5);
    }
    hp::LogPlain("  注入前的前台窗口：" + ForegroundTitle());
    hp::LogBlank();

    player.Play(timeline);
    if (!player.WaitForPlaying(1000)) {
        hp::LogError("演奏没有开始");
        ++g_failed;
        return;
    }

    if (stopAfterMs > 0) {
        SleepMs(stopAfterMs);
        const double before = hp::NowSeconds();
        player.EmergencyStop("真实演奏演练");
        const double latencyMs = (hp::NowSeconds() - before) * 1000.0;
        hp::LogInfo("  已在 " + std::to_string(stopAfterMs) + " ms 处急停，调用耗时 " +
                    Fmt("%.3f", latencyMs) + " ms");
        SleepMs(120);
    } else {
        player.WaitForStopped(60000);
    }

    const std::vector<double> lateness = recorder.LatenessMs();
    WaitStats stats;
    for (double value : lateness) {
        stats.Add(value / 1000.0);
    }

    hp::LogBlank();
    hp::LogInfo("  演奏结束");
    hp::LogPlain("      " + player.StatusText());
    hp::LogPlain("      " + player.ProgressText());
    hp::LogPlain("      起音误差：" + stats.Summary());
    hp::LogPlain("      工具当前持有的输入：" + g_input->PlayerStateSummary());
    hp::LogBlank();

    Check(lateness.size() > 0, "至少有一个音符真的被起了音（" + std::to_string(lateness.size()) + " 个）");
    Check(stats.QuantileAbsMs(0.99) < 5.0,
          "真实注入下 p99 起音误差 < 5 ms（实测 " + Fmt("%.3f", stats.QuantileAbsMs(0.99)) + " ms）");
    Check(!g_input->AnyPlayerInputDown(), "演奏结束后没有残留的按键/鼠标键");
    if (stopAfterMs > 0) {
        Check(player.LastStopReason() == StopReason::UserInput, "停止原因：检测到用户输入");
        Check(lateness.size() < static_cast<size_t>(timeline.NoteCount()),
              "急停之后剩下的音符没有被弹出来（" + std::to_string(lateness.size()) + "/" +
                  std::to_string(timeline.NoteCount()) + "）");
    } else {
        Check(player.LastStopReason() == StopReason::Finished, "停止原因：歌曲播放完成");
        Check(lateness.size() == static_cast<size_t>(timeline.NoteCount()),
              "全部音符都弹出来了（" + std::to_string(lateness.size()) + " 个）");
    }
}

// -----------------------------------------------------------------------------
// 7. 停止原因演示（需求书第二十六节）
// -----------------------------------------------------------------------------
void RunStopReasonDemo() {
    hp::LogBlank();
    hp::LogInfo("================ 7. 停止原因与状态文案（干跑） ================");

    PlayerOptions options;
    options.dryRun = true;
    SongPlayer player(*g_input, options);

    Song song = hp::test_songs::Metronome();
    const Timeline timeline = Timeline::Build(song);

    struct Case {
        StopReason reason;
        const char* detail;
    };
    const Case cases[] = {
        {StopReason::UserStop, ""},
        {StopReason::Finished, ""},
        {StopReason::UserInput, "Space"},
        {StopReason::WindowChanged, "切换到浏览器"},
        {StopReason::Shutdown, ""},
        {StopReason::Error, "播放线程异常"},
    };

    hp::LogPlain("  UI 需要显示的文案（状态 + 原因）：");
    for (const Case& item : cases) {
        player.Play(timeline);
        SleepMs(120);
        if (item.reason == StopReason::Finished) {
            hp::LogBlank();
            hp::LogPlain("      【" + std::string(hp::StopReasonName(item.reason)) + "】用真实播放结束来产生");
            player.Stop(StopReason::UserStop);
            const Timeline shortTimeline = Timeline::Build(hp::test_songs::Jig());
            player.Play(shortTimeline);
            player.WaitForStopped(10000);
        } else if (item.reason == StopReason::UserInput) {
            player.EmergencyStop(item.detail);
        } else {
            player.Stop(item.reason);
        }
        hp::LogPlain("      " + player.StatusText() + "　｜　" + player.ProgressText());
        hp::LogInfo("        （状态 " + std::string(hp::PlaybackStateName(player.State())) +
                    "，原因 " + hp::StopReasonText(item.reason, item.detail) + "）");
    }

    hp::LogBlank();
    Check(player.IsStopped(), "演示结束后处于已停止");
    Check(!g_input->AnyPlayerInputDown(), "演示结束后没有残留输入");
    hp::LogPlain("  以上就是 UI（Phase 7）直接可以用的状态文案。");
}

// -----------------------------------------------------------------------------
// 菜单与命令行
// -----------------------------------------------------------------------------
void PrintBanner() {
    hp::LogBlank();
    hp::LogPlain("  ============================================================");
    hp::LogPlain("    Harmonica Player  —  Player Test  （Phase 3：SongPlayer）");
    hp::LogPlain("    游戏口琴自动演奏工具 · 播放器自检工具");
    hp::LogPlain("  ============================================================");
    hp::LogBlank();
}

void PrintMenu() {
    hp::LogBlank();
    hp::LogPlain("  ------------------------------------------------------------");
    hp::LogPlain("   1  状态机自检       Play/Pause/Resume/Stop/Restart/自然结束");
    hp::LogPlain("   2  暂停位置验证     暂停冻结位置、继续从暂停处接着弹（不回开头）");
    hp::LogPlain("   3  UI 不阻塞        演奏中调用各命令的返回耗时");
    hp::LogPlain("   4  急停压力测试     随机时刻急停 30 轮：无残留、停止后不再起音");
    hp::LogPlain("   5  高精度调度      整曲每个音符的起音误差分布（干跑）");
    hp::LogPlain("   6  真实演奏        倒计时后真按键演奏（可选“中途急停”演练）");
    hp::LogPlain("   7  停止原因演示     UI 要显示的状态与原因文案");
    hp::LogPlain("   0  退出             退出前会自动释放所有工具按住的输入");
    hp::LogPlain("  ------------------------------------------------------------");
    hp::LogPlain("   说明：1/2/3/5/7 是干跑（不注入任何按键，绝对安全）；");
    hp::LogPlain("        4 默认真注入、6 一定真注入，跑之前先把焦点切到记事本或游戏。");
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
    hp::LogPlain("  Harmonica Player — Player Test（Phase 3）");
    hp::LogBlank();
    hp::LogPlain("  用法：player_test.exe [命令] [选项]");
    hp::LogBlank();
    hp::LogPlain("  不带任何参数          打开交互菜单（推荐）");
    hp::LogBlank();
    hp::LogPlain("  命令（都是干跑，不注入按键）：");
    hp::LogPlain("    --state             状态机自检");
    hp::LogPlain("    --pause             暂停位置与继续验证");
    hp::LogPlain("    --nonblock          UI 线程不阻塞验证");
    hp::LogPlain("    --schedule [N]      高精度调度实测（N = 音符数，默认 600）");
    hp::LogPlain("    --dry               给 --stress / --play 用：不注入按键");
    hp::LogPlain("    --status            停止原因与状态文案演示");
    hp::LogPlain("    --all               依次跑完上面 5 项（约 30 秒，零风险）");
    hp::LogBlank();
    hp::LogPlain("  命令（会真的按键）：");
    hp::LogPlain("    --stress [N]        急停压力测试（N = 轮数，默认 30）");
    hp::LogPlain("    --play [曲名]        真实演奏（默认 C 大调音阶；支持 scale 等英文别名）");
    hp::LogBlank();
    hp::LogPlain("  选项：");
    hp::LogPlain("    --stop-after-ms <ms> 真实演奏 N 毫秒后急停（演练急停路径）");
    hp::LogPlain("    --no-countdown      跳过真实演奏前的 5 秒倒计时");
    hp::LogPlain("    --help              本帮助");
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
            RunStateMachineTest();
            PauseForEnter();
        } else if (value == "2") {
            RunPausePositionTest();
            PauseForEnter();
        } else if (value == "3") {
            RunNonBlockingTest();
            PauseForEnter();
        } else if (value == "4") {
            hp::LogWarn("  急停压力测试会真的按键（30 轮，每轮几十到几百毫秒）。");
            hp::LogInline("  要真的按键吗？输入 y 真注入，直接回车 = 干跑（推荐先干跑）：");
            std::string answer;
            std::getline(std::cin, answer);
            const std::string trimmed = Trim(answer);
            RunEmergencyStressTest(30, !(trimmed == "y" || trimmed == "Y"));
            PauseForEnter();
        } else if (value == "5") {
            RunScheduleAccuracyTest(600, 20.0);
            PauseForEnter();
        } else if (value == "6") {
            PrintSongList();
            hp::LogInline("  输入要演奏的曲名（直接回车 = C 大调音阶）：");
            std::string songName;
            std::getline(std::cin, songName);
            songName = Trim(songName);
            if (songName.empty()) {
                songName = "C 大调音阶";
            }
            RunRealPlayTest(songName, 0, true);
            PauseForEnter();
        } else if (value == "7") {
            RunStopReasonDemo();
            PauseForEnter();
        } else if (!value.empty()) {
            hp::LogWarn("没有这个选项：" + value);
        }
    }
    return 0;
}

// 用宽字符命令行重新取参数并转 UTF-8（命令行里的中文曲名不会被 GBK 破坏）
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

    hp::InputSendOptions sendOptions;
    g_input = std::make_unique<InputController>(sendOptions);

    PrintBanner();

    const bool useCountdown = !HasFlag(args, "--no-countdown");
    const bool dry = HasFlag(args, "--dry");
    int exitCode = 0;

    if (args.size() <= 1) {
        exitCode = RunInteractiveMenu();
    } else if (HasFlag(args, "--help") || HasFlag(args, "-h")) {
        PrintHelp();
    } else if (HasFlag(args, "--all")) {
        RunStateMachineTest();
        RunPausePositionTest();
        RunNonBlockingTest();
        RunScheduleAccuracyTest(600, 20.0);
        RunStopReasonDemo();
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--state")) {
        RunStateMachineTest();
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--pause")) {
        RunPausePositionTest();
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--nonblock")) {
        RunNonBlockingTest();
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--status")) {
        RunStopReasonDemo();
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--schedule")) {
        const int noteCount = IntOption(args, "--schedule", 600);
        RunScheduleAccuracyTest(noteCount > 0 ? noteCount : 600, 20.0);
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--stress")) {
        const int rounds = IntOption(args, "--stress", 30);
        RunEmergencyStressTest(rounds > 0 ? rounds : 30, dry);
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--play")) {
        const std::string songName = StringOption(args, "--play", "C 大调音阶");
        const int stopAfterMs = IntOption(args, "--stop-after-ms", 0);
        RunRealPlayTest(songName, stopAfterMs, useCountdown);
        exitCode = (g_failed == 0) ? 0 : 1;
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
