// =============================================================================
//  Input Test —— Harmonica Player / Phase 1 的测试程序
//
//  作用：在不启动游戏的前提下，验证“工具能不能正确地向系统投递键鼠输入”，以及
//        “工具有没有在任何情况下留下被按住的键”。
//
//  它只做两件事：
//    1) 通过 InputController 发送输入（和将来播放器走的是同一条路径）；
//    2) 用 GetAsyncKeyState 从系统视角回读状态，做自动断言。
//
//  两种用法：
//    - 双击运行：出现编号菜单，选 2 跑自动测试（推荐第一次就这么做）
//    - 带参数运行（便于脚本化）：见 --help
//
//  平台：Windows 10/11 x64，C++17，无第三方依赖。
// =============================================================================

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "core/Log.h"
#include "core/Platform.h"
#include "core/Time.h"
#include "core/Types.h"
#include "input/InputController.h"

namespace {

using hp::InputController;
using hp::InputSendOptions;
using hp::Key;
using hp::KeyboardController;
using hp::MouseButton;
using hp::MouseController;
using hp::PlayMode;

// -----------------------------------------------------------------------------
// 全局单例指针：给控制台中断处理器用（Ctrl+C / 关窗口时也要释放按键）
// -----------------------------------------------------------------------------
std::unique_ptr<InputController> g_input;

int g_passed = 0;
int g_failed = 0;

// -----------------------------------------------------------------------------
// 小工具
// -----------------------------------------------------------------------------
std::chrono::milliseconds Ms(int value) {
    return std::chrono::milliseconds(value);
}

void WaitMs(int value) {
    std::this_thread::sleep_for(Ms(value));
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

std::string Fmt(const char* format, double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), format, value);
    return std::string(buffer);
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
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

void Countdown(int seconds) {
    for (int remaining = seconds; remaining > 0; --remaining) {
        hp::LogInline("\r  " + std::to_string(remaining) + " 秒后开始（现在把焦点切到目标窗口）…  ");
        WaitMs(1000);
    }
    hp::LogInline("\r" + std::string(52, ' ') + "\r");
}

// -----------------------------------------------------------------------------
// 绕过 InputController 的“裸注入”，只在测试程序里用。
// 用途：模拟“用户自己按住的键”（例如 Shift），用来验证 ReleaseAll 不会误释放它。
// 生产代码里绝不允许绕过 InputController 发输入。
// -----------------------------------------------------------------------------
bool InjectRawKeyEvent(unsigned short vk, bool keyDown) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    if (!keyDown) {
        input.ki.dwFlags = KEYEVENTF_KEYUP;
    }
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

// -----------------------------------------------------------------------------
// 环境自检
// -----------------------------------------------------------------------------
struct OsVersionInfo {
    DWORD major = 0;
    DWORD minor = 0;
    DWORD build = 0;
    bool  valid = false;
};

// 自己声明一次，避免依赖 winternl.h（windows.h 不带这个结构）
typedef struct _HP_OSVERSIONINFOW {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
} HP_OSVERSIONINFOW;

OsVersionInfo QueryOsVersion() {
    OsVersionInfo info;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return info;
    }
    using RtlGetVersionFn = LONG(WINAPI*)(HP_OSVERSIONINFOW*);
    auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(ntdll, "RtlGetVersion"));   // 不用 GetVersionEx：它在 Win8.1+ 会撒谎
    if (rtlGetVersion == nullptr) {
        return info;
    }
    HP_OSVERSIONINFOW raw{};
    raw.dwOSVersionInfoSize = sizeof(raw);
    if (rtlGetVersion(&raw) == 0) {
        info.major = raw.dwMajorVersion;
        info.minor = raw.dwMinorVersion;
        info.build = raw.dwBuildNumber;
        info.valid = true;
    }
    return info;
}

// 返回是否提权；known = 是否成功取到结论（受保护进程可能取不到）
bool IsProcessElevated(DWORD pid, bool& known) {
    known = false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return false;
    }
    bool elevated = false;
    HANDLE token = nullptr;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation{};
        DWORD size = sizeof(elevation);
        // 注意：第二个参数是 TOKEN_INFORMATION_CLASS 枚举值 TokenElevation，
        // 不是 TOKEN_ELEVATION 这个结构体名 —— 写成后者会报 C2275。
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
            elevated = elevation.TokenIsElevated != 0;
            known = true;
        }
        CloseHandle(token);
    }
    CloseHandle(process);
    return elevated;
}

std::string CurrentProcessPath() {
    wchar_t buffer[MAX_PATH * 2];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0) {
        return "(未知)";
    }
    return WideToUtf8(std::wstring(buffer, length));
}

// 当前前台窗口的描述，例如 “记事本（普通权限）”。
// 注入类的测试都会先打印它 —— 输入只会进入前台窗口，打出来才能确认目标对不对。
std::string ForegroundWindowDescription(bool* elevatedOut = nullptr) {
    if (elevatedOut != nullptr) {
        *elevatedOut = false;
    }
    HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return "(没有前台窗口)";
    }

    std::wstring wide;
    const int length = GetWindowTextLengthW(foreground);
    if (length > 0) {
        wide.resize(static_cast<size_t>(length) + 1);
        const int copied = GetWindowTextW(foreground, wide.data(), length + 1);
        wide.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    }
    std::string title = wide.empty() ? std::string("(无标题窗口)") : WideToUtf8(wide);

    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    if (pid == 0) {
        return title;
    }
    bool known = false;
    const bool elevated = IsProcessElevated(pid, known);
    if (elevatedOut != nullptr) {
        *elevatedOut = elevated;
    }
    if (!known) {
        return title;
    }
    return title + (elevated ? "（以管理员权限运行）" : "（普通权限）");
}

