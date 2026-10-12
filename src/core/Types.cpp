#include "core/Types.h"

#include <cstdio>

#include "core/Platform.h"   // 必须在其它头之前提供 VK_* 常量

namespace hp {

std::string VirtualKeyDisplayName(unsigned vk) {
    if (vk == 0) {
        return "VK_0x00";
    }
    // 功能键 F1-F24
    if (vk >= VK_F1 && vk <= VK_F24) {
        return "F" + std::to_string(vk - VK_F1 + 1);
    }
    // 字母与数字
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        return std::string(1, static_cast<char>(vk));
    }
    // 小键盘数字：用户的习惯叫法是 "NUM 0 / NUM 1"，这里统一显示成 Num0..Num9
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        return "Num" + std::to_string(vk - VK_NUMPAD0);
    }
    // 需求书十五节点名要认的键 + 常用键
    switch (vk) {
        case VK_SPACE:   return "Space";
        case VK_RETURN:  return "Enter";
        case VK_ESCAPE:  return "Esc";
        case VK_TAB:     return "Tab";
        case VK_BACK:    return "Backspace";
        case VK_DELETE:  return "Delete";
        case VK_INSERT:  return "Insert";
        case VK_HOME:    return "Home";
        case VK_END:     return "End";
        case VK_PRIOR:   return "PageUp";
        case VK_NEXT:    return "PageDown";
        case VK_LEFT:    return "Left";
        case VK_RIGHT:   return "Right";
        case VK_UP:      return "Up";
        case VK_DOWN:    return "Down";
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_SHIFT:   return "Shift";
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_CONTROL: return "Ctrl";
        case VK_LMENU:
        case VK_RMENU:
        case VK_MENU:    return "Alt";
        case VK_LWIN:
        case VK_RWIN:    return "Win";
        case VK_CAPITAL: return "CapsLock";
        case VK_NUMLOCK: return "NumLock";
        case VK_CLEAR:   return "NumClear";
        case VK_OEM_COMMA:  return "Comma";
        case VK_OEM_PERIOD: return "Period";
        case VK_OEM_1:      return "Semicolon";
        case VK_OEM_2:      return "Slash";
        case VK_OEM_3:      return "Grave";
        case VK_OEM_4:      return "BracketLeft";
        case VK_OEM_5:      return "Backslash";
        case VK_OEM_6:      return "BracketRight";
        case VK_OEM_7:      return "Quote";
        case VK_OEM_MINUS:  return "Minus";
        case VK_OEM_PLUS:   return "Equal";
        case VK_NUMPAD0:    return "Num0";
        case VK_NUMPAD1:    return "Num1";
        case VK_NUMPAD2:    return "Num2";
        case VK_NUMPAD3:    return "Num3";
        case VK_NUMPAD4:    return "Num4";
        case VK_NUMPAD5:    return "Num5";
        case VK_NUMPAD6:    return "Num6";
        case VK_NUMPAD7:    return "Num7";
        case VK_NUMPAD8:    return "Num8";
        case VK_NUMPAD9:    return "Num9";
        default:            break;
    }
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "VK_0x%02X", vk);
    return std::string(buffer);
}

namespace {

struct KeyEntry {
    Key key;
    unsigned short vk;
    const char* name;
    const char* display;
};

// ⚠ 顺序必须与 enum class Key 完全一致：Z X C V B N M ,
const KeyEntry kKeyTable[kKeyCount] = {
    {Key::Z,     'Z',          "Z", "Z"},
    {Key::X,     'X',          "X", "X"},
    {Key::C,     'C',          "C", "C"},
    {Key::V,     'V',          "V", "V"},
    {Key::B,     'B',          "B", "B"},
    {Key::N,     'N',          "N", "N"},
    {Key::M,     'M',          "M", "M"},
    {Key::Comma, VK_OEM_COMMA, ",", "逗号(,)"},
};

// ⚠ 顺序必须与 enum class PlayMode 完全一致：Normal Left Middle Right LeftMiddle RightMiddle
const char* kPlayModeName[kPlayModeCount] = {"Normal", "Left",  "Middle",
                                             "Right",  "LeftMiddle", "RightMiddle"};
const char* kPlayModeDisplay[kPlayModeCount] = {"正常", "左键", "中键", "右键", "左中键", "右中键"};

const char* kMouseName[kMouseButtonCount]    = {"Left", "Middle", "Right"};
const char* kMouseDisplay[kMouseButtonCount] = {"鼠标左键", "鼠标中键", "鼠标右键"};

std::string Trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' ||
                           text[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' ||
                           text[end - 1] == '\n')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// 只处理 ASCII，避免对 UTF-8 中文字节调用 toupper 造成未定义行为
std::string ToUpperAscii(const std::string& text) {
    std::string result = text;
    for (char& ch : result) {
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    }
    return result;
}

bool InKeyRange(int index)  { return index >= 0 && index < kKeyCount; }

}  // namespace

// ---------------------------------------------------------------------------
// Key
// ---------------------------------------------------------------------------
unsigned short VirtualKeyOf(Key key) {
    const int index = static_cast<int>(key);
    return InKeyRange(index) ? kKeyTable[index].vk : 0;
}

const char* KeyName(Key key) {
    const int index = static_cast<int>(key);
    return InKeyRange(index) ? kKeyTable[index].name : "?";
}

const char* KeyDisplayName(Key key) {
    const int index = static_cast<int>(key);
    return InKeyRange(index) ? kKeyTable[index].display : "?";
}

