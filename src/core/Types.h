#pragma once

// 口琴演奏的“领域类型”：8 个音调、6 种演奏模式、3 个鼠标键。
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
// 6 种演奏方式（鼠标是“修饰键”，按住才临时改变音高）
//   Normal      : 直接按键（本音）
//   Left        : 按住鼠标左键 + 按键（低一个八度）
//   Middle      : 按住鼠标中键 + 按键（升半音，钢琴黑键）
//   Right       : 按住鼠标右键 + 按键（高一个八度）
//   LeftMiddle  : 同时按住左键 + 中键 + 按键（低一个八度再升半音）
//   RightMiddle : 同时按住右键 + 中键 + 按键（高一个八度再升半音）
//
// 顺序 = “编号 1..6”（ParsePlayMode 的数字别名）与界面上按钮的顺序；
// 后两个是组合模式，要**同时按住两个鼠标键**（见 MouseButtonsOf）。
// ---------------------------------------------------------------------------
enum class PlayMode : int {
    Normal = 0,
    Left = 1,
    Middle = 2,
    Right = 3,
    LeftMiddle = 4,
    RightMiddle = 5
};
constexpr int kPlayModeCount = 6;

// 一个演奏方式最多要按住几个鼠标键（组合模式 = 2）
constexpr int kMaxMouseButtonsPerMode = 2;

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
// 名字（JSON / 日志用）："Normal" / "Left" / "Middle" / "Right" / "LeftMiddle" / "RightMiddle"
const char* PlayModeName(PlayMode mode);
// 界面用中文： "正常" / "左键" / "中键" / "右键" / "左中键" / "右中键"
const char* PlayModeDisplayName(PlayMode mode);
// 英文名、缩写（N/L/MID/R/LM/RM）、数字 1~6、中文别名（正常/左键/中键/右键/左中/中左/右中/中右…）
bool        ParsePlayMode(const std::string& text, PlayMode& out);

// ---- MouseButton ----
const char* MouseButtonName(MouseButton button);
const char* MouseButtonDisplayName(MouseButton button);

// 某个演奏模式需要按住的鼠标键；Normal 返回 false（不碰鼠标）。
// 组合模式（LeftMiddle/RightMiddle）这里返回**先按下的那个键**，
// 要拿到全部键请用下面的 MouseButtonsOf。
bool MouseButtonOf(PlayMode mode, MouseButton& out);

// 某个演奏模式要按住的**全部**鼠标键：按“先按下的在前”的顺序填进 out
// （容量必须是 kMaxMouseButtonsPerMode），返回实际个数：
//   0 = Normal（不碰鼠标）、1 = 单键模式、2 = 组合模式（八度键 + 中键）。
// InputController::PressMode/ReleaseMode 就是照它按下/抬起的 ——
// 组合模式因此真的同时按住两个鼠标键（缺了这个就会只按一个，游戏里音高不对）。
int MouseButtonsOf(PlayMode mode, MouseButton out[kMaxMouseButtonsPerMode]);

// 是否需要先按住鼠标键
inline bool PlayModeUsesMouse(PlayMode mode) { return mode != PlayMode::Normal; }

// 组合名，例如 "Left+Z"、"Normal+,"。仅用于日志和 UI。
std::string ComboName(PlayMode mode, Key key);

// 该虚拟键在发送时是否需要加 KEYEVENTF_EXTENDEDKEY 前缀
// （方向键、Insert/Delete/Home/End/PageUp/PageDown、右 Ctrl/Alt 等）。
// Phase 1 的 8 个键都不需要，但事件合成逻辑需要这个判断，先放在这里备用。
bool IsExtendedVirtualKey(unsigned short vk);

}  // namespace hp
