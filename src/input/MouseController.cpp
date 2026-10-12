#include "input/MouseController.h"

#include "core/Log.h"
#include "core/Platform.h"

namespace hp {
namespace {

// 与 KeyboardController 使用同一个标记值（ASCII "HPLY"）
constexpr ULONG_PTR kSyntheticSignature = 0x48504C59UL;

DWORD VirtualKeyOf(MouseButton button) {
    switch (button) {
        case MouseButton::Left:   return VK_LBUTTON;
        case MouseButton::Middle: return VK_MBUTTON;
        case MouseButton::Right:  return VK_RBUTTON;
    }
    return 0;
}

DWORD FlagOf(MouseButton button, bool buttonDown) {
    switch (button) {
        case MouseButton::Left:
            return buttonDown ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        case MouseButton::Middle:
            return buttonDown ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        case MouseButton::Right:
            return buttonDown ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    }
    return 0;
}

bool InMouseRange(int index) {
    return index >= 0 && index < kMouseButtonCount;
}

}  // namespace

bool MouseController::IsMousePhysicallyDown(MouseButton button) {
    const DWORD vk = VirtualKeyOf(button);
    return vk != 0 && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
}

bool MouseController::SendMouseEventLocked(MouseButton button, bool buttonDown) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = 0;
    input.mi.dy = 0;
    input.mi.mouseData = 0;
    input.mi.dwExtraInfo = kSyntheticSignature;

    DWORD flags = FlagOf(button, buttonDown);
    if (flags == 0) {
        lastFailed_ = true;
        lastError_ = ERROR_INVALID_PARAMETER;
        return false;
    }
    // MOUSEEVENTF_MOVE 是“相对移动”的标志位，这里必须为 0，
    // 否则某些程序会按坐标系误解这次按键事件。
    input.mi.dwFlags = flags;

    const UINT sent = SendInput(1, &input, sizeof(INPUT));
    if (sent != 1) {
        lastFailed_ = true;
        lastError_ = GetLastError();
        return false;
    }
    lastFailed_ = false;
    lastError_ = 0;
    return true;
}

bool MouseController::Press(MouseButton button) {
    std::lock_guard<std::mutex> lock(mtx_);
    const int index = static_cast<int>(button);
    if (!InMouseRange(index)) {
        return false;
    }
    if (playerDown_[index]) {
        return false;   // 已由工具按下，不重复下发
    }

    preHeldByUser_[index] = IsMousePhysicallyDown(button);

    if (!SendMouseEventLocked(button, true)) {
        return false;
    }
    playerDown_[index] = true;   // 只有发送成功才记 Player-owned
    return true;
}

bool MouseController::Release(MouseButton button) {
    std::lock_guard<std::mutex> lock(mtx_);
    const int index = static_cast<int>(button);
    if (!InMouseRange(index)) {
        return false;
    }
    if (!playerDown_[index]) {
        return false;   // 不是工具按下的：绝不发 UP
    }

    // 铁律②：这个鼠标键在我们按下之前**就已经被用户按住**了 —— 它实际上是用户的键。
    // 补一个 UP 就等于把用户自己按着的键抬起来，这里只清掉我们这边的记账。
    if (preHeldByUser_[index]) {
        playerDown_[index] = false;
        preHeldByUser_[index] = false;
        return false;
    }

    if (!SendMouseEventLocked(button, false)) {
        SendMouseEventLocked(button, false);   // 重试一次
        LogWarn(std::string("释放鼠标键失败（已重试）：") + MouseButtonDisplayName(button));
    }
    playerDown_[index] = false;
    preHeldByUser_[index] = false;
    return true;
}

void MouseController::ReleaseAll() {
    std::lock_guard<std::mutex> lock(mtx_);
    for (int i = 0; i < kMouseButtonCount; ++i) {
        if (!playerDown_[i]) {
            continue;
        }
        if (preHeldByUser_[i]) {
            // 用户本来就按着的鼠标键：一概不碰（铁律②），只清掉我们这边的记账
            playerDown_[i] = false;
            preHeldByUser_[i] = false;
            continue;
        }
        const MouseButton button = static_cast<MouseButton>(i);
        if (!SendMouseEventLocked(button, false)) {
            SendMouseEventLocked(button, false);
            LogWarn(std::string("ReleaseAll 释放鼠标键失败（已重试）：") + MouseButtonDisplayName(button));
        }
        playerDown_[i] = false;
        preHeldByUser_[i] = false;
    }
}

bool MouseController::IsPlayerDown(MouseButton button) const {
    std::lock_guard<std::mutex> lock(mtx_);
    const int index = static_cast<int>(button);
    return InMouseRange(index) && playerDown_[index];
}

int MouseController::PlayerDownCount() const {
    std::lock_guard<std::mutex> lock(mtx_);
    int count = 0;
    for (bool down : playerDown_) {
        if (down) {
            ++count;
        }
    }
    return count;
}

std::string MouseController::PlayerDownSummary() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::string summary;
    for (int i = 0; i < kMouseButtonCount; ++i) {
        if (!playerDown_[i]) {
            continue;
        }
        if (!summary.empty()) {
            summary += ", ";
        }
        summary += MouseButtonDisplayName(static_cast<MouseButton>(i));
    }
    return summary.empty() ? std::string("-") : summary;
}

bool MouseController::LastSendFailed() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return lastFailed_;
}

unsigned long MouseController::LastSendError() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return lastError_;
}

}  // namespace hp
