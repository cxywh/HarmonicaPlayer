#pragma once

// BeatMath：BPM / 拍号 / 时值 / 速度 的换算（Phase 8，需求书二十七、二十八、二十九、三十节）。
//
// 这一层是**纯逻辑**（不含任何 Win32，也不碰播放器），窗口和自检共用同一份实现：
//   - `dist\ui_test.exe --all` 里的 Phase 8 分组直接断言这里的每个函数；
//   - 界面只负责把结果画出来，不自己算毫秒、不自己数小节。
//
// 两个必须记住的约定（整个项目统一）：
//   1. “一拍”指的是**拍单位** `Song::beatUnit`，不是固定的四分音符。
//      6/8 的曲子 beatUnit = 八分音符，于是 120 BPM 下 1 拍 = 0.500 秒、一小节 6 拍 = 3.000 秒。
//      校验（`Song::Validate`）会强制“拍单位与拍号分母一致”，所以 4/4 → 四分、6/8 → 八分。
//   2. **BPM 永远指“每分钟多少拍”**（拍 = 拍单位）。速度（Playback Speed）不改 BPM，
//      它改的是时间轴映射：有效 BPM = BPM × 速度。

#include <string>

#include "song/Note.h"
#include "song/Song.h"

namespace hp {
namespace beat {

// ---- 速度（需求书二十九节：50% ~ 200%，六档预置 + 自定义）----
constexpr double kSpeedMin = 0.5;
constexpr double kSpeedMax = 2.0;
constexpr int kSpeedPresetCount = 6;
double SpeedPreset(int index);        // 0..5 → 0.5 / 0.75 / 1.0 / 1.25 / 1.5 / 2.0；越界返回 1.0
std::string SpeedPresetText(int index);   // 0..5 → "50%" … "200%"
// 速度范围校验：不在 50%~200% 一律拒绝（error 里写清楚为什么），成功时写 out
bool ClampSpeed(double input, double& out, std::string& error);

// ---- BPM ----
double SecondsPerBeat(double bpm);            // 60 / bpm（bpm <= 0 时 0）
double BeatMs(const Song& song);              // 一拍的毫秒数（= 60000 / bpm）
double EffectiveBpm(const Song& song, double speed);   // BPM × 速度
std::string BeatMsText(const Song& song);     // "1 拍 = 500 ms"

// ---- 拍号 ----
double BeatsPerBar(const Song& song);         // 每小节多少拍（= 拍号的分子）
double SecondsPerBar(const Song& song);       // 每小节多少秒
int BarCount(const Song& song);               // 整首有多少小节（不满一小节算一节；空曲 = 0）
std::string SignatureText(const Song& song);  // "4/4　每小节 4 拍 = 2.000 秒"

// ---- 小节定位（主界面的“小节：3 / 8”、编辑窗口的“小节”列）----
struct BarSpot {
    int bar = 1;              // 第几小节，从 1 开始
    double beatInBar = 0.0;   // 小节内第几拍，从 0 开始（0.5 表示小节内第二拍中间）
};

// 歌曲时间（秒）→ 小节位置。负数按 0 处理。
BarSpot LocateBar(const Song& song, double songSeconds);
// 拍位置（拍）→ 小节位置（编辑窗口用的是拍，不是秒）
BarSpot LocateBarByBeat(const Song& song, double beat);
int BarOfBeat(const Song& song, double beat);   // 只要小节号

// ---- 时值（需求书二十七节：全 / 二分 / 四分 / 八分 / 十六分）----
double NoteDurationMs(const Song& song, NoteValue value);   // 该时值等于多少毫秒
// 分数写法："1/4"（Phase 9 的文本曲谱也用这个写法）
std::string NoteFractionText(NoteValue value);
// 表格里的时值单元格："Quarter 1/4（500 ms）"
std::string NoteValueCellText(const Song& song, NoteValue value);

}  // namespace beat
}  // namespace hp
