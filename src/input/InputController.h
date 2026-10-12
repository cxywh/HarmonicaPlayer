#pragma once

// 全项目唯一的输入门面。
// 播放器、热键管理器、Hook、UI 都不许直接碰 SendInput / KeyboardController / MouseController，
// 一律经过这里（需求书第十节）。
//
// 线程模型（需求书第二十、三十八、四十一节）：
//   - 本类内部用一把 mutex 把“组合动作”（按住鼠标 + 按键、ReleaseAll）整体串起来，
//     保证 ReleaseAll 不会插到 “MouseDown…KeyUp…MouseUp” 中间；
//   - 单个按键/鼠标键的原子性由子控制器的 mutex 保证；
//   - Phase 6 会在这一层之上再加 stopRequested（atomic）与播放线程同步，
//     本 Phase 不做播放，因此不引入任何 Sleep 或等待。

#include <mutex>
#include <string>

#include "core/Types.h"
#include "input/KeyboardController.h"
#include "input/MouseController.h"

namespace hp {

class InputController {
public:
    InputController();
    explicit InputController(const InputSendOptions& options);
    ~InputController();   // 析构时 ReleaseAll()，异常/退出都不会留下按住的键

    InputController(const InputController&) = delete;
    InputController& operator=(const InputController&) = delete;

    // ---- 单键 ----
    bool PressKey(Key key);
    bool ReleaseKey(Key key);

    // ---- 单鼠标键 ----
    bool PressMouse(MouseButton button);
    bool ReleaseMouse(MouseButton button);

    // ---- 演奏模式 ----
    // Normal：不碰鼠标。
    // Left/Middle/Right：先按下对应鼠标键（返回其按下结果；Normal 返回 true）。
    // LeftMiddle/RightMiddle：**依次按下两个鼠标键**（先八度键，再中键），
    //   返回“两个都按下了”的结果。与单键模式同一条规矩：这里只负责按下，
    //   失败时**不做任何补救性抬起**（已经按下的键由调用方照常释放，
    //   绝不替调用方猜测它还想不想按着）。
    // ReleaseMode 是它的镜像动作（先抬中键，再抬八度键）。
    bool PressMode(PlayMode mode);
    bool ReleaseMode(PlayMode mode);

    // ---- 急停/停止的统一出口 ----
    // 只释放 Player-owned 的输入；用户自己按住的键（例如 Shift）保持不动。
    void ReleaseAll();

    // 工具当前是否还持有任何输入
    bool AnyPlayerInputDown() const;

    // 例如 "Z, 鼠标左键"；没有则返回 "-"
    std::string PlayerStateSummary() const;

    KeyboardController& Keyboard() { return keyboard_; }
    const KeyboardController& Keyboard() const { return keyboard_; }
    MouseController& Mouse() { return mouse_; }
    const MouseController& Mouse() const { return mouse_; }

    // 切换扫描码 / 虚拟键码发送方式（排障用，Phase 1 固定默认）
    void SetSendOptions(const InputSendOptions& options);

    // ---------------------------------------------------------------------
    // 自检专用：模拟"人手按键"。
    //   只用于验证由**用户按键**触发的功能（全局热键、Phase 6 的输入监控）；
    //   它们不是口琴的 8 个音调键，不参与 Player-owned 追踪，
    //   也不会被 ReleaseAll() 影响（两者互不干扰）。
    //   组合键由调用者用 SimulateRawKeyEvent 拼（先按修饰键，最后反序抬起）。
    //   故意不加 actionMutex_：内部有几十毫秒的"按住"时间，
    //   不能让急停/停止的 ReleaseAll 等它。
    bool SimulateRawKeyEvent(unsigned short vk, bool keyDown);
    bool SimulateUserKeyPress(unsigned short vk, int holdMs = 30);
    int SimulatedDownCount() const;
    void ReleaseSimulatedKeys();

private:
    // 保护“多步组合动作”的整体性（ReleaseAll vs PressMode/ReleaseMode）
    mutable std::mutex actionMutex_;
    KeyboardController keyboard_;
    MouseController mouse_;
};

}  // namespace hp
