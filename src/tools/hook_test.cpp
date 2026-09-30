// hook_test.cpp —— Phase 6 自检程序（用户输入立即停止）
//
// 需求书依据：
//   十四节 最高优先级：演奏中用户只要主动按任意键 → 立即停止、释放工具持有的输入、交还控制权
//   十五节 任意键：A-Z、0-9、F1-F24、方向键、Shift/Ctrl/Alt、Space/Tab/Enter/Esc/
//          Backspace/Insert/Delete/Home/End/PageUp/PageDown，只要新的 KeyDown 就急停
//   十六节 自动输入绝不能触发急停（否则播放器会自己把自己停掉）—— 本程序的**核心证据**
//   十七节 用 WH_KEYBOARD_LL，按"事件来源"区分：注入 → 忽略；真实按键 → 急停
//   十八节 刚按下的启动热键（F6）不能被当成用户输入
//   十九节 急停过程要非常快
//   二十节 与播放线程的竞态：急停后不得再产生新的自动输入
//   二十五节 鼠标按键也能触发；鼠标移动默认不触发（三项开关）
//   二十六节 停止原因要显示"检测到用户输入：Space"
//
// 关于"真人按键"这条路必须说清楚：
//   Windows 会给所有程序注入的输入打上 LLKHF_INJECTED 标志，程序**无法伪造**一条
//   看起来像真人按下的按键。所以：
//     - "注入必须被忽略"（十六节）可以全自动验证，而且用的是真钩子 + 真注入；
//     - "真人按键必须急停"（十四/十五节）用**同一段分发代码**喂真实形状的事件来验证，
//       再额外交给用户跑一次 --manual（真人按一下键）做最终确认。
//   本程序从不假装后者是自动验证过的。
//
// 命令行：--all / --classify / --injected / --userkey / --stress / --mouse /
//         --hotkey / --cleanup / --e2e [--inject] / --manual / --list / --help

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Log.h"
#include "core/Platform.h"
#include "core/Time.h"
#include "hotkey/GlobalHotkeyManager.h"
#include "hotkey/HotkeyBinding.h"
#include "input/InputController.h"
#include "monitor/InputMonitor.h"
#include "player/SongPlayer.h"
#include "song/TestSongs.h"
#include "song/Timeline.h"

// shellapi.h：CommandLineToArgvW（避免命令行里的中文被 ANSI 代码页破坏）
#include <shellapi.h>

namespace {

int g_passed = 0;
int g_failed = 0;
std::unique_ptr<hp::InputController> g_input;

// ---------------------------------------------------------------------------
// 断言与输出
// ---------------------------------------------------------------------------
void Section(const std::string& title) {
    hp::LogBlank();
    hp::LogLine(hp::LogLevel::Info, std::string("── ") + title + " ──");
}

void Check(bool ok, const std::string& what) {
    if (ok) {
        ++g_passed;
        hp::LogLine(hp::LogLevel::Ok, std::string("通过  ") + what);
    } else {
        ++g_failed;
        hp::LogLine(hp::LogLevel::Error, std::string("错误  ") + what);
    }
}

void Note(const std::string& text) {
    hp::LogLine(hp::LogLevel::Plain, std::string("      ") + text);
}

// ---------------------------------------------------------------------------
// 命令行
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// 测试上下文：把监控、播放器、热键串起来，并记录每次触发
// ---------------------------------------------------------------------------
struct TriggerRecord {
    std::string name;
    std::string detail;
    unsigned long long tick = 0;
};

struct TestContext {
    hp::SongPlayer* player = nullptr;
    std::atomic<int> triggers{0};
    std::atomic<long long> lastWakeTicks{0};   // 回调开始执行的时刻（用于算响应耗时）

    std::mutex mutex;
    TriggerRecord last;

    // 传给 EmergencyStop 的 detail 只填"是哪个键"：
    // 显示文本由 StopReasonName(UserInput)="检测到用户输入" + detail 拼成，
    // 这里再带一次前缀就会变成"检测到用户输入：检测到用户输入：W"（踩过）。
    std::string DetailOf(const hp::UserInputEvent& event) {
        return event.name;
    }

    // 监控回调：**在钩子线程里执行**，必须极短
    void OnUserInput(const hp::UserInputEvent& event) {
        lastWakeTicks.store(hp::NowTicks());
        const std::string detail = DetailOf(event);
        {
            std::lock_guard<std::mutex> lock(mutex);
            last.name = event.name;
            last.detail = detail;
            last.tick = event.tick;
        }
        triggers.fetch_add(1);
        if (player != nullptr && player->IsPlaying()) {
            player->EmergencyStop(detail);   // 十九节：非常快，内部无等待
        }
    }

    void Reset() {
        triggers.store(0);
        lastWakeTicks.store(0);
        std::lock_guard<std::mutex> lock(mutex);
        last = TriggerRecord{};
    }