void PrintEnvironment() {
    hp::LogBlank();
    hp::LogInfo("================ 环境自检 ================");
    hp::LogPlain(std::string("  程序路径    ：") + CurrentProcessPath());

    const OsVersionInfo os = QueryOsVersion();
    if (os.valid) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "Windows %lu.%lu.%lu (%s)", os.major, os.minor,
                      os.build, (sizeof(void*) == 8 ? "x64" : "x86"));
        hp::LogPlain(std::string("  操作系统    ：") + buffer);
    } else {
        hp::LogPlain("  操作系统    ：未能读取版本号");
    }

    bool selfKnown = false;
    const bool selfElevated = IsProcessElevated(GetCurrentProcessId(), selfKnown);
    hp::LogPlain(std::string("  本工具权限  ：") +
                 (!selfKnown ? "未知（未取到令牌信息）"
                             : (selfElevated ? "管理员（已提权）" : "普通用户（未提权）")));

    char layoutName[KL_NAMELENGTH] = {};
    if (GetKeyboardLayoutNameA(layoutName)) {
        hp::LogPlain(std::string("  键盘布局号  ：") + layoutName);
    }

    const InputSendOptions options = g_input->Keyboard().Options();
    hp::LogPlain(std::string("  发送方式    ：") +
                 (options.useScancode ? "扫描码（KEYEVENTF_SCANCODE，游戏兼容性更好）"
                                      : "虚拟键码（wVk，普通窗口程序更稳）"));

    // 前台窗口：如果它以管理员权限运行，而本工具没有提权，SendInput 会被 UIPI 丢弃
    bool foregroundElevated = false;
    const std::string foregroundDescription = ForegroundWindowDescription(&foregroundElevated);
    hp::LogPlain(std::string("  前台窗口    ：") + foregroundDescription);

    hp::LogBlank();
    if (foregroundElevated && !selfElevated) {
        hp::LogError("结论：前台程序是管理员权限，而本工具不是 —— SendInput 会被系统 UIPI 拦截。");
        hp::LogError("      请右键 input_test.exe → 以管理员身份运行，再重试。");
    } else if (!selfElevated) {
        hp::LogWarn("提示：本工具当前不是管理员权限。如果游戏以管理员身份运行，注入会被拦截；");
        hp::LogWarn("      届时请用“以管理员身份运行”启动本工具。");
    } else {
        hp::LogOk("权限检查通过：本工具已提权，可以向前台任意程序投递输入。");
    }
    hp::LogPlain("  说明：Windows 的输入注入采用“用户界面特权隔离”（UIPI）规则——");
    hp::LogPlain("        低权限进程不能向高权限窗口发送输入，这是系统安全设计，不是 bug。");
}

void PrintKeyTable() {
    hp::LogBlank();
    hp::LogInfo("================ 按键映射表 ================");
    hp::LogPlain("  序号  音调     按键   虚拟键码   扫描码   需扩展前缀");
    hp::LogPlain("  ----  -------  ----   --------   ------   ----------");
    for (int i = 0; i < hp::kKeyCount; ++i) {
        const Key key = static_cast<Key>(i);
        const unsigned short vk = hp::VirtualKeyOf(key);
        const UINT scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        char line[160];
        std::snprintf(line, sizeof(line), "  %-4d  %-7s  %-4s   0x%02X       0x%02X     %s", i + 1,
                      hp::KeyDisplayName(key), hp::KeyName(key), vk, scanCode,
                      hp::IsExtendedVirtualKey(vk) ? "是" : "否");
        hp::LogPlain(line);
    }
    hp::LogBlank();
    hp::LogPlain("  演奏方式（与上面的音调组合成 8 x 6 = 48 种操作）：");
    for (int i = 0; i < hp::kPlayModeCount; ++i) {
        const PlayMode mode = static_cast<PlayMode>(i);
        MouseButton buttons[hp::kMaxMouseButtonsPerMode] = {};
        const int count = hp::MouseButtonsOf(mode, buttons);
        std::string how;
        if (count <= 0) {
            how = "直接按键";
        } else {
            how = "按住";
            for (int b = 0; b < count; ++b) {
                if (b > 0) {
                    how += " + ";
                }
                how += hp::MouseButtonDisplayName(buttons[b]);
            }
            how += "再按键";
            if (count > 1) {
                how += "（两个键要同时按住）";
            }
        }
        hp::LogPlain(std::string("    ") + hp::PlayModeDisplayName(mode) + "（" + hp::PlayModeName(mode) +
                     "）：" + how);
    }
}

