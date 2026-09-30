#include "input/KeyboardController.h"

#include <algorithm>

#include "core/Log.h"
#include "core/Platform.h"

namespace hp {
namespace {

// 打进 dwExtraInfo 的标记，值为 ASCII "HPLY"。
// LLKHF_INJECTED 已经能识别注入事件（Phase 6 的主要判据），
// 这个标记是额外的一道保险：万一将来要区分“本工具注入”与“其它工具注入”。
constexpr ULONG_PTR kSyntheticSignature = 0x48504C59UL;

}  // namespace

KeyboardController::KeyboardController() = default;

KeyboardController::KeyboardController(const InputSendOptions& options) : options_(options) {}

bool KeyboardController::IsExtendedVirtualKey(unsigned short vk) {
    return hp::IsExtendedVirtualKey(vk);
}

bool KeyboardController::IsVirtualKeyPhysicallyDown(unsigned short vk) {
    return (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
}

bool KeyboardController::IsKeyPhysicallyDown(Key key) {
    const unsigned short vk = VirtualKeyOf(key);
    return vk != 0 && IsVirtualKeyPhysicallyDown(vk);
}

bool KeyboardController::SendKeyEventLocked(Key key, bool keyDown) {
    const unsigned short vk = VirtualKeyOf(key);
    if (vk == 0) {
        lastFailed_ = true;
        lastError_ = ERROR_INVALID_PARAMETER;
        return false;
    }

    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.dwExtraInfo = kSyntheticSignature;

    if (options_.useScancode) {
        const UINT scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        if (scanCode != 0) {
            input.ki.wScan = static_cast<WORD>(scanCode);
            input.ki.dwFlags |= KEYEVENTF_SCANCODE;
            if (IsExtendedVirtualKey(vk)) {
                input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            }
        } else {
            // 极少数键没有扫描码（或布局异常），退回虚拟键码发送，保证功能不中断
            input.ki.wVk = vk;
        }
    } else {
        input.ki.wVk = vk;
    }

    if (!keyDown) {
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
    }

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

bool KeyboardController::SimulateRawKeyEvent(unsigned short vk, bool keyDown) {
    if (vk == 0) {
        return false;
    }
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;   // 热键是按虚拟键码匹配的，这里直接用 VK 最可靠
    const UINT scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (scanCode != 0) {
        input.ki.wScan = static_cast<WORD>(scanCode);
    }
    if (IsExtendedVirtualKey(vk)) {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    if (!keyDown) {
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
    }
    input.ki.dwExtraInfo = kSyntheticSignature;

    std::lock_guard<std::mutex> lock(mtx_);
    if (SendInput(1, &input, sizeof(INPUT)) != 1) {
        lastFailed_ = true;
        lastError_ = GetLastError();
        return false;
    }
    lastFailed_ = false;
    lastError_ = 0;

    // 记账：哪些键是"被自检模拟按下、还没抬起"的。
    // 组合键会有多个（Ctrl / Shift / 主键），所以用列表而不是单个数。
    if (keyDown) {
        if (std::find(simulatedDown_.begin(), simulatedDown_.end(), vk) == simulatedDown_.end()) {
            simulatedDown_.push_back(vk);
        }
    } else {
        simulatedDown_.erase(std::remove(simulatedDown_.begin(), simulatedDown_.end(), vk),
                             simulatedDown_.end());
    }
    return true;
}

bool KeyboardController::SimulateUserKeyPress(unsigned short vk, int holdMs) {
    if (vk == 0) {
        return false;
    }
    // 只动这一个键：其它键（例如组合键的修饰键）的按下状态一律不碰。
    // 这里绝对不能再"顺手补抬起上一次的键"——那会把刚按下的 Ctrl/Shift 抬掉，
    // 组合键就永远匹配不上了（Phase 5 实测踩过）。
    if (!SimulateRawKeyEvent(vk, true)) {
        return false;
    }
    if (holdMs > 0) {
        // 自检专用：这里的 Sleep 只让"人手感"更真实，不参与任何演奏时间轴
        Sleep(static_cast<DWORD>(holdMs));
    }
    return SimulateRawKeyEvent(vk, false);
}

std::vector<unsigned short> KeyboardController::SimulatedDownKeys() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return simulatedDown_;
}

int KeyboardController::SimulatedDownCount() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return static_cast<int>(simulatedDown_.size());
}

void KeyboardController::ReleaseSimulatedKeys() {
    for (;;) {
        unsigned short pending = 0;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (simulatedDown_.empty()) {
                return;
            }
            pending = simulatedDown_.back();
        }
        SimulateRawKeyEvent(pending, false);   // 离开锁作用域后再投递
    }
}

bool KeyboardController::PressKey(Key key) {
    std::lock_guard<std::mutex> lock(mtx_);
    const int index = static_cast<int>(key);
    if (index < 0 || index >= kKeyCount) {
        return false;
    }
    if (playerDown_[index]) {
        // 已经由工具按下：绝不重复下发（重复按下会让游戏收到重复的“起音”）。
        return false;
    }

    // 只作诊断记录：这个键在我们按下之前是不是已经被（用户）按住。
    // 不改变释放策略 —— 见 README「已知问题 3」。
    preHeldByUser_[index] = IsKeyPhysicallyDown(key);

    if (!SendKeyEventLocked(key, true)) {
        return false;
    }
    playerDown_[index] = true;   // 只有发送成功才记 Player-owned
    return true;
}

bool KeyboardController::ReleaseKey(Key key) {
    std::lock_guard<std::mutex> lock(mtx_);
    const int index = static_cast<int>(key);
    if (index < 0 || index >= kKeyCount) {
        return false;
    }
    if (!playerDown_[index]) {
        // 不是工具按下的：绝不发 KeyUp。
        // 这正是“用户本来就按住 Shift，工具停止时不释放 Shift”的实现位置。
        return false;
    }

    if (!SendKeyEventLocked(key, false)) {
        // 抬起失败时重试一次；无论成败都必须清掉状态，否则会永远卡在“以为按着”。
        SendKeyEventLocked(key, false);
        LogWarn(std::string("释放按键失败（已重试）：") + KeyDisplayName(key));
    }
    ClearStateLocked(index);
    return true;
}

void KeyboardController::ReleaseAll() {
    std::lock_guard<std::mutex> lock(mtx_);
    for (int i = 0; i < kKeyCount; ++i) {
        if (!playerDown_[i]) {
            continue;
        }
        if (!SendKeyEventLocked(static_cast<Key>(i), false)) {
            SendKeyEventLocked(static_cast<Key>(i), false);
            LogWarn(std::string("ReleaseAll 释放按键失败（已重试）：") + KeyDisplayName(static_cast<Key>(i)));
        }
        ClearStateLocked(i);
    }
}

void KeyboardController::ClearStateLocked(int index) {
    playerDown_[index] = false;
    preHeldByUser_[index] = false;
}

bool KeyboardController::IsPlayerKeyDown(Key key) const {
    std::lock_guard<std::mutex> lock(mtx_);
    const int index = static_cast<int>(key);
    return index >= 0 && index < kKeyCount && playerDown_[index];
}

int KeyboardController::PlayerDownCount() const {
    std::lock_guard<std::mutex> lock(mtx_);
    int count = 0;
    for (bool down : playerDown_) {
        if (down) {
            ++count;
        }
    }
    return count;
}

std::string KeyboardController::PlayerDownSummary() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::string summary;
    for (int i = 0; i < kKeyCount; ++i) {
        if (!playerDown_[i]) {
            continue;
        }
        if (!summary.empty()) {
            summary += ", ";
        }
        summary += KeyDisplayName(static_cast<Key>(i));
    }
    return summary.empty() ? std::string("-") : summary;
}

void KeyboardController::SetOptions(const InputSendOptions& options) {
    std::lock_guard<std::mutex> lock(mtx_);
    options_ = options;
}

InputSendOptions KeyboardController::Options() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return options_;
}

bool KeyboardController::LastSendFailed() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return lastFailed_;
}

unsigned long KeyboardController::LastSendError() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return lastError_;
}

}  // namespace hp
