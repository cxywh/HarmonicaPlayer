#include "song/Note.h"

#include <cstdio>

namespace hp {
namespace {

struct NoteValueEntry {
    NoteValue value;
    const char* name;
    const char* display;
};

// 顺序与 enum class NoteValue 的值一致（分母）
const NoteValueEntry kNoteValues[] = {
    {NoteValue::Whole,        "Whole",        "全音符"},
    {NoteValue::Half,         "Half",         "二分音符"},
    {NoteValue::Quarter,      "Quarter",      "四分音符"},
    {NoteValue::Eighth,       "Eighth",       "八分音符"},
    {NoteValue::Sixteenth,    "Sixteenth",    "十六分音符"},
    {NoteValue::ThirtySecond, "ThirtySecond", "三十二分音符"},
};
constexpr int kNoteValueEntryCount =
    static_cast<int>(sizeof(kNoteValues) / sizeof(kNoteValues[0]));

const NoteValueEntry* FindEntry(NoteValue value) {
    for (const NoteValueEntry& entry : kNoteValues) {
        if (entry.value == value) {
            return &entry;
        }
    }
    return nullptr;
}

std::string Trim(const std::string& text) {
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

std::string ToUpperAscii(const std::string& text) {
    std::string result = text;
    for (char& ch : result) {
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    }
    return result;
}

}  // namespace

const char* NoteValueName(NoteValue value) {
    const NoteValueEntry* entry = FindEntry(value);
    return entry != nullptr ? entry->name : "?";
}

const char* NoteValueDisplayName(NoteValue value) {
    const NoteValueEntry* entry = FindEntry(value);
    return entry != nullptr ? entry->display : "?";
}

double NoteValueToBeats(NoteValue value) {
    const int denominator = static_cast<int>(value);
    return denominator > 0 ? 4.0 / static_cast<double>(denominator) : 0.0;
}

double NoteValueBeats(NoteValue value, NoteValue beatUnit) {
    const int denominator = static_cast<int>(value);
    const int unit = static_cast<int>(beatUnit);
    if (denominator <= 0 || unit <= 0) {
        return 0.0;
    }
    // 以 beatUnit 为 1 拍：四分音符在“八分音符为一拍”时等于 2 拍
    return static_cast<double>(unit) / static_cast<double>(denominator);
}

bool ParseNoteValue(const std::string& text, NoteValue& out) {
    const std::string raw = Trim(text);
    if (raw.empty()) {
        return false;
    }

    // 中文别名（不做大小写转换，避免碰多字节字符）
    struct ChineseAlias {
        const char* text;
        NoteValue value;
    };
    static const ChineseAlias kChineseAliases[] = {
        {"全音符", NoteValue::Whole},        {"全", NoteValue::Whole},
        {"二分音符", NoteValue::Half},       {"二分", NoteValue::Half},
        {"四分音符", NoteValue::Quarter},    {"四分", NoteValue::Quarter},
        {"八分音符", NoteValue::Eighth},     {"八分", NoteValue::Eighth},
        {"十六分音符", NoteValue::Sixteenth}, {"十六分", NoteValue::Sixteenth},
        {"三十二分音符", NoteValue::ThirtySecond}, {"三十二分", NoteValue::ThirtySecond},
    };
    for (const ChineseAlias& alias : kChineseAliases) {
        if (raw == alias.text) {
            out = alias.value;
            return true;
        }
    }

    const std::string upper = ToUpperAscii(raw);

    // "1/4" 形式
    const size_t slash = upper.find('/');
    if (slash != std::string::npos) {
        const int denominator = std::atoi(upper.substr(slash + 1).c_str());
        for (const NoteValueEntry& entry : kNoteValues) {
            if (static_cast<int>(entry.value) == denominator) {
                out = entry.value;
                return true;
            }
        }
        return false;
    }

    // 纯数字形式："4" = 四分音符
    bool allDigits = !upper.empty();
    for (char ch : upper) {
        if (ch < '0' || ch > '9') {
            allDigits = false;
            break;
        }
    }
    if (allDigits) {
        const int denominator = std::atoi(upper.c_str());
        for (const NoteValueEntry& entry : kNoteValues) {
            if (static_cast<int>(entry.value) == denominator) {
                out = entry.value;
                return true;
            }
        }
        return false;
    }

    // 英文名（大小写不敏感）
    for (const NoteValueEntry& entry : kNoteValues) {
        if (upper == ToUpperAscii(entry.name)) {
            out = entry.value;
            return true;
        }
    }
    return false;
}

std::string NoteValueListText() {
    std::string text;
    for (int i = 0; i < kNoteValueEntryCount; ++i) {
        if (!text.empty()) {
            text += " / ";
        }
        text += kNoteValues[i].display;
        text += "(";
        text += std::to_string(static_cast<int>(kNoteValues[i].value));
        text += ")";
    }
    return text;
}

bool NoteIsValid(const Note& note, std::string* reason) {
    if (note.startTime < 0.0) {
        if (reason != nullptr) {
            *reason = "起始时间为负";
        }
        return false;
    }
    if (note.duration <= 0.0) {
        if (reason != nullptr) {
            *reason = "持续时间为 0 或负";
        }
        return false;
    }
    if (note.startTime > 86400.0 || note.duration > 86400.0) {
        if (reason != nullptr) {
            *reason = "时间超出上限（86400 秒）";
        }
        return false;
    }
    return true;
}

std::string NoteToString(const Note& note) {
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "%s  起音 %.3fs  持续 %.3fs  释放 %.3fs",
                  ComboName(note.mode, note.key).c_str(), note.startTime, note.duration,
                  note.EndTime());
    return std::string(buffer);
}

}  // namespace hp
