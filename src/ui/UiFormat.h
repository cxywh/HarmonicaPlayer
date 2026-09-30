#pragma once

// UI 文案与格式化（Phase 7）。
//
// 这一层刻意**不包含任何 Win32**：所有数字/文案的换算都能在没有窗口的情况下自检
// （`dist\ui_test.exe`），窗口只负责把它们画出来。需求书三十六节那张界面草图里的
// 每一项（状态、当前音符 23 / 128、进度 65%、BPM、速度、表格的 时间/音符/模式/时值）
// 都在这里有对应的纯函数。
//
// 文案约定（用户明确要求）：中文、**禁 emoji**、状态用文字 + 颜色表达、不刷屏。

#include <string>

#include "core/Types.h"
#include "player/PlaybackTypes.h"
#include "song/Note.h"

namespace hp {
namespace ui {

// ---- 数字与时间 ----
std::string FormatSeconds3(double seconds);   // 0.25  → "0.250"（表格里的时间列）
std::string FormatClock(double seconds);      // 12.345 → "00:12.345"（进度显示）
std::string FormatBeats(double beats);        // 1.25  → "1.25"
// 长句折行（给 STATIC 标签用）：Windows 的 STATIC 对中文长句不折行，右边直接被切掉，
// 所以自己按"一行放得下多少个字"折好、插入换行。半角算 0.55 个字，全角算 1 个字；
// 最多 maxLines 行，还放不下就在末尾加"…"。折行点优先取标点、空格、斜杠后面。
std::string WrapLabelText(const std::string& text, double widthInChars, int maxLines);
int PercentOf(int current, int total);        // 23/128 → 17（total<=0 时 0）
std::string FormatPercent(int percent);       // 17 → "17%"
std::string FormatSpeedText(double speed);    // 1.0 → "100%"、0.75 → "75%"
std::string FormatMs(double ms);              // 500 -> "500"；666.667 -> "666.7"

// 速度输入解析：接受 "100%"、"100"、"1.0"、"0.5"，返回是否成功；成功时写 out
bool ParseSpeedText(const std::string& text, double& out, std::string& error);

// ---- 播放状态 ----
// "状态：演奏中" / "状态：已停止　原因：检测到用户输入：Space"
std::string StatusLine(PlaybackState state, StopReason reason, const std::string& detail);
// "当前音符：23 / 128"
std::string CurrentNoteText(int current, int total);

// 进度条文本（日志与自检用；窗口里画的是真进度条）
std::string ProgressBarText(int percent, int width = 16);

// ---- 音符表格的四列（需求书三十六节：时间 / 音符 / 模式 / 时值）----
std::string NoteTimeCell(double seconds);                       // "0.250"
std::string NoteKeyCell(Key key);                               // "Z" / ","
std::string NoteModeCell(PlayMode mode);                        // "Normal"
std::string NoteValueCell(double durationBeats, NoteValue beatUnit);  // "1/4（1.00 拍）"

// 时值列反查：拍数 → 最接近的时值名（"四分" → 用于下拉框回填）
std::string NearestValueName(double durationBeats, NoteValue beatUnit);
// 时值名 → 拍数（"1/4" / "QUARTER" / "四分"），失败返回 false
bool ValueBeatsByName(const std::string& text, NoteValue beatUnit, double& out);

// ---- 热键提示 ----
std::string HotkeyHint(const std::string& play, const std::string& pause, const std::string& stop,
                       const std::string& restart, const std::string& previous, const std::string& next);

// 自检用：字符串里是否含 emoji（界面禁 emoji 的自动守卫）
bool ContainsEmoji(const std::string& text);

}  // namespace ui
}  // namespace hp
