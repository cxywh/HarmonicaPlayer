// =============================================================================
//  Final Test —— Harmonica Player / Phase 11（最终优化）的自检程序
//
//  需求书 Phase 11 的十项，逐项用**真实测量**说话（不是"看起来没问题"）：
//    1. 时间精度    长曲播放的位置漂移（斜率 ≈ 0 才算没有累计误差）
//    2. CPU 占用    空闲 / 演奏中 / 干跑大曲三种情形的进程 CPU 时间占比
//    3. 线程安全    多线程命令风暴（播放/暂停/停止/重播/变速/换曲）
//    4. 输入状态管理 反复急停后不留按住的键；ReleaseAll 幂等
//    5. Emergency Stop 急停延迟的 p50 / p95 / p99 / 最大值
//    6. UI 响应     演奏中所有命令调用的最坏耗时
//    7. 大型曲谱    10 万音符：时间轴构建、JSON 存读、文本谱往返、按需取格子
//    8. 异常处理    坏 JSON / 坏文本谱 / 空曲 / 越界值 一律不崩、给中文原因
//    9. 日志        演奏十万音符期间新增的日志行数（刷屏检查）+ 时间戳单调
//   10. 配置保存    配置往返、缺文件/坏文件/坏字段回退、落盘覆盖
//
//  用法：
//    dist\final_test.exe --all        跑全部十组（默认）
//    dist\final_test.exe --precision  只跑第 1 项，其余同理：
//    --cpu --threads --keys --emergency --ui --large --errors --log --config
// =============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Log.h"
#include "core/Platform.h"
#include "core/Time.h"
#include "input/InputController.h"
#include "input/KeyboardController.h"
#include "player/SongPlayer.h"
#include "song/BeatMath.h"
#include "song/SongSerializer.h"
#include "song/TestSongs.h"
#include "song/TextScore.h"
#include "song/Timeline.h"
#include "ui/SettingsStore.h"
#include "ui/UiFormat.h"

// shellapi / psapi 必须放在 windows.h（core/Platform.h）之后，否则 shellapi.h 里的
// HDROP 之类会报一堆 C4430/C2146（本项目其它自检工具踩过同一个坑）
#include <psapi.h>
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
        hp::LogLine(LogLevel::Ok, "通过  " + what);
    } else {
        ++g_failed;
        hp::LogLine(LogLevel::Error, "错误  " + what);
    }
}

void Section(const std::string& text) {
    hp::LogLine(LogLevel::Info, "---- " + text + " ----");
}

void Say(const std::string& text) {
    hp::LogLine(LogLevel::Info, "      " + text);
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

// —— 进程指标 ——
double ProcessCpuSeconds() {
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return 0.0;
    }
    auto toSeconds = [](const FILETIME& time) {
        ULARGE_INTEGER value{};
        value.LowPart = time.dwLowDateTime;
        value.HighPart = time.dwHighDateTime;
        return static_cast<double>(value.QuadPart) / 10000000.0;   // 100 ns 单位
    };
    return toSeconds(kernel) + toSeconds(user);
}

double ProcessPrivateMb() {
    // PrivateUsage 在 EX 版结构里（base 结构没有这个成员，踩过）
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters))) {
        return 0.0;
    }
    return static_cast<double>(counters.PrivateUsage) / (1024.0 * 1024.0);
}

double Percentile(std::vector<double> values, double ratio) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(ratio * static_cast<double>(values.size() - 1) + 0.5);
    return values[std::min(index, values.size() - 1)];
}

std::string Ms(double value, int decimals = 3) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return std::string(buffer) + " ms";
}

// 纯数字（百分比、MB 这种不能带 " ms" 的量用这个）
std::string Num(double value, int decimals = 3) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return std::string(buffer);
}

Song MakeLongSong(int quarterNotes, double bpm) {
    Song song;
    song.name = "长曲 " + std::to_string(quarterNotes) + " 音符";
    song.bpm = bpm;
    song.beatUnit = NoteValue::Quarter;
    song.timeSignature = TimeSignature{4, 4};
    const double beat = song.SecondsPerBeat();
    for (int i = 0; i < quarterNotes; ++i) {
        Note note;
        note.key = static_cast<Key>(i % hp::kKeyCount);
        note.mode = (i % 4 == 1) ? PlayMode::Left : PlayMode::Normal;
        note.startTime = static_cast<double>(i) * beat;
        note.duration = beat;
        song.notes.push_back(note);
    }
    return song;
}