// -----------------------------------------------------------------------------
// 1. 自动测试
// -----------------------------------------------------------------------------
void RunAutoTest() {
    hp::LogBlank();
    hp::LogInfo("================ 自动测试开始 ================");
    hp::LogPlain("  本测试完全自动执行，会真实地向系统投递按键与鼠标事件。");
    hp::LogPlain("  如果此刻你的光标停在某个窗口上，可能会看到几次点击效果，属正常现象。");
    hp::LogBlank();
    hp::LogInfo(std::string("注入前的前台窗口：") + ForegroundWindowDescription());

    KeyboardController& keyboard = g_input->Keyboard();
    MouseController& mouse = g_input->Mouse();

    // ---- 1/6 8 个音调 ----
    hp::LogBlank();
    hp::LogInfo("--- 1/6  8 个音调：按下 → 系统可见 → 释放 ---");
    for (int i = 0; i < hp::kKeyCount; ++i) {
        const Key key = static_cast<Key>(i);
        const bool pressed = g_input->PressKey(key);
        WaitMs(25);
        const bool physicallyDown = KeyboardController::IsKeyPhysicallyDown(key);
        const bool tracked = keyboard.IsPlayerKeyDown(key);
        Check(pressed && physicallyDown && tracked,
              std::string("按下 ") + hp::KeyDisplayName(key) + "（工具已记录 + 系统可见）");

        const bool released = g_input->ReleaseKey(key);
        WaitMs(15);
        const bool physicallyUp = !KeyboardController::IsKeyPhysicallyDown(key);
        Check(released && physicallyUp && !keyboard.IsPlayerKeyDown(key),
              std::string("释放 ") + hp::KeyDisplayName(key) + "（系统可见已抬起）");
    }

    // ---- 2/6 状态机 ----
    hp::LogBlank();
    hp::LogInfo("--- 2/6  状态机：不重复按下、不误发抬起 ---");
    const bool firstPress = g_input->PressKey(Key::Z);
    const bool secondPress = g_input->PressKey(Key::Z);
    Check(firstPress && !secondPress, "重复按下 Z：第二次被拒绝，不重复下发给游戏");
    Check(keyboard.PlayerDownCount() == 1, "重复按下后，工具持有的按键数量仍为 1");

    const bool strayRelease = g_input->ReleaseKey(Key::X);
    Check(!strayRelease && !KeyboardController::IsKeyPhysicallyDown(Key::X),
          "释放一个没按下的 X：返回 false，且不发出任何抬起事件");

    g_input->ReleaseAll();
    WaitMs(15);
    Check(keyboard.PlayerDownCount() == 0 && !KeyboardController::IsKeyPhysicallyDown(Key::Z),
          "ReleaseAll 之后 Z 已释放，工具状态清空");

    // ---- 3/6 鼠标三键 ----
    hp::LogBlank();
    hp::LogInfo("--- 3/6  鼠标左键 / 中键 / 右键 ---");
    for (int i = 0; i < hp::kMouseButtonCount; ++i) {
        const MouseButton button = static_cast<MouseButton>(i);
        const bool pressed = g_input->PressMouse(button);
        WaitMs(20);
        Check(pressed && MouseController::IsMousePhysicallyDown(button) && mouse.IsPlayerDown(button),
              std::string("按下 ") + hp::MouseButtonDisplayName(button));

        const bool released = g_input->ReleaseMouse(button);
        WaitMs(15);
        Check(released && !MouseController::IsMousePhysicallyDown(button) && !mouse.IsPlayerDown(button),
              std::string("释放 ") + hp::MouseButtonDisplayName(button));
    }

    // ---- 4/6 用户自己按住的键必须保留 ----
    hp::LogBlank();
    hp::LogInfo("--- 4/6  用户键保护：模拟用户按住 Shift，工具停止后不得误释放 ---");
    const bool injectedShift = InjectRawKeyEvent(VK_LSHIFT, true);
    WaitMs(20);
    Check(injectedShift && KeyboardController::IsVirtualKeyPhysicallyDown(VK_LSHIFT),
          "已模拟“用户自己按住 Shift”");

    g_input->PressKey(Key::Z);
    g_input->PressMouse(MouseButton::Left);
    WaitMs(30);
    hp::LogPlain(std::string("  释放前工具持有的输入：") + g_input->PlayerStateSummary());
    g_input->ReleaseAll();
    WaitMs(20);

    Check(KeyboardController::IsVirtualKeyPhysicallyDown(VK_LSHIFT),
          "ReleaseAll 之后 Shift 仍然按着（用户自己的输入状态没有被误释放）");
    Check(!KeyboardController::IsKeyPhysicallyDown(Key::Z) &&
              !MouseController::IsMousePhysicallyDown(MouseButton::Left),
          "ReleaseAll 正确释放了工具自己的 Z 与鼠标左键");

    InjectRawKeyEvent(VK_LSHIFT, false);
    WaitMs(20);
    Check(!KeyboardController::IsVirtualKeyPhysicallyDown(VK_LSHIFT),
          "测试用的 Shift 已清理，环境干净");

    // ---- 5/6 六种演奏模式（含两个“八度 + 半音”组合）----
    hp::LogBlank();
    hp::LogInfo("--- 5/6  六种演奏方式（Normal / Left / Middle / Right / LeftMiddle / RightMiddle + Z） ---");
    for (int i = 0; i < hp::kPlayModeCount; ++i) {
        const PlayMode mode = static_cast<PlayMode>(i);
        MouseButton buttons[hp::kMaxMouseButtonsPerMode] = {};
        const int buttonCount = hp::MouseButtonsOf(mode, buttons);

        const bool modeDown = g_input->PressMode(mode);
        const bool keyDown = g_input->PressKey(Key::Z);
        WaitMs(40);

        bool ok = keyDown && KeyboardController::IsKeyPhysicallyDown(Key::Z);
        std::string what = std::string("模式 ") + hp::PlayModeDisplayName(mode) + "：";
        if (buttonCount == 0) {
            ok = ok && modeDown && mouse.PlayerDownCount() == 0;
            what += "只按下 Z，未触碰鼠标";
        } else {
            // 组合模式：要求的鼠标键必须**同时**处于按下状态，而且都被记为 Player-owned
            what += "鼠标键（";
            for (int b = 0; b < buttonCount; ++b) {
                ok = ok && MouseController::IsMousePhysicallyDown(buttons[b]) &&
                     mouse.IsPlayerDown(buttons[b]);
                if (b > 0) {
                    what += " + ";
                }
                what += hp::MouseButtonDisplayName(buttons[b]);
            }
            ok = ok && mouse.PlayerDownCount() == buttonCount;
            what += "）与 Z 同时处于按下状态";
        }
        Check(ok, what);

        g_input->ReleaseKey(Key::Z);
        g_input->ReleaseMode(mode);
        WaitMs(30);
        Check(!g_input->AnyPlayerInputDown(),
              std::string("模式 ") + hp::PlayModeDisplayName(mode) + " 结束后无任何残留输入");
    }

    // ---- 5b/6 组合模式专项：两个鼠标键必须同时按下、结束后两个都抬起 ----
    hp::LogBlank();
    hp::LogInfo("--- 5b/6  组合模式专项：左+中 / 右+中（八度 + 半音） ---");
    {
        struct ComboCase {
            PlayMode mode;
            MouseButton octave;   // 八度键
        };
        const ComboCase combos[] = {
            {PlayMode::LeftMiddle, MouseButton::Left},
            {PlayMode::RightMiddle, MouseButton::Right},
        };
        for (const ComboCase& item : combos) {
            const bool modeDown = g_input->PressMode(item.mode);
            WaitMs(40);
            // 系统视角回读：两个键真的同时按着（不是只按了一个）
            const bool octaveDown = MouseController::IsMousePhysicallyDown(item.octave);
            const bool middleDown = MouseController::IsMousePhysicallyDown(MouseButton::Middle);
            Check(modeDown && octaveDown && middleDown && mouse.PlayerDownCount() == 2,
                  std::string("组合 ") + hp::PlayModeName(item.mode) + "：" +
                      hp::MouseButtonDisplayName(item.octave) + " 与 鼠标中键 同时处于按下状态");

            const bool modeUp = g_input->ReleaseMode(item.mode);
            WaitMs(30);
            const bool bothUp = !MouseController::IsMousePhysicallyDown(item.octave) &&
                                !MouseController::IsMousePhysicallyDown(MouseButton::Middle);
            Check(modeUp && bothUp && !mouse.IsPlayerDown(item.octave) &&
                      !mouse.IsPlayerDown(MouseButton::Middle) && mouse.PlayerDownCount() == 0,
                  std::string("组合 ") + hp::PlayModeName(item.mode) + "：两个鼠标键都已抬起，没有残留");
        }
    }

    // ---- 6/6 收尾 ----
    hp::LogBlank();
    hp::LogInfo("--- 6/6  收尾检查：系统视角下没有卡住的键 ---");
    Check(!g_input->AnyPlayerInputDown(), "工具内部状态：没有 Player-owned 输入残留");
    bool anyStuck = false;
    for (int i = 0; i < hp::kKeyCount; ++i) {
        if (KeyboardController::IsKeyPhysicallyDown(static_cast<Key>(i))) {
            anyStuck = true;
        }
    }
    for (int i = 0; i < hp::kMouseButtonCount; ++i) {
        if (MouseController::IsMousePhysicallyDown(static_cast<MouseButton>(i))) {
            anyStuck = true;
        }
    }
    Check(!anyStuck, "系统视角：8 个音调键与 3 个鼠标键全部处于抬起状态");

    hp::LogBlank();
    hp::LogInfo("自动测试结束：通过 " + std::to_string(g_passed) + " 项，失败 " +
                std::to_string(g_failed) + " 项");
    if (g_failed == 0) {
        hp::LogOk("全部通过。可以进入“文本输入测试”（菜单 3）做人工确认。");
    } else {
        hp::LogError("存在失败项，请把上面的失败行原文反馈给我。");
    }
}

