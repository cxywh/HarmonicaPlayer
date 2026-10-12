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

// 组合模式（LeftMiddle/RightMiddle）要**同时按住两个鼠标键**：
// 循环里逐个按下（先八度键、后中键），整个过程在 actionMutex_ 内，
// ReleaseAll / ReleaseMode 不会插到两个键中间。
// 只释放“自己真正按下成功过”的键这件事由 MouseController 负责（Player-owned 追踪），
// 因此这里对“按不下去的键”不补发抬起，也不会碰到用户自己按住的鼠标键。
bool InputController::PressMode(PlayMode mode) {
    MouseButton buttons[kMaxMouseButtonsPerMode] = {};
    const int count = MouseButtonsOf(mode, buttons);
    if (count <= 0) {
        return true;   // Normal：不碰鼠标
    }
    std::lock_guard<std::mutex> lock(actionMutex_);
    bool allOk = true;
    for (int i = 0; i < count; ++i) {
        if (!mouse_.Press(buttons[i])) {
            allOk = false;   // 记下失败，但仍然把该按的都按到（多步动作要么全做、要么不做）
        }
    }
    return allOk;
}

bool InputController::ReleaseMode(PlayMode mode) {
    MouseButton buttons[kMaxMouseButtonsPerMode] = {};
    const int count = MouseButtonsOf(mode, buttons);
    if (count <= 0) {
        return true;   // Normal：不碰鼠标
    }
    std::lock_guard<std::mutex> lock(actionMutex_);
    bool allOk = true;
    // 与按下顺序相反：先抬中键，再抬八度键
    for (int i = count - 1; i >= 0; --i) {
        if (!mouse_.Release(buttons[i])) {
            allOk = false;   // 不是工具按下的键 —— 一概不发抬起（只记账）
        }
    }
    return allOk;
}

void InputController::ReleaseAll() {
    // 顺序：先键盘后鼠标，整体不可被打断。
    // 单次调用必须极快（不 Sleep、不等待），因为急停路径会直接调用它。
    // mouse_.ReleaseAll() 会遍历全部 3 个鼠标键，组合模式（左+中 / 右+中）自然被覆盖，
    // 急停 / 演奏结束之后不会留下被按住的鼠标键。
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
