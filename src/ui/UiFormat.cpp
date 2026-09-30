#include "ui/UiFormat.h"

#include "song/BeatMath.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

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

std::string FormatSeconds3(double seconds) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.3f", seconds);
    return buffer;
}

std::string FormatClock(double seconds) {
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    const int totalMs = static_cast<int>(seconds * 1000.0 + 0.5);
    const int minutes = totalMs / 60000;
    const int rest = totalMs % 60000;
    const int secs = rest / 1000;
    const int millis = rest % 1000;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d.%03d", minutes, secs, millis);
    return buffer;
}

namespace {

size_t Utf8CharLen(unsigned char lead) {
    if (lead < 0x80) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        return 2;
    }
    if ((lead & 0xF0) == 0xE0) {
        return 3;
    }
    if ((lead & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

// 折在这几个字符后面读起来最自然（标点、空格、路径分隔符、连字符）
bool BreakAfter(const std::string& ch) {
    static const char* kBreaks[] = {"，", "。", "、", "；", "：", "）", "！", "？", "】",
                                    "》", " ",  "\\", "/",  "-",  "(",  ")"};
    for (const char* item : kBreaks) {
        if (ch == item) {
            return true;
        }
    }
    return false;
}

}  // namespace

// 中文折行用的字符判定：收尾标点 / 开引号（都是 UTF-8 字符串）
bool IsClosingPunct(const std::string& ch) {
    static const char* kList[] = {"\u3002", "\uff0c", "\u3001", "\uff1b", "\uff1a", "\uff01", "\uff1f",
                                  "\uff09", "\u300d", "\u300f", "\u3011", "\u2026", "\uff1e"};
    for (const char* item : kList) {
        if (ch == item) {
            return true;
        }
    }
    return false;
}

bool IsOpeningPunct(const std::string& ch) {
    static const char* kList[] = {"\uff08", "\u300c", "\u300e", "\u3010", "\u300a", "\uff1c"};
    for (const char* item : kList) {
        if (ch == item) {
            return true;
        }
    }
    return false;
}

std::string LastUtf8Char(const std::string& text) {
    if (text.empty()) {
        return std::string();
    }
    size_t index = text.size() - 1;
    while (index > 0 && (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80) {
        --index;
    }
    return text.substr(index);
}

std::string WrapLabelText(const std::string& text, double widthInChars, int maxLines) {
    if (text.empty() || maxLines <= 0) {
        return text;
    }
    if (widthInChars < 8.0) {
        widthInChars = 8.0;
    }

    // 先切成"字符"再装行，才好在断点处做中文折行禁则（半角算 0.55 字宽，全角算 1 字宽）
    std::vector<std::string> chars;
    for (size_t i = 0; i < text.size();) {
        const size_t length = std::min(Utf8CharLen(static_cast<unsigned char>(text[i])), text.size() - i);
        chars.push_back(text.substr(i, length));
        i += length;
    }
    const auto charWidth = [](const std::string& ch) { return ch.size() == 1 ? 0.55 : 1.0; };

    std::vector<std::string> lines;
    std::string line;
    double width = 0.0;
    bool overflow = false;
    for (size_t i = 0; i < chars.size(); ++i) {
        const std::string& ch = chars[i];
        if (line.empty() && ch == " ") {   // 行首的空格不要
            continue;
        }
        const double chWidth = charWidth(ch);
        if (width + chWidth > widthInChars && !line.empty()) {
            // 禁则一：行首不能是收尾标点（。，、；：！？）」』】…），让它跟着上一行走
            if (IsClosingPunct(ch)) {
                line += ch;
                width += chWidth;
                if (static_cast<int>(lines.size()) + 1 < maxLines) {
                    lines.push_back(line);
                    line.clear();
                    width = 0.0;
                }
                continue;
            }
            // 禁则二：行尾不能是开引号 / 开括号（（「『【《），把它挪到下一行开头
            std::string carry;
            const std::string tail = LastUtf8Char(line);
            if (!tail.empty() && IsOpeningPunct(tail) && line.size() > tail.size()) {
                carry = tail;
                line.resize(line.size() - tail.size());
            }
            lines.push_back(line);
            if (static_cast<int>(lines.size()) >= maxLines) {
                overflow = i + 1 < chars.size();   // 还有字符没放下
                break;
            }
            line = carry;
            width = carry.empty() ? 0.0 : charWidth(carry);
        }
        line += ch;
        width += chWidth;
        if (BreakAfter(ch) && width >= widthInChars * 0.6 &&
            static_cast<int>(lines.size()) + 1 < maxLines) {
            lines.push_back(line);
            line.clear();
            width = 0.0;
        }
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    if (lines.empty()) {
        return std::string();
    }
    if (overflow || static_cast<int>(lines.size()) > maxLines) {
        std::string& last = lines[static_cast<size_t>(maxLines) - 1];
        // 最后一行留一个位置给省略号
        double used = 0.0;
        size_t keep = last.size();
        size_t cursor = 0;
        while (cursor < last.size()) {
            const size_t length = std::min(Utf8CharLen(static_cast<unsigned char>(last[cursor])),
                                           last.size() - cursor);
            const double chWidth = length == 1 ? 0.55 : 1.0;
            if (used + chWidth > widthInChars - 1.0) {
                keep = cursor;
                break;
            }
            used += chWidth;
            cursor += length;
        }
        last = last.substr(0, keep) + "…";
        lines.resize(static_cast<size_t>(maxLines));
    }
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            out += "\n";
        }
        out += lines[i];
    }
    return out;
}

std::string FormatBeats(double beats) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.4g", beats);
    return buffer;
}

int PercentOf(int current, int total) {
    if (total <= 0) {
        return 0;
    }
    int percent = static_cast<int>((static_cast<double>(current) * 100.0) /
                                   static_cast<double>(total) + 0.5);
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    return percent;
}

std::string FormatPercent(int percent) {
    return std::to_string(percent) + "%";
}

std::string FormatMs(double ms) {
    const bool whole = std::fabs(ms - std::floor(ms + 0.5)) < 1e-6;
    char buffer[64] = "";
    std::snprintf(buffer, sizeof(buffer), whole ? "%.0f" : "%.1f", ms);
    return std::string(buffer);
}

std::string FormatSpeedText(double speed) {
    const int percent = static_cast<int>(speed * 100.0 + 0.5);
    return FormatPercent(percent);
}

bool ParseSpeedText(const std::string& text, double& out, std::string& error) {
    const std::string trimmed = TrimAscii(text);
    if (trimmed.empty()) {
        error = "速度不能为空（例如 100% 或 1.0）";
        return false;
    }
    bool percentForm = false;
    std::string number = trimmed;
    if (number.back() == '%') {
        percentForm = true;
        number = TrimAscii(number.substr(0, number.size() - 1));
    }
    if (number.empty()) {
        error = "速度里没有数字（例如 100% 或 1.0）";
        return false;
    }
    char* end = nullptr;
    const double value = std::strtod(number.c_str(), &end);
    if (end == nullptr || *end != '\0') {
        error = "速度只能填数字，可以带百分号（例如 100% 或 1.0）";
        return false;
    }
    double speed = percentForm ? value / 100.0 : value;
    // 不带百分号时：大于 4 视为百分比写法（"100" 就是 100%）
    if (!percentForm && value > 4.0) {
        speed = value / 100.0;
    }
    // 需求书二十九节：50% ~ 200%（六档预置 + 自定义）。范围判定只在 BeatMath 里写一份。
    double clamped = 1.0;
    if (!beat::ClampSpeed(speed, clamped, error)) {
        return false;
    }
    out = clamped;
    return true;
}

std::string StatusLine(PlaybackState state, StopReason reason, const std::string& detail) {
    std::string text = "状态：";
    text += PlaybackStateName(state);
    if (state == PlaybackState::Stopped && reason != StopReason::None) {
        text += "\u3000原因：";
        text += StopReasonText(reason, detail);
    }
    return text;
}

std::string CurrentNoteText(int current, int total) {
    if (current < 0) {
        current = 0;
    }
    return "当前音符：" + std::to_string(current) + " / " + std::to_string(total);
}

std::string ProgressBarText(int percent, int width) {
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    if (width < 4) {
        width = 4;
    }
    const int filled = static_cast<int>((percent * width) / 100.0 + 0.5);
    std::string bar;
    for (int i = 0; i < width; ++i) {
        bar += (i < filled) ? "#" : ".";
    }
    return "[" + bar + "] " + FormatPercent(percent);
}

std::string NoteTimeCell(double seconds) {
    return FormatSeconds3(seconds);
}

std::string NoteKeyCell(Key key) {
    return KeyName(key);
}

std::string NoteModeCell(PlayMode mode) {
    return PlayModeName(mode);
}

std::string NearestValueName(double durationBeats, NoteValue beatUnit) {
    const NoteValue candidates[] = {NoteValue::Whole,   NoteValue::Half,     NoteValue::Quarter,
                                    NoteValue::Eighth,  NoteValue::Sixteenth,
                                    NoteValue::ThirtySecond};
    NoteValue best = NoteValue::Quarter;
    double bestDelta = -1.0;
    for (NoteValue value : candidates) {
        const double beats = NoteValueBeats(value, beatUnit);
        const double delta = std::fabs(beats - durationBeats);
        if (bestDelta < 0.0 || delta < bestDelta) {
            bestDelta = delta;
            best = value;
        }
    }
    return NoteValueName(best);
}

std::string NoteValueCell(double durationBeats, NoteValue beatUnit) {
    return NearestValueName(durationBeats, beatUnit) + "（" + FormatBeats(durationBeats) + " 拍）";
}

bool ValueBeatsByName(const std::string& text, NoteValue beatUnit, double& out) {
    // 先按“分数/名字”解析（1/4、QUARTER、四分…），失败再按纯数字当拍数
    NoteValue value = NoteValue::Quarter;
    if (ParseNoteValue(text, value)) {
        out = NoteValueBeats(value, beatUnit);
        return true;
    }
    const std::string trimmed = TrimAscii(text);
    if (trimmed.empty()) {
        return false;
    }
    char* end = nullptr;
    const double beats = std::strtod(trimmed.c_str(), &end);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    out = beats;
    return true;
}

std::string HotkeyHint(const std::string& play, const std::string& pause, const std::string& stop,
                       const std::string& restart, const std::string& previous, const std::string& next) {
    return "热键：" + play + " 播放　" + pause + " 暂停　" + stop + " 停止　" + restart + " 重播　" +
           previous + " 上一首　" + next + " 下一首（可在设置里改）";
}

bool ContainsEmoji(const std::string& text) {
    // UTF-8 解码后判断码点是否落在常见的 emoji / 装饰符号区
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char first = static_cast<unsigned char>(text[i]);
        unsigned int code = 0;
        size_t length = 1;
        if (first < 0x80) {
            code = first;
        } else if ((first & 0xE0) == 0xC0) {
            code = first & 0x1F;
            length = 2;
        } else if ((first & 0xF0) == 0xE0) {
            code = first & 0x0F;
            length = 3;
        } else if ((first & 0xF8) == 0xF0) {
            code = first & 0x07;
            length = 4;
        } else {
            ++i;
            continue;
        }
        if (i + length > text.size()) {
            break;
        }
        for (size_t k = 1; k < length; ++k) {
            code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        }
        // 界面约定是"文字 + 颜色"，一个符号都不该出现，所以把符号区整块挡掉：
        //   箭头 U+2190-21FF、数学/技术符号 U+2200-22FF、几何与杂项符号 U+2300-27BF、
        //   装饰符 U+2B00-2BFF、emoji 主体 U+1F000 起、变体选择符与键帽。
        if (code >= 0x1F000 || (code >= 0x2190 && code <= 0x2BFF) || code == 0xFE0F ||
            code == 0x20E3) {
            return true;
        }
        i += length;
    }
    return false;
}

}  // namespace ui
}  // namespace hp