// -----------------------------------------------------------------------------
// 2. 文本输入测试（人工确认）
// -----------------------------------------------------------------------------
void RunKeySequenceTest(bool useCountdown, int holdMs, int gapMs) {
    hp::LogBlank();
    hp::LogInfo("================ 文本输入测试 ================");
    hp::LogPlain("  请先把光标点到能看到字符的地方（记事本、浏览器地址栏、游戏聊天框）。");
    hp::LogPlain("  程序会依次按下并释放 Z X C V B N M , ，正常应看到这 8 个字符依次出现。");
    if (useCountdown) {
        Countdown(5);
    }
    hp::LogInfo(std::string("注入前的前台窗口：") + ForegroundWindowDescription());
    hp::LogPlain("  （键盘输入只会进入前台窗口；如果这里显示的不是目标窗口，请先切过去再重跑）");

    int sendFailureCount = 0;
    unsigned long lastSendError = 0;

    for (int i = 0; i < hp::kKeyCount; ++i) {
        const Key key = static_cast<Key>(i);
        const double start = hp::NowSeconds();

        g_input->PressKey(key);
        WaitMs(holdMs);
        const bool visible = KeyboardController::IsKeyPhysicallyDown(key);
        g_input->ReleaseKey(key);

        if (g_input->Keyboard().LastSendFailed()) {
            ++sendFailureCount;
            lastSendError = g_input->Keyboard().LastSendError();
        }

        const double elapsedMs = (hp::NowSeconds() - start) * 1000.0;
        hp::LogInfo(std::to_string(i + 1) + "/8  发送 " + hp::KeyDisplayName(key) + "  按住 " +
                    Fmt("%.1f", elapsedMs) + " ms  系统可见=" + (visible ? "是" : "否") +
                    "  注入失败=" +
                    (sendFailureCount > 0 && lastSendError != 0
                         ? ("是（错误码 " + std::to_string(lastSendError) + "）")
                         : "否"));
        WaitMs(gapMs);
    }

    g_input->ReleaseAll();
    hp::LogBlank();
    if (sendFailureCount > 0 && lastSendError == ERROR_ACCESS_DENIED) {
        hp::LogError("SendInput 被系统拒绝（错误码 5 = 拒绝访问）。");
        hp::LogError("原因：目标窗口的进程权限比本工具高，Windows 的 UIPI 会丢弃低权限进程发来的输入。");
        hp::LogError("处理：用“以管理员身份运行”启动本工具（游戏若以管理员运行，这是必须的）；");
        hp::LogError("      或者先注入到一个权限相同的普通窗口（例如本控制台、记事本）验证。");
        hp::LogBlank();
    }
    hp::LogPlain("  请确认：目标窗口里是否依次出现了  Z X C V B N M ,  这 8 个字符？");
    hp::LogPlain("  如果 8 个字符都出现了 → 输入通道正常，Phase 1 的核心目标达成。");
    hp::LogPlain("  如果某个字符没出现 → 记下是哪一个，并告诉我（这通常意味着该键被别的程序占用了）。");
}

