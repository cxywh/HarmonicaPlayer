#include "song/TextScore.h"

#include <cctype>
#include <cmath>
#include <cstdio>

#include "core/Types.h"
#include "song/BeatMath.h"
#include "song/Note.h"

namespace hp {
namespace text_score {

namespace {

constexpr int kMaxIssues = 24;

bool IsAsciiSpace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\v' || ch == '\f';
}

std::string TrimAscii(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && IsAsciiSpace(text[begin])) {
        ++begin;
    }
    while (end > begin && IsAsciiSpace(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// 去掉行尾注释：'#' 或 '//' 之后的内容。
// 时值里的斜杠是单个的（"1/4"），所以 "//" 不会误伤。
std::string StripComment(const std::string& line) {
    const size_t hash = line.find('#');
    const size_t slashes = line.find("//");
    size_t cut = std::string::npos;
    if (hash != std::string::npos) {
        cut = hash;
    }
    if (slashes != std::string::npos && (cut == std::string::npos || slashes < cut)) {
        cut = slashes;
    }
    return cut == std::string::npos ? line : line.substr(0, cut);
}

std::vector<std::string> SplitTokens(const std::string& line) {
    std::vector<std::string> tokens;
    std::string current;
    for (char ch : line) {
        if (IsAsciiSpace(ch)) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (ch == '\r') {
            // CRLF / CR：换行统一处理
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                continue;   // 交给 '\n' 收尾
            }
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    lines.push_back(current);
    return lines;
}

// 演奏方式前缀：与 ParsePlayMode 一致，但**额外接受单字母 M = 中键**。
// 只有在写成 "M+键名" 时才会走到这里，所以不会和"音调 M"打架
// （ParsePlayMode 故意不认单字母 M，就是这个原因）。
bool ParseModePrefix(const std::string& text, PlayMode& out) {
    const std::string trimmed = TrimAscii(text);
    std::string upper;
    upper.reserve(trimmed.size());
    for (char ch : trimmed) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    if (upper == "M" || upper == "MID") {
        out = PlayMode::Middle;
        return true;
    }
    return ParsePlayMode(trimmed, out);
}

// 输出用的演奏方式写法：普通模式不写前缀
std::string ModePrefixText(PlayMode mode) {
    switch (mode) {
        case PlayMode::Left:
            return "L";
        case PlayMode::Middle:
            return "MIDDLE";
        case PlayMode::Right:
            return "R";
        case PlayMode::Normal:
        default:
            return std::string();
    }
}

std::string Number(double value, int decimals) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return std::string(buffer);
}

}  // namespace

std::string TextScoreResult::Summary() const {
    std::string text;
    if (ok) {
        text = "文本谱解析成功：" + std::to_string(noteLines) + " 个音符，共 " +
               Number(totalBeats, 3) + " 拍（" + Number(song.TotalDuration(), 3) + " 秒）";
        if (skippedLines > 0) {
            text += "；跳过 " + std::to_string(skippedLines) + " 个空行/注释行";
        }
        return text;
    }
    text = "文本谱解析失败：第 " + std::to_string(issues.empty() ? 0 : issues.front().line) +
           " 行起有 " + std::to_string(issues.size() + moreIssues) + " 处问题";
    if (!issues.empty()) {
        text += "；" + issues.front().message;
    }
    return text;
}

std::string TextScoreResult::IssueText() const {
    std::string text;
    for (const TextScoreIssue& issue : issues) {
        text += "第 " + std::to_string(issue.line) + " 行";
        if (!issue.text.empty()) {
            text += "「" + issue.text + "」";
        }
        text += "：" + issue.message + "\n";
    }
    if (moreIssues > 0) {
        text += "…… 另有 " + std::to_string(moreIssues) + " 处问题未列出。\n";
    }
    if (text.empty()) {
        text = "（没有可显示的问题）\n";
    }
    return text;
}

TextScoreResult ParseTextScore(const std::string& text, const Song& header) {
    TextScoreResult result;
    result.song = header;
    result.song.notes.clear();

    if (!(header.bpm > 0.0)) {
        result.issues.push_back(TextScoreIssue{0, std::string(), "曲头的 BPM 不合法，无法换算时间"});
        return result;
    }

    std::string body = text;
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF &&
        static_cast<unsigned char>(body[1]) == 0xBB && static_cast<unsigned char>(body[2]) == 0xBF) {
        body.erase(0, 3);   // UTF-8 BOM
    }

    std::vector<std::string> lines = SplitLines(body);
    // 文本以换行结尾时 SplitLines 会多出一个空元素，那不是真的空行，去掉
    if (!lines.empty() && lines.back().empty() && !body.empty() &&
        (body.back() == '\n' || body.back() == '\r')) {
        lines.pop_back();
    }
    result.totalLines = static_cast<int>(lines.size());

    const double secondsPerBeat = header.SecondsPerBeat();
    double beatCursor = 0.0;

    for (size_t index = 0; index < lines.size(); ++index) {
        const int lineNumber = static_cast<int>(index) + 1;
        const std::string stripped = TrimAscii(StripComment(lines[index]));
        if (stripped.empty()) {
            ++result.skippedLines;
            continue;
        }

        const std::vector<std::string> tokens = SplitTokens(stripped);
        if (tokens.size() > 2) {
            if (result.issues.size() < kMaxIssues) {
                result.issues.push_back(TextScoreIssue{
                    lineNumber, stripped,
                    "一行只能写「[演奏方式+]键名 [时值]」，多出来的部分是「" + tokens[2] + "」"});
            } else {
                ++result.moreIssues;
            }
            continue;
        }

        const std::string& spec = tokens[0];
        PlayMode mode = PlayMode::Normal;
        std::string keyText = spec;

        const size_t plus = spec.find('+');
        if (plus != std::string::npos) {
            const std::string modeText = spec.substr(0, plus);
            keyText = spec.substr(plus + 1);
            if (modeText.empty() || keyText.empty()) {
                if (result.issues.size() < kMaxIssues) {
                    result.issues.push_back(TextScoreIssue{lineNumber, stripped,
                                                           "「+」两边都要有内容，例如 L+V 或 MIDDLE+,"});
                } else {
                    ++result.moreIssues;
                }
                continue;
            }
            PlayMode parsedMode = PlayMode::Normal;
            if (!ParseModePrefix(modeText, parsedMode)) {
                if (result.issues.size() < kMaxIssues) {
                    result.issues.push_back(TextScoreIssue{
                        lineNumber, stripped,
                        "无法识别的演奏方式「" + modeText +
                            "」，可用 Normal / Left / Middle / Right（缩写 N / L / M / R，中键也可写 MIDDLE）"});
                } else {
                    ++result.moreIssues;
                }
                continue;
            }
            mode = parsedMode;
        }

        Key key = Key::Z;
        if (!ParseKey(keyText, key)) {
            std::string hint = "无法识别的键名「" + keyText + "」，可用 ";
            for (int i = 0; i < kKeyCount; ++i) {
                hint += KeyName(static_cast<Key>(i));
                if (i + 1 < kKeyCount) {
                    hint += " ";
                }
            }
            if (result.issues.size() < kMaxIssues) {
                result.issues.push_back(TextScoreIssue{lineNumber, stripped, hint});
            } else {
                ++result.moreIssues;
            }
            continue;
        }

        NoteValue value = NoteValue::Quarter;   // 没写时值就是 1/4（一拍）
        if (tokens.size() == 2) {
            NoteValue parsedValue = NoteValue::Quarter;
            if (!ParseNoteValue(tokens[1], parsedValue)) {
                if (result.issues.size() < kMaxIssues) {
                    result.issues.push_back(TextScoreIssue{
                        lineNumber, stripped,
                        "无法识别的时值「" + tokens[1] + "」，可用 1/1 1/2 1/4 1/8 1/16（或 全音符/二分音符/四分音符/八分音符/十六分音符）"});
                } else {
                    ++result.moreIssues;
                }
                continue;
            }
            value = parsedValue;
        }

        const double beats = NoteValueBeats(value, header.beatUnit);
        if (!(beats > 0.0)) {
            if (result.issues.size() < kMaxIssues) {
                result.issues.push_back(TextScoreIssue{lineNumber, stripped,
                                                       "这个时值换不出正数拍数（拍单位与拍号可能不匹配）"});
            } else {
                ++result.moreIssues;
            }
            continue;
        }

        Note note;
        note.key = key;
        note.mode = mode;
        note.startTime = beatCursor * secondsPerBeat;
        note.duration = beats * secondsPerBeat;
        result.song.notes.push_back(note);

        beatCursor += beats;
        ++result.noteLines;
        result.totalBeats = beatCursor;
    }

    if (result.issues.empty() && result.moreIssues == 0) {
        if (result.noteLines == 0) {
            result.issues.push_back(
                TextScoreIssue{0, std::string(), "没有解析到任何音符（文本里只有空行或注释？）"});
            return result;
        }
        result.ok = true;
    }
    return result;
}

std::string SongToTextScore(const Song& song, std::string* inexactReason) {
    if (inexactReason != nullptr) {
        inexactReason->clear();
    }

    const NoteValue candidates[] = {NoteValue::Whole,  NoteValue::Half,     NoteValue::Quarter,
                                    NoteValue::Eighth, NoteValue::Sixteenth, NoteValue::ThirtySecond};

    const double secondsPerBeat = song.SecondsPerBeat();
    if (!(secondsPerBeat > 0.0)) {
        if (inexactReason != nullptr) {
            *inexactReason = "曲目的 BPM 不合法，无法换算";
        }
        return std::string();
    }

    std::string text;
    double beatCursor = 0.0;
    for (size_t i = 0; i < song.notes.size(); ++i) {
        const Note& note = song.notes[i];
        const double startBeats = note.startTime / secondsPerBeat;
        if (startBeats > beatCursor + 1e-6) {
            if (inexactReason != nullptr && inexactReason->empty()) {
                *inexactReason = "第 " + std::to_string(i + 1) + " 个音符前有 " +
                                 Number(startBeats - beatCursor, 3) + " 拍空档（文本谱不表示休止符）";
            }
        } else if (startBeats + 1e-6 < beatCursor) {
            // 与前一个音符重叠：文本谱是顺序累加的，写不出重叠
            if (inexactReason != nullptr && inexactReason->empty()) {
                *inexactReason = "第 " + std::to_string(i + 1) + " 个音符与前一个重叠（文本谱是顺序累加）";
            }
        }

        const double beats = note.duration / secondsPerBeat;
        NoteValue chosen = NoteValue::Quarter;
        bool exact = false;
        for (NoteValue candidate : candidates) {
            if (std::fabs(NoteValueBeats(candidate, song.beatUnit) - beats) < 1e-6) {
                chosen = candidate;
                exact = true;
                break;
            }
        }
        if (!exact) {
            // 挑最接近的时值写出去，并把"这是近似"如实标出来
            double bestDelta = 1e30;
            for (NoteValue candidate : candidates) {
                const double delta = std::fabs(NoteValueBeats(candidate, song.beatUnit) - beats);
                if (delta < bestDelta) {
                    bestDelta = delta;
                    chosen = candidate;
                }
            }
            if (inexactReason != nullptr && inexactReason->empty()) {
                *inexactReason = "第 " + std::to_string(i + 1) + " 个音符的时值 " + Number(beats, 3) +
                                 " 拍不是标准时值，只能按最接近的写法输出";
            }
        }

        const std::string prefix = ModePrefixText(note.mode);
        if (!prefix.empty()) {
            text += prefix + "+";   // 普通模式不写前缀
        }
        text += KeyName(note.key);
        text += " " + beat::NoteFractionText(chosen) + "\n";

        beatCursor = startBeats + beats;
    }
    return text;
}

}  // namespace text_score
}  // namespace hp
