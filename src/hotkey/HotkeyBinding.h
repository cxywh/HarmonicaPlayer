#pragma once

// 热键绑定：一个动作对应"哪个键 + 哪些修饰键"（需求书第十三节）。
//
//   默认：F6 = 播放 / 开始　F7 = 停止　F8 = 从头开始
//   并且允许用户自定义（第十三节原文"热键之后应该允许用户自定义"）。
//
// 这里只有"键是什么"这一层语义，不含任何 Windows 注册状态，
// 所以它可以被配置文件、设置界面、自检工具自由地传来传去。

#include <string>
#include <vector>

namespace hp {

// ---------------------------------------------------------------------------
// 热键动作：与播放器的命令一一对应
//   前三个是需求书第十三节约定的（F6/F7/F8）；
//   后两个是演奏中换曲用的（F9/F10），同样可以被用户改键。
// ---------------------------------------------------------------------------
enum class HotkeyAction : int {
    Play = 0,      // F6 播放 / 开始
    Stop = 1,      // F7 停止
    Restart = 2,   // F8 从头开始
    Previous = 3,  // F9 上一首（演奏中切曲，不停）
    Next = 4,      // F10 下一首
    Pause = 5,     // F5 暂停 / 继续（后加的，放在末尾是为了不打乱已存配置里的编号）
    Count = 6
};

constexpr int kHotkeyActionCount = 6;

const char* HotkeyActionName(HotkeyAction action);         // "Play"
const char* HotkeyActionDisplayName(HotkeyAction action);  // "播放 / 开始"
const char* HotkeyActionDescription(HotkeyAction action);  // 给设置界面用的一句话

// ---------------------------------------------------------------------------
// 修饰键（位组合）
// ---------------------------------------------------------------------------
enum HotkeyModifier : unsigned {
    kModifierNone = 0,
    kModifierAlt = 1,
    kModifierCtrl = 2,
    kModifierShift = 4,
    kModifierWin = 8,
};

// ---------------------------------------------------------------------------
// 一个绑定
// ---------------------------------------------------------------------------
struct HotkeyBinding {
    unsigned vk = 0;           // Windows 虚拟键码；0 = 未绑定
    unsigned modifiers = kModifierNone;

    bool IsEmpty() const { return vk == 0; }
    bool IsValid() const;      // 有键，且这个键在我们支持的范围内
    bool HasCtrl() const { return (modifiers & kModifierCtrl) != 0; }
    bool HasAlt() const { return (modifiers & kModifierAlt) != 0; }
    bool HasShift() const { return (modifiers & kModifierShift) != 0; }
    bool HasWin() const { return (modifiers & kModifierWin) != 0; }

    // 没有修饰键的普通字母/数字/空格：注册成全局热键会把那个键从游戏里抢走，
    // 所以允许设置但要给用户一句提醒（见 IsRisky）
    bool IsRisky() const;

    // "Ctrl+Shift+F6"；未绑定 → "(未绑定)"
    std::string ToString() const;
    // 修饰键部分（按 Ctrl+Alt+Shift+Win 的固定顺序）
    std::string ModifierText() const;

    bool Equals(const HotkeyBinding& other) const {
        return vk == other.vk && modifiers == other.modifiers;
    }
};

// 默认绑定（需求书第十三节）
HotkeyBinding DefaultHotkey(HotkeyAction action);

// ---------------------------------------------------------------------------
// 解析与显示
// ---------------------------------------------------------------------------
// 可写："F6"、"f6"、"Ctrl+F6"、"Ctrl+Shift+F8"、"Alt+F1"、"Win+A"、"Space"
// 失败时把原因写进 error（中文，可直接显示）
bool ParseHotkey(const std::string& text, HotkeyBinding& out, std::string* error = nullptr);

// 虚拟键码 → 名字（"F6" / "A" / "Space"）；不认识的键给 "VK_0x77"
std::string VirtualKeyName(unsigned vk);

// 是否是我们支持设置的键（F1–F24、A–Z、0–9、Space、Enter、Escape、Tab）
bool IsSupportedVirtualKey(unsigned vk);

// 所有可设置键的清单，用于设置界面的下拉框
std::vector<unsigned> SupportedVirtualKeys();

// 设置界面用的一段帮助文本
std::string HotkeyHelpText();

// ---------------------------------------------------------------------------
// 排除键位：这些键按下时不算"用户输入打断演奏"（游戏里的走路 / 跳跃键）
//   只支持单个键（不带 Ctrl / Alt / Shift / Win）——急停判定跑在钩子回调里，
//   那一刻只看得到键码，看不到修饰键状态，带修饰键的写法没有意义。
// ---------------------------------------------------------------------------
constexpr size_t kMaxIgnoredKeys = 32;

// 解析一个排除键（"W" / "Space" / "F1"…）；none 和带修饰键的写法都算错
bool ParseIgnoredKey(const std::string& text, unsigned& vk, std::string* error);

// 往列表里加一个：自动去重、把名字规范化成 VirtualKeyName 的写法
bool AddIgnoreKey(std::vector<std::string>& keys, const std::string& text, std::string* message);

// 删掉第 index 个（越界返回 false）
bool RemoveIgnoreKeyAt(std::vector<std::string>& keys, size_t index);

// 清掉读进来的脏数据（解析不出来的、重复的），返回丢掉几个
size_t SanitizeIgnoredKeys(std::vector<std::string>& keys);

// 这个键码在不在排除集合里（急停判定每来一个键就要问一次，所以单独拎出来好测）
bool IsIgnoredKey(unsigned vk, const std::vector<unsigned>& keys);

}  // namespace hp
