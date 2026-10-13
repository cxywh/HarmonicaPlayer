#include "ui/UiModel.h"

#include <algorithm>
#include <cmath>

#include "song/BeatMath.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "ui/UiFormat.h"

namespace hp {
namespace ui {
namespace {

std::string TrimAscii(const std::string& text) {
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

}  // namespace

const char* ForegroundPolicyName(ForegroundPolicy policy) {
    switch (policy) {
        case ForegroundPolicy::Ignore: return "不处理";
        case ForegroundPolicy::Stop:   return "停止";
        case ForegroundPolicy::Pause:
        default:                       return "暂停";
    }
}

const char* ForegroundPolicyJsonName(ForegroundPolicy policy) {
    switch (policy) {
        case ForegroundPolicy::Ignore: return "Ignore";
        case ForegroundPolicy::Stop:   return "Stop";
        case ForegroundPolicy::Pause:
        default:                       return "Pause";
    }
}

bool ParseForegroundPolicy(const std::string& text, ForegroundPolicy& out) {
    const std::string trimmed = TrimAscii(text);
    if (trimmed == "不处理" || trimmed == "Ignore" || trimmed == "ignore" || trimmed == "0") {
        out = ForegroundPolicy::Ignore;
        return true;
    }
    if (trimmed == "暂停" || trimmed == "Pause" || trimmed == "pause" || trimmed == "1") {
        out = ForegroundPolicy::Pause;
        return true;
    }
    if (trimmed == "停止" || trimmed == "Stop" || trimmed == "stop" || trimmed == "2") {
        out = ForegroundPolicy::Stop;
        return true;
    }
    return false;
}

std::string AppSettings::Summary() const {
    return "热键 播放 " + playHotkey + "、停止 " + stopHotkey + "、重播 " + restartHotkey +
           "、上一首 " + previousHotkey + "、下一首 " + nextHotkey + "　速度 " +
           FormatSpeedText(speed) + "　" +
           (dryRun ? "干跑" : "真注入") + "　前台变化 " + ForegroundPolicyName(foregroundPolicy);
}

std::string AppSettings::DetailText() const {
    std::string text;
    text += "热键：播放 " + playHotkey + "　停止 " + stopHotkey + "　重播 " + restartHotkey +
            "　上一首 " + previousHotkey + "　下一首 " + nextHotkey;
    text += "\n播放速度：" + FormatSpeedText(speed) + (dryRun ? "（干跑，不注入按键）" : "（真注入）");
    text += "\n时间轴模式：" + std::string(naturalMode
                                      ? ("自然（每个音符抖动 " + FormatBeats(jitterMs) + " ms 以内）")
                                      : "精确");
    text += "\n用户输入急停：键盘 " + std::string(monitorKeyboard ? "开" : "关") + "　鼠标按键 " +
            std::string(monitorMouseButtons ? "开" : "关") + "　鼠标移动 " +
            std::string(monitorMouseMove ? "开" : "关");
    text += "\n前台窗口变化：" + std::string(ForegroundPolicyName(foregroundPolicy));
    text += "\n最近曲谱：" + (songPath.empty() ? std::string("（还没打开过）") : songPath);
    return text;
}

std::vector<EditorRow> BuildEditorRows(const Song& song) {
    std::vector<EditorRow> rows;
    rows.reserve(song.notes.size());
    for (const Note& note : song.notes) {
        EditorRow row;
        row.key = note.key;
        row.mode = note.mode;
        row.beat = song.SecondsToBeats(note.startTime);
        row.durationBeats = song.SecondsToBeats(note.duration);
        rows.push_back(row);
    }
    for (const Note& rest : song.rests) {
        EditorRow row;
        row.rest = true;
        row.beat = song.SecondsToBeats(rest.startTime);
        row.durationBeats = song.SecondsToBeats(rest.duration);
        rows.push_back(row);
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const EditorRow& a, const EditorRow& b) { return a.beat < b.beat; });
    return rows;
}

Song BuildSongFromRows(const std::string& name, double bpm, NoteValue beatUnit,
                       const TimeSignature& signature, const std::vector<EditorRow>& rows) {
    // 表格里存的就是**拍**（与 JSON 文件一致），这里一次性换算成绝对时间。
    // 刻意不经过 BeatNote 的“时值”量化：表格允许填 0.3 拍这种非标准时值，
    // 直接按拍数换算才能保证“表格 ↔ 时间轴”严格一致（往返无损）。
    Song song;
    song.name = name;
    song.bpm = (bpm > 0.0) ? bpm : 120.0;
    song.beatUnit = beatUnit;
    song.timeSignature = signature;
    song.notes.reserve(rows.size());
    for (const EditorRow& row : rows) {
        Note note;
        note.startTime = song.BeatsToSeconds(row.beat);
        note.duration = song.BeatsToSeconds(row.durationBeats);
        if (row.rest) {
            song.rests.push_back(note);   // 休止：只记时间，不进演奏列表
            continue;
        }
        note.key = row.key;
        note.mode = row.mode;
        song.notes.push_back(note);
    }
    song.SortNotes();
    return song;
}

Song DescribeContext(const std::string& name, double bpm, NoteValue beatUnit,
                     const TimeSignature& signature) {
    Song song;
    song.name = name;
    song.bpm = bpm;
    song.beatUnit = beatUnit;
    song.timeSignature = signature;
    return song;
}

namespace {

// 持续时间是否正好等于某个标准时值（用于决定时值列写 "Quarter 1/4" 还是 "约 0.7 拍"）
bool ExactNoteValue(double durationBeats, NoteValue beatUnit, NoteValue& out) {
    const NoteValue candidates[] = {NoteValue::Whole,  NoteValue::Half,     NoteValue::Quarter,
                                    NoteValue::Eighth, NoteValue::Sixteenth, NoteValue::ThirtySecond};
    for (NoteValue value : candidates) {
        if (std::fabs(NoteValueBeats(value, beatUnit) - durationBeats) < 1e-6) {
            out = value;
            return true;
        }
    }
    return false;
}

}  // namespace

std::string NoteRowCellText(const Timeline& timeline, const Song& song, int row, int column) {
    if (row < 0 || row >= timeline.NoteCount()) {
        return std::string();
    }
    const Note& note = timeline.At(row);
    switch (column) {
        case 0:
            return std::to_string(row + 1);
        case 1:
            return NoteTimeCell(note.startTime);
        case 2:
            return NoteKeyCell(note.key);
        case 3:
            return NoteModeCell(note.mode);
        case 4: {
            const double bpm = timeline.Bpm();
            const double quarterBeats = (bpm > 0.0) ? (note.duration * bpm / 60.0) : note.duration;
            return NoteValueCellForSong(song, quarterBeats);
        }
        default:
            return std::string();
    }
}

std::string NoteValueCellForSong(const Song& song, double quarterBeats) {
    const double unitBeats = NoteValueToBeats(song.beatUnit);   // 一拍 = 几个四分拍
    const double beats = (unitBeats > 0.0) ? (quarterBeats / unitBeats) : quarterBeats;
    NoteValue exact = NoteValue::Quarter;
    if (ExactNoteValue(beats, song.beatUnit, exact)) {
        return beat::NoteValueCellText(song, exact);
    }
    return "约 " + FormatBeats(beats) + " 拍（" + FormatMs(beats * beat::BeatMs(song)) + " ms）";
}

int DurationChoiceIndexFor(double beats) {
    for (int i = 0; i < kDurationChoiceCount; ++i) {
        const double delta = kDurationChoices[i].beats - beats;
        if (!kDurationChoices[i].rest && delta < 1e-6 && delta > -1e-6) {
            return i;
        }
    }
    return -1;
}

std::string DurationUserCell(double beats, double bpm) {
    const int index = DurationChoiceIndexFor(beats);
    const std::string name =
        index >= 0 ? std::string(kDurationChoices[index].label) : (FormatBeats(beats) + " 拍");
    if (!(bpm > 0.0)) {
        return name;
    }
    // 一拍 = 一个四分音符 = 60/BPM 秒（新建曲谱窗口只写 4/4、四分音符为一拍）
    return name + "（" + FormatBeats(beats * 60.0 / bpm) + " 秒）";
}

int ExtendLastRow(std::vector<EditorRow>& rows, double* cursorBeats) {
    if (rows.empty()) {
        return -1;
    }
    const int index = static_cast<int>(rows.size()) - 1;
    EditorRow& last = rows[static_cast<size_t>(index)];
    // 简谱的 "-"：音符本身不变，只把时值再延长一拍（**不**新增一行，模型里始终是一个音符）
    last.durationBeats += 1.0;
    if (cursorBeats != nullptr) {
        const double end = last.beat + last.durationBeats;
        if (end > *cursorBeats) {
            *cursorBeats = end;
        }
    }
    return index;
}

std::string KeyNoteName(Key key) {
    // 顺序必须与 enum class Key 一致：Z X C V B N M ,  →  1 2 3 4 5 6 7 i
    static const char* kNames[kKeyCount] = {"1", "2", "3", "4", "5", "6", "7", "i"};
    const int index = static_cast<int>(key);
    return (index >= 0 && index < kKeyCount) ? std::string(kNames[index]) : std::string("?");
}

std::string PlayModeButtonName(PlayMode mode) {
    switch (mode) {
        case PlayMode::Normal: return "不按鼠标（本音）";
        case PlayMode::Left:   return "按住左键（降调）";
        case PlayMode::Middle: return "按住中键（半音）";
        case PlayMode::Right:  return "按住右键（升调）";
        // 组合模式：要同时按住两个鼠标键（少了“+中键”用户会以为只按一个键）
        case PlayMode::LeftMiddle:  return "按住左键+中键（降调+半音）";
        case PlayMode::RightMiddle: return "按住右键+中键（升调+半音）";
    }
    return "?";
}

const DurationChoice kDurationChoices[kDurationChoiceCount] = {
    // 标准时值（按从长到短）：全音符 4 拍、二分音符 2 拍、四分音符 1 拍、八分音符 1/2 拍、十六分音符 1/4 拍
    {"全音符 4 拍", 4.0, false},
    {"二分音符 2 拍", 2.0, false},
    {"四分音符 1 拍", 1.0, false},
    {"八分音符 1/2 拍", 0.5, false},
    {"十六分音符 1/4 拍", 0.25, false},
    // 附点：时值 × 1.5
    {"附点四分 1.5 拍", 1.5, false},
    {"附点八分 0.75 拍", 0.75, false},
    // 休止：只占时间、不出声（进表格看得见，演奏时跳过）
    {"休止两拍", 2.0, true},
    {"休止一拍", 1.0, true},
    {"休止半拍", 0.5, true},
};

EditorRowText DescribeRow(const EditorRow& row, const Song& song) {
    EditorRowText text;
    const double seconds = (song.bpm > 0.0) ? (row.beat * song.SecondsPerBeat()) : 0.0;
    text.time = FormatSeconds3(seconds);
    text.beat = FormatBeats(row.beat);
    text.bar = std::to_string(beat::BarOfBeat(song, row.beat));
    if (row.rest) {   // 休止行：音调列写"休止"、演奏方式列写"—"（不按键）
        text.key = "休止";
        text.mode = "—";
        text.value = "休止 " +
                     NoteValueCellForSong(song, row.durationBeats * NoteValueToBeats(song.beatUnit));
        return text;
    }
    text.key = NoteKeyCell(row.key);
    text.mode = NoteModeCell(row.mode);
    text.value = NoteValueCellForSong(song, row.durationBeats * NoteValueToBeats(song.beatUnit));
    return text;
}

std::string TempoText(const Song& song) {
    if (!(song.bpm > 0.0)) {
        return std::string(labels::kBpmPrefix) + "未设置";
    }
    return std::string(labels::kBpmPrefix) + FormatBeats(song.bpm) + "（" + beat::BeatMsText(song) + "）";
}

std::string SignatureLine(const Song& song) {
    return std::string(labels::kSignaturePrefix) + beat::SignatureText(song);
}

std::string BarText(int bar, int total) {
    if (total <= 0) {
        return std::string(labels::kBarPrefix) + "0 / 0";
    }
    return std::string(labels::kBarPrefix) + std::to_string(bar) + " / " + std::to_string(total);
}

std::string ModeText(bool naturalMode, double jitterMs) {
    if (!naturalMode) {
        return std::string(labels::kModePrefix) + labels::kExactMode;
    }
    return std::string(labels::kModePrefix) + labels::kNaturalMode + "（抖动 " + FormatBeats(jitterMs) + " ms 以内）";
}

std::string SpeedChoiceText(double speed) {
    return FormatSpeedText(speed);
}

int SpeedPresetIndexFor(double speed) {
    for (int i = 0; i < beat::kSpeedPresetCount; ++i) {
        if (std::fabs(beat::SpeedPreset(i) - speed) < 1e-9) {
            return i;
        }
    }
    return -1;
}

std::string SpeedCaption() {
    return "播放速度（六档预置或自定义 50% ~ 200%）：";
}

namespace labels {
const char* kAppTitle = "Harmonica Player —— 游戏口琴自动演奏工具";
const char* kSongPrefix = "当前歌曲：";
const char* kPlay = "播放";
const char* kPause = "暂停";
const char* kStop = "停止";
const char* kRestart = "重播";
const char* kOpenSong = "打开曲谱";
const char* kEditSong = "编辑曲谱";
const char* kNewSong = "新建曲谱";        // 没有曲谱时这个按钮换成它（Phase 12 补）
const char* kNewSongName = "新曲谱";      // 新建时的默认曲名（不能空，否则应用时会被拒）
const char* kNewSongHint =
    "这是新曲谱：下面选好音符、模式（演奏方式）、拍位置、持续拍数，点「新增」把它加进表格；"
    "填完点「应用并关闭」送回主界面，想留档就点「另存为」存进 songs 文件夹。";
const char* kTextScore = "文本曲谱";
const char* kSettings = "设置";

// 文本曲谱窗口（Phase 9）：文案集中在这里，禁 emoji 守卫才扫得到
const char* kTextScoreTitle = "文本曲谱（一行一个音符）";
const char* kTextScoreHint =
    "一行一个音符：[演奏方式+]键名 [时值]。演奏方式写 Normal / Left / Middle / Right / "
    "LeftMiddle / RightMiddle（缩写 N / L / M / R / LM / RM），"
    "键名是 Z X C V B N M 与逗号，时值写 1/1 1/2 1/4 1/8 1/16（不写就是 1/4）。\n"
    "空行与以 # 或 // 开头的行会被跳过；时间由时值依次累加得到，文本谱不表示休止符。";
const char* kTextScoreName = "曲名：";
const char* kTextScoreParse = "解析预览";
const char* kTextScoreExample = "载入示例";
const char* kTextScoreApply = "载入为主曲谱";
const char* kTextScoreClose = "关闭";
const char* kSaveAs = "另存为";
const char* kLoad = "载入";
const char* kApply = "应用";
const char* kCancel = "取消";
const char* kClose = "关闭";
const char* kRestoreDefaults = "恢复默认";
const char* kAddNote = "新增";
const char* kDeleteNote = "删除";
const char* kApplyRow = "应用到选中";
const char* kMoveUp = "上移";
const char* kMoveDown = "下移";
const char* kDryRun = "干跑（不注入按键）";
const char* kReady = "就绪。可以选择曲谱后按播放，或直接用全局热键。";
const char* kNoSong = "（没有曲谱：请先打开或编辑一首）";
const char* kGroupPlayback = "播放控制";
const char* kGroupStatus = "状态";
const char* kGroupNotes = "音符表";
const char* kGroupInputGuard = "用户输入急停";
const char* kGroupForeground = "前台窗口变化";
const char* kGroupHotkeys = "全局热键";
const char* kBpmPrefix = "BPM：";
const char* kSignaturePrefix = "拍号：";
const char* kBarPrefix = "小节：";
const char* kSpeedPrefix = "速度：";
const char* kModePrefix = "模式：";
const char* kExactMode = "精确";
const char* kNaturalMode = "自然";
const char* kCustomSpeed = "自定义（在设置里填）";
const char* kGroupPlaybackOpt = "播放选项";
const char* kExtendAction = "延时（把上一个音符延长一拍）";   // 简谱的 "-"
}  // namespace labels

bool ParseBpmText(const std::string& text, double& out, std::string& error) {
    const std::string trimmed = TrimAscii(text);
    if (trimmed.empty()) {
        error = "BPM 不能为空（例如 120）";
        return false;
    }
    char* end = nullptr;
    const double value = std::strtod(trimmed.c_str(), &end);
    if (end == nullptr || *end != '\0') {
        error = "BPM 只能是数字（例如 120、120.5）";
        return false;
    }
    if (!(value >= 1.0 && value <= 1000.0)) {
        error = "BPM 要在 1 ~ 1000 之间";
        return false;
    }
    out = value;
    return true;
}

}  // namespace ui
}  // namespace hp
