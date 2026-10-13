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

bool MouseController::PhysicalStateLocked(MouseButton button) const {
    if (physicalProbe_) {
        return physicalProbe_(button);   // 自检替身②：由测试指定“系统此刻看到的状态”
    }
    return IsMousePhysicallyDown(button);
}

void MouseController::SetEventSink(EventSink sink) {
    std::lock_guard<std::mutex> lock(mtx_);
    eventSink_ = std::move(sink);
}

void MouseController::SetPhysicalProbe(PhysicalProbe probe) {
    std::lock_guard<std::mutex> lock(mtx_);
    physicalProbe_ = std::move(probe);
}

void MouseController::ClearTestHooks() {
    std::lock_guard<std::mutex> lock(mtx_);
    eventSink_ = nullptr;
    physicalProbe_ = nullptr;
}

bool MouseController::SendMouseEventLocked(MouseButton button, bool buttonDown) {
    // 自检替身①：只把事件交给记录器，绝不碰系统（生产环境没有替身，直接往下走 SendInput）。
    if (eventSink_) {
        const bool ok = eventSink_(button, buttonDown);
        lastFailed_ = !ok;
        lastError_ = ok ? 0UL : ERROR_INVALID_PARAMETER;
        return ok;
    }

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

    // 铁律②判据：这个鼠标键在我们按下之前是不是**用户**按住的？
    //   ⚠ 只有在我们自己没有任何“还没落地”的事件时，这个读值才能算成用户的。
    //   否则读到的“按下”很可能是我们自己刚发出去的事件：SendInput 只是把事件排进系统输入
    //   队列，而系统对上一次 RIGHTUP 的处理还要经过本程序自己装的低级鼠标钩子（默认开着），
    //   于是“抬起旧 mode → 立刻按下新 mode”这对事件挨得极近时，这里会把**我们自己**的按下
    //   读成“用户本来就按着”，之后 Release/ReleaseAll 按铁律②一律不补 UP ——
    //   结果就是右键一直卡在按下不放（《父亲》里实测到的就是这个）。
    //   ourDownUnsettled_ 记住“我们发过事件、系统状态还没确认落地”这个窗口；窗口内读到
    //   的按下不归用户。只要亲眼看到它真的抬起了，窗口立刻结束（用户按住的情形照样认得出）。
    const bool physicalDown = PhysicalStateLocked(button);
    if (!physicalDown) {
        ourDownUnsettled_[index] = false;
    }
    preHeldByUser_[index] = physicalDown && !ourDownUnsettled_[index];

    if (!SendMouseEventLocked(button, true)) {
        return false;
    }
    playerDown_[index] = true;         // 只有发送成功才记 Player-owned
    ourDownUnsettled_[index] = true;   // 刚发过按下：物理状态可能还没反映
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
        // 我们没有在这上面叠事件，物理状态归用户：可疑窗口一并结束（用户抬手即归零）。
        ourDownUnsettled_[index] = false;
        return false;
    }

    if (!SendMouseEventLocked(button, false)) {
        SendMouseEventLocked(button, false);   // 重试一次
        LogWarn(std::string("释放鼠标键失败（已重试）：") + MouseButtonDisplayName(button));
    }
    playerDown_[index] = false;
    preHeldByUser_[index] = false;
    // 刚刚发出 UP：如果系统此刻还读成“按下”，说明它还没落地 —— 这段窗口里读到的按下
    // 不能算成用户的，标记下来交给 Press 判断（正是上面那个坑的堵法）。
    ourDownUnsettled_[index] = PhysicalStateLocked(button);
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
            ourDownUnsettled_[i] = false;
            continue;
        }
        const MouseButton button = static_cast<MouseButton>(i);
        if (!SendMouseEventLocked(button, false)) {
            SendMouseEventLocked(button, false);
            LogWarn(std::string("ReleaseAll 释放鼠标键失败（已重试）：") + MouseButtonDisplayName(button));
        }
        playerDown_[i] = false;
        preHeldByUser_[i] = false;
        ourDownUnsettled_[i] = PhysicalStateLocked(button);
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