// -----------------------------------------------------------------------------
// 3. 六种演奏方式测试（人工确认）
// -----------------------------------------------------------------------------
void RunPlayModeTest(bool useCountdown, int holdMs) {
    hp::LogBlank();
    hp::LogInfo("================ 六种演奏方式测试 ================");
    hp::LogPlain("  每种方式会按住 300 毫秒，程序会实时回读系统状态并打印。");
    hp::LogPlain("  组合模式（左+中 / 右+中）要同时按住两个鼠标键，两行都应显示“是”。");
    if (useCountdown) {
        Countdown(5);
    }
    hp::LogInfo(std::string("注入前的前台窗口：") + ForegroundWindowDescription());

    for (int i = 0; i < hp::kPlayModeCount; ++i) {
        const PlayMode mode = static_cast<PlayMode>(i);
        MouseButton buttons[hp::kMaxMouseButtonsPerMode] = {};
        const int buttonCount = hp::MouseButtonsOf(mode, buttons);

        g_input->PressMode(mode);
        g_input->PressKey(Key::Z);
        WaitMs(60);   // 等系统状态稳定后再回读

        const bool keyDown = KeyboardController::IsKeyPhysicallyDown(Key::Z);
        bool allMouseDown = true;
        for (int b = 0; b < buttonCount; ++b) {
            allMouseDown = allMouseDown && MouseController::IsMousePhysicallyDown(buttons[b]);
        }
        WaitMs(holdMs - 60 > 0 ? holdMs - 60 : 0);

        g_input->ReleaseKey(Key::Z);
        g_input->ReleaseMode(mode);
        WaitMs(10);
        bool allMouseUp = true;
        for (int b = 0; b < buttonCount; ++b) {
            allMouseUp = allMouseUp && !MouseController::IsMousePhysicallyDown(buttons[b]);
        }
        const bool clean = !g_input->AnyPlayerInputDown() && allMouseUp;

        std::string line = "  " + hp::ComboName(mode, Key::Z) + "  ";
        line += "键盘按下=" + std::string(keyDown ? "是" : "否");
        if (buttonCount > 0) {
            line += "  ";
            for (int b = 0; b < buttonCount; ++b) {
                if (b > 0) {
                    line += " + ";
                }
                line += hp::MouseButtonDisplayName(buttons[b]);
            }
            line += std::string("按下=") + (allMouseDown ? "是" : "否");
        } else {
            line += "  未使用鼠标（正常）";
        }
        line += std::string("  释放后无残留=") + (clean ? "是" : "否");
        hp::LogInfo(line);
    }
    hp::LogBlank();
    hp::LogPlain("  六种方式都应为“是 / 是 / 是”。任一为“否”请把这一行原文反馈给我。");
}

// -----------------------------------------------------------------------------
// 4. 实时按键监视（验证真实按键被正确读取 + 没有卡键）
// -----------------------------------------------------------------------------
void RunMonitor() {
    hp::LogBlank();
    hp::LogInfo("================ 实时按键监视 ================");
    hp::LogPlain("  本模式不做任何注入，显示的就是你自己按的键。");
    hp::LogPlain("  请依次按一下 Z X C V B N M ,  再看键盘上的逗号键，以及鼠标左/中/右键。");
    hp::LogPlain("  程序会打印每一次“按下 / 抬起”；按 Esc 结束。");
    hp::LogPlain("  如果某个键打印了“按下”却没有打印“抬起”，说明它此刻是卡住的。");
    hp::LogBlank();

    bool previousKey[hp::kKeyCount] = {};
    bool previousMouse[hp::kMouseButtonCount] = {};
    bool previousEscape = false;

    // 先报告一次初始化状态，若一开始就有键是按下状态，说明有残留
    bool initialStuck = false;
    for (int i = 0; i < hp::kKeyCount; ++i) {
        const bool down = KeyboardController::IsKeyPhysicallyDown(static_cast<Key>(i));
        previousKey[i] = down;
        if (down) {
            initialStuck = true;
            hp::LogWarn(std::string("进入监视时 ") + hp::KeyDisplayName(static_cast<Key>(i)) + " 已是按下状态");
        }
    }
    for (int i = 0; i < hp::kMouseButtonCount; ++i) {
        const bool down = MouseController::IsMousePhysicallyDown(static_cast<MouseButton>(i));
        previousMouse[i] = down;
        if (down) {
            initialStuck = true;
            hp::LogWarn(std::string("进入监视时 ") +
                        hp::MouseButtonDisplayName(static_cast<MouseButton>(i)) + " 已是按下状态");
        }
    }
    if (!initialStuck) {
        hp::LogOk("初始状态干净：8 个音调键与 3 个鼠标键都是抬起的。");
    }
    previousEscape = KeyboardController::IsVirtualKeyPhysicallyDown(VK_ESCAPE);

    for (;;) {
        for (int i = 0; i < hp::kKeyCount; ++i) {
            const Key key = static_cast<Key>(i);
            const bool down = KeyboardController::IsKeyPhysicallyDown(key);
            if (down != previousKey[i]) {
                previousKey[i] = down;
                if (down) {
                    hp::LogInfo(std::string("按下  ") + hp::KeyDisplayName(key));
                } else {
                    hp::LogInfo(std::string("抬起  ") + hp::KeyDisplayName(key));
                }
            }
        }
        for (int i = 0; i < hp::kMouseButtonCount; ++i) {
            const MouseButton button = static_cast<MouseButton>(i);
            const bool down = MouseController::IsMousePhysicallyDown(button);
            if (down != previousMouse[i]) {
                previousMouse[i] = down;
                hp::LogInfo(std::string(down ? "按下  " : "抬起  ") + hp::MouseButtonDisplayName(button));
            }
        }

        const bool escape = KeyboardController::IsVirtualKeyPhysicallyDown(VK_ESCAPE);
        if (escape && !previousEscape) {
            hp::LogInfo("按下  Esc —— 结束监视");
            break;
        }
        previousEscape = escape;

        WaitMs(15);
    }

    // 收尾：确认没有卡键
    bool stuck = false;
    for (int i = 0; i < hp::kKeyCount; ++i) {
        if (KeyboardController::IsKeyPhysicallyDown(static_cast<Key>(i))) {
            stuck = true;
            hp::LogWarn(std::string("结束监视时 ") + hp::KeyDisplayName(static_cast<Key>(i)) + " 仍处于按下状态");
        }
    }
    for (int i = 0; i < hp::kMouseButtonCount; ++i) {
        if (MouseController::IsMousePhysicallyDown(static_cast<MouseButton>(i))) {
            stuck = true;
            hp::LogWarn(std::string("结束监视时 ") +
                        hp::MouseButtonDisplayName(static_cast<MouseButton>(i)) + " 仍处于按下状态");
        }
    }
    if (!stuck) {
        hp::LogOk("结束监视：没有卡住的按键。");
    }
}

