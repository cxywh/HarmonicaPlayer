#include "input/InputController.h"

#include "core/Log.h"

namespace hp {

InputController::InputController() = default;

InputController::InputController(const InputSendOptions& options) : keyboard_(options) {}

InputController::~InputController() {
    // 最重要的兜底：只要 InputController 被销毁，就把工具自己按住的输入全部放开。
    ReleaseAll();
}

bool InputController::PressKey(Key key) {
    return keyboard_.PressKey(key);
}

bool InputController::ReleaseKey(Key key) {
    return keyboard_.ReleaseKey(key);
}

bool InputController::PressMouse(MouseButton button) {
    return mouse_.Press(button);
}

bool InputController::ReleaseMouse(MouseButton button) {
    return mouse_.Release(button);
}

bool InputController::PressMode(PlayMode mode) {
    MouseButton button{};
    if (!MouseButtonOf(mode, button)) {
        return true;   // Normal：不碰鼠标
    }
    std::lock_guard<std::mutex> lock(actionMutex_);
    return mouse_.Press(button);
}

bool InputController::ReleaseMode(PlayMode mode) {
    MouseButton button{};
    if (!MouseButtonOf(mode, button)) {
        return true;   // Normal：不碰鼠标
    }
    std::lock_guard<std::mutex> lock(actionMutex_);
    return mouse_.Release(button);
}

void InputController::ReleaseAll() {
    // 顺序：先键盘后鼠标，整体不可被打断。
    // 单次调用必须极快（不 Sleep、不等待），因为急停路径会直接调用它。
    std::lock_guard<std::mutex> lock(actionMutex_);
    keyboard_.ReleaseAll();
    mouse_.ReleaseAll();
}

bool InputController::AnyPlayerInputDown() const {
    return keyboard_.PlayerDownCount() > 0 || mouse_.PlayerDownCount() > 0;
}

std::string InputController::PlayerStateSummary() const {
    const std::string keys = keyboard_.PlayerDownSummary();
    const std::string buttons = mouse_.PlayerDownSummary();
    const bool hasKeys = keys != "-";
    const bool hasButtons = buttons != "-";
    if (!hasKeys && !hasButtons) {
        return "-";
    }
    std::string summary;
    if (hasKeys) {
        summary += keys;
    }
    if (hasButtons) {
        if (!summary.empty()) {
            summary += ", ";
        }
        summary += buttons;
    }
    return summary;
}

void InputController::SetSendOptions(const InputSendOptions& options) {
    keyboard_.SetOptions(options);
    LogInfo(std::string("发送方式已切换为：") + (options.useScancode ? "扫描码（游戏推荐）" : "虚拟键码"));
}

bool InputController::SimulateUserKeyPress(unsigned short vk, int holdMs) {
    // 自检专用：不碰 Player-owned 状态，因此不需要 actionMutex_（见头文件说明）。
    return keyboard_.SimulateUserKeyPress(vk, holdMs);
}

bool InputController::SimulateRawKeyEvent(unsigned short vk, bool keyDown) {
    return keyboard_.SimulateRawKeyEvent(vk, keyDown);
}

int InputController::SimulatedDownCount() const {
    return keyboard_.SimulatedDownCount();
}

void InputController::ReleaseSimulatedKeys() {
    keyboard_.ReleaseSimulatedKeys();
}

}  // namespace hp
