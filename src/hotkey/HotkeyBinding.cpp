#include "hotkey/HotkeyBinding.h"

#include <cstdio>
#include <cstdlib>

#include "core/Platform.h"
#include "core/Types.h"   // VirtualKeyDisplayName：键名表只有一份实现

namespace hp {
namespace {

std::string Trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string ToUpperAscii(const std::string& text) {
    std::string upper = text;
    for (char& c : upper) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return upper;
}

bool ParseFunctionKey(const std::string& token, unsigned& vk) {
    if (token.size() < 2 || token[0] != 'F') {
        return false;
    }
    for (size_t i = 1; i < token.size(); ++i) {
        if (token[i] < '0' || token[i] > '9') {
            return false;
        }
    }
    const int number = std::atoi(token.c_str() + 1);
    if (number < 1 || number > 24) {
        return false;
    }
    vk = static_cast<unsigned>(VK_F1 + number - 1);
    return true;
}

}  // namespace

const char* HotkeyActionName(HotkeyAction action) {
    switch (action) {
        case HotkeyAction::Play:
            return "Play";
        case HotkeyAction::Stop:
            return "Stop";
        case HotkeyAction::Restart:
            return "Restart";
        case HotkeyAction::Previous:
            return "Previous";
        case HotkeyAction::Next:
            return "Next";
        case HotkeyAction::Pause:
            return "Pause";
        default:
            return "Unknown";
    }
}

const char* HotkeyActionDisplayName(HotkeyAction action) {
    switch (action) {
        case HotkeyAction::Play:
            return "播放 / 开始";
        case HotkeyAction::Stop:
            return "停止";
        case HotkeyAction::Restart:
            return "从头开始";
        case HotkeyAction::Previous:
            return "上一首";
        case HotkeyAction::Next:
            return "下一首";
        case HotkeyAction::Pause:
            return "暂停 / 继续";
        default:
            return "未知动作";
    }
}

const char* HotkeyActionDescription(HotkeyAction action) {
    switch (action) {
        case HotkeyAction::Play:
            return "停着的时候按它开始演奏（正在演奏时按它不打断）";
        case HotkeyAction::Stop:
            return "立刻停止演奏并释放所有被工具按住的键";
        case HotkeyAction::Restart:
            return "回到曲子开头重新演奏";
        case HotkeyAction::Previous:
            return "演奏中按它切到 songs 列表里的上一首（首尾循环，接着往下演）";
        case HotkeyAction::Next:
            return "演奏中按它切到 songs 列表里的下一首（首尾循环，接着往下演）";
        case HotkeyAction::Pause:
            return "演奏中按它暂停（位置留着），再按一次从原地继续；没在演奏时按它什么也不做";
        default:
            return "";
    }
}

bool HotkeyBinding::IsValid() const {
    return IsSupportedVirtualKey(vk);
}

bool HotkeyBinding::IsRisky() const {
    if (modifiers != kModifierNone) {
        return false;
    }
    // 无修饰键的字母/数字/空格等，会把这个键从游戏手里抢走
    return (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') || vk == VK_SPACE ||
           vk == VK_RETURN || vk == VK_TAB || vk == VK_ESCAPE ||
           (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) ||
           vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN;
}

std::string HotkeyBinding::ModifierText() const {
    std::string text;
    if (HasCtrl()) {
        text += "Ctrl+";
    }
    if (HasAlt()) {
        text += "Alt+";
    }
    if (HasShift()) {
        text += "Shift+";
    }
    if (HasWin()) {
        text += "Win+";
    }
    return text;
}

std::string HotkeyBinding::ToString() const {
    if (IsEmpty()) {
        return "(未绑定)";
    }
    return ModifierText() + VirtualKeyName(vk);
}

HotkeyBinding DefaultHotkey(HotkeyAction action) {
    HotkeyBinding binding;
    switch (action) {
        case HotkeyAction::Play:
            binding.vk = VK_NUMPAD5;   // 需求书第十三节：F6 = 播放 / 开始
            break;
        case HotkeyAction::Stop:
            binding.vk = VK_NUMPAD4;   // F7 = 停止
            break;
        case HotkeyAction::Restart:
            binding.vk = VK_NUMPAD0;   // F8 = 从头开始
            break;
        case HotkeyAction::Previous:
            binding.vk = VK_UP;   // F9 = 上一首（新增，可改键）
            break;
        case HotkeyAction::Next:
            binding.vk = VK_DOWN;  // F10 = 下一首（新增，可改键）
            break;
        case HotkeyAction::Pause:
            binding.vk = VK_NUMPAD6;   // F5 = 暂停 / 继续（后加的，可改键）
            break;
        default:
            binding.vk = 0;
            break;
    }
    binding.modifiers = kModifierNone;
    return binding;
}

std::string VirtualKeyName(unsigned vk) {
    if (vk == 0) {
        return "(未绑定)";
    }
    // 键名表只有一份实现（core/Types.cpp::VirtualKeyDisplayName），
    // 这里只多做一件事：未绑定的显示成"(未绑定)"而不是"VK_0x00"。
    return VirtualKeyDisplayName(vk);
}

// 小键盘记号：NUM0 / NUMPAD0 / NUM 0（空格在切词时已被去掉）都认
static bool ParseNumpadToken(const std::string& token, unsigned& vk) {
    std::string body = token;
    if (body.rfind("NUMPAD", 0) == 0) {
        body = body.substr(6);
    } else if (body.rfind("NUM", 0) == 0 && body != "NUMLOCK") {
        body = body.substr(3);
    } else {
        return false;
    }
    if (body.size() == 1 && body[0] >= '0' && body[0] <= '9') {
        vk = static_cast<unsigned>(VK_NUMPAD0 + (body[0] - '0'));
        return true;
    }
    return false;
}

bool IsSupportedVirtualKey(unsigned vk) {
    if (vk >= VK_F1 && vk <= VK_F24) {
        return true;
    }
    if (vk >= 'A' && vk <= 'Z') {
        return true;
    }
    if (vk >= '0' && vk <= '9') {
        return true;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        return true;    // 小键盘 0-9（NUM 0 / NUM 1 …）
    }
    if (vk == VK_CLEAR) {
        return true;    // 小键盘 5（NumLock 关时）
    }
    switch (vk) {
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
            return true;    // 方向键：以前只在显示名里有，能看见却设不上
        case VK_SPACE:
        case VK_RETURN:
        case VK_ESCAPE:
        case VK_TAB:
        case VK_BACK:
        case VK_DELETE:
        case VK_INSERT:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
            return true;
        default:
            return false;
    }
}

std::vector<unsigned> SupportedVirtualKeys() {
    std::vector<unsigned> keys;
    for (unsigned vk = VK_F1; vk <= VK_F24; ++vk) {
        keys.push_back(vk);
    }
    for (unsigned vk = 'A'; vk <= 'Z'; ++vk) {
        keys.push_back(vk);
    }
    for (unsigned vk = '0'; vk <= '9'; ++vk) {
        keys.push_back(vk);
    }
    const unsigned named[] = {VK_SPACE,  VK_TAB,   VK_RETURN, VK_ESCAPE,
                              VK_BACK,   VK_DELETE, VK_INSERT, VK_HOME,
                              VK_END,    VK_PRIOR,  VK_NEXT,   VK_CLEAR,
                              VK_LEFT,   VK_RIGHT,  VK_UP,     VK_DOWN};
    for (unsigned vk = VK_NUMPAD0; vk <= VK_NUMPAD9; ++vk) {
        keys.push_back(vk);
    }
    for (unsigned vk : named) {
        keys.push_back(vk);
    }
    return keys;
}

bool ParseHotkey(const std::string& text, HotkeyBinding& out, std::string* error) {
    const std::string raw = Trim(text);
    if (raw.empty()) {
        if (error != nullptr) {
            *error = "热键是空的（想取消绑定请输入 none）";
        }
        return false;
    }
    if (ToUpperAscii(raw) == "NONE" || raw == "-" || ToUpperAscii(raw) == "未绑定") {
        out = HotkeyBinding{};   // vk = 0 → 取消绑定
        return true;
    }

    HotkeyBinding binding;
    size_t start = 0;
    bool sawKey = false;
    while (start <= raw.size()) {
        const size_t plus = raw.find('+', start);
        const std::string token = ToUpperAscii(Trim(plus == std::string::npos
                                                        ? raw.substr(start)
                                                        : raw.substr(start, plus - start)));
        if (!token.empty()) {
            if (sawKey) {
                if (error != nullptr) {
                    *error = "只能有一个主键（已经是 " + VirtualKeyName(binding.vk) + "，又多了一个 " +
                             token + "）";
                }
                return false;
            }
            if (token == "CTRL" || token == "CONTROL") {
                binding.modifiers |= kModifierCtrl;
            } else if (token == "ALT") {
                binding.modifiers |= kModifierAlt;
            } else if (token == "SHIFT") {
                binding.modifiers |= kModifierShift;
            } else if (token == "WIN" || token == "WINDOWS" || token == "LOGO" || token == "SUPER") {
                binding.modifiers |= kModifierWin;
            } else {
                unsigned vk = 0;
                if (ParseFunctionKey(token, vk)) {
                    binding.vk = vk;
                } else if (token.size() == 1 && IsSupportedVirtualKey(token[0])) {
                    binding.vk = static_cast<unsigned>(token[0]);
                } else if (token == "SPACE" || token == "空格") {
                    binding.vk = VK_SPACE;
                } else if (token == "ENTER" || token == "RETURN" || token == "回车") {
                    binding.vk = VK_RETURN;
                } else if (token == "ESC" || token == "ESCAPE") {
                    binding.vk = VK_ESCAPE;
                } else if (token == "TAB") {
                    binding.vk = VK_TAB;
                } else if (token == "BACKSPACE" || token == "BACK") {
                    binding.vk = VK_BACK;
                } else if (token == "DELETE" || token == "DEL") {
                    binding.vk = VK_DELETE;
                } else if (token == "INSERT" || token == "INS") {
                    binding.vk = VK_INSERT;
                } else if (token == "LEFT" || token == "左") {
                    binding.vk = VK_LEFT;
                } else if (token == "RIGHT" || token == "右") {
                    binding.vk = VK_RIGHT;
                } else if (token == "UP" || token == "上") {
                    binding.vk = VK_UP;
                } else if (token == "DOWN" || token == "下") {
                    binding.vk = VK_DOWN;
                } else if (token == "NUMLOCK") {
                    binding.vk = VK_NUMLOCK;
                } else if (ParseNumpadToken(token, vk)) {
                    binding.vk = vk;
                } else if (token == "HOME") {
                    binding.vk = VK_HOME;
                } else if (token == "END") {
                    binding.vk = VK_END;
                } else if (token == "PAGEUP" || token == "PGUP") {
                    binding.vk = VK_PRIOR;
                } else if (token == "PAGEDOWN" || token == "PGDN") {
                    binding.vk = VK_NEXT;
                } else if (token.size() > 2 && token[0] == '0' &&
                           (token[1] == 'X' || token[1] == 'x')) {
                    const long value = std::strtol(token.c_str() + 2, nullptr, 16);
                    if (value <= 0 || value > 0xFF) {
                        if (error != nullptr) {
                            *error = "虚拟键码超出范围：" + token;
                        }
                        return false;
                    }
                    binding.vk = static_cast<unsigned>(value);
                } else {
                    if (error != nullptr) {
                        *error = "认不出这一段：" + token +
                                 "（可写 F1-F24、单个字母或数字、Space/Enter/Esc/Tab 等）";
                    }
                    return false;
                }
                sawKey = true;
            }
        }
        if (plus == std::string::npos) {
            break;
        }
        start = plus + 1;
    }

    if (binding.vk == 0) {
        if (error != nullptr) {
            *error = "只写了修饰键，没有主键（例如应该写 Ctrl+F6）";
        }
        return false;
    }
    if (!IsSupportedVirtualKey(binding.vk)) {
        if (error != nullptr) {
            *error = "这个键不支持设置：" + VirtualKeyName(binding.vk);
        }
        return false;
    }
    out = binding;
    return true;
}

std::string HotkeyHelpText() {
    // 短一点：设置窗口那一行原来放不下被截断（实测肉眼可见尾巴没了），
    // 而且现在热键是"按下即设置"，用不着在界面里枚举写法。
    return "支持的写法：F6、Ctrl+F6、Ctrl+Shift+F8、Alt+F1、Space、Enter、Esc、Tab；none = 取消绑定。";
}

bool ParseIgnoredKey(const std::string& text, unsigned& vk, std::string* error) {
    // 单独的修饰键/锁定键也能进排除列表（ctrl / shift / alt / capslock / tab 等）
    {
        std::string t;
        t.reserve(text.size());
        for (char ch : text) {
            t.push_back(static_cast<char>((ch >= 'a' && ch <= 'z') ? ch - 'a' + 'A' : ch));
        }
        const size_t b = t.find_first_not_of(" \t");
        const size_t e = t.find_last_not_of(" \t");
        t = (b == std::string::npos) ? std::string() : t.substr(b, e - b + 1);
        static const struct { const char* name; unsigned vk; } kStandalone[] = {
            {"CTRL", VK_CONTROL},   {"CONTROL", VK_CONTROL}, {"SHIFT", VK_SHIFT},   {"ALT", VK_MENU},
            {"CAPSLOCK", VK_CAPITAL}, {"CAPS", VK_CAPITAL},  {"TAB", VK_TAB},       {"SPACE", VK_SPACE},
            {"ENTER", VK_RETURN},   {"RETURN", VK_RETURN},   {"ESC", VK_ESCAPE},    {"ESCAPE", VK_ESCAPE},
            {"BACKSPACE", VK_BACK}, {"LWIN", VK_LWIN},       {"RWIN", VK_RWIN},     {"WIN", VK_LWIN},
            {"APPS", VK_APPS},
        };
        for (const auto& entry : kStandalone) {
            if (t == entry.name) {
                vk = entry.vk;
                return true;
            }
        }
    }
    HotkeyBinding binding;
    std::string parseError;
    if (!ParseHotkey(text, binding, &parseError)) {
        if (error != nullptr) {
            *error = parseError;
        }
        return false;
    }
    if (binding.IsEmpty()) {
        if (error != nullptr) {
            *error = "排除键不能是 none（列表空着就等于没有排除键）";
        }
        return false;
    }
    if (binding.modifiers != kModifierNone) {
        if (error != nullptr) {
            *error = "排除键只认单个键，不带 Ctrl / Alt / Shift / Win";
        }
        return false;
    }
    vk = binding.vk;
    return true;
}

bool AddIgnoreKey(std::vector<std::string>& keys, const std::string& text, std::string* message) {
    unsigned vk = 0;
    std::string error;
    if (!ParseIgnoredKey(text, vk, &error)) {
        if (message != nullptr) {
            *message = error;
        }
        return false;
    }
    const std::string name = VirtualKeyName(vk);
    for (const std::string& existing : keys) {
        unsigned other = 0;
        if (ParseIgnoredKey(existing, other, nullptr) && other == vk) {
            if (message != nullptr) {
                *message = name + " 已经在排除列表里了。";
            }
            return false;
        }
    }
    if (keys.size() >= kMaxIgnoredKeys) {
        if (message != nullptr) {
            *message = "排除键最多 " + std::to_string(kMaxIgnoredKeys) + " 个。";
        }
        return false;
    }
    keys.push_back(name);
    if (message != nullptr) {
        *message = "已加入排除键：" + name + "（按它不会再打断演奏）";
    }
    return true;
}

bool RemoveIgnoreKeyAt(std::vector<std::string>& keys, size_t index) {
    if (index >= keys.size()) {
        return false;
    }
    keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

bool IsIgnoredKey(unsigned vk, const std::vector<unsigned>& keys) {
    if (vk == 0) {
        return false;
    }
    for (unsigned key : keys) {
        if (key == vk) {
            return true;
        }
    }
    return false;
}

size_t SanitizeIgnoredKeys(std::vector<std::string>& keys) {
    std::vector<std::string> kept;
    size_t dropped = 0;
    for (const std::string& key : keys) {
        unsigned vk = 0;
        if (!ParseIgnoredKey(key, vk, nullptr)) {
            ++dropped;
            continue;
        }
        const std::string name = VirtualKeyName(vk);
        bool duplicate = false;
        for (const std::string& existing : kept) {
            unsigned other = 0;
            if (ParseIgnoredKey(existing, other, nullptr) && other == vk) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            ++dropped;
            continue;
        }
        kept.push_back(name);
    }
    keys.swap(kept);
    return dropped;
}

}  // namespace hp
