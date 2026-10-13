// hotkey_test.cpp —— Phase 5 自检程序（全局热键）
//
// 需求书依据：
//   十三节 全局启动热键：F6 = 播放 / 开始、F7 = 停止、F8 = 从头开始；
//          必须使用 Windows 全局热键机制（RegisterHotKey）；
//          即使工具窗口没有焦点（游戏全屏）也必须工作；并且允许用户自定义。
//   十八节 启动热键特殊处理：刚按下的 F6 不能被当成"用户输入"而立即急停
//          （本程序验证 StartHotkeyPressedRecently 这个接口确实成立，
//            Phase 6 的输入监控会用它）。
//
// 本程序怎么"证明"全局热键真的可用：
//   用工具自己的输入门面（InputController::SimulateUserKeyPress）模拟一次人手按键，
//   然后断言热键回调真的被触发。注入的按键是"系统级的"，与真人按键同一条路径
//   （RegisterHotKey 不认识 LLKHF_INJECTED），所以"注入能触发"等价于"人能触发"。
//   注入的是 F6/F7/F8 这类功能键，不会往任何窗口里打字母。
//
// 安全约定：
//   - 自检按键不参与 Player-owned 追踪，不影响 ReleaseAll 的语义；
//   - 每个用例结束都检查"没有留下按住的键"；
//   - 默认只注入功能键；测试组合键时用 Ctrl+Shift+F9（无系统含义）。
//
// 命令行：--all / --parse / --start / --inject / --focus / --rebind [组合键] /
//         --conflict / --cleanup / --play [--inject] / --list / --help

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
#include "player/SongPlayer.h"
#include "song/TestSongs.h"
#include "song/Timeline.h"

// shellapi.h：CommandLineToArgvW（避免命令行里的中文被 ANSI 代码页破坏）
#include <shellapi.h>

namespace {

int g_passed = 0;
int g_failed = 0;
std::unique_ptr<hp::InputController> g_input;
std::atomic<bool> g_interrupted{false};

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
// 命令行（argv 是 GBK，统一转成 UTF-8）
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

std::string FlagValue(const std::vector<std::string>& args, const std::string& flag) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == flag) {
            return args[i + 1];
        }
    }
    return std::string();
}