// -----------------------------------------------------------------------------
// 5. 不注入按键的逻辑自检（--logic / 菜单 7）
//    组合模式（左+中 / 右+中）在真按键之前，先在这里把“名字 ↔ 枚举 ↔ 鼠标键集合”
//    全部钉死：这一路**不向系统投递任何输入**，所以随时可以跑、也不用担心打扰别人。
// -----------------------------------------------------------------------------
void RunLogicSelfTest() {
    hp::LogBlank();
    hp::LogInfo("================ 逻辑自检（不注入任何按键） ================");
    hp::LogPlain("  真按键的那一路在菜单 2 与菜单 4（会真的按下鼠标键）；这里只查换算表。");

    // ---- 1) 6 种演奏方式：名字、显示名、互不相同 ----
    hp::LogBlank();
    hp::LogInfo("--- 1/4  6 种演奏方式的名字 ---");
    Check(hp::kPlayModeCount == 6, "演奏方式一共 6 种（kPlayModeCount = 6）");
    Check(hp::kMouseButtonCount == 3, "鼠标键仍是 3 个（ReleaseAll 遍历全部 3 个，组合模式必被覆盖）");
    bool namesOk = true;
    for (int i = 0; i < hp::kPlayModeCount; ++i) {
        const PlayMode mode = static_cast<PlayMode>(i);
        const std::string name = hp::PlayModeName(mode);
        const std::string display = hp::PlayModeDisplayName(mode);
        bool good = !name.empty() && name != "?" && !display.empty() && display != "?";
        for (int j = 0; j < i && good; ++j) {
            if (name == hp::PlayModeName(static_cast<PlayMode>(j)) ||
                display == hp::PlayModeDisplayName(static_cast<PlayMode>(j))) {
                good = false;
            }
        }
        if (!good) {
            hp::LogError("      第 " + std::to_string(i) + " 种的名字有问题：" + name + " / " + display);
        }
        namesOk = namesOk && good;
        hp::LogPlain("      " + std::to_string(i + 1) + "  " + display + "（" + name + "）");
    }
    Check(namesOk, "6 个名字与显示名都能取到且互不相同");

    // ---- 2) 别名解析（英文、缩写、数字、中文）----
    hp::LogBlank();
    hp::LogInfo("--- 2/4  别名解析 ---");
    struct ModeAlias {
        const char* text;
        PlayMode mode;
    };
    const ModeAlias aliases[] = {
        {"Normal", PlayMode::Normal},       {"normal", PlayMode::Normal},
        {"N", PlayMode::Normal},            {"1", PlayMode::Normal},
        {"正常", PlayMode::Normal},         {"Left", PlayMode::Left},
        {"L", PlayMode::Left},              {"2", PlayMode::Left},
        {"左键", PlayMode::Left},           {"Middle", PlayMode::Middle},
        {"MID", PlayMode::Middle},          {"3", PlayMode::Middle},
        {"中键", PlayMode::Middle},         {"Right", PlayMode::Right},
        {"R", PlayMode::Right},             {"4", PlayMode::Right},
        {"右键", PlayMode::Right},          {"LeftMiddle", PlayMode::LeftMiddle},
        {"leftmiddle", PlayMode::LeftMiddle}, {"LM", PlayMode::LeftMiddle},
        {"lm", PlayMode::LeftMiddle},       {"5", PlayMode::LeftMiddle},
        {"左中", PlayMode::LeftMiddle},     {"中左", PlayMode::LeftMiddle},
        {"左键+中键", PlayMode::LeftMiddle}, {"RightMiddle", PlayMode::RightMiddle},
        {"rightmiddle", PlayMode::RightMiddle}, {"RM", PlayMode::RightMiddle},
        {"rm", PlayMode::RightMiddle},      {"6", PlayMode::RightMiddle},
        {"右中", PlayMode::RightMiddle},    {"中右", PlayMode::RightMiddle},
        {"右键+中键", PlayMode::RightMiddle},
    };
    const int aliasTotal = static_cast<int>(sizeof(aliases) / sizeof(aliases[0]));
    int aliasOk = 0;
    for (const ModeAlias& item : aliases) {
        PlayMode parsed = PlayMode::Normal;
        if (hp::ParsePlayMode(item.text, parsed) && parsed == item.mode) {
            ++aliasOk;
        } else {
            hp::LogError(std::string("      没认出来：") + item.text + " → " +
                         hp::PlayModeName(parsed));
        }
    }
    Check(aliasOk == aliasTotal, "全部 " + std::to_string(aliasTotal) + " 种写法都解析对（认对 " +
                                    std::to_string(aliasOk) + " 个）");

    // ---- 3) 不认识的要拒绝（单字母 M 仍然是“音调 M”，不是中键）----
    hp::LogBlank();
    hp::LogInfo("--- 3/4  不该认的写法 ---");
    PlayMode parsed = PlayMode::Normal;
    Check(!hp::ParsePlayMode("M", parsed) && !hp::ParsePlayMode("Up", parsed) &&
              !hp::ParsePlayMode("", parsed) && !hp::ParsePlayMode("7", parsed) &&
              !hp::ParsePlayMode("0", parsed),
          "单字母 M / 未知名字 / 空串 / 数字 0 与 7 都不被当成演奏方式（M 仍是音调 M）");

    // ---- 4) 每种模式要按的鼠标键集合 ----
    hp::LogBlank();
    hp::LogInfo("--- 4/4  每种演奏方式要按的鼠标键 ---");
    struct ButtonCase {
        PlayMode mode;
        int count;              // 要按住几个鼠标键
        MouseButton first;      // 先按下的
        MouseButton second;     // 再按下的（count == 2 时才有意义）
    };
    const ButtonCase cases[] = {
        {PlayMode::Normal, 0, MouseButton::Left, MouseButton::Left},
        {PlayMode::Left, 1, MouseButton::Left, MouseButton::Left},
        {PlayMode::Middle, 1, MouseButton::Middle, MouseButton::Middle},
        {PlayMode::Right, 1, MouseButton::Right, MouseButton::Right},
        {PlayMode::LeftMiddle, 2, MouseButton::Left, MouseButton::Middle},
        {PlayMode::RightMiddle, 2, MouseButton::Right, MouseButton::Middle},
    };
    const int caseTotal = static_cast<int>(sizeof(cases) / sizeof(cases[0]));
    int caseOk = 0;
    for (const ButtonCase& item : cases) {
        MouseButton buttons[hp::kMaxMouseButtonsPerMode] = {};
        const int count = hp::MouseButtonsOf(item.mode, buttons);
        bool good = (count == item.count) && (hp::PlayModeUsesMouse(item.mode) == (item.count > 0));
        if (count >= 1) {
            good = good && buttons[0] == item.first;
        }
        if (count >= 2) {
            good = good && buttons[1] == item.second && buttons[1] != buttons[0];
        }
        // 单键版接口（老调用点用的那个）必须与之一致：Normal 返回 false，其余返回第一个键
        MouseButton single = MouseButton::Left;
        const bool singleOk = hp::MouseButtonOf(item.mode, single);
        good = good && (singleOk == (item.count > 0)) && (!singleOk || single == item.first);

        std::string text = std::string("      ") + hp::PlayModeName(item.mode) + " → " +
                           std::to_string(count) + " 个鼠标键";
        for (int b = 0; b < count; ++b) {
            text += (b == 0 ? "：" : " + ");
            text += hp::MouseButtonDisplayName(buttons[b]);
        }
        if (count == 0) {
            text += "（直接按键）";
        }
        hp::LogPlain(text);
        if (!good) {
            hp::LogError("      这一条不对（期望 " + std::to_string(item.count) + " 个键）");
        }
        caseOk += good ? 1 : 0;
    }
    Check(caseOk == caseTotal,
          "6 种模式的鼠标键集合都对（组合模式 = 两个键，左+中 / 右+中）");

    // 环境干净：跑完逻辑自检不该有任何 Player-owned 输入
    Check(!g_input->AnyPlayerInputDown(), "逻辑自检结束时没有任何 Player-owned 输入残留");

    hp::LogBlank();
    hp::LogInfo("逻辑自检结束：通过 " + std::to_string(g_passed) + " 项，失败 " +
                std::to_string(g_failed) + " 项");
}

