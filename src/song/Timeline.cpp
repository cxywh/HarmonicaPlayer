#include "song/Timeline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <random>

namespace hp {
namespace {

// 判定“同一时刻起音”的时间容差（秒）。比这更近就算和弦。
constexpr double kSimultaneousEpsilon = 1e-6;

}  // namespace

Timeline::Timeline() = default;

Timeline Timeline::Build(const Song& song, const TimelineOptions& options,
                         std::vector<std::string>* problems) {
    Timeline timeline;
    timeline.name_ = song.name;
    timeline.bpm_ = song.bpm;
    timeline.beatUnit_ = song.beatUnit;
    timeline.signature_ = song.timeSignature;
    timeline.options_ = options;

    if (problems != nullptr && !(song.bpm > 0.0)) {
        problems->push_back("BPM 非法（" + std::to_string(song.bpm) + "），时间轴无法计算");
    }

    // 1) 校验 + 丢弃非法音符；同时记下“原始绝对时间”
    struct RawNote {
        Note note;
        double originalStart = 0.0;
    };
    std::vector<RawNote> raw;
    raw.reserve(song.notes.size());

    int dropped = 0;
    for (size_t i = 0; i < song.notes.size(); ++i) {
        const Note& note = song.notes[i];
        std::string reason;
        if (!NoteIsValid(note, &reason)) {
            ++dropped;
            if (problems != nullptr) {
                problems->push_back("第 " + std::to_string(i + 1) + " 个音符被丢弃：" + reason +
                                    "（" + NoteToString(note) + "）");
            }
            continue;
        }
        RawNote entry;
        entry.note = note;
        entry.originalStart = note.startTime;
        raw.push_back(entry);
    }

    // 2) 按起音时间稳定排序（时间相同则保持原有先后，和弦的按下顺序可预期）
    std::stable_sort(raw.begin(), raw.end(), [](const RawNote& a, const RawNote& b) {
        return a.originalStart < b.originalStart;
    });

    // 3) 自然模式：对**原始时间**叠加 ±jitter 的随机偏移
    double maxJitterMs = 0.0;
    if (options.naturalMode && !raw.empty()) {
        const double jitterSeconds = std::max(0.0, options.jitterMs) / 1000.0;
        std::mt19937 rng(options.randomSeed);
        std::uniform_real_distribution<double> distribution(-jitterSeconds, jitterSeconds);

        for (RawNote& entry : raw) {
            // 关键：偏移只加在原始时间上，绝不参考上一个音符的结果 → 不累积
            const double offset = distribution(rng);
            maxJitterMs = std::max(maxJitterMs, std::fabs(offset) * 1000.0);
            entry.note.startTime = std::max(0.0, entry.originalStart + offset);
        }
        // 抖动可能把两个音符的先后微调过来，重新稳定排序以维持二分查找的前提
        std::stable_sort(raw.begin(), raw.end(),
                         [](const RawNote& a, const RawNote& b) {
                             return a.note.startTime < b.note.startTime;
                         });
    }

    // 4) 落地成绝对时间轴
    timeline.notes_.reserve(raw.size());
    for (const RawNote& entry : raw) {
        timeline.notes_.push_back(entry.note);
    }

    // 5) 统计
    TimelineStats& stats = timeline.stats_;
    stats.noteCount = static_cast<int>(timeline.notes_.size());
    stats.droppedNotes = dropped;
    stats.jitterApplied = options.naturalMode && stats.noteCount > 0;
    stats.maxJitterMs = stats.jitterApplied ? maxJitterMs : 0.0;

    if (!timeline.notes_.empty()) {
        stats.firstNoteTime = timeline.notes_.front().startTime;

        double lastEnd = 0.0;
        for (const Note& note : timeline.notes_) {
            lastEnd = std::max(lastEnd, note.EndTime());
        }
        stats.lastNoteEndTime = lastEnd;

        if (lastEnd > 0.0) {
            stats.noteOnDensity = static_cast<double>(stats.noteCount) / lastEnd;
        }

        // 相邻起音间隔
        double minGap = 1e30;
        double maxGap = 0.0;
        int peak = 1;
        int run = 1;
        for (size_t i = 1; i < timeline.notes_.size(); ++i) {
            const double gap =
                timeline.notes_[i].startTime - timeline.notes_[i - 1].startTime;
            minGap = std::min(minGap, gap);
            maxGap = std::max(maxGap, gap);
            if (gap <= kSimultaneousEpsilon) {
                ++run;
                peak = std::max(peak, run);
            } else {
                run = 1;
            }
        }
        stats.minGapMs = (timeline.notes_.size() > 1) ? minGap * 1000.0 : 0.0;
        stats.maxGapMs = (timeline.notes_.size() > 1) ? maxGap * 1000.0 : 0.0;
        stats.simultaneousPeak = peak;

        const double perBar = song.SecondsPerBar();
        if (perBar > 0.0) {
            stats.bars = static_cast<int>(std::ceil(lastEnd / perBar - 1e-9));
        }
    }

    return timeline;
}

const Note& Timeline::At(int index) const {
    static const Note kEmpty;
    if (index < 0 || index >= static_cast<int>(notes_.size())) {
        return kEmpty;
    }
    return notes_[static_cast<size_t>(index)];
}

int Timeline::LowerBound(double timeSeconds) const {
    const auto it = std::lower_bound(notes_.begin(), notes_.end(), timeSeconds,
                                     [](const Note& note, double value) {
                                         return note.startTime < value;
                                     });
    return static_cast<int>(it - notes_.begin());
}

int Timeline::UpperBound(double timeSeconds) const {
    const auto it = std::upper_bound(notes_.begin(), notes_.end(), timeSeconds,
                                     [](double value, const Note& note) {
                                         return value < note.startTime;
                                     });
    return static_cast<int>(it - notes_.begin());
}

int Timeline::CountStartedBy(double timeSeconds) const {
    return UpperBound(timeSeconds);
}

int Timeline::IndexAtOrBefore(double timeSeconds) const {
    return UpperBound(timeSeconds) - 1;
}

void Timeline::PrintStats(std::ostream& os) const {
    char line[256];
    os << "  歌曲        ：" << name_ << "\n";
    std::snprintf(line, sizeof(line), "  BPM / 拍号  ：%.1f / %s（一拍 %s，每小节 %.3f 秒）\n",
                  bpm_, signature_.ToString().c_str(), NoteValueDisplayName(beatUnit_),
                  signature_.BeatsPerBar() * (bpm_ > 0.0 ? 60.0 / bpm_ : 0.0));
    os << line;
    std::snprintf(line, sizeof(line), "  音符数量    ：%d 个（丢弃 %d 个）\n", stats_.noteCount,
                  stats_.droppedNotes);
    os << line;
    std::snprintf(line, sizeof(line), "  起音范围    ：%.4f s 起，%.4f s 结束，总时长 %.4f s（约 %d 小节）\n",
                  stats_.firstNoteTime, stats_.lastNoteEndTime, stats_.lastNoteEndTime,
                  stats_.bars);
    os << line;
    std::snprintf(line, sizeof(line), "  起音密度    ：%.2f 个/秒；相邻间隔 %.3f ms ~ %.3f ms\n",
                  stats_.noteOnDensity, stats_.minGapMs, stats_.maxGapMs);
    os << line;
    std::snprintf(line, sizeof(line), "  同时起音峰值：%d 个（和弦）\n", stats_.simultaneousPeak);
    os << line;
    if (stats_.jitterApplied) {
        std::snprintf(line, sizeof(line),
                      "  自然模式    ：已开启，抖动上限 ±%.1f ms（本次实际最大 %.3f ms），基于原始时间、不累积\n",
                      options_.jitterMs, stats_.maxJitterMs);
    } else {
        std::snprintf(line, sizeof(line), "  自然模式    ：关闭（精确模式，严格按时间轴执行）\n");
    }
    os << line;
}

void Timeline::PrintTable(std::ostream& os, int maxRows) const {
    if (notes_.empty()) {
        os << "  （时间轴为空）\n";
        return;
    }

    const double perBeat = bpm_ > 0.0 ? 60.0 / bpm_ : 0.0;
    char line[256];
    std::snprintf(line, sizeof(line), "  %-6s %-11s %-9s %-11s %-10s %-11s %-11s", "序号",
                  "起音(秒)", "第几拍", "音符", "模式", "持续(秒)", "结束(秒)");
    os << line << "\n";
    os << "  ------ ----------- --------- ---------- --------- ---------- ----------\n";

    const int total = static_cast<int>(notes_.size());
    const int rows = (maxRows > 0 && total > maxRows) ? maxRows : total;
    for (int i = 0; i < rows; ++i) {
        const Note& note = notes_[static_cast<size_t>(i)];
        const double beat = perBeat > 0.0 ? note.startTime / perBeat : 0.0;
        std::snprintf(line, sizeof(line), "  %-6d %-11.4f %-9.3f %-11s %-10s %-11.4f %-11.4f",
                      i + 1, note.startTime, beat, KeyDisplayName(note.key),
                      PlayModeDisplayName(note.mode), note.duration, note.EndTime());
        os << line << "\n";
    }
    if (rows < total) {
        os << "  …… 另外 " << (total - rows) << " 个音符未显示（表格上限 " << maxRows << " 行）\n";
    }
}

}  // namespace hp