bool ParseKey(const std::string& text, Key& out) {
    const std::string trimmed = Trim(text);
    if (trimmed.empty()) {
        return false;
    }
    if (trimmed == "逗号") {           // 中文别名，先于大写转换处理（不碰多字节字符）
        out = Key::Comma;
        return true;
    }
    const std::string upper = ToUpperAscii(trimmed);
    if (upper == "COMMA") {
        out = Key::Comma;
        return true;
    }
    if (upper.size() == 1) {
        for (int i = 0; i < kKeyCount; ++i) {
            if (kKeyTable[i].name[0] == upper[0]) {
                out = static_cast<Key>(i);
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// PlayMode
// ---------------------------------------------------------------------------
const char* PlayModeName(PlayMode mode) {
    const int index = static_cast<int>(mode);
    return (index >= 0 && index < kPlayModeCount) ? kPlayModeName[index] : "?";
}

const char* PlayModeDisplayName(PlayMode mode) {
    const int index = static_cast<int>(mode);
    return (index >= 0 && index < kPlayModeCount) ? kPlayModeDisplay[index] : "?";
}

bool ParsePlayMode(const std::string& text, PlayMode& out) {
    const std::string raw = Trim(text);
    // 中文别名先比（不做大小写转换）。
    // 组合模式（八度 + 半音）先比："左键+中键" 这种写法里也含 "左键"，
    // 顺序反了会被前面的单键别名先吃掉。
    if (raw == "左中" || raw == "中左" || raw == "左中键" || raw == "左键+中键") {
        out = PlayMode::LeftMiddle;
        return true;
    }
    if (raw == "右中" || raw == "中右" || raw == "右中键" || raw == "右键+中键") {
        out = PlayMode::RightMiddle;
        return true;
    }
    if (raw == "正常") { out = PlayMode::Normal; return true; }
    if (raw == "左键" || raw == "左") { out = PlayMode::Left; return true; }
    if (raw == "中键" || raw == "中") { out = PlayMode::Middle; return true; }
    if (raw == "右键" || raw == "右") { out = PlayMode::Right; return true; }

    const std::string upper = ToUpperAscii(raw);
    // 组合模式：LM / RM 或写全名
    if (upper == "LEFTMIDDLE" || upper == "LEFTMID" || upper == "LM") {
        out = PlayMode::LeftMiddle;
        return true;
    }
    if (upper == "RIGHTMIDDLE" || upper == "RIGHTMID" || upper == "RM") {
        out = PlayMode::RightMiddle;
        return true;
    }
    if (upper == "NORMAL" || upper == "N") { out = PlayMode::Normal; return true; }
    if (upper == "LEFT"   || upper == "L") { out = PlayMode::Left;   return true; }
    // 注意：这里故意不接受单字母 "M" —— 文本谱里 "M" 表示音调 M，
    // 中键模式请写 MIDDLE / 中键，避免 "M+," 之类的歧义。
    if (upper == "MIDDLE" || upper == "MID") { out = PlayMode::Middle; return true; }
    if (upper == "RIGHT"  || upper == "R")   { out = PlayMode::Right;  return true; }

    // 数字形式：1/2/3/4/5/6（沿用界面里“正常/左键/中键/右键/左中键/右中键”的编号）
    if (upper == "1") { out = PlayMode::Normal;      return true; }
    if (upper == "2") { out = PlayMode::Left;        return true; }
    if (upper == "3") { out = PlayMode::Middle;      return true; }
    if (upper == "4") { out = PlayMode::Right;       return true; }
    if (upper == "5") { out = PlayMode::LeftMiddle;  return true; }
    if (upper == "6") { out = PlayMode::RightMiddle; return true; }
    return false;
}

// ---------------------------------------------------------------------------
// MouseButton
// ---------------------------------------------------------------------------
const char* MouseButtonName(MouseButton button) {
    const int index = static_cast<int>(button);
    return (index >= 0 && index < kMouseButtonCount) ? kMouseName[index] : "?";
}

const char* MouseButtonDisplayName(MouseButton button) {
    const int index = static_cast<int>(button);
    return (index >= 0 && index < kMouseButtonCount) ? kMouseDisplay[index] : "?";
}

int MouseButtonsOf(PlayMode mode, MouseButton out[kMaxMouseButtonsPerMode]) {
    if (out == nullptr) {
        return 0;
    }
    switch (mode) {
        case PlayMode::Normal:
            return 0;
        case PlayMode::Left:
            out[0] = MouseButton::Left;
            return 1;
        case PlayMode::Middle:
            out[0] = MouseButton::Middle;
            return 1;
        case PlayMode::Right:
            out[0] = MouseButton::Right;
            return 1;
        // 组合模式：先按八度键（左/右），再按中键（升半音）；松开时反过来。
        case PlayMode::LeftMiddle:
            out[0] = MouseButton::Left;
            out[1] = MouseButton::Middle;
            return 2;
        case PlayMode::RightMiddle:
            out[0] = MouseButton::Right;
            out[1] = MouseButton::Middle;
            return 2;
    }
    return 0;
}

bool MouseButtonOf(PlayMode mode, MouseButton& out) {
    MouseButton buttons[kMaxMouseButtonsPerMode] = {};
    if (MouseButtonsOf(mode, buttons) <= 0) {
        return false;   // Normal：不碰鼠标
    }
    out = buttons[0];   // 组合模式给出先按下的那个键（全部键见 MouseButtonsOf）
    return true;
}

std::string ComboName(PlayMode mode, Key key) {
    std::string result = PlayModeName(mode);
    result += '+';
    result += KeyName(key);
    return result;
}

bool IsExtendedVirtualKey(unsigned short vk) {
    switch (vk) {
        case VK_RMENU: case VK_RCONTROL: case VK_INSERT: case VK_DELETE:
        case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_NUMLOCK: case VK_DIVIDE: case VK_SNAPSHOT:
        case VK_LWIN: case VK_RWIN: case VK_APPS:
            return true;
        default:
            return false;
    }
}

}  // namespace hp
