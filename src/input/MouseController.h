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

private:
    bool SendMouseEventLocked(MouseButton button, bool buttonDown);

    mutable std::mutex mtx_;
    std::array<bool, kMouseButtonCount> playerDown_{};
    std::array<bool, kMouseButtonCount> preHeldByUser_{};   // 我们按下之前已被用户按住（这类键不补 UP，铁律②）
    bool lastFailed_ = false;
    unsigned long lastError_ = 0;
};

}  // namespace hp
