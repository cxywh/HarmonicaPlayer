#pragma once

// 键盘输入的唯一出口。
//
// 设计要点（对应需求书第十、十一、十二、四十节）：
//   1. 只有本类可以调用 SendInput 发键盘事件，其它模块一律通过它；
//   2. 只有“真正发送成功”的按下才会被记为 Player-owned；
//   3. 释放时只放 Player-owned 的键 —— 用户自己按住的键（例如 Shift）一概不碰；
//   4. 全部状态由互斥量保护，可被 UI 线程 / 播放线程 / 急停线程并发调用。

#include <array>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "core/Types.h"

namespace hp {

// 事件发送策略。Phase 1 用默认值；Phase 5/7 接入 Settings 后由配置决定。
struct InputSendOptions {
    // true  ：用 KEYEVENTF_SCANCODE + MapVirtualKey 发送“扫描码”。
    //         绝大多数游戏（DirectInput / Raw Input）只认扫描码，默认开启。
    // false ：用虚拟键码（wVk）发送，普通窗口程序更稳，排障时切换。
    bool useScancode = true;
};

class KeyboardController {
public:
    KeyboardController();
    explicit KeyboardController(const InputSendOptions& options);

    // 按下 Z/X/C/V/B/N/M/,
    // 返回值：true = 本次真的发出了按下事件；
    //         false = 该键已经由工具按下（不重复下发），或 SendInput 失败。
    bool PressKey(Key key);

    // 释放“工具自己按下”的键。非 Player-owned 的键一概不动。
    // 返回值：true = 本次真的发出了抬起事件。
    bool ReleaseKey(Key key);

    // 释放全部 Player-owned 键；用户自己按住的键不受影响。
    void ReleaseAll();

    bool IsPlayerKeyDown(Key key) const;

    // 系统视角的物理状态（GetAsyncKeyState），包含注入事件。
    // 仅用于测试与排障，不可用于判断“是不是用户按的”。
    static bool IsKeyPhysicallyDown(Key key);
    static bool IsVirtualKeyPhysicallyDown(unsigned short vk);

    int PlayerDownCount() const;
    std::string PlayerDownSummary() const;   // 例如 "Z, 逗号(,)"；没有则返回 "-"

    void SetOptions(const InputSendOptions& options);
    InputSendOptions Options() const;

    // 最近一次 SendInput 是否失败 / GetLastError 的值（诊断用，不弹窗不刷屏）
    bool LastSendFailed() const;
    unsigned long LastSendError() const;

    // 该虚拟键是否需要用 KEYEVENTF_EXTENDEDKEY（转发 core::Types 的实现）
    static bool IsExtendedVirtualKey(unsigned short vk);

    // ---------------------------------------------------------------------
    // 自检专用（不注入任何真实输入）：与 MouseController 完全同构的两件替身。
    //   默认没有替身 → 走 SendInput / GetAsyncKeyState，生产行为一字不变。
    //   ① EventSink：逐事件记录输入层**实际发出的**按下/抬起（取证用，不碰系统）；
    //   ② PhysicalProbe：模拟“我们刚发出的事件还没反映到 GetAsyncKeyState”的窗口。
    // ---------------------------------------------------------------------
    using EventSink = std::function<bool(Key key, bool keyDown)>;
    using PhysicalProbe = std::function<bool(Key key)>;
    void SetEventSink(EventSink sink);
    void SetPhysicalProbe(PhysicalProbe probe);
    void ClearTestHooks();

    // ---------------------------------------------------------------------
    // 自检专用：模拟"人手按键"（例如 F6 这种全局热键，或 Ctrl+Shift+F9 这种组合键）。
    //   - 只用于验证"全局热键 / 输入监控"这类由**用户按键**触发的功能；
    //   - 它们不是口琴的 8 个音调键，不参与 Player-owned 追踪
    //     （不会出现在 PlayerDownSummary / AnyPlayerInputDown 里）；
    //   - 仍然带本工具的注入标记（dwExtraInfo），Phase 6 的钩子能据此认出"这是注入的"；
    //   - 组合键的按法由调用者用 SimulateRawKeyEvent 自行拼（先按修饰键，最后反序抬起）。
    //
    // SimulateUserKeyPress 内部会 Sleep(holdMs)，只允许自检用，
    // 绝不允许在播放线程里调用。

    // 投递一个原始按键事件（按下 / 抬起），用于自检时拼组合键。
    // 调用者必须自己保证成对抬起；退出前可用 SimulatedDownCount() /
    // ReleaseSimulatedKeys() 检查并补齐（绝不允许留下按住的键）。
    bool SimulateRawKeyEvent(unsigned short vk, bool keyDown);

    // 按下 - 等 holdMs - 抬起，一次完整的单键动作（不动其它键的按下状态）
    bool SimulateUserKeyPress(unsigned short vk, int holdMs = 30);

    // 当前被模拟按下但还没抬起的键（自检收尾用）
    std::vector<unsigned short> SimulatedDownKeys() const;
    int SimulatedDownCount() const;
    // 把所有被模拟按下的键补齐抬起（自检收尾，保证不留残留）
    void ReleaseSimulatedKeys();

private:
    // 真正构造并投递一个 INPUT 结构。调用者必须已持有 mtx_。
    bool SendKeyEventLocked(Key key, bool keyDown);
    bool PhysicalStateLocked(Key key) const;
    void ClearStateLocked(int index);

    mutable std::mutex mtx_;
    std::array<bool, kKeyCount> playerDown_{};          // 工具自己按下的键
    std::array<bool, kKeyCount> preHeldByUser_{};        // 我们按下之前该键已被用户按住（这类键不补 KeyUp，铁律②）
    // 我们自己发过事件、系统物理状态可能还没落地的窗口（同 MouseController）：
    // 窗口内读到的“按下”不能算成用户的，否则永远不补 KeyUp，键会卡在按下。
    std::array<bool, kKeyCount> ourDownUnsettled_{};
    EventSink eventSink_;                               // 自检替身①：见上面的说明
    PhysicalProbe physicalProbe_;                        // 自检替身②：见上面的说明
    InputSendOptions options_{};
    bool lastFailed_ = false;
    unsigned long lastError_ = 0;
    // 自检模拟按下但还没抬起的键（虚拟键码），收尾时用来查漏（组合键会有多个）
    std::vector<unsigned short> simulatedDown_;
};

}  // namespace hp
