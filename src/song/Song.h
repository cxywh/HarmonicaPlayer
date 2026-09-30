#pragma once

// 歌曲：BPM、拍号、拍谱 → 绝对时间轴的换算。
//
// 说明（Phase 2 范围）：
//   - 本 Phase 负责把“作者视角”的拍谱（第几拍 + 时值）换算成**绝对时间**（秒），
//     并把结果放进 Song::notes。播放器此后只认 Song::notes，不再碰 BPM。
//   - 拍号的完整语义、连音/附点、曲中变速等留给 Phase 8；
//     这里先把数据结构和平移换定死，保证后面扩展不需要改播放器。

#include <iosfwd>
#include <string>
#include <vector>

#include "song/Note.h"

namespace hp {

// ---------------------------------------------------------------------------
// 拍号：至少支持 4/4、3/4、6/8，结构上可以扩展任意 x/y。
// 约定：拍号分母对应的时值就是“一拍”（4/4 一拍=四分音符，6/8 一拍=八分音符）。
// ---------------------------------------------------------------------------
struct TimeSignature {
    int numerator = 4;    // 每小节几拍
    int denominator = 4;  // 一拍是什么时值（4 = 四分音符）

    // "4/4"
    std::string ToString() const;
    // 分母是否为 2 的幂且落在支持的时值里（1/2/4/8/16/32）
    bool IsSupported() const;
    // 分母对应的时值：4 -> Quarter，8 -> Eighth
    NoteValue DenominatorValue() const;
    // 每小节拍数（按上面的约定就是 numerator）
    double BeatsPerBar() const { return static_cast<double>(numerator); }
};

// ---------------------------------------------------------------------------
// 作者视角的音符：用“第几拍 + 时值”描述
// ---------------------------------------------------------------------------
struct BeatNote {
    Key key = Key::Z;
    PlayMode mode = PlayMode::Normal;
    double beat = 0.0;                          // 从歌曲开始的拍数（0 = 第一拍开头）
    NoteValue value = NoteValue::Quarter;       // 时值

    double DurationBeats(NoteValue beatUnit) const { return NoteValueBeats(value, beatUnit); }
};

// ---------------------------------------------------------------------------
// 歌曲
// ---------------------------------------------------------------------------
struct Song {
    std::string name = "未命名";
    double bpm = 120.0;                          // 每分钟拍数
    NoteValue beatUnit = NoteValue::Quarter;     // “一拍”对应哪个时值（默认四分音符）
    TimeSignature timeSignature{4, 4};
    std::vector<Note> notes;                     // **绝对时间轴**（秒）；播放器只认这里
    // 休止段（只占时间、不出声）。演奏效果已经体现在后面音符的 startTime 上，
    // 这里单独记下来只是为了：①编辑器里看得见"这里停了几拍"；②存进 JSON 的 "rests"，
    // 下次打开/编辑还在。播放器不读这个列表。
    std::vector<Note> rests;

    // ---- BPM / 拍号换算 ----
    double SecondsPerBeat() const;               // 60 / bpm
    double SecondsPerBar() const;                // 每小节秒数
    double SecondsFor(NoteValue value) const;    // 某个时值等于多少秒
    double BeatsToSeconds(double beats) const;
    double SecondsToBeats(double seconds) const;

    // ---- 统计 ----
    double TotalDuration() const;                // 最后一个音符的结束时间（空曲 = 0）
    int NoteCount() const;
    int BarCount() const;
    int DistinctKeyCount() const;
    int MaxSimultaneousNotes() const;            // 同一时刻最多同时按下几个音符

    // ---- 编辑 ----
    // 把拍谱换算成绝对时间轴（会覆盖 notes）
    void SetNotesFromBeats(const std::vector<BeatNote>& beatNotes);
    // 按 startTime 稳定排序；时间相同时保持原有先后
    void SortNotes();

    // ---- 校验：返回问题列表，空 = 通过 ----
    std::vector<std::string> Validate() const;

    // 一行摘要
    std::string Summary() const;

    // 打印拍谱（作者视角），用于人工核对
    void PrintBeatTable(std::ostream& os) const;
};

// 便捷构造
Song MakeSongFromBeats(const std::string& name, double bpm, NoteValue beatUnit,
                       const TimeSignature& signature, const std::vector<BeatNote>& beatNotes);

// 解析 "4/4" / "3/4" / "6/8"（允许空格，如 " 4 / 4 "）；分母必须是 2 的幂且受支持
bool ParseTimeSignature(const std::string& text, TimeSignature& out);

}  // namespace hp
