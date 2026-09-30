#include "song/Song.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ostream>

namespace hp {

namespace {

// 去掉首尾空白与制表符（拍号解析用）
std::string TrimSpaces(const std::string& text) {
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

}  // namespace

// ---------------------------------------------------------------------------
// TimeSignature
// ---------------------------------------------------------------------------
std::string TimeSignature::ToString() const {
    return std::to_string(numerator) + "/" + std::to_string(denominator);
}

bool TimeSignature::IsSupported() const {
    if (numerator <= 0 || numerator > 64) {
        return false;
    }
    switch (denominator) {
        case 1: case 2: case 4: case 8: case 16: case 32:
            return true;
        default:
            return false;
    }
}

NoteValue TimeSignature::DenominatorValue() const {
    return static_cast<NoteValue>(denominator);
}

// ---------------------------------------------------------------------------
// Song
// ---------------------------------------------------------------------------
double Song::SecondsPerBeat() const {
    if (!(bpm > 0.0)) {
        return 0.0;
    }
    return 60.0 / bpm;
}

double Song::SecondsPerBar() const {
    return SecondsPerBeat() * timeSignature.BeatsPerBar();
}

double Song::SecondsFor(NoteValue value) const {
    return NoteValueBeats(value, beatUnit) * SecondsPerBeat();
}

double Song::BeatsToSeconds(double beats) const {
    return beats * SecondsPerBeat();
}

double Song::SecondsToBeats(double seconds) const {
    const double perBeat = SecondsPerBeat();
    return perBeat > 0.0 ? seconds / perBeat : 0.0;
}

double Song::TotalDuration() const {
    double total = 0.0;
    for (const Note& note : notes) {
        total = std::max(total, note.EndTime());
    }
    return total;
}

int Song::NoteCount() const {
    return static_cast<int>(notes.size());
}

int Song::BarCount() const {
    const double perBar = SecondsPerBar();
    if (!(perBar > 0.0)) {
        return 0;
    }
    return static_cast<int>(std::ceil(TotalDuration() / perBar - 1e-9));
}

int Song::DistinctKeyCount() const {
    bool used[kKeyCount] = {};
    for (const Note& note : notes) {
        const int index = static_cast<int>(note.key);
        if (index >= 0 && index < kKeyCount) {
            used[index] = true;
        }
    }
    int count = 0;
    for (bool flag : used) {
        if (flag) {
            ++count;
        }
    }
    return count;
}

int Song::MaxSimultaneousNotes() const {
    if (notes.empty()) {
        return 0;
    }
    // 用“起音时间相同”的连续段来近似（同一时刻按下 = 同时起音）
    std::vector<double> starts;
    starts.reserve(notes.size());
    for (const Note& note : notes) {
        starts.push_back(note.startTime);
    }
    std::sort(starts.begin(), starts.end());
    int peak = 1;
    int run = 1;
    for (size_t i = 1; i < starts.size(); ++i) {
        if (std::fabs(starts[i] - starts[i - 1]) < 1e-6) {
            ++run;
            peak = std::max(peak, run);
        } else {
            run = 1;
        }
    }
    return peak;
}

void Song::SetNotesFromBeats(const std::vector<BeatNote>& beatNotes) {
    notes.clear();
    notes.reserve(beatNotes.size());
    for (const BeatNote& beatNote : beatNotes) {
        Note note;
        note.key = beatNote.key;
        note.mode = beatNote.mode;
        note.startTime = BeatsToSeconds(beatNote.beat);
        note.duration = beatNote.DurationBeats(beatUnit) * SecondsPerBeat();
        notes.push_back(note);
    }
    SortNotes();
}

void Song::SortNotes() {
    // stable_sort：起始时间相同的音符保持原始先后（和弦的按下顺序可预期）
    std::stable_sort(notes.begin(), notes.end(),
                     [](const Note& a, const Note& b) { return a.startTime < b.startTime; });
}

std::vector<std::string> Song::Validate() const {
    std::vector<std::string> problems;

    if (!(bpm > 0.0)) {
        problems.push_back("BPM 必须大于 0（当前 " + std::to_string(bpm) + "）");
    } else if (bpm > 1000.0) {
        problems.push_back("BPM 过大（当前 " + std::to_string(bpm) + "，上限 1000）");
    }
    if (!timeSignature.IsSupported()) {
        problems.push_back("不支持或不合理的拍号：" + timeSignature.ToString());
    }
    if (beatUnit != timeSignature.DenominatorValue()) {
        problems.push_back("“一拍”的时值（" + std::string(NoteValueDisplayName(beatUnit)) +
                           "）与拍号分母（" + timeSignature.ToString() +
                           "）不一致：建议把 beatUnit 设为 " +
                           NoteValueDisplayName(timeSignature.DenominatorValue()));
    }
    if (notes.empty()) {
        problems.push_back("歌曲里没有音符");
    }

    int index = 0;
    for (const Note& note : notes) {
        std::string reason;
        if (!NoteIsValid(note, &reason)) {
            problems.push_back("第 " + std::to_string(index + 1) + " 个音符不合法：" + reason +
                               "（" + NoteToString(note) + "）");
        }
        ++index;
    }
    return problems;
}

std::string Song::Summary() const {
    char buffer[320];
    std::snprintf(buffer, sizeof(buffer),
                  "%s  |  BPM %.1f  |  拍号 %s  |  一拍=%s  |  音符 %d 个（用键 %d 种）  |  "
                  "小节约 %d  |  总时长 %.3f s",
                  name.c_str(), bpm, timeSignature.ToString().c_str(),
                  NoteValueDisplayName(beatUnit), NoteCount(), DistinctKeyCount(), BarCount(),
                  TotalDuration());
    return std::string(buffer);
}

void Song::PrintBeatTable(std::ostream& os) const {
    char line[240];
    std::snprintf(line, sizeof(line), "  %-6s %-8s %-12s %-10s %-12s %-12s", "序号", "第几拍",
                  "音符", "模式", "时值", "起音(秒)");
    os << line << "\n";
    os << "  ------ -------- ------------ ---------- ------------ ------------\n";

    // 反推：用 startTime / duration 还原拍位与时值（换算的逆运算，便于人工核对）
    const double perBeat = SecondsPerBeat();
    for (size_t i = 0; i < notes.size(); ++i) {
        const Note& note = notes[i];
        const double beat = perBeat > 0.0 ? note.startTime / perBeat : 0.0;
        const double durationBeats = perBeat > 0.0 ? note.duration / perBeat : 0.0;
        const double quarterNotes = durationBeats;   // 以四分音符为单位
        const int denominator = quarterNotes > 0.0 ? static_cast<int>(std::lround(4.0 / quarterNotes)) : 0;

        std::string valueText = "?";
        for (int d = 1; d <= 32; d *= 2) {
            if (d == denominator) {
                valueText = NoteValueDisplayName(static_cast<NoteValue>(d));
                break;
            }
        }

        std::snprintf(line, sizeof(line), "  %-6d %-8.3f %-12s %-10s %-12s %-12.4f", // NOLINT
                      static_cast<int>(i + 1), beat, KeyDisplayName(note.key),
                      PlayModeDisplayName(note.mode), valueText.c_str(), note.startTime);
        os << line << "\n";
    }
}

Song MakeSongFromBeats(const std::string& name, double bpm, NoteValue beatUnit,
                       const TimeSignature& signature, const std::vector<BeatNote>& beatNotes) {
    Song song;
    song.name = name;
    song.bpm = bpm;
    song.beatUnit = beatUnit;
    song.timeSignature = signature;
    song.SetNotesFromBeats(beatNotes);
    return song;
}

bool ParseTimeSignature(const std::string& text, TimeSignature& out) {
    const size_t slash = text.find('/');
    if (slash == std::string::npos) {
        return false;   // 必须写成 "分子/分母"
    }
    const std::string numeratorText = TrimSpaces(text.substr(0, slash));
    const std::string denominatorText = TrimSpaces(text.substr(slash + 1));
    if (numeratorText.empty() || denominatorText.empty()) {
        return false;
    }
    // 只接受纯数字：出现小数点/字母一律拒绝（"4/4abc" 不是拍号）
    for (char c : numeratorText) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    for (char c : denominatorText) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    const int numerator = std::atoi(numeratorText.c_str());
    const int denominator = std::atoi(denominatorText.c_str());

    TimeSignature parsed;
    parsed.numerator = numerator;
    parsed.denominator = denominator;
    if (numerator <= 0 || numerator > 64) {
        return false;
    }
    if (denominator < 1 || denominator > 32) {
        return false;
    }
    if ((denominator & (denominator - 1)) != 0) {
        return false;   // 分母必须是 2 的幂
    }
    out = parsed;
    return out.IsSupported();
}

}  // namespace hp