    std::string LastDetail() {
        std::lock_guard<std::mutex> lock(mutex);
        return last.detail;
    }
};

// 喂一条"真实用户按键"（injected = false，走真实分发路径）
bool FeedRealKey(hp::InputMonitor& monitor, unsigned vk) {
    hp::RawHookEvent raw;
    raw.kind = hp::UserInputKind::KeyDown;
    raw.keyUp = false;
    raw.vk = vk;
    raw.injected = false;
    raw.extraInfo = 0;
    raw.tick = GetTickCount64();
    return monitor.ProcessRawEventForTest(raw);
}

bool FeedRealMouse(hp::InputMonitor& monitor, hp::MouseButton button) {
    hp::RawHookEvent raw;
    raw.kind = hp::UserInputKind::MouseButtonDown;
    raw.keyUp = false;
    raw.button = button;
    raw.injected = false;
    raw.tick = GetTickCount64();
    return monitor.ProcessRawEventForTest(raw);
}

bool FeedRealMouseMove(hp::InputMonitor& monitor) {
    hp::RawHookEvent raw;
    raw.kind = hp::UserInputKind::MouseMove;
    raw.injected = false;
    raw.tick = GetTickCount64();
    return monitor.ProcessRawEventForTest(raw);
}

// 用真钩子能收到的注入事件来做"注入必须被忽略"的验证。
// 选 F13-F15：它们不在普通键盘上、任何程序都不会响应，注入它们不会打扰任何窗口。
constexpr unsigned kHarmlessInjectedKeys[] = {VK_F13, VK_F14, VK_F15};

void InjectHarmlessKeys(int repeats) {
    for (int i = 0; i < repeats; ++i) {
        for (unsigned vk : kHarmlessInjectedKeys) {
            g_input->SimulateUserKeyPress(static_cast<unsigned short>(vk), 8);
        }
        Sleep(5);
    }
}

// 压力测试随机打断用的键池（覆盖字母、数字、功能键、方向键、修饰键）
const unsigned kVkPool[] = {'Q', 'W',  'E', 'R', 'A',     'S',    'D',    '1',
                            '2', VK_SPACE, VK_TAB, VK_F2, VK_LEFT, VK_RIGHT, VK_LSHIFT};
const size_t kKeyPoolSize = sizeof(kVkPool) / sizeof(kVkPool[0]);

// ---------------------------------------------------------------------------
// 用例 1：分类器与键名（纯逻辑 + 真实分发路径上的忽略分支）
// ---------------------------------------------------------------------------
void RunClassifierTest(hp::InputMonitor& monitor) {
    Section("分类：注入判据与键名表");

    Check(hp::InputMonitor::IsInjectedEvent(true, 0),
          "带注入标志的事件被判为注入（十六节第一道判据）");
    Check(hp::InputMonitor::IsInjectedEvent(false, 0x48504C59ULL),
          "带本工具标记（HPLY）的事件被判为注入（第二道保险）");
    Check(!hp::InputMonitor::IsInjectedEvent(false, 0), "普通事件不被误判为注入");

    // 需求书十五节点名要认的键
    struct NameCase {
        unsigned vk;
        const char* expect;
    };
    const NameCase cases[] = {
        {'A', "A"},           {'Z', "Z"},          {'0', "0"},         {'9', "9"},
        {VK_F1, "F1"},        {VK_F6, "F6"},       {VK_F12, "F12"},    {VK_F24, "F24"},
        {VK_LEFT, "Left"},    {VK_RIGHT, "Right"}, {VK_UP, "Up"},      {VK_DOWN, "Down"},
        {VK_LSHIFT, "Shift"}, {VK_LCONTROL, "Ctrl"},{VK_LMENU, "Alt"}, {VK_SPACE, "Space"},
        {VK_TAB, "Tab"},      {VK_RETURN, "Enter"}, {VK_ESCAPE, "Esc"}, {VK_BACK, "Backspace"},
        {VK_INSERT, "Insert"},{VK_DELETE, "Delete"},{VK_HOME, "Home"}, {VK_END, "End"},
        {VK_PRIOR, "PageUp"}, {VK_NEXT, "PageDown"},
    };
    int named = 0;
    for (const NameCase& item : cases) {
        if (hp::VirtualKeyDisplayName(item.vk) == item.expect) {
            ++named;
        } else {
            Check(false, std::string("键名不对：") + item.expect + " → " +
                             hp::VirtualKeyDisplayName(item.vk));
        }
    }
    Check(named == static_cast<int>(sizeof(cases) / sizeof(cases[0])),
          "十五节键表 " + std::to_string(named) + "/" +
              std::to_string(sizeof(cases) / sizeof(cases[0])) + " 个键名全部可读");

    // 走真实分发路径：注入事件被忽略、抬起被忽略
    const hp::InputMonitorStats before = monitor.Stats();
    hp::RawHookEvent injected;
    injected.kind = hp::UserInputKind::KeyDown;
    injected.vk = 'W';
    injected.injected = true;
    monitor.ProcessRawEventForTest(injected);
    Check(monitor.Stats().injectedIgnored == before.injectedIgnored + 1 &&
              monitor.Stats().triggered == before.triggered,
          "注入事件在真实分发路径里被忽略（不触发急停）");

    hp::RawHookEvent keyUp;
    keyUp.kind = hp::UserInputKind::KeyDown;
    keyUp.keyUp = true;
    keyUp.vk = 'W';
    keyUp.injected = false;
    monitor.ProcessRawEventForTest(keyUp);
    Check(monitor.Stats().keyUpIgnored == before.keyUpIgnored + 1 &&
              monitor.Stats().triggered == before.triggered,
          "键盘抬起不触发急停（十五节只要按下）");

    hp::RawHookEvent marked;
    marked.kind = hp::UserInputKind::KeyDown;
    marked.vk = 'X';
    marked.injected = false;
    marked.extraInfo = 0x48504C59ULL;   // 只带 HPLY 标记、没有注入标志位
    monitor.ProcessRawEventForTest(marked);
    Check(monitor.Stats().injectedIgnored == before.injectedIgnored + 2 &&
              monitor.Stats().triggered == before.triggered,
          "只带 HPLY 标记的事件也被忽略（第二道保险有效）");
}

// ---------------------------------------------------------------------------
// 用例 2：注入输入不能触发急停（十六节，核心证据：真钩子 + 真注入）
// ---------------------------------------------------------------------------
void RunInjectedInputTest(hp::InputMonitor& monitor, TestContext& context, bool dryRun) {
    Section("十六节核心：注入输入绝不触发急停");

    hp::PlayerOptions options;
    options.dryRun = dryRun;
    hp::SongPlayer player(*g_input, options);
    context.player = &player;
    context.Reset();

    const hp::Song song = hp::test_songs::Scale();
    const hp::Timeline timeline = hp::Timeline::Build(song);
    player.Play(timeline);
    if (!player.WaitForPlaying(2000)) {
        Check(false, "播放器没进入演奏中，用例无法进行");
        return;
    }

    const hp::InputMonitorStats before = monitor.Stats();
    Note("在演奏中进行注入（F13/F14/F15，任何程序都不会响应，也不打扰任何窗口）");
    InjectHarmlessKeys(6);
    Sleep(200);

    const hp::InputMonitorStats after = monitor.Stats();
    Check(after.keyboardEvents > before.keyboardEvents,
          "真钩子确实收到了注入事件（键盘事件 " + std::to_string(before.keyboardEvents) + " → " +
              std::to_string(after.keyboardEvents) + " 条）");
    Check(after.injectedIgnored >= 18,
          "这些事件全部因为'是注入的'被忽略（" + std::to_string(after.injectedIgnored) +
              " 条）");
    Check(after.triggered == before.triggered, "注入没有触发任何一次急停");
    Check(context.triggers.load() == 0, "急停回调一次都没被调用");
    Check(player.IsPlaying(), "播放器还在正常演奏（没有自己把自己停掉）");
    Check(!monitor.SawRealUserInput(), "监控没有把它当成真人输入");
    Check(player.FiredNoteCount() > 0, "演奏确实在推进（已起音 " +
                                          std::to_string(player.FiredNoteCount()) + " 个）");

    player.Stop(hp::StopReason::UserStop);
    player.WaitForStopped(1000);
    Check(player.HeldInputCount() == 0, "收尾：没有留下按住的键");
    Note("钩子回调最坏耗时 " + std::to_string(after.worstCallbackMs) + " ms");
    context.player = nullptr;
}

// ---------------------------------------------------------------------------
// 用例 3：真实用户按键 → 立刻急停（十四/十五/十九/二十/二十六节）
// ---------------------------------------------------------------------------
void RunRealUserKeyTest(hp::InputMonitor& monitor, TestContext& context) {
    Section("十四/十五节：用户按键立刻急停");

    hp::PlayerOptions options;
    options.dryRun = true;
    hp::SongPlayer player(*g_input, options);
    context.player = &player;

    const hp::Song song = hp::test_songs::Scale();
    const hp::Timeline timeline = hp::Timeline::Build(song);

    context.Reset();
    player.Play(timeline);
    Check(player.WaitForPlaying(2000), "演奏开始");
    Sleep(400);
    const int firedBefore = player.FiredNoteCount();

    // 一条"真人按下 W"的事件，走真实分发路径
    const long long beforeFeed = hp::NowTicks();
    FeedRealKey(monitor, 'W');
    const long long wakeTicks = context.lastWakeTicks.load();
    const bool stopped = player.WaitForStopped(2000);
    const double responseMs = hp::TicksToSeconds(wakeTicks - beforeFeed) * 1000.0;

    Check(context.triggers.load() == 1, "急停回调被调用 1 次");
    Check(stopped, "播放器立刻停止");
    Check(player.LastStopReason() == hp::StopReason::UserInput,
          std::string("停止原因 = ") + hp::StopReasonName(player.LastStopReason()));
    Check(context.LastDetail() == "W" &&
              player.StatusText().find("检测到用户输入：W") != std::string::npos,
          "停止原因文本（二十六节）：" + context.LastDetail() + " → " + player.StatusText());
    Check(player.StatusText().find("检测到用户输入：W") != std::string::npos,
          "播放器状态文本带原因：" + player.StatusText());
    Check(player.HeldInputCount() == 0, "工具持有的输入已全部释放（十四节'交还控制权'）");
    Check(firedBefore > 0, "急停前确实在演奏（已起音 " + std::to_string(firedBefore) + " 个）");
    Note("从事件到回调开始：" + std::to_string(responseMs) + " ms");

    // 二十节：急停之后不得再产生新的自动输入
    const int firedAtStop = player.FiredNoteCount();
    Sleep(400);
    Check(player.FiredNoteCount() == firedAtStop,
          "急停之后不再产生新的自动输入（起音数停在 " + std::to_string(firedAtStop) + "）");
    Check(g_input->AnyPlayerInputDown() == false, "没有任何输入还被工具按着");

    // 十五节的键表：逐个"演奏中打断"，确认任意键都能停、名字都对
    struct KeyCase {
        unsigned vk;
        const char* name;
    };
    const KeyCase keys[] = {
        {'Q', "Q"},           {'0', "0"},          {VK_F1, "F1"},       {VK_F12, "F12"},
        {VK_LEFT, "Left"},    {VK_UP, "Up"},       {VK_LSHIFT, "Shift"},{VK_LCONTROL, "Ctrl"},
        {VK_LMENU, "Alt"},    {VK_SPACE, "Space"}, {VK_TAB, "Tab"},     {VK_RETURN, "Enter"},
        {VK_ESCAPE, "Esc"},   {VK_BACK, "Backspace"},{VK_INSERT, "Insert"},
        {VK_DELETE, "Delete"},{VK_HOME, "Home"},   {VK_END, "End"},     {VK_PRIOR, "PageUp"},
        {VK_NEXT, "PageDown"},
    };
    int stopCount = 0;
    for (const KeyCase& item : keys) {
        context.Reset();
        player.Play(timeline);
        if (!player.WaitForPlaying(1000)) {
            Check(false, std::string("演奏没起来：") + item.name);
            break;
        }
        Sleep(80);
        FeedRealKey(monitor, item.vk);
        const bool ok = player.WaitForStopped(1000);
        const bool named = context.LastDetail() == item.name;
        const bool clean = player.HeldInputCount() == 0;
        if (ok && named && clean) {
            ++stopCount;
        } else {
            Check(false, std::string("按 ") + item.name + " 没停干净（停止=" +
                             (ok ? "是" : "否") + " 原因文本=" + context.LastDetail() + "）");
        }
    }
    Check(stopCount == static_cast<int>(sizeof(keys) / sizeof(keys[0])),
          "十五节键表 " + std::to_string(stopCount) + "/" +
              std::to_string(sizeof(keys) / sizeof(keys[0])) + " 个键都能立刻停干净");

    player.Stop(hp::StopReason::UserStop);
    context.player = nullptr;
}

// ---------------------------------------------------------------------------
// 用例 4：急停压力（二十节的竞态，多轮重复）
// ---------------------------------------------------------------------------
void RunStressTest(hp::InputMonitor& monitor, TestContext& context, int rounds) {
    Section("二十节：急停竞态压力（演奏中随时打断）");

    hp::PlayerOptions options;
    options.dryRun = true;
    hp::SongPlayer player(*g_input, options);
    context.player = &player;
    const hp::Timeline timeline = hp::Timeline::Build(hp::test_songs::Scale());

    int clean = 0;
    double worstResponseMs = 0.0;
    for (int round = 0; round < rounds; ++round) {
        context.Reset();
        player.Play(timeline);
        if (!player.WaitForPlaying(1000)) {
            Check(false, "第 " + std::to_string(round + 1) + " 轮演奏没起来");
            break;
        }
        // 随机一点的位置打断
        Sleep(20 + (round * 37) % 220);
        const unsigned vk = kVkPool[static_cast<size_t>(round) % kKeyPoolSize];
        const long long before = hp::NowTicks();
        FeedRealKey(monitor, vk);
        const long long wake = context.lastWakeTicks.load();
        const bool stopped = player.WaitForStopped(1000);
        const double responseMs = hp::TicksToSeconds(wake - before) * 1000.0;
        if (responseMs > worstResponseMs) {
            worstResponseMs = responseMs;
        }
        const int firedAtStop = player.FiredNoteCount();
        Sleep(30);
        const bool noMoreNotes = player.FiredNoteCount() == firedAtStop;
        const bool cleanInput = player.HeldInputCount() == 0 && !g_input->AnyPlayerInputDown();
        if (stopped && noMoreNotes && cleanInput) {
            ++clean;
        } else {
            Check(false, "第 " + std::to_string(round + 1) + " 轮留下问题（停止=" +
                             (stopped ? "是" : "否") + " 无新音符=" +
                             (noMoreNotes ? "是" : "否") + " 无残留键=" +
                             (cleanInput ? "是" : "否") + "）");
        }
    }
    Check(clean == rounds, "急停压力 " + std::to_string(clean) + "/" + std::to_string(rounds) +
                               " 轮全部停干净、无残留、无新输入");
    Note("最坏响应（事件→回调）" + std::to_string(worstResponseMs) + " ms");

    player.Stop(hp::StopReason::UserStop);
    context.player = nullptr;
}

// ---------------------------------------------------------------------------
// 用例 5：鼠标（二十五节）
// ---------------------------------------------------------------------------
void RunMouseTest(hp::InputMonitor& monitor, TestContext& context) {
    Section("二十五节：鼠标按键与鼠标移动");

    hp::PlayerOptions options;
    options.dryRun = true;
    hp::SongPlayer player(*g_input, options);
    context.player = &player;
    const hp::Timeline timeline = hp::Timeline::Build(hp::test_songs::Scale());

    // 1) 鼠标左键（真实）→ 急停
    context.Reset();
    player.Play(timeline);
    player.WaitForPlaying(1000);
    Sleep(150);
    FeedRealMouse(monitor, hp::MouseButton::Left);
    Check(player.WaitForStopped(1000), "鼠标左键按下 → 立刻急停");
    Check(context.LastDetail() == "鼠标左键" &&
              player.StatusText().find("检测到用户输入：鼠标左键") != std::string::npos,
          "停止原因带鼠标键名：" + context.LastDetail() + " → " + player.StatusText());
    player.Stop(hp::StopReason::UserStop);

    // 2) 注入的鼠标点击 → 忽略
    //    这里喂同形状的"注入"事件而不是真的点一下鼠标：真点会点到光标底下的窗口，
    //    属于不该有的副作用。钩子对注入的判定机制已由上面的键盘真注入证明。
    const hp::InputMonitorStats before = monitor.Stats();
    player.Play(timeline);
    player.WaitForPlaying(1000);
    Sleep(150);
    hp::RawHookEvent injectedClick;
    injectedClick.kind = hp::UserInputKind::MouseButtonDown;
    injectedClick.button = hp::MouseButton::Left;
    injectedClick.injected = true;
    injectedClick.tick = GetTickCount64();
    monitor.ProcessRawEventForTest(injectedClick);
    Sleep(100);
    Check(monitor.Stats().injectedIgnored > before.injectedIgnored,
          "注入的鼠标点击被忽略（注入计数 " + std::to_string(before.injectedIgnored) + " → " +
              std::to_string(monitor.Stats().injectedIgnored) + "）");
    Check(monitor.Stats().triggered == before.triggered, "注入的鼠标点击没有触发急停");
    Check(player.IsPlaying(), "播放器仍在演奏");
    player.Stop(hp::StopReason::UserStop);

    // 3) 鼠标移动默认关闭
    const hp::InputMonitorStats beforeMove = monitor.Stats();
    player.Play(timeline);
    player.WaitForPlaying(1000);
    Sleep(150);
    FeedRealMouseMove(monitor);
    Sleep(150);
    Check(monitor.Stats().moveIgnored == beforeMove.moveIgnored + 1,
          "鼠标移动默认被忽略（二十五节：默认不触发）");
    Check(player.IsPlaying(), "鼠标移动没有打断演奏");
    player.Stop(hp::StopReason::UserStop);

    // 4) 打开"鼠标移动触发"后应当触发
    hp::InputMonitorOptions withMove = monitor.Options();
    withMove.mouseMove = true;
    monitor.SetOptions(withMove);
    context.Reset();
    player.Play(timeline);
    player.WaitForPlaying(1000);
    Sleep(150);
    FeedRealMouseMove(monitor);
    Check(player.WaitForStopped(1000), "打开开关后，鼠标移动也能触发急停");
    Check(context.LastDetail() == "鼠标移动" &&
              player.StatusText().find("检测到用户输入：鼠标移动") != std::string::npos,
          "停止原因文本：" + context.LastDetail() + " → " + player.StatusText());
    player.Stop(hp::StopReason::UserStop);
    monitor.SetOptions(hp::InputMonitorOptions{});   // 恢复默认（键盘开、鼠标键开、移动关）

    // 5) 关掉"鼠标按键"后不触发
    hp::InputMonitorOptions noButtons = monitor.Options();
    noButtons.mouseButtons = false;
    monitor.SetOptions(noButtons);
    context.Reset();
    player.Play(timeline);
    player.WaitForPlaying(1000);
    Sleep(150);
    FeedRealMouse(monitor, hp::MouseButton::Right);
    Sleep(150);
    Check(context.triggers.load() == 0 && player.IsPlaying(),
          "设置里关掉鼠标按键后，右键不再触发急停");
    player.Stop(hp::StopReason::UserStop);
    monitor.SetOptions(hp::InputMonitorOptions{});
    Check(monitor.Options().mouseButtons && !monitor.Options().mouseMove,
          "设置已恢复默认（" + monitor.DescribeOptions() + "）");
    context.player = nullptr;
}

// ---------------------------------------------------------------------------
// 用例 6：十八节（启动热键不能被当成用户输入）
// ---------------------------------------------------------------------------
void RunStartHotkeyGuardTest(hp::GlobalHotkeyManager& hotkeys, hp::InputMonitor& monitor,
                             TestContext& context) {
    Section("十八节：刚按下的启动热键不能被当成用户输入");

    const unsigned startVk = hotkeys.StartHotkeyVirtualKey();
    Check(startVk == VK_F6, "启动热键当前是 " + hp::VirtualKeyDisplayName(startVk));

    // 先真触发一次启动热键（注入的，Phase 5 的热键管理器会收到）
    const int before = hotkeys.EventCount(hp::HotkeyAction::Play);
    g_input->SimulateUserKeyPress(static_cast<unsigned short>(startVk), 30);
    Check(hotkeys.WaitForEvent(hp::HotkeyAction::Play, before + 1, 2000),
          "注入 " + hp::VirtualKeyDisplayName(startVk) + " 真的触发了启动热键");
    Check(hotkeys.StartHotkeyPressedRecently(700), "启动热键时间戳已记录");

    // 紧接着来一条"真实按下的 F6"（真人刚按下的那一瞬间）：必须被忽略
    const hp::InputMonitorStats beforeIgnore = monitor.Stats();
    context.Reset();
    FeedRealKey(monitor, startVk);
    Check(monitor.Stats().hotkeyIgnored == beforeIgnore.hotkeyIgnored + 1,
          "刚按下的 F6 被当作启动热键忽略（十八节）");
    Check(context.triggers.load() == 0, "没有触发急停");

    // 过了时间窗再按 F6：应当正常触发（说明只是"忽略刚按下那一下"，不是永久静音）
    Sleep(800);
    context.Reset();
    FeedRealKey(monitor, startVk);
    Check(context.triggers.load() == 1,
          "过了 700 ms 再按 F6 就当成用户输入了（时间窗是有界的）");
    Check(context.LastDetail() == "F6",
          "原因文本只剩键名（显示时会拼成\"检测到用户输入：F6\"）：" + context.LastDetail());
}

// ---------------------------------------------------------------------------
// 用例 7：停止与清理
// ---------------------------------------------------------------------------
void RunCleanupTest(hp::InputMonitor& monitor, TestContext& context) {
    Section("停止与清理");

    // 1) 运行期间只允许一个监控（低级钩子是进程级的）
    hp::InputMonitor second;
    std::string secondError;
    const bool secondStarted = second.Start(hp::InputMonitorOptions{}, hp::InputMonitorCallbacks{},
                                            &secondError);
    Check(!secondStarted, "运行期间第二个监控被拒绝：" + secondError);

    // 2) 停止之后钩子真的卸掉了
    monitor.Stop();
    Check(!monitor.IsRunning(), "Stop() 之后监控不再运行");
    Check(!monitor.ProcessRawEventForTest(hp::RawHookEvent{}), "停止后连自检事件也不接受");

    const hp::InputMonitorStats frozen = monitor.Stats();
    InjectHarmlessKeys(2);
    Sleep(200);
    const hp::InputMonitorStats afterStop = monitor.Stats();
    Check(afterStop.keyboardEvents == frozen.keyboardEvents,
          "停止后钩子真的卸掉了（键盘事件数冻结在 " + std::to_string(frozen.keyboardEvents) +
              " 条）");

    // 3) 停掉之后可以让别的（或自己）重新装上
    hp::InputMonitorCallbacks callbacks;
    callbacks.onUserInput = [&context](const hp::UserInputEvent& event) { context.OnUserInput(event); };
    std::string error;
    const bool restarted = monitor.Start(hp::InputMonitorOptions{}, callbacks, &error);
    Check(restarted && monitor.IsRunning(),
          "停止后可以重新启动：" + (restarted ? std::string("成功") : error));

    context.Reset();
    FeedRealKey(monitor, 'A');
    Check(context.triggers.load() == 1, "重新启动后真人按键又能触发急停");

    // 4) 收尾：留一个干净状态给后面的用例（它们要自己装监控）
    monitor.Stop();
    Check(!monitor.IsRunning(), "用例结束时已停止");
}

// ---------------------------------------------------------------------------
// 用例 8：端到端（真注入演奏 + 模拟硬件按键中断）
// ---------------------------------------------------------------------------
void RunEndToEnd(bool dryRun) {
    Section(std::string("端到端：演奏中被用户输入打断（") + (dryRun ? "干跑" : "真注入") + "）");

    hp::PlayerOptions options;
    options.dryRun = dryRun;
    hp::SongPlayer player(*g_input, options);
    TestContext context;
    context.player = &player;

    hp::InputMonitorCallbacks callbacks;
    callbacks.onUserInput = [&context](const hp::UserInputEvent& event) { context.OnUserInput(event); };

    hp::InputMonitor monitor;
    std::string error;
    if (!monitor.Start(hp::InputMonitorOptions{}, callbacks, &error)) {
        Check(false, "监控没起来：" + error);
        return;
    }

    const hp::Timeline timeline = hp::Timeline::Build(hp::test_songs::Scale());
    Note("曲目：" + timeline.SongName() + "　音符 " + std::to_string(timeline.NoteCount()) +
         " 个　时长 " + std::to_string(timeline.TotalDuration()) + " 秒");
    if (!dryRun) {
        Note("演奏会真的往当前前台窗口打字，请先切到记事本（hotkey-e2e.sh 会做好这件事）");
    }

    player.Play(timeline);
    Check(player.WaitForPlaying(2000), "演奏开始");
    Sleep(1200);
    const int firedBeforeStop = player.FiredNoteCount();
    Note("打断前已起音 " + std::to_string(firedBeforeStop) + " 个");

    FeedRealKey(monitor, 'W');   // 模拟"用户按了 W"
    Check(player.WaitForStopped(2000), "用户输入让演奏立刻停止");
    Check(player.LastStopReason() == hp::StopReason::UserInput,
          "停止原因 = " + std::string(hp::StopReasonName(player.LastStopReason())));
    Check(context.LastDetail() == "W" &&
              player.StatusText().find("检测到用户输入：W") != std::string::npos,
          "原因文本（二十六节）：" + player.StatusText());
    Check(player.HeldInputCount() == 0, "没有留下按住的键");

    const int firedAtStop = player.FiredNoteCount();
    Sleep(500);
    Check(player.FiredNoteCount() == firedAtStop, "停止之后没有新的自动输入");

    monitor.Stop();
    player.Quit();
}

// ---------------------------------------------------------------------------
// 用例 9：手动验证（真人按键 —— 程序无法伪造，必须由人来按）
// ---------------------------------------------------------------------------
int RunManualTest(int seconds) {
    Section("手动验证：请你自己按一个键（这是唯一能证明'真人按键'的路径）");

    hp::PlayerOptions options;
    options.dryRun = true;   // 干跑：不会往你的窗口里打字，放心按
    hp::SongPlayer player(*g_input, options);
    TestContext context;
    context.player = &player;

    hp::InputMonitorCallbacks callbacks;
    callbacks.onUserInput = [&context](const hp::UserInputEvent& event) { context.OnUserInput(event); };

    hp::InputMonitor monitor;
    std::string error;
    if (!monitor.Start(hp::InputMonitorOptions{}, callbacks, &error)) {
        hp::LogError("监控没起来：" + error);
        return 1;
    }

    const hp::Timeline timeline = hp::Timeline::Build(hp::test_songs::Scale());
    player.Play(timeline);
    if (!player.WaitForPlaying(2000)) {
        hp::LogError("播放器没进入演奏中");
        monitor.Stop();
        return 1;
    }

    hp::LogLine(hp::LogLevel::Warn, "演奏中（干跑，不会打字）。请在 " +
                                        std::to_string(seconds) + " 秒内用手按任意一个键" +
                                        "（例如 Space）…");
    for (int i = seconds; i > 0; --i) {
        hp::LogInline("  还有 " + std::to_string(i) + " 秒，请按一个键…");
        if (monitor.SawRealUserInput()) {
            break;
        }
        // 曲子会自己放完（6 秒）。放完就续播，保证整个窗口里都处于"演奏中"，
        // 否则用户按下的那一下会被算成"曲子自然结束"而不是"用户输入"。
        if (!player.IsPlaying()) {
            player.Play(timeline);
            player.WaitForPlaying(1000);
        }
        Sleep(1000);
    }
    hp::LogBlank();

    const bool saw = monitor.SawRealUserInput();
    const bool stopped = player.WaitForStopped(1500) || player.IsStopped();
    const std::string detail = context.LastDetail();

    if (saw) {
        Check(true, "检测到真人按键：" + detail + " → " + player.StatusText());
        Check(stopped, "演奏已经停止");
        Check(player.LastStopReason() == hp::StopReason::UserInput, "停止原因类型 = 检测到用户输入");
        Check(player.HeldInputCount() == 0, "没有留下按住的键");
    } else {
        hp::LogLine(hp::LogLevel::Warn, std::to_string(seconds) +
                    " 秒内没有检测到真人按键（可能你没按，或按键被别的程序吃掉了）。"
                    "这一步不能由程序代替，可以再跑一次 --manual。");
    }

    Note(monitor.DescribeStats());
    monitor.Stop();
    player.Quit();
    return (g_failed == 0) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// 全部自动验证
// ---------------------------------------------------------------------------
int RunAllTests() {
    hp::GlobalHotkeyManager hotkeys;
    std::string hotkeyError;
    if (!hotkeys.Start(nullptr, &hotkeyError)) {
        Check(false, "热键管理器启动失败（十八节用例需要它）：" + hotkeyError);
    }

    hp::InputMonitor monitor;
    TestContext context;
    hp::InputMonitorCallbacks callbacks;
    callbacks.onUserInput = [&context](const hp::UserInputEvent& event) { context.OnUserInput(event); };
    // 十八节：刚按下的启动热键要忽略
    callbacks.shouldIgnore = [&hotkeys](const hp::UserInputEvent& event) {
        return event.vk != 0 && hotkeys.StartHotkeyPressedRecently(700) &&
               event.vk == hotkeys.StartHotkeyVirtualKey();
    };

    std::string error;
    if (!monitor.Start(hp::InputMonitorOptions{}, callbacks, &error)) {
        Check(false, "输入监控启动失败：" + error);
        hotkeys.Stop();
        return 1;
    }
    Check(monitor.IsRunning(), "输入监控已启动（键盘钩子 " +
                                   std::string(monitor.Options().keyboard ? "开" : "关") +
                                   "，鼠标钩子 " +
                                   std::string(monitor.Options().mouseButtons ? "开" : "关") + "）");
    Note(monitor.DescribeOptions());

    RunClassifierTest(monitor);
    RunInjectedInputTest(monitor, context, true);
    RunRealUserKeyTest(monitor, context);
    RunStressTest(monitor, context, 20);
    RunMouseTest(monitor, context);
    RunStartHotkeyGuardTest(hotkeys, monitor, context);
    RunCleanupTest(monitor, context);

    hp::LogBlank();
    hp::LogLine(hp::LogLevel::Info, "钩子统计：");
    Note(monitor.DescribeStats());

    RunEndToEnd(true);

    hp::LogLine(hp::LogLevel::Warn,
                "注意：--all 不含真人按键验证（程序无法伪造非注入事件），"
                "请单独跑一次 --manual 用真人按键确认。");
    hotkeys.Stop();
    return (g_failed == 0) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// 交互菜单
// ---------------------------------------------------------------------------
int RunInteractiveMenu() {
    hp::GlobalHotkeyManager hotkeys;
    hotkeys.Start(nullptr, nullptr);

    hp::InputMonitor monitor;
    TestContext context;
    hp::InputMonitorCallbacks callbacks;
    callbacks.onUserInput = [&context](const hp::UserInputEvent& event) { context.OnUserInput(event); };
    callbacks.shouldIgnore = [&hotkeys](const hp::UserInputEvent& event) {
        return event.vk != 0 && hotkeys.StartHotkeyPressedRecently(700) &&
               event.vk == hotkeys.StartHotkeyVirtualKey();
    };
    std::string error;
    if (!monitor.Start(hp::InputMonitorOptions{}, callbacks, &error)) {
        hp::LogError("输入监控启动失败：" + error);
        return 1;
    }
    Note(monitor.DescribeOptions());
    Note("现在演奏中按任意键就会立刻停止（真实按键；程序注入的按键不会触发，十六节）");

    bool running = true;
    while (running) {
        hp::LogBlank();
        hp::LogLine(hp::LogLevel::Info, "请选择：");
        Note("1  环境自检（钩子状态、设置、统计）");
        Note("2  分类与键名（纯逻辑，零风险）");
        Note("3  注入输入不能触发急停（十六节核心，真钩子 + 真注入）");
        Note("4  用户按键立刻急停（十四/十五/十九/二十六节）");
        Note("5  急停压力 20 轮（二十节竞态）");
        Note("6  鼠标输入（按键 / 移动 / 开关）");
        Note("7  十八节：启动热键不误触");
        Note("8  停止与清理");
        Note("9  端到端（干跑）");
        Note("m  手动验证：请你自己按一个键");
        Note("0  退出");
        hp::LogInline("输入后回车：");

        std::string line;
        if (!std::getline(std::cin, line)) {
            break;
        }
        if (line == "0") {
            running = false;
        } else if (line == "1") {
            Check(monitor.IsRunning(), "输入监控运行中");
            Note("监控线程 " + std::to_string(monitor.MonitorThreadId()) + "，主线程 " +
                 std::to_string(GetCurrentThreadId()));
            Note(monitor.DescribeOptions());
            Note(monitor.DescribeStats());
            Check(monitor.Stats().worstCallbackMs < 5.0,
                  "钩子回调最坏耗时 " + std::to_string(monitor.Stats().worstCallbackMs) +
                      " ms（必须远小于系统 300 ms 上限）");
        } else if (line == "2") {
            RunClassifierTest(monitor);
        } else if (line == "3") {
            RunInjectedInputTest(monitor, context, true);
        } else if (line == "4") {
            RunRealUserKeyTest(monitor, context);
        } else if (line == "5") {
            RunStressTest(monitor, context, 20);
        } else if (line == "6") {
            RunMouseTest(monitor, context);
        } else if (line == "7") {
            RunStartHotkeyGuardTest(hotkeys, monitor, context);
        } else if (line == "8") {
            RunCleanupTest(monitor, context);
        } else if (line == "9") {
            RunEndToEnd(true);
        } else if (line == "m" || line == "M") {
            hp::LogLine(hp::LogLevel::Info,
                        "手动验证请退出菜单后用命令行跑：hook_test.exe --manual");
        } else {
            hp::LogWarn("看不懂这个选项：" + line);
        }
    }

    monitor.Stop();
    hotkeys.Stop();
    hp::LogBlank();
    hp::LogLine(hp::LogLevel::Info, "本次测试：通过 " + std::to_string(g_passed) + " 项，失败 " +
                                        std::to_string(g_failed) + " 项。");
    g_input->ReleaseSimulatedKeys();
    return (g_failed == 0) ? 0 : 1;
}

void PrintHelp() {
    Note("用法：hook_test.exe [选项]");
    Note("  --all        全部自动验证（不含真人按键，见 --manual）");
    Note("  --classify   分类与键名（纯逻辑）");
    Note("  --injected   注入输入不能触发急停（十六节核心）");
    Note("  --userkey    用户按键立刻急停（十四/十五节）");
    Note("  --stress [N] 急停压力（默认 20 轮）");
    Note("  --mouse      鼠标按键与移动");
    Note("  --hotkey     十八节：启动热键不误触");
    Note("  --cleanup    停止与清理");
    Note("  --e2e [--inject]  端到端（默认干跑）");
    Note("  --manual [--manual-seconds N]  手动验证：请你自己按一个键（默认 8 秒窗口）");
    Note("  --list       列出默认设置");
    Note("  --help       本帮助");
    Note("无参数 = 交互菜单。");
    Note("说明：程序注入的按键带 LLKHF_INJECTED，无法伪装成真人按键，"
         "所以'真人按键'这条路径只能靠 --manual 由人确认。");
}

void PrintBanner() {
    hp::LogBlank();
    hp::LogLine(hp::LogLevel::Info, "游戏口琴自动演奏工具 —— Phase 6 自检（用户输入立即停止）");
    Note("需求书十四～二十节：任意键急停、注入不误触、启动热键特殊处理、竞态无残留");
    Note("需求书二十五/二十六节：鼠标按键可触发（移动默认关）、停止原因要能显示");
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    const std::vector<std::string> args = Utf8Arguments();
    g_input = std::make_unique<hp::InputController>(hp::InputSendOptions());

    PrintBanner();

    if (args.size() <= 1) {
        return RunInteractiveMenu();
    }
    if (HasFlag(args, "--help") || HasFlag(args, "-h")) {
        PrintHelp();
        return 0;
    }
    if (HasFlag(args, "--list")) {
        Note("默认设置：键盘按键 开　鼠标按键 开　鼠标移动 关（需求书二十五节）");
        Note("注入判据：LLKHF_INJECTED / LLMHF_INJECTED 标志，或 dwExtraInfo = 0x48504C59 (HPLY)");
        Note("停止原因文本：\"检测到用户输入：<键名>\"（需求书二十六节）");
        return 0;
    }
    if (HasFlag(args, "--manual")) {
        int seconds = 8;
        for (size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == "--manual-seconds") {
                const int value = std::atoi(args[i + 1].c_str());
                if (value > 0) {
                    seconds = value;
                }
            }
        }
        return RunManualTest(seconds);
    }
    if (HasFlag(args, "--all")) {
        const int code = RunAllTests();
        hp::LogBlank();
        hp::LogLine(hp::LogLevel::Info, "本次测试：通过 " + std::to_string(g_passed) +
                                            " 项，失败 " + std::to_string(g_failed) + " 项。");
        g_input->ReleaseSimulatedKeys();
        return code;
    }
    if (HasFlag(args, "--e2e")) {
        RunEndToEnd(!HasFlag(args, "--inject"));
        hp::LogBlank();
        hp::LogLine(hp::LogLevel::Info, "本次测试：通过 " + std::to_string(g_passed) +
                                            " 项，失败 " + std::to_string(g_failed) + " 项。");
        g_input->ReleaseSimulatedKeys();
        return (g_failed == 0) ? 0 : 1;
    }

    // 其余用例都要一个运行中的监控
    hp::GlobalHotkeyManager hotkeys;
    hotkeys.Start(nullptr, nullptr);

    hp::InputMonitor monitor;
    TestContext context;
    hp::InputMonitorCallbacks callbacks;
    callbacks.onUserInput = [&context](const hp::UserInputEvent& event) { context.OnUserInput(event); };
    callbacks.shouldIgnore = [&hotkeys](const hp::UserInputEvent& event) {
        return event.vk != 0 && hotkeys.StartHotkeyPressedRecently(700) &&
               event.vk == hotkeys.StartHotkeyVirtualKey();
    };
    std::string error;
    if (!monitor.Start(hp::InputMonitorOptions{}, callbacks, &error)) {
        hp::LogError("输入监控启动失败：" + error);
        return 1;
    }

    if (HasFlag(args, "--classify")) {
        RunClassifierTest(monitor);
    }
    if (HasFlag(args, "--injected")) {
        RunInjectedInputTest(monitor, context, !HasFlag(args, "--inject"));
    }
    if (HasFlag(args, "--userkey")) {
        RunRealUserKeyTest(monitor, context);
    }
    if (HasFlag(args, "--stress")) {
        int rounds = 20;
        for (size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == "--stress") {
                const int value = std::atoi(args[i + 1].c_str());
                if (value > 0) {
                    rounds = value;
                }
            }
        }
        RunStressTest(monitor, context, rounds);
    }
    if (HasFlag(args, "--mouse")) {
        RunMouseTest(monitor, context);
    }
    if (HasFlag(args, "--hotkey")) {
        RunStartHotkeyGuardTest(hotkeys, monitor, context);
    }
    if (HasFlag(args, "--cleanup")) {
        RunCleanupTest(monitor, context);
    }

    monitor.Stop();
    hotkeys.Stop();
    hp::LogBlank();
    hp::LogLine(hp::LogLevel::Info, "本次测试：通过 " + std::to_string(g_passed) + " 项，失败 " +
                                        std::to_string(g_failed) + " 项。");
    g_input->ReleaseSimulatedKeys();
    return (g_failed == 0) ? 0 : 1;
}