// -----------------------------------------------------------------------------
// 菜单
// -----------------------------------------------------------------------------
void PrintBanner(const std::string& sendMode) {
    hp::LogBlank();
    hp::LogPlain("  ============================================================");
    hp::LogPlain("    Harmonica Player  —  Input Test  （Phase 1：输入系统）");
    hp::LogPlain("    游戏口琴自动演奏工具 · 输入通道自检工具");
    hp::LogPlain("  ============================================================");
    hp::LogPlain(std::string("    当前发送方式：") + sendMode);
    hp::LogBlank();
}

void PrintMenu() {
    hp::LogBlank();
    hp::LogPlain("  ------------------------------------------------------------");
    hp::LogPlain("   1  环境自检       是否提权、前台窗口、注入会不会被系统拦截");
    hp::LogPlain("   2  自动测试       程序自己验证 8 键 + 3 鼠标键 + 状态机 + 用户键保护");
    hp::LogPlain("   3  文本输入测试   切到记事本，程序敲出 Z X C V B N M , （人工确认）");
    hp::LogPlain("   4  演奏方式测试   Normal / Left / Middle / Right / LeftMiddle / RightMiddle + Z");
    hp::LogPlain("   5  实时按键监视   按真实按键，验证读取正确且没有卡键");
    hp::LogPlain("   6  按键映射表     8 个音调的虚拟键码与扫描码");
    hp::LogPlain("   7  逻辑自检       6 种演奏方式的名字/别名/鼠标键集合（不注入任何按键）");
    hp::LogPlain("   0  退出           退出前会自动释放所有工具按住的输入");
    hp::LogPlain("  ------------------------------------------------------------");
}

int RunInteractiveMenu() {
    for (;;) {
        PrintMenu();
        hp::LogInline("  请选择 [0-7]：");
        std::string choice;
        if (!std::getline(std::cin, choice)) {
            break;   // 输入被关闭（管道结束）
        }
        const std::string value = Trim(choice);

        if (value == "0" || value == "q" || value == "Q") {
            break;
        } else if (value == "1") {
            PrintEnvironment();
            PauseForEnter();
        } else if (value == "2") {
            RunAutoTest();
            PauseForEnter();
        } else if (value == "3") {
            RunKeySequenceTest(/*useCountdown=*/true, 120, 200);
            PauseForEnter();
        } else if (value == "4") {
            RunPlayModeTest(/*useCountdown=*/true, 300);
            PauseForEnter();
        } else if (value == "5") {
            RunMonitor();
            PauseForEnter();
        } else if (value == "6") {
            PrintKeyTable();
            PauseForEnter();
        } else if (value == "7") {
            RunLogicSelfTest();
            PauseForEnter();
        } else if (!value.empty()) {
            hp::LogWarn("没有这个选项：" + value);
        }
    }
    return 0;
}