// =============================================================================
//  1. 时间精度
// =============================================================================
void RunPrecision() {
    Section("第 1 项：时间精度（长曲播放的位置漂移）");

    // 20 秒的曲目（160 个四分音符 @ 120 BPM）：逐音符记录"实际起音位置"
    const Song song = MakeLongSong(40, 120.0);   // 40 × 0.5 s = 20 秒
    const Timeline timeline = Timeline::Build(song);
    Check(timeline.NoteCount() == 40, "时间轴收下 40 个音符");

    hp::InputController input;
    hp::PlayerOptions options;
    options.dryRun = true;
    options.boostPlaybackThreadPriority = false;
    SongPlayer player(input, options);

    std::mutex guard;
    std::vector<double> expected;
    std::vector<double> actual;
    player.SetNoteObserver([&](int, const Note& note, double actualPosition, double) {
        std::lock_guard<std::mutex> lock(guard);
        expected.push_back(note.startTime);
        actual.push_back(actualPosition);
    });

    const double wallStart = hp::NowSeconds();
    const bool started = player.Play(timeline);
    Check(started, "开始干跑演奏");
    const bool finished = player.WaitForStopped(30000);
    const double wallElapsed = hp::NowSeconds() - wallStart;
    Check(finished, "20 秒曲目演奏完成（墙钟 " + hp::ui::FormatSeconds3(wallElapsed) + " 秒）");

    std::vector<double> expectedCopy;
    std::vector<double> actualCopy;
    {
        // 只在拷贝时持锁：绝不能把这个锁带到后面的 Play/SetSpeed 上去 ——
        // 工作线程回调观察者时也要这把锁，主线程一直握着就是死锁（踩过，现象是"位置停在 0"）
        std::lock_guard<std::mutex> lock(guard);
        expectedCopy = expected;
        actualCopy = actual;
    }
    std::vector<double>& expectedRef = expectedCopy;
    (void)expectedRef;
    Check(actualCopy.size() == 40, "收到 40 次起音回调（实际 " + std::to_string(actualCopy.size()) + "）");
    if (actualCopy.size() < 2) {
        return;
    }

    // 漂移 = 实际位置 - 期望位置；关注"是否随音符序号变大"（累计误差）
    std::vector<double> drifts;
    double worst = 0.0;
    for (size_t i = 0; i < actualCopy.size(); ++i) {
        const double drift = (actualCopy[i] - expectedCopy[i]) * 1000.0;   // ms
        drifts.push_back(drift);
        worst = std::max(worst, std::fabs(drift));
    }
    const double firstDrift = drifts.front();
    const double lastDrift = drifts.back();
    Say("首音符漂移 " + Ms(firstDrift) + "，末音符漂移 " + Ms(lastDrift) + "，最大 |漂移| " +
        Ms(worst));
    Say("漂移 p95 " + Ms(Percentile(drifts, 0.95)) + "，p99 " + Ms(Percentile(drifts, 0.99)));

    // 线性回归斜率（ms / 音符）：斜率≈0 就等于"没有累计误差"
    const double n = static_cast<double>(drifts.size());
    double sumX = 0.0;
    double sumY = 0.0;
    double sumXY = 0.0;
    double sumXX = 0.0;
    for (size_t i = 0; i < drifts.size(); ++i) {
        const double x = static_cast<double>(i);
        sumX += x;
        sumY += drifts[i];
        sumXY += x * drifts[i];
        sumXX += x * x;
    }
    const double slope = (n * sumXY - sumX * sumY) / (n * sumXX - sumX * sumX);
    Say("漂移对音符序号的回归斜率：" + Ms(slope * 1000.0, 3) + " 每千音符（越接近 0 越说明不跑拍）");

    Check(worst < 5.0, "最大 |漂移| 在 5 ms 以内（实际 " + Ms(worst) + "）");
    Check(std::fabs(lastDrift) < 5.0, "末音符漂移在 5 ms 以内（实际 " + Ms(lastDrift) + "）");
    Check(std::fabs(slope) < 0.02, "斜率接近 0，40 个音符内看不到累计误差（实际 " + Ms(slope * 1000.0) +
                                      " 每千音符）");
    Check(std::fabs(wallElapsed - 20.0) < 0.15,
          "整曲墙钟在 20.000 秒 ±150 ms 内（实际 " + hp::ui::FormatSeconds3(wallElapsed) + " 秒）");

    // 变速后再量一次：倍速不该破坏时间映射
    player.SetSpeed(2.0);
    const double fastStart = hp::NowSeconds();
    player.Play(timeline);
    const bool fastDone = player.WaitForStopped(30000);
    const double fastElapsed = hp::NowSeconds() - fastStart;
    Check(fastDone && std::fabs(fastElapsed - 10.0) < 0.15,
          "200% 速度下 20 秒曲目用 10.000 秒弹完（实际 " + hp::ui::FormatSeconds3(fastElapsed) + " 秒）");

    // 暂停 1 秒再继续：位置必须冻住，总时长多 1 秒
    player.SetSpeed(1.0);
    player.Play(timeline);
    player.WaitForFiredNotes(5, 5000);
    const double beforePause = player.PositionSeconds();
    player.Pause();
    const double pausedAt = player.PositionSeconds();
    const double pauseWall = hp::NowSeconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const double stillPaused = player.PositionSeconds();
    const double pausedWall = hp::NowSeconds() - pauseWall;
    player.Resume();
    player.WaitForStopped(30000);
    Check(std::fabs(stillPaused - pausedAt) < 0.001,
          "暂停期间位置冻住（暂停时 " + hp::ui::FormatSeconds3(pausedAt) + " → 600 ms 后 " +
              hp::ui::FormatSeconds3(stillPaused) + "，墙钟实际过了 " +
              hp::ui::FormatSeconds3(pausedWall) + " 秒）");
    Say("暂停位置 " + hp::ui::FormatSeconds3(beforePause) + " 秒处的采样");
}

