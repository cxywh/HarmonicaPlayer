#pragma once

// 音符与时值。
//
// 设计原则（对应需求书第四、五、六、三十一节）：
//   1. Note 只存“绝对时间”——startTime / duration 都是从歌曲开始计算的秒数，
//      不存“相对上一个音符多久”。播放器按绝对目标时间调度，误差不会累积。
//   2. 时值（全/二分/四分/八分/十六分…）只用于“作者视角”的记谱，
//      在构建时间轴时一次性换算成绝对秒，换完就不再有“拍”的概念。

#include <string>

#include "core/Types.h"

namespace hp {

// ---------------------------------------------------------------------------
// 时值：以“全音符 = 1”为基准，用分母表示
//   Whole = 1、Half = 2、Quarter = 4、Eighth = 8、Sixteenth = 16、ThirtySecond = 32
// ---------------------------------------------------------------------------
enum class NoteValue : int {
    Whole = 1,
    Half = 2,
    Quarter = 4,
    Eighth = 8,
    Sixteenth = 16,
    ThirtySecond = 32
};

const char* NoteValueName(NoteValue value);         // "Whole" / "Quarter" …
const char* NoteValueDisplayName(NoteValue value);  // "全音符" / "四分音符" …

// 该时值等于多少“拍”（以四分音符为 1 拍）：Whole=4、Half=2、Quarter=1、Eighth=0.5 …
double NoteValueToBeats(NoteValue value);

// 该时值等于多少“拍”，但以 beatUnit 指定的时值为 1 拍
// （6/8 常见做法是以八分音符为一拍，此时四分音符 = 2 拍）
double NoteValueBeats(NoteValue value, NoteValue beatUnit);

// 文本解析，支持："1/4"、"4"、"QUARTER"、"四分"、"四分音符"
bool ParseNoteValue(const std::string& text, NoteValue& out);

// 全部时值的中文列表，用于 UI 下拉
std::string NoteValueListText();

// ---------------------------------------------------------------------------
// 一个音符
// ---------------------------------------------------------------------------
struct Note {
    Key key = Key::Z;
    PlayMode mode = PlayMode::Normal;
    double startTime = 0.0;  // 绝对时间（秒），从歌曲开始计算
    double duration = 0.0;   // 按住时长（秒）

    double EndTime() const { return startTime + duration; }
};

// 合法性检查：时长必须为正、起始时间不能为负
bool NoteIsValid(const Note& note, std::string* reason = nullptr);

// 例如 "Left+Z  起音 0.500s  持续 0.250s"
std::string NoteToString(const Note& note);

}  // namespace hp