// -----------------------------------------------------------------------------
// 控制台中断处理：Ctrl+C / 关闭窗口 也要把工具按住的键放开
// -----------------------------------------------------------------------------
BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
    if (g_input) {
        hp::LogWarn("收到中断信号，正在释放工具按住的输入…");
        g_input->ReleaseAll();
    }
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        return TRUE;   // 自己处理完，不再转给默认处理器
    }
    return FALSE;
}

// -----------------------------------------------------------------------------
// 命令行参数
// -----------------------------------------------------------------------------
bool HasFlag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            return true;
        }
    }
    return false;
}

int IntOption(int argc, char** argv, const char* name, int fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return std::atoi(argv[i + 1]);
        }
    }
    return fallback;
}

void PrintHelp() {
    hp::LogPlain("  Harmonica Player — Input Test（Phase 1）");
    hp::LogBlank();
    hp::LogPlain("  用法：input_test.exe [命令] [选项]");
    hp::LogBlank();
    hp::LogPlain("  不带任何参数          打开交互菜单（推荐）");
    hp::LogBlank();
    hp::LogPlain("  命令：");
    hp::LogPlain("    --env               环境自检");
    hp::LogPlain("    --list              打印按键映射表");
    hp::LogPlain("    --autotest          自动测试（不需要人工操作，返回码 0=全通过）");
    hp::LogPlain("    --keys              文本输入测试（8 个字符依次敲出，人工确认）");
    hp::LogPlain("    --modes             六种演奏方式测试（会真按鼠标键，人工确认）");
    hp::LogPlain("    --logic             逻辑自检（6 种演奏方式的名字/别名/鼠标键集合，不注入）");
    hp::LogPlain("    --monitor           实时按键监视（按 Esc 结束）");
    hp::LogPlain("    --full              = 环境自检 + 自动测试 + 文本输入测试");
    hp::LogPlain("    --help              本帮助");
    hp::LogBlank();
    hp::LogPlain("  选项：");
    hp::LogPlain("    --no-countdown      跳过倒计时（默认倒计时 5 秒）");
    hp::LogPlain("    --delay-ms <n>      开始注入前先等 n 毫秒（默认 0；用于把焦点切到目标窗口）");
    hp::LogPlain("    --hold-ms <n>       每个音符按住多少毫秒（默认 120）");
    hp::LogPlain("    --gap-ms <n>        音符之间间隔多少毫秒（默认 200）");
    hp::LogPlain("    --vk                改用虚拟键码发送（默认扫描码；排障用）");
}

}  // namespace

int main(int argc, char** argv) {
    // 让中文在控制台正常显示（配合编译选项 /utf-8）
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    InputSendOptions options;
    options.useScancode = !HasFlag(argc, argv, "--vk");
    g_input = std::make_unique<InputController>(options);

    const std::string sendModeText = options.useScancode ? "扫描码（游戏推荐）" : "虚拟键码（排障）";
    PrintBanner(sendModeText);

    const bool useCountdown = !HasFlag(argc, argv, "--no-countdown");
    const int holdMs = IntOption(argc, argv, "--hold-ms", 120);
    const int gapMs = IntOption(argc, argv, "--gap-ms", 200);
    const int startDelayMs = IntOption(argc, argv, "--delay-ms", 0);

    // 命令行模式下开始注入前的统一等待（把焦点切到目标窗口用）。
    // 交互菜单里由每个动作自己的倒计时负责，不需要它。
    auto applyStartDelay = [startDelayMs]() {
        if (startDelayMs > 0) {
            hp::LogInfo("将在 " + std::to_string(startDelayMs) + " 毫秒后开始注入（可用这段时间切换窗口）…");
            WaitMs(startDelayMs);
        }
    };

    int exitCode = 0;
    if (argc <= 1) {
        exitCode = RunInteractiveMenu();
    } else if (HasFlag(argc, argv, "--help") || HasFlag(argc, argv, "-h")) {
        PrintHelp();
    } else if (HasFlag(argc, argv, "--env")) {
        PrintEnvironment();
    } else if (HasFlag(argc, argv, "--list")) {
        PrintKeyTable();
    } else if (HasFlag(argc, argv, "--autotest")) {
        applyStartDelay();
        RunAutoTest();
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(argc, argv, "--keys")) {
        RunKeySequenceTest(useCountdown, holdMs, gapMs);
    } else if (HasFlag(argc, argv, "--modes")) {
        RunPlayModeTest(useCountdown, 300);
    } else if (HasFlag(argc, argv, "--logic")) {
        RunLogicSelfTest();
        exitCode = (g_failed == 0) ? 0 : 1;
    } else if (HasFlag(argc, argv, "--monitor")) {
        RunMonitor();
    } else if (HasFlag(argc, argv, "--full")) {
        PrintEnvironment();
        PrintKeyTable();
        applyStartDelay();
        RunAutoTest();
        RunKeySequenceTest(useCountdown, holdMs, gapMs);
        exitCode = (g_failed == 0) ? 0 : 1;
    } else {
        hp::LogWarn("无法识别的参数，已显示帮助。");
        PrintHelp();
    }

    // 显式收尾：析构 InputController 会 ReleaseAll（即便上面漏了某条路径）
    hp::LogInfo("正在退出并释放工具按住的输入…");
    g_input.reset();
    hp::LogOk(std::string("已退出。本次自动测试：通过 ") + std::to_string(g_passed) + " 项，失败 " +
              std::to_string(g_failed) + " 项。");
    return exitCode;
}