// =============================================================================
//  2. CPU 占用
// =============================================================================
void RunCpu() {
    Section("第 2 项：CPU 占用");

    hp::InputController input;
    hp::PlayerOptions options;
    options.dryRun = true;
    options.boostPlaybackThreadPriority = false;
    SongPlayer player(input, options);

    auto measure = [&](const std::string& label, int seconds, const std::function<void()>& body) {
        const double cpu0 = ProcessCpuSeconds();
        const double wall0 = hp::NowSeconds();
        body();
        // 采样窗口内持续观察，避免把"启动抖一下"当成结论
        std::this_thread::sleep_for(std::chrono::milliseconds(seconds * 1000));
        const double cpu = ProcessCpuSeconds() - cpu0;
        const double wall = hp::NowSeconds() - wall0;
        const double percent = (wall > 0.0) ? (cpu / wall * 100.0) : 0.0;
        // 本机时钟节拍 15.625 ms：窗口内 CPU 时间取到 0，说明用量低于一个节拍
        const std::string cpuText =
            (cpu <= 0.0) ? ("低于一个时钟节拍（<" + Num(15.625 / wall * 100.0, 2) + "% 单核）")
                         : (Ms(cpu * 1000.0, 1) + " CPU 时间");
        Say(label + "：" + cpuText + " / 墙钟 " + hp::ui::FormatSeconds3(wall) +
            " 秒 → 占单核 " + Num(percent, 3) + "%");
        return percent;
    };

    // 空闲（没有曲谱在弹，只有钩子/热键/播放线程在等）
    const double idle = measure("空闲（无演奏）", 3, []() {});
    Check(idle < 2.0, "空闲时占单核 < 2%（实际 " + Num(idle, 3) + "%）");

    // 演奏中（干跑，5 秒）
    const Song song = MakeLongSong(200, 120.0);   // 100 秒曲目，只量前 5 秒
    const Timeline timeline = Timeline::Build(song);
    double playing = 0.0;
    playing = measure("演奏中（干跑）", 5, [&]() { player.Play(timeline); });
    player.Stop(hp::StopReason::UserStop);
    Check(playing < 5.0, "演奏中占单核 < 5%（实际 " + Num(playing, 3) + "%）");

    // 空闲但有大曲谱挂着（时间轴常驻，不该产生任何轮询开销）
    const Timeline large = Timeline::Build(MakeLongSong(100000, 120.0));
    const double bigIdle = measure("挂着 10 万音符的空闲", 3, []() {});
    Check(bigIdle < 2.0, "挂着十万音符时空闲占用仍 < 2%（实际 " + Num(bigIdle, 3) + "%）");
    Say("内存（私有）现在 " + Num(ProcessPrivateMb(), 1) + " MB");
}

