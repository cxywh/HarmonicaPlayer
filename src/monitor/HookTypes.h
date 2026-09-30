#pragma once

// 输入监控的类型定义（需求书第十四～十八、二十五、二十六节）。
//
// 拆成单独的头文件，是为了让"分类逻辑"可以在没有键盘钩子的情况下被单测：
// 钩子回调只负责把 Win32 的 KBDLLHOOKSTRUCT / MSLLHOOKSTRUCT 翻译成这里的
// RawHookEvent，之后"是不是注入的、是不是抬起、要不要忽略、要不要急停"
// 全部走同一段代码。测试用 ProcessRawEventForTest() 喂同样形状的数据，
// 验的就跟真实钩子走的是同一条路。

#include <functional>
#include <string>

#include "core/Types.h"

namespace hp {

// ---------------------------------------------------------------------------
// 事件种类（需求书十五节要的"任意键"，二十五节要的鼠标键）
// ---------------------------------------------------------------------------
enum class UserInputKind : int {
    KeyDown = 0,         // 键盘按下（含系统键，例如 Alt 组合）
    MouseButtonDown = 1, // 鼠标左 / 中 / 右键按下
    MouseMove = 2,       // 鼠标移动（默认不触发急停，二十五节）
};

const char* UserInputKindName(UserInputKind kind);

// ---------------------------------------------------------------------------
// 钩子看到的一条原始事件：与 Win32 结构体解耦，便于单测与日志
// ---------------------------------------------------------------------------
struct RawHookEvent {
    UserInputKind kind = UserInputKind::KeyDown;
    bool keyUp = false;                  // 键盘抬起（十五节只要按下，抬起不触发）
    unsigned vk = 0;                     // 键盘虚拟键码
    MouseButton button = MouseButton::Left;   // 仅鼠标事件有意义
    bool injected = false;               // LLKHF_INJECTED / LLMHF_INJECTED
    unsigned long long extraInfo = 0;    // 注入标记（本工具是 0x48504C59 "HPLY"）
    unsigned long long tick = 0;         // 事件到达的时刻（GetTickCount64）
};

// ---------------------------------------------------------------------------
// 判定为"用户自己的输入"之后，交给上层的事件
// ---------------------------------------------------------------------------
struct UserInputEvent {
    UserInputKind kind = UserInputKind::KeyDown;
    unsigned vk = 0;
    MouseButton button = MouseButton::Left;
    std::string name;          // "Space"、"W"、"鼠标左键"、"鼠标移动"
    unsigned long long tick = 0;
    bool injected = false;     // 正常路径永远是 false（注入的不会走到这里）
};

// ---------------------------------------------------------------------------
// 设置（需求书二十五节的三项开关）
// ---------------------------------------------------------------------------
struct InputMonitorOptions {
    bool keyboard = true;        // 键盘按键触发急停（十五节）
    bool mouseButtons = true;    // 鼠标左/中/右键触发急停（二十五节）
    bool mouseMove = false;      // 鼠标移动：默认关闭（二十五节明确要求）
};

// ---------------------------------------------------------------------------
// 统计：既给 UI 看，也是自检的证据
// ---------------------------------------------------------------------------
struct InputMonitorStats {
    long long keyboardEvents = 0;     // 钩子看到的键盘事件总数（含注入与抬起）
    long long mouseEvents = 0;        // 钩子看到的鼠标事件总数
    long long injectedIgnored = 0;    // 因为"是注入的"被忽略（十六节）
    long long keyUpIgnored = 0;       // 抬起事件不触发（十五节只要按下）
    long long moveIgnored = 0;        // 鼠标移动被忽略（默认关闭）
    long long disabledIgnored = 0;    // 该类别在设置里被关掉
    long long hotkeyIgnored = 0;      // 因为"是刚按下的启动热键"被忽略（十八节）
    long long triggered = 0;          // 真正触发急停的次数
    double worstCallbackMs = 0.0;     // 钩子回调最坏耗时（必须远小于系统 300 ms 上限）
    long long simulatedForTest = 0;   // 自检模拟"硬件按键"的次数
};

// ---------------------------------------------------------------------------
// 上层回调
// ---------------------------------------------------------------------------
struct InputMonitorCallbacks {
    // 收到真实用户输入 → 调它。**必须在钩子线程里极短返回**
    // （通常就是 player.EmergencyStop(event.name)，实测微秒级）。
    // 注意 detail 只填"是哪个键"：显示文本由 StopReasonName(用户输入)="检测到用户输入"
    // 和 detail 拼成，这里再带一次前缀会变成"检测到用户输入：检测到用户输入：W"。
    std::function<void(const UserInputEvent&)> onUserInput;

    // 问一句"这条要不要忽略"（需求书十八节：刚按下的启动热键必须忽略）。
    // 返回 true = 忽略，不触发急停。默认可以不给。
    std::function<bool(const UserInputEvent&)> shouldIgnore;

    // 每收到一个**真实**按键（注入的走不到这里）就调一次，用于"放行模式"下认热键。
    // 与 onUserInput 的区别：它在 shouldIgnore **之前**调用，所以绑成热键的键也能被认出来。
    // 必须极短：低级钩子 300ms 超时会静默丢钩子，这里只允许"匹配 + PostMessage"。
    std::function<void(const UserInputEvent&)> onHookKey;
};

}  // namespace hp