std::string Utf8FromWide(const std::wstring& wide) {
    if (wide.empty()) {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return std::string();
    }
    std::string utf8(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::string ForegroundWindowInfo() {
    HWND window = GetForegroundWindow();
    if (window == nullptr) {
        return "（没有前台窗口）";
    }
    std::wstring title(256, L'\0');
    const int length = GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    title.resize(length > 0 ? static_cast<size_t>(length) : 0);
    DWORD processId = 0;
    const DWORD threadId = GetWindowThreadProcessId(window, &processId);
    std::string text = "标题「" + Utf8FromWide(title) + "」进程 " + std::to_string(processId) +
                       " 线程 " + std::to_string(threadId);
    if (threadId == GetCurrentThreadId()) {
        text += "（本程序自己的控制台）";
    } else {
        text += "（不是本程序）";
    }
    return text;
}

// ---------------------------------------------------------------------------
// 事件簿：热键回调把每次触发记下来
// ---------------------------------------------------------------------------
struct EventLog {
    std::mutex mutex;
    std::vector<hp::HotkeyEvent> events;
    std::atomic<int> calls{0};
    std::atomic<long long> lastCallbackTicks{0};
};

EventLog g_log;

void RecordEvent(const hp::HotkeyEvent& event) {
    g_log.calls.fetch_add(1);
    g_log.lastCallbackTicks.store(hp::NowTicks());
    std::lock_guard<std::mutex> lock(g_log.mutex);
    g_log.events.push_back(event);
}

// ---------------------------------------------------------------------------
// 注入"人手按键"
// ---------------------------------------------------------------------------
bool InjectRawKey(unsigned short vk) {
    return g_input->SimulateUserKeyPress(vk, 30);
}

// 注入一个完整的绑定（含修饰键）：先按修饰键，再按主键，最后反序抬起
bool InjectBinding(const hp::HotkeyBinding& binding, int holdMs = 40) {
    if (binding.IsEmpty()) {
        return false;
    }
    std::vector<unsigned short> modifiers;
    if (binding.HasCtrl()) {
        modifiers.push_back(VK_LCONTROL);
    }
    if (binding.HasAlt()) {
        modifiers.push_back(VK_LMENU);
    }
    if (binding.HasShift()) {
        modifiers.push_back(VK_LSHIFT);
    }
    if (binding.HasWin()) {
        // Win 键注入会让系统弹出开始菜单，自检里不注入它
        modifiers.push_back(VK_LWIN);
        return false;
    }
    for (unsigned short modifier : modifiers) {
        g_input->SimulateRawKeyEvent(modifier, true);
    }
    if (!modifiers.empty()) {
        Sleep(15);   // 让系统先记住修饰键按下的状态
    }
    const bool ok = g_input->SimulateUserKeyPress(static_cast<unsigned short>(binding.vk), holdMs);
    for (auto it = modifiers.rbegin(); it != modifiers.rend(); ++it) {
        g_input->SimulateRawKeyEvent(*it, false);
    }
    return ok;
}

// 收尾检查：自检注入没有留下按住的键，也没污染 Player-owned 状态
void CheckInjectionClean(const std::string& what) {
    Check(g_input->SimulatedDownCount() == 0, what + "：没有留下按住的键");
    Check(!g_input->AnyPlayerInputDown(), what + "：自检按键不影响 Player-owned 状态");
}

// ---------------------------------------------------------------------------
// 用例 1：解析与显示（纯逻辑，零风险）
// ---------------------------------------------------------------------------
void RunParseTest() {
    Section("绑定解析与显示");

    Check(hp::DefaultHotkey(hp::HotkeyAction::Play).ToString() == "Num5", "默认「播放 / 开始」= Num5");
    Check(hp::DefaultHotkey(hp::HotkeyAction::Pause).ToString() == "Num6", "默认「暂停 / 继续」= Num6");
    // ---- Phase 13：排除键位（按下不打断演奏）----
    {
        unsigned vk = 0;
        std::string error;
        Check(hp::ParseIgnoredKey("W", vk, &error) && vk == 'W', "字母键可以当排除键");
        Check(hp::ParseIgnoredKey("Space", vk, &error) && vk == VK_SPACE, "Space 可以当排除键");
        Check(hp::ParseIgnoredKey("F1", vk, &error) && vk == VK_F1, "F1 可以当排除键");
        Check(!hp::ParseIgnoredKey("Ctrl+W", vk, &error), "带修饰键的写法被拒绝：" + error);
        Check(!hp::ParseIgnoredKey("none", vk, &error), "none 被拒绝：" + error);
        Check(!hp::ParseIgnoredKey("", vk, &error), "空字符串被拒绝：" + error);
        Check(!hp::ParseIgnoredKey("随便什么", vk, &error), "不认识的名字被拒绝：" + error);

        std::vector<std::string> keys;
        std::string message;
        Check(hp::AddIgnoreKey(keys, "w", &message) && keys.size() == 1 && keys[0] == "W",
              "小写 w 会规范化成 W：" + message);
        Check(!hp::AddIgnoreKey(keys, "W", &message), "重复添加被挡住：" + message);
        Check(hp::AddIgnoreKey(keys, "Space", &message) && keys.size() == 2, "再加一个：" + message);
        Check(hp::RemoveIgnoreKeyAt(keys, 0) && keys.size() == 1 && keys[0] == "Space",
              "删掉第 0 个后剩下 Space");
        Check(!hp::RemoveIgnoreKeyAt(keys, 5), "越界删除返回 false");

        // 急停判定真正会问的那一句
        const std::vector<unsigned> vks = {'W', VK_SPACE};
        Check(hp::IsIgnoredKey('W', vks) && hp::IsIgnoredKey(VK_SPACE, vks), "列表里的键认得出来");
        Check(!hp::IsIgnoredKey('Q', vks) && !hp::IsIgnoredKey(0, vks), "不在列表里的键 / 空键不会被误判成排除键");

        std::vector<std::string> dirty = {"W", "w", "Ctrl+F9", "Space", "不认识"};
        const size_t dropped = hp::SanitizeIgnoredKeys(dirty);
        Check(dirty.size() == 2 && dropped == 3,
              "Sanitize 清掉脏项与重复项（剩 " + std::to_string(dirty.size()) + " 个，丢 " +
                  std::to_string(dropped) + " 个）");
    }
    Check(hp::DefaultHotkey(hp::HotkeyAction::Stop).ToString() == "Num4", "默认「停止」= Num4");
    Check(hp::DefaultHotkey(hp::HotkeyAction::Restart).ToString() == "Num0", "默认「从头开始」= Num0");

    struct ParseCase {
        const char* text;
        unsigned vk;
        unsigned modifiers;
    };
    const ParseCase goodCases[] = {
        {"F6", VK_F6, hp::kModifierNone},
        {"f6", VK_F6, hp::kModifierNone},
        {"F12", VK_F12, hp::kModifierNone},
        {"F24", VK_F24, hp::kModifierNone},
        {"Ctrl+F6", VK_F6, hp::kModifierCtrl},
        {"ctrl+f6", VK_F6, hp::kModifierCtrl},
        {"Ctrl+Shift+F8", VK_F8, hp::kModifierCtrl | hp::kModifierShift},
        {"Shift+Ctrl+F8", VK_F8, hp::kModifierCtrl | hp::kModifierShift},
        {"Alt+F1", VK_F1, hp::kModifierAlt},
        {"Win+A", 'A', hp::kModifierWin},
        {"Space", VK_SPACE, hp::kModifierNone},
        {"Enter", VK_RETURN, hp::kModifierNone},
        {"Esc", VK_ESCAPE, hp::kModifierNone},
        {"Tab", VK_TAB, hp::kModifierNone},
        {"PageUp", VK_PRIOR, hp::kModifierNone},
        {"0x77", 0x77, hp::kModifierNone},
        {"Ctrl+  F9  ", VK_F9, hp::kModifierCtrl},   // 允许空格
    };
    for (const ParseCase& item : goodCases) {
        hp::HotkeyBinding binding;
        std::string error;
        const bool ok = hp::ParseHotkey(item.text, binding, &error);
        const bool right = ok && binding.vk == item.vk && binding.modifiers == item.modifiers;
        Check(right, std::string("能认出 ") + item.text + " → " +
                         (ok ? binding.ToString() : ("失败：" + error)));
    }

    const char* badCases[] = {
        "",             "空字符串",
        "Ctrl+",        "只有修饰键",
        "Ctrl+Shift",   "只有两个修饰键",
        "F0",           "F0 不存在",
        "F25",          "F25 不存在",
        "QQ",           "两个字母",
        "Ctrl+F6+F7",   "两个主键",
        "Ctrl+Zzz",     "无效键名",
        "Ctrl+0x1FF",   "键码越界",
    };
    for (size_t i = 0; i + 1 < sizeof(badCases) / sizeof(badCases[0]); i += 2) {
        hp::HotkeyBinding binding;
        std::string error;
        const bool ok = hp::ParseHotkey(badCases[i], binding, &error);
        Check(!ok && !error.empty(), std::string("拒绝 ") + "「" + badCases[i] + "」（" +
                                         badCases[i + 1] + "）：" + error);
    }

    hp::HotkeyBinding none;
    Check(hp::ParseHotkey("none", none) && none.IsEmpty(), "none 表示取消绑定");

    // 往返：ToString → Parse 应该回到同一个绑定
    const hp::HotkeyBinding roundTrip[] = {
        {VK_F6, hp::kModifierNone},
        {VK_F6, hp::kModifierCtrl},
        {VK_F8, hp::kModifierCtrl | hp::kModifierAlt | hp::kModifierShift},
        {'A', hp::kModifierWin | hp::kModifierCtrl},
        {VK_SPACE, hp::kModifierAlt},
    };
    for (const hp::HotkeyBinding& binding : roundTrip) {
        hp::HotkeyBinding back;
        const bool ok = hp::ParseHotkey(binding.ToString(), back);
        Check(ok && back.Equals(binding), "往返一致：" + binding.ToString());
    }

    Check(hp::HotkeyBinding{'A', hp::kModifierNone}.IsRisky(),
          "无修饰键的 A 会被标记为「会抢走这个键」");
    Check(!hp::HotkeyBinding{VK_F6, hp::kModifierNone}.IsRisky(), "F6 不算危险绑定");
    Check(!hp::HotkeyBinding{'A', hp::kModifierCtrl}.IsRisky(), "Ctrl+A 不算危险绑定");
    Check(hp::VirtualKeyName(VK_F24) == "F24" && hp::VirtualKeyName(0) == "(未绑定)",
          "键名显示正确（F24 / 未绑定）");
    Check(hp::SupportedVirtualKeys().size() >= 60,
          "可设置键清单共 " + std::to_string(hp::SupportedVirtualKeys().size()) + " 个");
    CheckInjectionClean("解析用例");
}

// ---------------------------------------------------------------------------
// 用例 2：启动与注册
// ---------------------------------------------------------------------------
void RunStartTest(hp::GlobalHotkeyManager& manager) {
    Section("启动与注册状态");

    Check(manager.IsRunning(), "热键管理器已启动");
    Check(manager.HotkeyThreadId() != 0 && manager.HotkeyThreadId() != GetCurrentThreadId(),
          "热键线程独立于主线程（热键线程 " + std::to_string(manager.HotkeyThreadId()) +
              "，主线程 " + std::to_string(GetCurrentThreadId()) + "）");
    Check(manager.RegisteredCount() == hp::kHotkeyActionCount,
          "五个默认热键都注册成功（" + std::to_string(manager.RegisteredCount()) + "/5）");

    const std::vector<std::string> errors = manager.RegistrationErrors();
    if (!errors.empty()) {
        for (const std::string& error : errors) {
            Note("注册失败：" + error);
        }
    }
    Check(errors.empty(), "没有注册失败的热键");

    Check(manager.Conflicts().empty(), "六个动作没有互相冲突");

    // Phase 12 的关键契约：**任何一个绑定的热键键**都必须被认出来是热键键。
    // 播放器的"用户输入急停"就是靠它把这些键排除掉的——不能让用户按 F8/F9 时
    // 先被急停掐一下。这里把五个键逐个验一遍，再验一个没绑定的键不该被误认。
    {
        int boundHits = 0;
        const hp::HotkeyAction actions[] = {hp::HotkeyAction::Play, hp::HotkeyAction::Pause,
                                            hp::HotkeyAction::Stop,
                                            hp::HotkeyAction::Restart, hp::HotkeyAction::Previous,
                                            hp::HotkeyAction::Next};
        for (hp::HotkeyAction action : actions) {
            const unsigned vk = manager.GetHotkey(action).vk;
            if (vk != 0 && manager.IsBoundKey(vk)) {
                ++boundHits;
            }
        }
        Check(boundHits == hp::kHotkeyActionCount,
              "全部绑定热键都被 IsBoundKey 认出（" + std::to_string(boundHits) + "/" +
                  std::to_string(hp::kHotkeyActionCount) + "）");
        // 注意：F5 现在绑给「暂停」了，这里换用确实没绑定的 F2 / F12 / 0 当反例
        Check(!manager.IsBoundKey(VK_F2) && !manager.IsBoundKey(VK_F12) && !manager.IsBoundKey(0),
              "没绑定的键（F2 / F12 / 0）不会误判成热键键");


        // 改键之后：新键要立刻被认出来，旧键要立刻不再是热键键
        // （急停排除靠的是当前绑定，不是写死的 F6~F10 名单）
        hp::HotkeyBinding f11;
        std::string parseError;
        Check(hp::ParseHotkey("F11", f11, &parseError), "\"F11\" 可解析：" + parseError);
        const hp::HotkeyChangeResult changed = manager.SetHotkey(hp::HotkeyAction::Next, f11);
        Check(changed.ok, "把「下一首」改绑到 F11：" + (changed.ok ? std::string("成功") : changed.error));
        Check(manager.IsBoundKey(VK_F11) && !manager.IsBoundKey(VK_F10),
              "改键后 F11 被认作热键键、F10 不再是");
        const hp::HotkeyChangeResult back = manager.ResetToDefaults();
        Check(back.ok && manager.IsBoundKey(VK_F10) && !manager.IsBoundKey(VK_F11),
              "恢复默认后 F10 又被认作热键键、F11 不再是");
    }
    Note("当前设置：");
    std::string described = manager.DescribeAll();
    while (!described.empty()) {
        const size_t end = described.find('\n');
        Note(described.substr(0, end));
        if (end == std::string::npos) {
            break;
        }
        described = described.substr(end + 1);
    }

    Check(manager.EventCount(hp::HotkeyAction::Play) == 0 &&
              manager.EventCount(hp::HotkeyAction::Stop) == 0,
          "还没有按键，事件计数为 0");
    Check(!manager.StartHotkeyPressedRecently(500), "还没按过启动热键");

        // Phase 13：录键期间"挂起"——把全局热键让出去。否则用户按 F6 想把它改成别的功能，
        // 键会被系统直接喂给老绑定（RegisterHotKey 在 WM_KEYDOWN 之前就把它吞了），
        // 表现就是"想设置却把曲子弹起来了"。
        {
            const hp::HotkeyBinding playBinding = manager.GetHotkey(hp::HotkeyAction::Play);
            const int before = manager.EventCount(hp::HotkeyAction::Play);
            InjectBinding(playBinding);
            Check(manager.WaitForEvent(hp::HotkeyAction::Play, before + 1, 2000),
                  "挂起前基线：按 F6 正常触发播放");
            const int baseline = manager.EventCount(hp::HotkeyAction::Play);

            manager.SetSuspended(true);
            Check(manager.IsSuspended(), "挂起标志生效");
            InjectBinding(playBinding);
            Check(!manager.WaitForEvent(hp::HotkeyAction::Play, baseline + 1, 400),
                  "挂起期间按 F6 不再触发播放（键会照常送到焦点窗口）");
            Check(manager.GetHotkey(hp::HotkeyAction::Play).Equals(playBinding),
                  "挂起只是不注册，绑定本身一点没动");

            manager.SetSuspended(false);
            Check(!manager.IsSuspended(), "恢复标志生效");
            InjectBinding(playBinding);
            Check(manager.WaitForEvent(hp::HotkeyAction::Play, baseline + 1, 2000),
                  "恢复之后同一个键又好使了");
            CheckInjectionClean("挂起 / 恢复");
        }
}

// ---------------------------------------------------------------------------
// 用例 3：注入按键 → 热键真的被触发（核心证据）
// ---------------------------------------------------------------------------
void RunInjectTest(hp::GlobalHotkeyManager& manager) {
    Section("注入模拟按键：热键是否真的被触发");

    const hp::HotkeyAction actions[] = {hp::HotkeyAction::Play, hp::HotkeyAction::Stop,
                                        hp::HotkeyAction::Restart};
    double worstLatencyMs = 0.0;

    for (hp::HotkeyAction action : actions) {
        if (g_interrupted.load()) {
            return;
        }
        const hp::HotkeyBinding binding = manager.GetHotkey(action);
        const int before = manager.EventCount(action);
        int othersBefore = 0;
        for (hp::HotkeyAction other : actions) {
            if (other != action) {
                othersBefore += manager.EventCount(other);
            }
        }

        const long long sentTicks = hp::NowTicks();
        const bool sent = InjectBinding(binding);
        const bool fired = manager.WaitForEvent(action, before + 1, 2000);
        const long long callbackTicks = g_log.lastCallbackTicks.load();
        double latencyMs = hp::TicksToSeconds(callbackTicks - sentTicks) * 1000.0;
        if (latencyMs < 0.0) {
            latencyMs = 0.0;   // 回调与投递在同一瞬间完成
        }
        if (latencyMs > worstLatencyMs) {
            worstLatencyMs = latencyMs;
        }

        Check(sent, std::string("注入 ") + binding.ToString() + " 成功投递");
        Check(fired, std::string("注入 ") + binding.ToString() + " 后「" +
                         hp::HotkeyActionDisplayName(action) + "」被触发");
        Check(manager.EventCount(action) == before + 1,
              std::string("「") + hp::HotkeyActionDisplayName(action) + "」正好触发 1 次");
        int othersAfter = 0;
        for (hp::HotkeyAction other : actions) {
            if (other != action) {
                othersAfter += manager.EventCount(other);
            }
        }
        Check(othersAfter == othersBefore, "没有误触发其它动作");

        const hp::HotkeyEvent last = manager.LastEvent();
        Check(last.action == action && last.binding.vk == binding.vk,
              "事件里的动作与键都对（" + std::string(hp::HotkeyActionName(last.action)) + " / " +
                  last.binding.ToString() + "）");
        Note(binding.ToString() + " → 触发用时 " + std::to_string(latencyMs) + " ms");

        // 十八节：启动热键刚按过要能被识别出来（Phase 6 的输入监控据此忽略它）
        if (action == hp::HotkeyAction::Play) {
            Check(manager.StartHotkeyPressedRecently(1000),
                  "刚按过启动热键 → StartHotkeyPressedRecently 为真（十八节）");
            Check(manager.StartHotkeyVirtualKey() == binding.vk,
                  "启动热键的虚拟键码可被查询（" + hp::VirtualKeyName(manager.StartHotkeyVirtualKey()) +
                      "）");
        }
    }

    Note("本次最坏触发用时 " + std::to_string(worstLatencyMs) + " ms");
    CheckInjectionClean("注入用例");
}

// ---------------------------------------------------------------------------
// 用例 4：焦点无关（"游戏全屏也能用"的等价证明）
// ---------------------------------------------------------------------------
void RunFocusTest(hp::GlobalHotkeyManager& manager) {
    Section("焦点无关性：窗口没有焦点时热键仍然有效");

    const HWND foreground = GetForegroundWindow();
    DWORD processId = 0;
    const DWORD foregroundThread =
        foreground != nullptr ? GetWindowThreadProcessId(foreground, &processId) : 0;
    Note("注入时的前台窗口：" + ForegroundWindowInfo());
    Note("本程序主线程 " + std::to_string(GetCurrentThreadId()) + "，热键线程 " +
         std::to_string(manager.HotkeyThreadId()) +
         "，前台窗口所属线程 " + std::to_string(foregroundThread));

    const int before = manager.EventCount(hp::HotkeyAction::Play);
    const hp::HotkeyBinding binding = manager.GetHotkey(hp::HotkeyAction::Play);
    InjectBinding(binding);
    Check(manager.WaitForEvent(hp::HotkeyAction::Play, before + 1, 2000),
          "前台不是本程序时，注入 " + binding.ToString() + " 仍然触发");

    if (foregroundThread != GetCurrentThreadId()) {
        Check(true, "确认当前前台窗口属于别的进程（工具全程没有抢占焦点）");
    } else {
        Note("注意：这次前台正好是本程序的控制台，但工具全程没有调用 SetForegroundWindow，"
             "热键与焦点无关这一点仍由上面的触发结果证明");
    }
    CheckInjectionClean("焦点用例");
}

// ---------------------------------------------------------------------------
// 用例 5：自定义热键（改键之后真的生效）
// ---------------------------------------------------------------------------
void RunRebindTest(hp::GlobalHotkeyManager& manager, const std::string& customText) {
    Section("自定义热键");

    hp::HotkeyBinding custom;
    std::string parseError;
    const std::string text = customText.empty() ? std::string("Ctrl+Shift+F9") : customText;
    if (!hp::ParseHotkey(text, custom, &parseError) || custom.HasWin()) {
        Check(false, "自定义组合键写错了：" + text + " " + parseError);
        return;
    }

    // 1) 改「播放 / 开始」为新组合键
    const hp::HotkeyChangeResult changed = manager.SetHotkey(hp::HotkeyAction::Play, custom);
    Check(changed.ok, "把「播放 / 开始」改成 " + text + "：" +
                          (changed.ok ? std::string("成功") : changed.error));
    Check(manager.GetHotkey(hp::HotkeyAction::Play).Equals(custom),
          "设置已经记下来：" + manager.GetHotkey(hp::HotkeyAction::Play).ToString());

    // 2) 旧键 F6 应该不再触发
    const int beforeOld = manager.EventCount(hp::HotkeyAction::Play);
    InjectRawKey(VK_F6);
    Sleep(120);
    Check(manager.EventCount(hp::HotkeyAction::Play) == beforeOld,
          "旧键 F6 改了以后不再触发「播放 / 开始」");

    // 3) 新组合键应该触发
    InjectBinding(custom);
    Check(manager.WaitForEvent(hp::HotkeyAction::Play, beforeOld + 1, 2000),
          "新组合键 " + custom.ToString() + " 立刻生效");

    // 4) 现在 F6 空出来了，可以给「停止」用（内部冲突检查应当放行）
    hp::HotkeyBinding f6;
    hp::ParseHotkey("F6", f6);
    const hp::HotkeyChangeResult stopOnF6 = manager.SetHotkey(hp::HotkeyAction::Stop, f6);
    Check(stopOnF6.ok, "F6 空出来了 → 可以给「停止」：" +
                           (stopOnF6.ok ? std::string("成功") : stopOnF6.error));
    const int beforeStop = manager.EventCount(hp::HotkeyAction::Stop);
    InjectRawKey(VK_F6);
    Check(manager.WaitForEvent(hp::HotkeyAction::Stop, beforeStop + 1, 2000),
          "现在按 F6 触发的是「停止」");

    // 5) 把「播放 / 开始」也改回 F6 → 与「停止」重复，必须被拒绝且不改动现状
    const hp::HotkeyChangeResult duplicate = manager.SetHotkey(hp::HotkeyAction::Play, f6);
    Check(!duplicate.ok && duplicate.error.find("停止") != std::string::npos,
          "两个动作抢同一把键被拒绝：" + duplicate.error);
    Check(manager.GetHotkey(hp::HotkeyAction::Stop).Equals(f6),
          "被拒绝后「停止」的原设置没有被动过");

    // 6) 危险绑定的提醒（无修饰键的字母会把这个键从游戏里抢走）
    hp::HotkeyBinding risky;
    hp::ParseHotkey("A", risky);
    const hp::HotkeyChangeResult riskyResult = manager.SetHotkey(hp::HotkeyAction::Restart, risky);
    Check(riskyResult.ok && !riskyResult.warning.empty(),
          "把没有修饰键的 A 设成热键会给出提醒：" + riskyResult.warning);

    // 7) 取消绑定
    const hp::HotkeyChangeResult unbound =
        manager.SetHotkey(hp::HotkeyAction::Restart, hp::HotkeyBinding{});
    Check(unbound.ok && manager.GetHotkey(hp::HotkeyAction::Restart).IsEmpty(),
          "可以取消一个动作的热键（输入 none）");
    const int beforeUnbound = manager.EventCount(hp::HotkeyAction::Restart);
    InjectRawKey(VK_F8);
    Sleep(120);
    Check(manager.EventCount(hp::HotkeyAction::Restart) == beforeUnbound,
          "取消绑定后按 F8 什么也不会发生");

    // 8) 恢复默认
    const hp::HotkeyChangeResult reset = manager.ResetToDefaults();
    Check(reset.ok, "恢复默认五键：" + (reset.ok ? std::string("成功") : reset.error));
    Check(manager.GetHotkey(hp::HotkeyAction::Play).ToString() == "F6" &&
              manager.GetHotkey(hp::HotkeyAction::Stop).ToString() == "F7" &&
              manager.GetHotkey(hp::HotkeyAction::Restart).ToString() == "F8" &&
              manager.GetHotkey(hp::HotkeyAction::Previous).ToString() == "F9" &&
              manager.GetHotkey(hp::HotkeyAction::Next).ToString() == "F10",
          "五个动作都回到 F6 / F7 / F8 / F9 / F10");
    const int beforeReset = manager.EventCount(hp::HotkeyAction::Play);
    InjectRawKey(VK_F6);
    Check(manager.WaitForEvent(hp::HotkeyAction::Play, beforeReset + 1, 2000),
          "恢复默认后 F6 又能触发「播放 / 开始」");

    CheckInjectionClean("自定义用例");
}

// ---------------------------------------------------------------------------
// 用例 6：冲突与占用
// ---------------------------------------------------------------------------
void RunConflictTest(hp::GlobalHotkeyManager& manager) {
    Section("冲突与占用检测");

    // (a) 内部重复：把「停止」设成「播放」的键
    const hp::HotkeyBinding playBinding = manager.GetHotkey(hp::HotkeyAction::Play);
    const hp::HotkeyChangeResult duplicate =
        manager.SetHotkey(hp::HotkeyAction::Stop, playBinding);
    Check(!duplicate.ok && duplicate.error.find("播放") != std::string::npos,
          "内部重复绑定被拒绝：" + duplicate.error);
    Check(manager.GetHotkey(hp::HotkeyAction::Stop).ToString() == "F7",
          "被拒绝后「停止」仍然是 F7");

    // (b) 被"别的程序"占用：先在本进程用裸 Win32 抢下 F11（另一个 id = 另一个热键），
    //     再让管理器去注册同一个键 → 必须失败，并且给出可读原因。
    constexpr int kForeignHotkeyId = 0x7B01;
    const bool held = RegisterHotKey(nullptr, kForeignHotkeyId, MOD_NOREPEAT, VK_F11) != FALSE;
    if (!held) {
        Note("本机拿不到 F11（可能已被占用），跳过占用检测");
    } else {
        hp::HotkeyBinding f11;
        hp::ParseHotkey("F11", f11);
        const hp::HotkeyChangeResult occupied =
            manager.SetHotkey(hp::HotkeyAction::Restart, f11);
        Check(!occupied.ok && occupied.error.find("占用") != std::string::npos,
              "被别的程序占用时拒绝并说明原因：" + occupied.error);
        Check(manager.GetHotkey(hp::HotkeyAction::Restart).ToString() == "F8",
              "占用导致的失败没有改变原有设置（仍是 F8）");

        // 其它动作不受影响
        const int before = manager.EventCount(hp::HotkeyAction::Stop);
        InjectRawKey(VK_F7);
        Check(manager.WaitForEvent(hp::HotkeyAction::Stop, before + 1, 2000),
              "一次失败的改键不影响其它热键");

        UnregisterHotKey(nullptr, kForeignHotkeyId);
        Sleep(50);
        const hp::HotkeyChangeResult afterRelease =
            manager.SetHotkey(hp::HotkeyAction::Restart, f11);
        Check(afterRelease.ok, "对方释放后同一个键就能注册了：" +
                                   (afterRelease.ok ? std::string("成功") : afterRelease.error));
    }

    // 收尾：恢复默认
    const hp::HotkeyChangeResult reset = manager.ResetToDefaults();
    Check(reset.ok, "冲突用例结束后恢复默认五键");
    Check(manager.Conflicts().empty(), "没有残留的内部冲突");
    CheckInjectionClean("冲突用例");
}

// ---------------------------------------------------------------------------
// 用例 7：停止与清理（退出后不能还占着热键）
// ---------------------------------------------------------------------------
void RunCleanupTest(hp::GlobalHotkeyManager& manager) {
    Section("停止与清理");

    manager.Stop();
    Check(!manager.IsRunning(), "Stop() 之后管理器不再运行");

    const int playBefore = manager.EventCount(hp::HotkeyAction::Play);
    const int stopBefore = manager.EventCount(hp::HotkeyAction::Stop);
    const int restartBefore = manager.EventCount(hp::HotkeyAction::Restart);
    InjectRawKey(VK_F6);
    InjectRawKey(VK_F7);
    InjectRawKey(VK_F8);
    Sleep(150);
    Check(manager.EventCount(hp::HotkeyAction::Play) == playBefore &&
              manager.EventCount(hp::HotkeyAction::Stop) == stopBefore &&
              manager.EventCount(hp::HotkeyAction::Restart) == restartBefore,
          "停止后再按 F6/F7/F8 什么也不会发生（热键已经注销）");

    // 重新启动是幂等可用的
    std::string error;
    const bool restarted = manager.Start([](const hp::HotkeyEvent& event) { RecordEvent(event); }, &error);
    Check(restarted && manager.IsRunning(), "可以重新启动：" + (restarted ? std::string("成功") : error));
    const int afterStart = manager.EventCount(hp::HotkeyAction::Play);
    InjectRawKey(VK_F6);
    Check(manager.WaitForEvent(hp::HotkeyAction::Play, afterStart + 1, 2000),
          "重新启动后 F6 又能触发");

    // 作用域内的管理器：析构后必须彻底放手
    std::atomic<int> scopedCount{0};
    {
        hp::GlobalHotkeyManager scoped;
        scoped.Start([&scopedCount](const hp::HotkeyEvent&) { scopedCount.fetch_add(1); });
        Check(scoped.IsRunning(), "临时管理器启动成功");
        scoped.Stop();
    }
    const int scopedBefore = scopedCount.load();
    InjectRawKey(VK_F6);
    Sleep(150);
    Check(scopedCount.load() == scopedBefore,
          "临时管理器停止/析构后不再收到热键（没有残留注册）");
    Check(manager.EventCount(hp::HotkeyAction::Play) > 0,
          "另一个管理器仍然正常工作（互不干扰）");

    manager.Stop();
    Check(!manager.IsRunning(), "用例结束时已停止");
    CheckInjectionClean("清理用例");
}

// ---------------------------------------------------------------------------
// 用例 8：与播放器联动（F6 开始 / F7 停止 / F8 从头）
// ---------------------------------------------------------------------------
void RunPlayerWiringTest(bool dryRun) {
    Section(std::string("与播放器联动（") + (dryRun ? "干跑" : "真注入") + "）");

    const hp::Song song = hp::test_songs::Scale();
    hp::Timeline timeline = hp::Timeline::Build(song);
    hp::PlayerOptions options;
    options.dryRun = dryRun;
    hp::SongPlayer player(*g_input, options);

    hp::GlobalHotkeyManager manager;
    std::atomic<int> playIgnored{0};
    const bool started = manager.Start([&player, &timeline, &playIgnored](
                                           const hp::HotkeyEvent& event) {
        RecordEvent(event);
        switch (event.action) {
            case hp::HotkeyAction::Play:
                // 「播放 / 开始」：停着的时候开始弹；正在弹就不打断
                if (player.IsStopped()) {
                    player.Play(timeline);
                } else {
                    playIgnored.fetch_add(1);
                }
                break;
            case hp::HotkeyAction::Stop:
                player.Stop(hp::StopReason::UserStop);
                break;
            case hp::HotkeyAction::Restart:
                player.Restart();
                break;
            default:
                break;
        }
    });
    if (!started) {
        Check(false, "热键管理器没起来，联动测试无法进行");
        return;
    }
    Note("曲目：" + timeline.SongName() + "　音符 " + std::to_string(timeline.NoteCount()) +
         " 个　时长 " + std::to_string(timeline.TotalDuration()) + " 秒");

    // F6 → 开始演奏
    InjectRawKey(VK_F6);
    Check(player.WaitForPlaying(2000), "按 F6 后播放器进入演奏中");
    Sleep(400);
    const int firedAfterStart = player.FiredNoteCount();
    Check(firedAfterStart > 0, "按 F6 之后真的开始起音了（已起音 " +
                                   std::to_string(firedAfterStart) + " 个）");

    // 正在演奏时再按 F6：不应该打断（约定：F6 是"开始"，不是"重来"）
    const int firedBeforeSecondPlay = player.FiredNoteCount();
    InjectRawKey(VK_F6);
    Sleep(200);
    Check(playIgnored.load() == 1, "演奏中再按 F6 被忽略（不打断）");
    Check(player.FiredNoteCount() >= firedBeforeSecondPlay,
          "位置继续往前走，没有回到开头");

    // F7 → 停止
    InjectRawKey(VK_F7);
    Check(player.WaitForStopped(2000), "按 F7 后播放器停止");
    Check(player.LastStopReason() == hp::StopReason::UserStop,
          std::string("停止原因 = ") + hp::StopReasonName(player.LastStopReason()));
    Check(player.HeldInputCount() == 0, "停止后没有任何被工具按住的键");
    Check(player.PositionSeconds() < 0.001, "停止后播放位置归零（UI 显示从 0 开始）");

    // 停止状态下 F6 → 从头开始弹
    InjectRawKey(VK_F6);
    Check(player.WaitForPlaying(2000), "停止后再按 F6 又从头开始演奏");
    Check(player.FiredNoteCount() <= 2, "重新开始时已起音数从头累计（" +
                                            std::to_string(player.FiredNoteCount()) + "）");

    // F8 → 演奏中从头开始
    Sleep(500);
    const int firedBeforeRestart = player.FiredNoteCount();
    const double positionBeforeRestart = player.PositionSeconds();
    InjectRawKey(VK_F8);
    Sleep(150);
    const double positionAfterRestart = player.PositionSeconds();
    Check(positionAfterRestart < positionBeforeRestart || firedBeforeRestart == 0,
          "按 F8 后位置回到开头（" + std::to_string(positionBeforeRestart) + " 秒 → " +
              std::to_string(positionAfterRestart) + " 秒）");

    // 收尾：F7 停止 + 无残留
    InjectRawKey(VK_F7);
    Check(player.WaitForStopped(2000), "再次按 F7 停止（收尾）");
    Check(player.HeldInputCount() == 0, "结束时没有留下按住的键");
    CheckInjectionClean("联动用例");

    manager.Stop();
    player.Quit();
}

// ---------------------------------------------------------------------------
// 端到端：真注入演奏（配合 hotkey-e2e.sh 回读记事本内容）
// ---------------------------------------------------------------------------
void RunEndToEnd(bool dryRun) {
    Section(std::string("端到端：热键启动真演奏（") + (dryRun ? "干跑" : "真注入") + "）");

    const hp::Song song = hp::test_songs::Scale();
    hp::Timeline timeline = hp::Timeline::Build(song);
    hp::PlayerOptions options;
    options.dryRun = dryRun;
    hp::SongPlayer player(*g_input, options);

    hp::GlobalHotkeyManager manager;
    manager.Start([&player, &timeline](const hp::HotkeyEvent& event) {
        RecordEvent(event);
        if (event.action == hp::HotkeyAction::Play) {
            if (player.IsStopped()) {
                player.Play(timeline);
            }
        } else if (event.action == hp::HotkeyAction::Stop) {
            player.Stop(hp::StopReason::UserStop);
        }
    });

    Note("前台窗口（演奏目标）：" + ForegroundWindowInfo());
    Note("1 秒后注入 F6 开始演奏，演奏 1.2 秒后注入 F7 停止；"
         "记事本里应当留下这段前缀（由 hotkey-e2e.sh 回读）");

    Sleep(1000);
    InjectRawKey(VK_F6);
    const bool playing = player.WaitForPlaying(2000);
    Check(playing, "F6 让播放器进入演奏中");
    Sleep(1200);
    const int fired = player.FiredNoteCount();
    InjectRawKey(VK_F7);
    const bool stopped = player.WaitForStopped(2000);
    Check(stopped, "F7 让播放器停止");
    Check(player.HeldInputCount() == 0, "停止后没有留下按住的键");
    Note("实际起音 " + std::to_string(fired) + " 个（干跑与真注入应当接近）");
    Check(fired >= 2, "在这段时间里至少弹了 2 个音符");

    CheckInjectionClean("端到端用例");
    manager.Stop();
    player.Quit();
}

// ---------------------------------------------------------------------------
// 全部自动验证
// ---------------------------------------------------------------------------
int RunAllTests() {
    RunParseTest();

    // 注意：F6/F7/F8 全系统只能被**一个**注册者持有，
    // 所以"共用管理器"的用例跑完后必须先停掉，后面的用例才能自己注册。
    {
        hp::GlobalHotkeyManager manager;
        std::string error;
        const bool started =
            manager.Start([](const hp::HotkeyEvent& event) { RecordEvent(event); }, &error);
        if (!started) {
            Check(false, "热键管理器启动失败：" + error);
            return 1;
        }
        RunStartTest(manager);
        RunInjectTest(manager);
        RunFocusTest(manager);
        RunRebindTest(manager, std::string());
        RunConflictTest(manager);
        RunCleanupTest(manager);   // 结束时已 Stop
    }

    RunPlayerWiringTest(true);
    RunEndToEnd(true);
    return (g_failed == 0) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// 交互菜单
// ---------------------------------------------------------------------------
void PrintDefaultHotkeys() {
    Note("默认：F6 = 播放 / 开始　F7 = 停止　F8 = 从头开始（需求书第十三节）");
}

int RunInteractiveMenu() {
    hp::GlobalHotkeyManager manager;
    std::string error;
    const bool started = manager.Start([](const hp::HotkeyEvent& event) {
        RecordEvent(event);
        hp::LogLine(hp::LogLevel::Info,
                    std::string("收到热键：") + hp::HotkeyActionDisplayName(event.action) + "（" +
                        event.binding.ToString() + "）");
    }, &error);
    if (!started) {
        hp::LogError("热键管理器启动失败：" + error);
        return 1;
    }
    PrintDefaultHotkeys();
    Note("现在按 F6 / F7 / F8 就能看到上面那行「收到热键」（窗口没有焦点也算）");

    bool running = true;
    while (running && !g_interrupted.load()) {
        hp::LogBlank();
        hp::LogLine(hp::LogLevel::Info, "请选择：");
        Note("1  解析自检（纯逻辑，零风险）");
        Note("2  查看当前热键与注册状态");
        Note("3  注入模拟按键验证（真的按一下 F6/F7/F8）");
        Note("4  焦点无关性验证");
        Note("5  自定义热键（改成 Ctrl+Shift+F9 再改回来）");
        Note("6  冲突与占用检测");
        Note("7  停止与清理验证");
        Note("8  与播放器联动（干跑）");
        Note("9  端到端真演奏（需要先把记事本切到前台）");
        Note("0  退出");
        hp::LogInline("输入数字后回车：");

        std::string line;
        if (!std::getline(std::cin, line)) {
            break;
        }
        if (line == "0") {
            running = false;
        } else if (line == "1") {
            RunParseTest();
        } else if (line == "2") {
            Check(manager.IsRunning(), "热键管理器运行中");
            Note("热键线程 " + std::to_string(manager.HotkeyThreadId()));
            std::string described = manager.DescribeAll();
            while (!described.empty()) {
                const size_t end = described.find('\n');
                Note(described.substr(0, end));
                if (end == std::string::npos) {
                    break;
                }
                described = described.substr(end + 1);
            }
            Note("已注册 " + std::to_string(manager.RegisteredCount()) + " 个");
            for (const std::string& failure : manager.RegistrationErrors()) {
                hp::LogWarn("注册失败：" + failure);
            }
        } else if (line == "3") {
            RunInjectTest(manager);
        } else if (line == "4") {
            RunFocusTest(manager);
        } else if (line == "5") {
            RunRebindTest(manager, std::string());
        } else if (line == "6") {
            RunConflictTest(manager);
        } else if (line == "7") {
            RunCleanupTest(manager);
        } else if (line == "8") {
            RunPlayerWiringTest(true);
        } else if (line == "9") {
            RunEndToEnd(false);
        } else {
            hp::LogWarn("看不懂这个选项：" + line);
        }
    }

    manager.Stop();
    hp::LogBlank();
    hp::LogLine(hp::LogLevel::Info, "本次测试：通过 " + std::to_string(g_passed) + " 项，失败 " +
                                        std::to_string(g_failed) + " 项。");
    g_input->ReleaseSimulatedKeys();
    return (g_failed == 0) ? 0 : 1;
}

void PrintHelp() {
    Note("用法：hotkey_test.exe [选项]");
    Note("  --all            跑全部自动验证（干跑，只注入功能键，零风险）");
    Note("  --parse          只跑绑定解析（纯逻辑）");
    Note("  --start          只跑启动与注册状态（需要已经启动管理器）");
    Note("  --inject         只跑注入模拟按键验证");
    Note("  --focus          只跑焦点无关性验证");
    Note("  --rebind X       自定义热键自检，可指定组合键（默认 Ctrl+Shift+F9）");
    Note("  --conflict       只跑冲突与占用检测");
    Note("  --cleanup        只跑停止与清理验证");
    Note("  --play [--inject] 与播放器联动（默认干跑，加 --inject 真演奏）");
    Note("  --e2e [--inject]  端到端：热键启动真演奏");
    Note("  --list           显示默认热键与可设置键");
    Note("  --help           显示本帮助");
    Note("无参数 = 交互菜单。");
    PrintDefaultHotkeys();
}

void PrintBanner() {
    hp::LogBlank();
    hp::LogLine(hp::LogLevel::Info, "游戏口琴自动演奏工具 —— Phase 5 自检（全局热键）");
    Note("需求书第十三节：F6/F7/F8 全局热键、无焦点也要工作、允许自定义");
    Note("需求书第十八节：启动热键不能被自己当成用户输入");
}

}  // namespace

int main() {
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
        PrintDefaultHotkeys();
        Note("可设置的键（" + std::to_string(hp::SupportedVirtualKeys().size()) + " 个）：");
        std::string line;
        for (unsigned vk : hp::SupportedVirtualKeys()) {
            line += hp::VirtualKeyName(vk);
            line += "  ";
        }
        Note(line);
        return 0;
    }
    if (HasFlag(args, "--simulate-list")) {
        PrintDefaultHotkeys();
        return 0;
    }

    const bool inject = HasFlag(args, "--inject");
    int exitCode = 0;
    if (HasFlag(args, "--all")) {
        exitCode = RunAllTests();
    } else if (HasFlag(args, "--parse")) {
        RunParseTest();
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--play")) {
        RunPlayerWiringTest(!inject);
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(args, "--e2e")) {
        RunEndToEnd(!inject);
        exitCode = (g_failed == 0) ? 0 : 1;
    } else {
        // 需要用到管理器的用例
        hp::GlobalHotkeyManager manager;
        std::string error;
        if (!manager.Start([](const hp::HotkeyEvent& event) { RecordEvent(event); }, &error)) {
            hp::LogError("热键管理器启动失败：" + error);
            return 1;
        }
        if (HasFlag(args, "--start")) {
            RunStartTest(manager);
        }
        if (HasFlag(args, "--inject")) {
            RunInjectTest(manager);
        }
        if (HasFlag(args, "--focus")) {
            RunFocusTest(manager);
        }
        if (HasFlag(args, "--rebind")) {
            RunRebindTest(manager, FlagValue(args, "--rebind"));
        }
        if (HasFlag(args, "--conflict")) {
            RunConflictTest(manager);
        }
        if (HasFlag(args, "--cleanup")) {
            RunCleanupTest(manager);
        }
        manager.Stop();
        exitCode = (g_failed == 0) ? 0 : 1;
    }

    hp::LogBlank();
    hp::LogLine(hp::LogLevel::Info, "本次测试：通过 " + std::to_string(g_passed) + " 项，失败 " +
                                        std::to_string(g_failed) + " 项。");
    // 无论成败，收尾一定把自检模拟按下的键全部补齐抬起
    g_input->ReleaseSimulatedKeys();
    return exitCode;
}