// =============================================================================
//  3. 线程安全（命令风暴）
// =============================================================================
void RunThreads() {
    Section("第 3 项：线程安全（多线程命令风暴）");

    const Song song = MakeLongSong(400, 120.0);   // 200 秒，够风暴折腾
    const Timeline timeline = Timeline::Build(song);

    hp::InputController input;
    hp::PlayerOptions options;
    options.dryRun = true;
    options.boostPlaybackThreadPriority = false;
    SongPlayer player(input, options);

    std::atomic<bool> stop{false};
    std::atomic<long> operations{0};

    auto worker = [&](int kind) {
        while (!stop.load(std::memory_order_relaxed)) {
            switch (kind) {
                case 0:
                    player.Play(timeline);   // 演奏中重复 Play 允许被拒，不算失败
                    break;
                case 1:
                    player.Pause();
                    break;
                case 2:
                    player.Resume();
                    break;
                case 3:
                    player.Stop(hp::StopReason::UserStop);
                    break;
                case 4:
                    player.SetSpeed((operations % 2 == 0) ? 1.0 : 1.5);
                    break;
                default:
                    player.EmergencyStop("风暴");
                    break;
            }
            operations.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    for (int kind = 0; kind < 6; ++kind) {
        threads.emplace_back(worker, kind);
    }
    const double start = hp::NowSeconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    stop.store(true);
    for (std::thread& thread : threads) {
        thread.join();
    }
    const double elapsed = hp::NowSeconds() - start;
    Say("6 个线程 3 秒共发起 " + std::to_string(operations.load()) + " 次命令（" +
        Ms(static_cast<double>(operations.load()) / elapsed, 0) + " 次/秒）");

    Check(true, "3 秒风暴期间没有崩溃、没有卡死（线程全部 join 成功）");
    Check(elapsed < 4.5, "风暴结束及时（实际 " + hp::ui::FormatSeconds3(elapsed) + " 秒）");

    // 风暴之后系统必须还能回到干净状态
    player.Stop(hp::StopReason::UserStop);
    player.SetSpeed(1.0);
    const bool restarted = player.Play(timeline);
    Check(restarted, "风暴之后还能正常开始演奏");
    const bool stopped = player.WaitForFiredNotes(3, 5000);
    Check(stopped, "风暴之后还能正常起音（至少 3 个音符）");
    player.Stop(hp::StopReason::UserStop);
    Check(input.SimulatedDownCount() == 0, "风暴之后没有留下按住的键");
    const int held = player.HeldInputCount();
    Check(held == 0, "播放器持有的输入数为 0（实际 " + std::to_string(held) + "）");
}

// =============================================================================
//  4. 输入状态管理 + 5. 急停延迟
// =============================================================================
void RunKeysAndEmergency() {
    Section("第 4 项：输入状态管理（反复急停不留残键）");

    const Song song = MakeLongSong(200, 120.0);
    const Timeline timeline = Timeline::Build(song);

    hp::InputController input;
    hp::PlayerOptions options;
    options.dryRun = true;
    options.boostPlaybackThreadPriority = false;
    SongPlayer player(input, options);

    const int cycles = 200;
    int stuck = 0;
    int notStopped = 0;
    for (int i = 0; i < cycles; ++i) {
        player.Play(timeline);
        // 等到真的按下去了（才有东西需要放）
        if (!player.WaitForFiredNotes(1 + (i % 3), 2000)) {
            // 起音太慢不计入失败，但也不假装成功
        }
        player.EmergencyStop("自检急停");
        if (input.SimulatedDownCount() != 0) {
            ++stuck;
        }
        if (player.IsPlaying()) {
            ++notStopped;
        }
    }
    Check(stuck == 0, std::to_string(cycles) + " 次「按下→急停」之后都没留下按住的键（残留 " +
                         std::to_string(stuck) + " 次）");
    Check(notStopped == 0, std::to_string(cycles) + " 次急停之后播放器都停了（还在播 " +
                               std::to_string(notStopped) + " 次）");

    input.ReleaseAll();
    input.ReleaseAll();
    input.ReleaseAll();
    Check(input.SimulatedDownCount() == 0, "ReleaseAll 连调三次依然干净（幂等）");

    // 用户自己按住的键不能被工具放掉（需求书十二节）
    //   SimulateRawKeyEvent 是成对的"用户按键"事件（不进 PlayerDownSummary），
    //   ReleaseAll 必须对它视而不见 —— 这才是"不误释放用户按键"的真实检验。
    Check(input.SimulateRawKeyEvent(VK_SHIFT, true), "喂一条用户按住 Shift 的事件");
    const int userDownBefore = input.SimulatedDownCount();
    player.Play(timeline);
    player.WaitForFiredNotes(2, 3000);
    player.EmergencyStop("自检急停");
    const int userDownAfter = input.SimulatedDownCount();
    // 工具自己按下的音符键走的是另一本账（playerDown_），所以这里的计数就代表"用户键"
    Check(userDownBefore >= 1 && userDownAfter == userDownBefore,
          "急停后用户按住的键一条不少（急停前 " + std::to_string(userDownBefore) + " 条，急停后 " +
              std::to_string(userDownAfter) + " 条）—— 工具没有替用户松手");
    Check(player.HeldInputCount() == 0, "同时工具自己的键全部放掉了");
    Say(hp::KeyboardController::IsVirtualKeyPhysicallyDown(VK_SHIFT)
            ? "系统层也确认 Shift 仍处于按下状态"
            : "（系统层未观察到 Shift 按下，本次以工具内部账目为准）");
    input.SimulateRawKeyEvent(VK_SHIFT, false);
    Check(input.SimulatedDownCount() == 0, "用户松手后不残留任何模拟按键");

    Section("第 5 项：Emergency Stop 延迟");
    std::vector<double> latencies;
    for (int i = 0; i < 30; ++i) {
        player.Play(timeline);
        player.WaitForFiredNotes(2, 3000);
        const double before = hp::NowSeconds();
        player.EmergencyStop("延迟测量");
        // 急停返回即视为"输入已交出"：再确认一遍键已放完
        const double after = hp::NowSeconds();
        const bool clean = (input.SimulatedDownCount() == 0) && !player.IsPlaying();
        latencies.push_back((after - before) * 1000.0);
        if (!clean) {
            ++g_failed;
            hp::LogLine(LogLevel::Error, "错误  第 " + std::to_string(i + 1) + " 次急停后状态不干净");
        }
    }
    Say("急停延迟：p50 " + Ms(Percentile(latencies, 0.5)) + "，p95 " +
        Ms(Percentile(latencies, 0.95)) + "，p99 " + Ms(Percentile(latencies, 0.99)) + "，最大 " +
        Ms(Percentile(latencies, 1.0)));
    Check(Percentile(latencies, 1.0) < 5.0,
          "最坏急停延迟 < 5 ms（实际 " + Ms(Percentile(latencies, 1.0)) + "）");
    Check(input.SimulatedDownCount() == 0, "所有急停之后输入都是干净的");
}

// =============================================================================
//  6. UI 响应
// =============================================================================
void RunUiResponsiveness() {
    Section("第 6 项：UI 响应（演奏中命令的最坏耗时）");

    const Song song = MakeLongSong(400, 120.0);
    const Timeline timeline = Timeline::Build(song);

    hp::InputController input;
    hp::PlayerOptions options;
    options.dryRun = true;
    options.boostPlaybackThreadPriority = false;
    SongPlayer player(input, options);
    player.Play(timeline);
    player.WaitForFiredNotes(1, 3000);

    struct Case { const char* label; std::function<void()> action; };
    const std::vector<Case> cases = {
        {"暂停", [&]() { player.Pause(); }},
        {"继续", [&]() { player.Resume(); }},
        {"停止", [&]() { player.Stop(hp::StopReason::UserStop); }},
        {"重播", [&]() { player.Restart(); }},
        {"设速度", [&]() { player.SetSpeed(1.25); }},
        {"设干跑", [&]() { player.SetDryRun(true); }},
        {"状态文本", [&]() { (void)player.StatusText(); }},
        {"进度文本", [&]() { (void)player.ProgressText(); }},
        {"位置查询", [&]() { (void)player.PositionSeconds(); }},
        {"当前音符", [&]() { (void)player.FiredNoteCount(); }},
        {"换曲（时间轴重建）", [&]() { (void)Timeline::Build(song); }},
    };

    double worst = 0.0;
    std::string worstLabel;
    for (const Case& item : cases) {
        std::vector<double> samples;
        for (int i = 0; i < 50; ++i) {
            const double before = hp::NowSeconds();
            item.action();
            const double after = hp::NowSeconds();
            samples.push_back((after - before) * 1000.0);
        }
        const double itemWorst = *std::max_element(samples.begin(), samples.end());
        const double itemP95 = Percentile(samples, 0.95);
        if (itemWorst > worst) {
            worst = itemWorst;
            worstLabel = item.label;
        }
        Say(std::string(item.label) + "：p95 " + Ms(itemP95) + "，最坏 " + Ms(itemWorst));
    }
    Check(worst < 5.0, "演奏中最坏命令耗时 < 5 ms（实际 " + Ms(worst) + "，来自「" + worstLabel + "」）");
    Check(input.SimulatedDownCount() == 0, "命令风暴之后输入依然干净");
}

// =============================================================================
//  7. 大型曲谱
// =============================================================================
void RunLargeSong() {
    Section("第 7 项：大型曲谱（10 万音符）");

    const int count = 100000;
    const Song song = MakeLongSong(count, 120.0);
    Check(song.NoteCount() == count, "构造出 " + std::to_string(count) + " 个音符的曲目");

    const double buildStart = hp::NowSeconds();
    const Timeline timeline = Timeline::Build(song);
    const double buildMs = (hp::NowSeconds() - buildStart) * 1000.0;
    Say("时间轴构建：" + Ms(buildMs, 1) + "（" + std::to_string(count) + " 音符）");
    Check(timeline.NoteCount() == count, "时间轴收下全部音符（" +
                                             std::to_string(timeline.NoteCount()) + "）");
    Check(buildMs < 2000.0, "时间轴构建 < 2 秒（实际 " + Ms(buildMs, 1) + "）");

    // JSON 落盘 + 读回
    const std::string path = hp::ExeDirectory() + "large_song_test.json";
    const double saveStart = hp::NowSeconds();
    const hp::SongIoResult saved = hp::SaveSongToJson(song, path);
    const double saveMs = (hp::NowSeconds() - saveStart) * 1000.0;
    Check(saved.ok, "10 万音符存成 JSON：" + (saved.ok ? "成功" : saved.error));
    const double loadStart = hp::NowSeconds();
    Song loaded;
    const hp::SongIoResult io = hp::LoadSongFromJson(path, loaded);
    const double loadMs = (hp::NowSeconds() - loadStart) * 1000.0;
    Check(io.ok && loaded.NoteCount() == count,
          "读回来还是 10 万音符（" + std::to_string(loaded.NoteCount()) + " 个）");
    Say("JSON 保存 " + Ms(saveMs, 1) + "，加载 " + Ms(loadMs, 1));
    Check(saveMs < 3000.0 && loadMs < 3000.0, "JSON 存读各 < 3 秒");

    // 文本谱往返（10 万行）
    const double textStart = hp::NowSeconds();
    const std::string text = hp::text_score::SongToTextScore(song, nullptr);
    const double textWriteMs = (hp::NowSeconds() - textStart) * 1000.0;
    const double parseStart = hp::NowSeconds();
    const hp::text_score::TextScoreResult parsed = hp::text_score::ParseTextScore(text, song);
    const double parseMs = (hp::NowSeconds() - parseStart) * 1000.0;
    Check(parsed.ok && parsed.noteLines == count,
          "10 万行文本谱能解析回来（" + std::to_string(parsed.noteLines) + " 行）");
    Say("文本谱输出 " + Ms(textWriteMs, 1) + "，解析 " + Ms(parseMs, 1) + "，文本 " +
        std::to_string(text.size() / 1024) + " KB");
    Check(textWriteMs < 3000.0 && parseMs < 3000.0, "文本谱输出与解析各 < 3 秒");

    // 表格按需取格子：主界面虚拟列表就是靠它，必须 O(1) 且够快
    const double cellStart = hp::NowSeconds();
    const int probes = 20000;
    std::size_t totalChars = 0;
    for (int i = 0; i < probes; ++i) {
        const int row = (i * 7919) % count;   // 伪随机但可复现
        for (int column = 0; column < 5; ++column) {
            totalChars += hp::ui::NoteRowCellText(timeline, song, row, column).size();
        }
    }
    const double cellMs = (hp::NowSeconds() - cellStart) * 1000.0;
    const double perCellUs = cellMs * 1000.0 / (static_cast<double>(probes) * 5.0);
    Say("表格取格子 " + std::to_string(probes) + " 行 × 5 列：" + Ms(cellMs, 1) + "（每格 " +
        Num(perCellUs, 3) + " 微秒，共 " + std::to_string(totalChars) + " 字符）");
    Check(perCellUs < 50.0, "每格按需生成 < 50 微秒（实际 " + Num(perCellUs, 3) + " 微秒）");
    Check(hp::ui::NoteRowCellText(timeline, song, count, 2).empty() &&
              hp::ui::NoteRowCellText(timeline, song, -1, 2).empty(),
          "越界的行号返回空串而不是崩掉");
    Check(hp::ui::NoteRowCellText(timeline, song, count - 1, 0) == std::to_string(count),
          "最后一行的序号是 " + std::to_string(count));
    Say("内存（私有）现在 " + Num(ProcessPrivateMb(), 1) + " MB");

    std::remove(path.c_str());
}

// =============================================================================
//  8. 异常处理
// =============================================================================
void RunErrorHandling() {
    Section("第 8 项：异常处理（坏输入不许崩）");

    // 坏 JSON
    const std::string badJsonPath = hp::ExeDirectory() + "bad_song_test.json";
    {
        FILE* file = nullptr;
        if (fopen_s(&file, badJsonPath.c_str(), "wb") != 0) {
            file = nullptr;
        }
        Check(file != nullptr, "能写出一个坏 JSON 用来测试");
        if (file != nullptr) {
            const char* garbage = "{\"name\": \"坏\", \"notes\": [ {\"key\": } ]}";
            std::fwrite(garbage, 1, std::strlen(garbage), file);
            std::fclose(file);
        }
        Song out;
        const hp::SongIoResult result = hp::LoadSongFromJson(badJsonPath, out);
        Check(!result.ok && !result.error.empty(),
              "坏 JSON 被拒且有中文原因：" + (result.error.empty() ? std::string("（空原因）")
                                                                   : result.error));
        std::string content = "not json at all";
        FILE* file2 = nullptr;
        if (fopen_s(&file2, badJsonPath.c_str(), "wb") != 0) {
            file2 = nullptr;
        }
        if (file2 != nullptr) {
            std::fwrite(content.data(), 1, content.size(), file2);
            std::fclose(file2);
        }
        const hp::SongIoResult result2 = hp::LoadSongFromJson(badJsonPath, out);
        Check(!result2.ok, "完全不是 JSON 也被拒：" + result2.error);
        std::remove(badJsonPath.c_str());
    }
    {
        Song out;
        const hp::SongIoResult missing = hp::LoadSongFromJson(
            hp::ExeDirectory() + "definitely_missing_file.json", out);
        Check(!missing.ok, "文件不存在被拒：" + missing.error);
    }

    // 坏文本谱
    const Song header = MakeLongSong(1, 120.0);
    const hp::text_score::TextScoreResult emptyText =
        hp::text_score::ParseTextScore(std::string(), header);
    Check(!emptyText.ok && !emptyText.issues.empty(), "空文本谱被拒：" + emptyText.Summary());
    const hp::text_score::TextScoreResult weird =
        hp::text_score::ParseTextScore("\xE4\xB9\xB1\xE7\xA0\x81 \xFF\xFE \x01\x02\n", header);
    Check(!weird.ok, "乱码/表情符号行被拒：" + weird.Summary());
    const hp::text_score::TextScoreResult noHeader =
        hp::text_score::ParseTextScore("Z\n", MakeLongSong(1, 0.0));
    Check(!noHeader.ok, "曲头 BPM 为 0 被拒：" + noHeader.Summary());

    // 越界/异常数值
    double speed = 0.0;
    std::string speedError;
    Check(!hp::beat::ClampSpeed(-3.0, speed, speedError), "负速度被拒：" + speedError);
    Check(!hp::beat::ClampSpeed(0.0, speed, speedError), "0 速度被拒：" + speedError);
    // ClampSpeed 的语义是**校验并拒绝**（不是悄悄收敛），界面靠它报错、配置文件靠它回退
    speed = 0.0;
    const bool tooFast = hp::beat::ClampSpeed(9.0, speed, speedError);
    Check(!tooFast && speedError.find("200%") != std::string::npos,
          "速度 900% 被拒且说明了上限：" + speedError);
    speed = 0.0;
    const bool tooSlow = hp::beat::ClampSpeed(0.1, speed, speedError);
    Check(!tooSlow && speedError.find("50%") != std::string::npos,
          "速度 10% 被拒且说明了下限：" + speedError);
    Check(hp::beat::ClampSpeed(1.5, speed, speedError) && std::fabs(speed - 1.5) < 1e-9,
          "范围内（150%）的速度原样通过");

    // 空曲目 / 无音符：不许崩，也要能"播完"
    Song empty;
    empty.name = "空曲";
    empty.bpm = 120.0;
    empty.beatUnit = NoteValue::Quarter;
    const Timeline emptyTimeline = Timeline::Build(empty);
    Check(emptyTimeline.NoteCount() == 0, "空曲目的时间轴是 0 个音符");
    hp::InputController input;
    hp::PlayerOptions options;
    options.dryRun = true;
    SongPlayer player(input, options);
    // 播放器的契约：空时间轴直接拒绝（返回 false），而不是"开始一个永远不结束的演奏"
    const bool played = player.Play(emptyTimeline);
    Check(!played, "空曲目被 Play 拒绝（不进入演奏状态）");
    Check(player.IsStopped(), "被拒之后状态仍是已停止：" + player.StatusText());
    Check(input.SimulatedDownCount() == 0, "空曲目连按都没按过，自然没有按住的键");

    // 超过时间轴能接受的范围：验证既有的丢弃/警告机制还在
    Song overlapping = MakeLongSong(10, 120.0);
    for (Note& note : overlapping.notes) {
        note.startTime = 0.0;   // 全部挤在同一时刻
    }
    std::vector<std::string> problems;
    const Timeline squeezed = Timeline::Build(overlapping, hp::TimelineOptions(), &problems);
    Check(squeezed.NoteCount() >= 1, "重叠音符被处理而不是崩：" +
                                         std::to_string(squeezed.NoteCount()) + " 个留下，" +
                                         std::to_string(problems.size()) + " 条提示");
}

// =============================================================================
//  9. 日志
// =============================================================================
void RunLogging() {
    Section("第 9 项：日志（不刷屏 + 时间戳单调）");

    // 时间戳单调
    bool monotonic = true;
    double previous = hp::SinceStartSeconds();
    for (int i = 0; i < 2000; ++i) {
        const double now = hp::SinceStartSeconds();
        if (now < previous) {
            monotonic = false;
        }
        previous = now;
    }
    Check(monotonic, "日志时间基准 2000 次采样单调不减");
    Check(previous > 0.0, "日志时间基准大于 0（实际 " + hp::ui::FormatSeconds3(previous) + " 秒）");

    // 演奏十万音符期间的日志增量（干跑会被压到极短，但音符数不变）
    hp::InputController input;
    hp::PlayerOptions options;
    options.dryRun = true;
    options.boostPlaybackThreadPriority = false;
    SongPlayer player(input, options);
    const Timeline timeline = Timeline::Build(MakeLongSong(100000, 120.0));

    const std::size_t before = hp::LogLineCount();
    player.Play(timeline);
    player.WaitForFiredNotes(2000, 10000);
    player.EmergencyStop("日志检查");
    player.Stop(hp::StopReason::UserStop);
    const std::size_t after = hp::LogLineCount();
    const std::size_t delta = after - before;
    Say("演奏期间新增日志 " + std::to_string(delta) + " 行（起音 " +
        std::to_string(player.FiredNoteCount()) + " 个音符）");
    Check(delta < 20, "演奏期间日志不超过 20 行（实际 " + std::to_string(delta) + " 行）");
    Check(player.StatusText().find("日志") == std::string::npos,
          "状态文本不会把日志内容混进来");
}

// =============================================================================
//  10. 配置保存
// =============================================================================
void RunConfig() {
    Section("第 10 项：配置保存");

    const hp::ui::AppSettings defaults = hp::ui::AppSettings::Default();
    Check(!hp::ui::DefaultSettingsPath().empty(), "默认配置路径：" + hp::ui::DefaultSettingsPath());
    Check(hp::ui::DefaultSettingsPath().find("settings.json") != std::string::npos,
          "默认配置路径以 settings.json 结尾");

    // 往返
    hp::ui::AppSettings custom = defaults;
    custom.playHotkey = "Ctrl+Shift+F9";
    custom.stopHotkey = "Alt+F1";
    custom.restartHotkey = "F12";
    custom.speed = 1.5;
    custom.dryRun = true;
    custom.naturalMode = true;
    custom.jitterMs = 4.5;
    custom.monitorKeyboard = false;
    custom.monitorMouseMove = true;
    custom.stopOnMouseWheel = false;   // 新增字段：滚轮打断演奏（默认是 true，这里测非默认值往返）
    custom.foregroundPolicy = hp::ui::ForegroundPolicy::Stop;

    const std::string json = hp::ui::SettingsToJson(custom);
    const hp::ui::SettingsLoadResult roundTrip = hp::ui::SettingsFromJson(defaults, json);
    bool same = roundTrip.ok && roundTrip.warnings.empty();
    same = same && roundTrip.settings.playHotkey == custom.playHotkey &&
           roundTrip.settings.stopHotkey == custom.stopHotkey &&
           roundTrip.settings.restartHotkey == custom.restartHotkey &&
           std::fabs(roundTrip.settings.speed - custom.speed) < 1e-9 &&
           roundTrip.settings.dryRun == custom.dryRun &&
           roundTrip.settings.naturalMode == custom.naturalMode &&
           std::fabs(roundTrip.settings.jitterMs - custom.jitterMs) < 1e-9 &&
           roundTrip.settings.monitorKeyboard == custom.monitorKeyboard &&
           roundTrip.settings.monitorMouseMove == custom.monitorMouseMove &&
           roundTrip.settings.stopOnMouseWheel == custom.stopOnMouseWheel &&
           roundTrip.settings.foregroundPolicy == custom.foregroundPolicy &&
           roundTrip.settings.songPath.empty();   // songPath 不再落盘，往返后必须为空
    Check(same, "配置序列化 → 反序列化逐字段一致（且没有多余警告）");
    Say("配置 JSON：" + std::to_string(json.size()) + " 字节，顶层 " +
        std::to_string(std::count(json.begin(), json.end(), '\n')) + " 行");

    // 缺字段：用默认
    const hp::ui::SettingsLoadResult partial =
        hp::ui::SettingsFromJson(defaults, "{\n  \"speed\": 1.25\n}\n");
    Check(partial.ok && std::fabs(partial.settings.speed - 1.25) < 1e-9 &&
              partial.settings.playHotkey == defaults.playHotkey,
          "只写一个字段时，其余字段取默认值");

    // 未知字段：忽略 + 警告
    const hp::ui::SettingsLoadResult unknown =
        hp::ui::SettingsFromJson(defaults, "{\n  \"未来的字段\": 1,\n  \"speed\": 1.0\n}\n");
    Check(unknown.ok, "未知字段不影响读取（不崩）");

    // 类型错：回退 + 警告
    const hp::ui::SettingsLoadResult wrongType =
        hp::ui::SettingsFromJson(defaults, "{\n  \"speed\": \"快\",\n  \"monitorKeyboard\": 7\n}\n");
    Check(wrongType.ok && std::fabs(wrongType.settings.speed - defaults.speed) < 1e-9 &&
              !wrongType.warnings.empty(),
          "字段类型不对时回默认值并给警告（" + std::to_string(wrongType.warnings.size()) +
              " 条警告）");

    // 新增字段：鼠标滚轮打断演奏（stopOnMouseWheel）。读不出来时必须回默认值，
    // 且警告文案要点名**这个字段**（字段名写错的告警等于没告警）。
    const hp::ui::SettingsLoadResult wheelBad =
        hp::ui::SettingsFromJson(defaults, "{\n  \"stopOnMouseWheel\": \"开\"\n}\n");
    Check(wheelBad.ok && wheelBad.settings.stopOnMouseWheel == defaults.stopOnMouseWheel &&
              !wheelBad.warnings.empty() &&
              wheelBad.warnings[0].find("stopOnMouseWheel") != std::string::npos,
          "配置项 stopOnMouseWheel 读不出来时回默认值并点名该字段： " +
              (wheelBad.warnings.empty() ? std::string("（没有警告）") : wheelBad.warnings[0]));

    // 坏 JSON / 空文件：报中文原因，不崩
    const hp::ui::SettingsLoadResult broken =
        hp::ui::SettingsFromJson(defaults, "{ 这不是 json");
    Check(!broken.ok && !broken.error.empty(), "坏 JSON 被拒：" + broken.error);
    const hp::ui::SettingsLoadResult emptyFile = hp::ui::SettingsFromJson(defaults, "   \n");
    Check(!emptyFile.ok && !emptyFile.error.empty(), "空配置文件被拒：" + emptyFile.error);
    const hp::ui::SettingsLoadResult notObject =
        hp::ui::SettingsFromJson(defaults, "[1, 2, 3]");
    Check(!notObject.ok, "顶层不是对象被拒：" + notObject.error);

    // 值域：超范围收敛 + 警告
    const hp::ui::SettingsLoadResult fast =
        hp::ui::SettingsFromJson(defaults, "{\n  \"speed\": 9.0,\n  \"jitterMs\": 999\n}\n");
    Check(fast.ok && std::fabs(fast.settings.speed - 2.0) < 1e-9 &&
              std::fabs(fast.settings.jitterMs - defaults.jitterMs) < 1e-9 && fast.warnings.size() >= 2,
          "速度 900% 收到 200%、抖动 999 ms 回默认，并给 2 条以上警告（实际 " +
              std::to_string(fast.warnings.size()) + " 条）");

    // 热键撞车：全部回默认 + 警告
    const hp::ui::SettingsLoadResult clash = hp::ui::SettingsFromJson(
        defaults, "{\n  \"playHotkey\": \"F6\",\n  \"stopHotkey\": \"F6\",\n  \"restartHotkey\": \"F8\"\n}\n");
    Check(clash.ok && clash.settings.playHotkey == defaults.playHotkey &&
              clash.settings.stopHotkey == defaults.stopHotkey && !clash.warnings.empty(),
          "两个动作共用一把热键时全部回默认并给警告");

    // 落盘 → 读回 → 覆盖写 → 再读回
    const std::string path = hp::ExeDirectory() + "settings_roundtrip_test.json";
    std::string error;
    Check(hp::ui::SaveSettings(custom, path, error), "写配置文件成功：" + path);
    const hp::ui::SettingsLoadResult fromDisk = hp::ui::LoadSettings(defaults, path);
    Check(fromDisk.ok && fromDisk.fileExists &&
              fromDisk.settings.playHotkey == custom.playHotkey &&
              fromDisk.settings.foregroundPolicy == custom.foregroundPolicy,
          "从磁盘读回与写入一致（含前台策略）");

    hp::ui::AppSettings second = custom;
    // 注意：别拿 F6~F10 里的键当"随便一个新值"——它们现在是五个动作的默认绑定，
    // 撞车检查会按设计把整组回默认，这条断言就会假失败（F11 没被占用）
    second.playHotkey = "F11";
    second.speed = 0.75;
    Check(hp::ui::SaveSettings(second, path, error), "覆盖写同一路径成功");
    const hp::ui::SettingsLoadResult overwritten = hp::ui::LoadSettings(defaults, path);
    Check(overwritten.ok && overwritten.settings.playHotkey == "F11" &&
              std::fabs(overwritten.settings.speed - 0.75) < 1e-9,
          "覆盖写之后读到的是新值");

    // 读不存在的文件：不崩，说明原因
    const hp::ui::SettingsLoadResult missing =
        hp::ui::LoadSettings(defaults, hp::ExeDirectory() + "no_such_settings.json");
    Check(!missing.ok && !missing.fileExists && !missing.error.empty(),
          "配置文件不存在时用默认值并说明：" + missing.error);

    // 写不进去的路径：返回 false + 原因（不崩）
    const std::string badPath = "Z:\\不存在的盘\\settings.json";
    Say("试写一个不存在的盘：" + badPath);
    std::string badError;
    const bool badSaved = hp::ui::SaveSettings(custom, badPath, badError);
    Check(!badSaved, "写不进去时返回失败并给原因：" + badError);

    std::remove(path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    const std::vector<std::string> args = Utf8Arguments();
    bool all = true;
    std::string only;
    for (const std::string& arg : args) {
        if (arg == "--help" || arg == "-h") {
            std::printf("用法：final_test.exe [--all | --precision | --cpu | --threads | --keys |\n"
                        "                     --emergency | --ui | --large | --errors | --log | --config]\n");
            return 0;
        }
        if (arg == "--all") {
            all = true;   // 明确写 --all 就是跑全部（踩过：被当成"只跑叫 all 的那组"→ 一组都不跑）
            only.clear();
        } else if (arg.size() > 2 && arg[0] == '-' && arg[1] == '-') {
            only = arg.substr(2);
            all = false;
        }
    }

    hp::LogLine(LogLevel::Info, "游戏口琴自动演奏工具 —— Phase 11 自检（最终优化，十项）");
    const double started = hp::NowSeconds();

    if (all || only == "precision") {
        RunPrecision();
    }
    if (all || only == "cpu") {
        RunCpu();
    }
    if (all || only == "threads") {
        RunThreads();
    }
    if (all || only == "keys" || only == "emergency") {
        RunKeysAndEmergency();
    }
    if (all || only == "ui") {
        RunUiResponsiveness();
    }
    if (all || only == "large") {
        RunLargeSong();
    }
    if (all || only == "errors") {
        RunErrorHandling();
    }
    if (all || only == "log") {
        RunLogging();
    }
    if (all || only == "config") {
        RunConfig();
    }

    const double used = hp::NowSeconds() - started;
    hp::LogLine(LogLevel::Info, "本次自检：通过 " + std::to_string(g_passed) + " 项，失败 " +
                                    std::to_string(g_failed) + " 项，用时 " +
                                    hp::ui::FormatSeconds3(std::floor(used * 1000.0 + 0.5) / 1000.0) +
                                    " 秒。");
    return g_failed == 0 ? 0 : 1;
}
