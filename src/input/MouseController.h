#pragma once

// 鼠标按键输入的唯一出口。
//
// 与 KeyboardController 完全同构：
//   - 只有本类可以调用 SendInput 发鼠标事件；
//   - 只释放“工具自己按下”的鼠标键；
//   - 状态由互斥量保护。
//
// 注意：本类只做“按键”，不做鼠标移动。
// 需求书第二十五节明确要求鼠标移动默认不参与任何逻辑（既不触发急停，也不由本工具产生）。

#include <array>
#include <functional>
#include <mutex>
#include <string>

#include "core/Types.h"

namespace hp {

class MouseController {
public:
    MouseController() = default;

    // 按下鼠标左/中/右键。已按下则不重复下发。
    bool Press(MouseButton button);

    // 释放“工具自己按下”的鼠标键。非 Player-owned 一概不动。
    bool Release(MouseButton button);

    void ReleaseAll();

    bool IsPlayerDown(MouseButton button) const;

    // 系统视角的物理状态（GetAsyncKeyState），包含注入事件；仅测试/排障用。
    static bool IsMousePhysicallyDown(MouseButton button);

    int PlayerDownCount() const;
    std::string PlayerDownSummary() const;   // 例如 "鼠标左键, 鼠标右键"；没有则返回 "-"

    bool LastSendFailed() const;
    unsigned long LastSendError() const;

    // ---------------------------------------------------------------------
    // 自检专用（不注入任何真实输入）：把“发送”与“系统物理状态回读”换成可注入的替身。
    //   默认没有替身 → 走 SendInput / GetAsyncKeyState，生产行为一字不变。
    //   用途：
    //     ① EventSink：逐事件记录输入层**实际发出的**按下/抬起（取证用，不碰系统）；
    //     ② PhysicalProbe：模拟真实系统里“我们刚发出的事件还没反映到 GetAsyncKeyState”
    //        的那个窗口（SendInput 只是把事件排进输入队列）——它正是右键卡死的成因，
    //        有了它就能把这种时序变成确定可复现、可回归的用例，而不是靠运气复现。
    // ---------------------------------------------------------------------
    using EventSink = std::function<bool(MouseButton button, bool buttonDown)>;
    using PhysicalProbe = std::function<bool(MouseButton button)>;
    void SetEventSink(EventSink sink);
    void SetPhysicalProbe(PhysicalProbe probe);
    void ClearTestHooks();

private:
    bool SendMouseEventLocked(MouseButton button, bool buttonDown);
    bool PhysicalStateLocked(MouseButton button) const;
    bool EventSinkEnabledLocked() const { return static_cast<bool>(eventSink_); }

    mutable std::mutex mtx_;
    std::array<bool, kMouseButtonCount> playerDown_{};
    std::array<bool, kMouseButtonCount> preHeldByUser_{};   // 我们按下之前已被用户按住（这类键不补 UP，铁律②）
    // 我们自己发过事件、但系统物理状态可能还没落地的窗口（见 Press 里那段注释）。
    // 这个窗口内读到的“按下”一律不归用户 —— 否则会把我们自己的事件误判成“用户在按”，
    // 于是永远不补 UP，鼠标键就卡在按下不放。
    std::array<bool, kMouseButtonCount> ourDownUnsettled_{};
    EventSink eventSink_;                                   // 自检替身①：见上面的说明
    PhysicalProbe physicalProbe_;                           // 自检替身②：见上面的说明
    bool lastFailed_ = false;
    unsigned long lastError_ = 0;
};

}  // namespace hp
