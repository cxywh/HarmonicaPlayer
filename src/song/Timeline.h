#pragma once

// Timeline：歌曲的绝对时间轴。
//
// 职责（对应需求书第四、五、六、三十节）：
//   1. 把 Song 里的音符整理成“严格按起音时间排序”的绝对时间轴；
//   2. 提供调度需要的二分查询（下一个该响的音符、进度、已开始数量）；
//   3. 提供统计信息（总时长、密度、最小间隔、最大同时音符数）；
//   4. 可选“自然模式”：给每个音符叠加 ±jitterMs 的随机偏移。
//
// 关键约束：**一切时间都是绝对时间**。播放器拿到的永远是
// “这个音符应该在歌曲第 N 秒响起”，而不是“距上一个音符多久”。
// 因此任何一次调度延迟都不会传给下一个音符 —— 误差不累积。

#include <iosfwd>
#include <string>
#include <vector>

#include "song/Song.h"

namespace hp {

// 时间轴构建选项
struct TimelineOptions {
    // 自然模式：给每个音符的“原始绝对时间”叠加 ±jitterMs 的随机偏移。
    //   - 偏移基于原始时间计算，不是基于上一个音符的实际执行时间 → 不累积；
    //   - 上限固定（默认 3ms），不会改变整体节拍；
    //   - 可以用 naturalMode = false 关闭（精确模式）。
    bool naturalMode = false;
    double jitterMs = 3.0;
    unsigned int randomSeed = 20260925u;
};

struct TimelineStats {
    int noteCount = 0;                  // 成功进入时间轴的音符数
    int droppedNotes = 0;               // 因不合法被丢弃的音符数
    double firstNoteTime = 0.0;         // 第一个音符的起音（秒）
    double lastNoteEndTime = 0.0;       // 最后一个音符的结束 = 播放总时长
    double noteOnDensity = 0.0;         // 每秒起音数
    double minGapMs = 0.0;              // 相邻起音的最小间隔
    double maxGapMs = 0.0;              // 相邻起音的最大间隔
    int simultaneousPeak = 0;           // 同一时刻最多几个音符
    int bars = 0;                       // 按拍号估算的小节数
    bool jitterApplied = false;         // 本次是否应用了自然模式抖动
    double maxJitterMs = 0.0;           // 实际用到的最大偏移绝对值（自然模式）
};

class Timeline {
public:
    Timeline();

    // 从 Song 构建绝对时间轴。
    //   problems：非空时把校验问题（含被丢弃的音符）带出来，便于 UI 提示。
    static Timeline Build(const Song& song, const TimelineOptions& options = {},
                          std::vector<std::string>* problems = nullptr);

    bool Empty() const { return notes_.empty(); }
    int NoteCount() const { return static_cast<int>(notes_.size()); }
    const std::vector<Note>& Notes() const { return notes_; }
    const Note& At(int index) const;

    double TotalDuration() const { return stats_.lastNoteEndTime; }
    const TimelineStats& Stats() const { return stats_; }
    const std::string& SongName() const { return name_; }
    double Bpm() const { return bpm_; }
    TimeSignature Signature() const { return signature_; }
    bool NaturalMode() const { return options_.naturalMode; }

    // ---- 调度用的二分查询（notes_ 已按 startTime 升序） ----
    // 第一个 startTime >= timeSeconds 的下标；没有则返回 NoteCount()
    int LowerBound(double timeSeconds) const;
    // 第一个 startTime > timeSeconds 的下标
    int UpperBound(double timeSeconds) const;
    // 到 timeSeconds 为止已经该起音的音符数（= UpperBound）
    int CountStartedBy(double timeSeconds) const;

    // 到 timeSeconds 为止最后一个已起音的音符下标（没有则 -1）
    int IndexAtOrBefore(double timeSeconds) const;

    // 打印时间轴表格（默认输出到 std::cout）
    void PrintTable(std::ostream& os, int maxRows = 40) const;
    // 只打印统计摘要
    void PrintStats(std::ostream& os) const;

private:
    std::string name_;
    double bpm_ = 120.0;
    NoteValue beatUnit_ = NoteValue::Quarter;
    TimeSignature signature_{4, 4};
    std::vector<Note> notes_;
    TimelineOptions options_{};
    TimelineStats stats_{};
};

}  // namespace hp
