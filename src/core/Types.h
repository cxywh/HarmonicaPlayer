#pragma once

// 口琴演奏的“领域类型”：8 个音调、4 种演奏模式、3 个鼠标键。
// 这一层只做“名字 ↔ 枚举 ↔ Windows 虚拟键码”的映射，不含任何输入发送逻辑，
// 后续 Phase 的曲谱解析（JSON / 文本谱）都复用这里的解析函数。

#include <string>

namespace hp {

// 通用键名显示（任意虚拟键码 → "Space" / "F6" / "Left" / "VK_0x77"）。
// 需求书十五节列的"任意键"（含方向键）都在这里给出可读名字，
// 二十六节的"原因：检测到用户输入：Space"就是用它拼出来的。
std::string VirtualKeyDisplayName(unsigned vk);

// ---------------------------------------------------------------------------
// 8 个音调：Z X C V B N M ,
// ---------------------------------------------------------------------------
enum class Key : int {
    Z = 0,
    X = 1,
    C = 2,
    V = 3,
    B = 4,
    N = 5,
    M = 6,
    Comma = 7
};
constexpr int kKeyCount = 8;

// ---------------------------------------------------------------------------
// 4 种演奏方式
//   Normal : 直接按键
//   Left   : 按住鼠标左键 + 按键
//   Middle : 按住鼠标中键 + 按键
//   Right  : 按住鼠标右键 + 按键
// ---------------------------------------------------------------------------
enum class PlayMode : int {
    Normal = 0,
    Left = 1,
    Middle = 2,
    Right = 3
};
constexpr int kPlayModeCount = 4;

// ---------------------------------------------------------------------------
// 鼠标按键
// ---------------------------------------------------------------------------
enum class MouseButton : int {
    Left = 0,
    Middle = 1,
    Right = 2
};
constexpr int kMouseButtonCount = 3;

// ---- Key -> Windows 虚拟键码 / 名字 ----
unsigned short VirtualKeyOf(Key key);      // 例如 Key::Comma -> VK_OEM_COMMA(0xBC)
const char*    KeyName(Key key);           // 谱面用："Z" ... ","
const char*    KeyDisplayName(Key key);    // 日志/UI 用："Z" ... "逗号(,)"
bool           ParseKey(const std::string& text, Key& out);

// ---- PlayMode ----
const char* PlayModeName(PlayMode mode);         // "Normal" / "Left" / "Middle" / "Right"
const char* PlayModeDisplayName(PlayMode mode);  // "正常" / "左键" / "中键" / "右键"
bool        ParsePlayMode(const std::string& text, PlayMode& out);

// ---- MouseButton ----
const char* MouseButtonName(MouseButton button);
const char* MouseButtonDisplayName(MouseButton button);

// 某个演奏模式需要按住的鼠标键；Normal 返回 false（不碰鼠标）。
bool MouseButtonOf(PlayMode mode, MouseButton& out);

// 是否需要先按住鼠标键
inline bool PlayModeUsesMouse(PlayMode mode) { return mode != PlayMode::Normal; }

// 组合名，例如 "Left+Z"、"Normal+,"。仅用于日志和 UI。
std::string ComboName(PlayMode mode, Key key);

// 该虚拟键在发送时是否需要加 KEYEVENTF_EXTENDEDKEY 前缀
// （方向键、Insert/Delete/Home/End/PageUp/PageDown、右 Ctrl/Alt 等）。
// Phase 1 的 8 个键都不需要，但事件合成逻辑需要这个判断，先放在这里备用。
bool IsExtendedVirtualKey(unsigned short vk);

}  // namespace hp
