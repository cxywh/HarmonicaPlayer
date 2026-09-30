#include "song/TestSongs.h"

#include <algorithm>

namespace hp {
namespace test_songs {

// 曲目表用的前向声明（实现见本文件后半部分）
Song Metronome();
Song Scale();
Song ScaleFast();
Song Waltz();
Song Jig();
Song AllModes();
Song LongDefault();

namespace {

const char* kScaleKeys[] = {"Z", "X", "C", "V", "B", "N", "M", ","};

Key KeyByIndex(int index) {
    const int wrapped = ((index % kKeyCount) + kKeyCount) % kKeyCount;
    return static_cast<Key>(wrapped);
}

PlayMode ModeByIndex(int index) {
    const int wrapped = ((index % kPlayModeCount) + kPlayModeCount) % kPlayModeCount;
    return static_cast<PlayMode>(wrapped);
}

// 曲目表：中文曲名 + ASCII 别名 + 中文关键词 → 构造函数。
// 顺序有讲究：关键词匹配是从上往下找第一个命中的，所以“快速”要排在“音阶”前面。
struct SongEntry {
    const char* display;   // 中文曲名（也是菜单里显示的名字）
    const char* ascii;     // ASCII 别名，空格分隔（大小写不敏感）
    const char* keyword;   // 中文关键词（包含匹配，方便输入「音阶」这种简写）
    Song (*build)();
};

const SongEntry kSongEntries[] = {
    {"节拍器 60BPM", "metronome metro tick", "节拍", &Metronome},
    {"快速音阶 200BPM", "fast", "快速", &ScaleFast},
    {"C 大调音阶", "scale", "音阶", &Scale},
    {"圆舞曲 3/4", "waltz", "圆舞", &Waltz},
    {"吉格舞曲 6/8", "jig", "吉格", &Jig},
    {"32 种操作全覆盖", "modes all-modes", "覆盖", &AllModes},
    {"长曲 600 音", "long", "长曲", &LongDefault},
};
constexpr int kSongEntryCount = static_cast<int>(sizeof(kSongEntries) / sizeof(kSongEntries[0]));

// 去首尾空白 + ASCII 转小写（不碰多字节字符，避免破坏中文）
std::string TrimLowerAscii(const std::string& text) {
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
    std::string result = text.substr(begin, end - begin);
    for (char& ch : result) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return result;
}

// asciiList 里是否有 token 这个词（空格分隔）
bool ContainsWord(const char* asciiList, const std::string& token) {
    if (token.empty()) {
        return false;
    }
    const std::string list = TrimLowerAscii(asciiList != nullptr ? asciiList : "");
    size_t position = 0;
    while (position < list.size()) {
        while (position < list.size() && list[position] == ' ') {
            ++position;
        }
        size_t next = list.find(' ', position);
        if (next == std::string::npos) {
            next = list.size();
        }
        if (list.compare(position, next - position, token) == 0) {
            return true;
        }
        position = next;
    }
    return false;
}

BeatNote Beat(Key key, PlayMode mode, double beat, NoteValue value) {
    BeatNote note;
    note.key = key;
    note.mode = mode;
    note.beat = beat;
    note.value = value;
    return note;
}

}  // namespace

Song LongDefault() {
    return LongSong(600);
}

std::vector<std::string> Names() {
    std::vector<std::string> names;
    names.reserve(static_cast<size_t>(kSongEntryCount));
    for (const SongEntry& entry : kSongEntries) {
        names.push_back(entry.display);
    }
    return names;
}

Song Metronome() {
    std::vector<BeatNote> beats;
    for (int i = 0; i < 16; ++i) {
        beats.push_back(Beat(Key::Z, PlayMode::Normal, static_cast<double>(i),
                             NoteValue::Quarter));
    }
    return MakeSongFromBeats("节拍器 60BPM", 60.0, NoteValue::Quarter, TimeSignature{4, 4}, beats);
}

Song Scale() {
    std::vector<BeatNote> beats;
    // 上行：8 个四分音符（第 0 ~ 7 拍），每拍 0.5 秒（120 BPM）
    for (int i = 0; i < kKeyCount; ++i) {
        beats.push_back(Beat(KeyByIndex(i), PlayMode::Normal, static_cast<double>(i),
                             NoteValue::Quarter));
    }
    // 下行：8 个八分音符（第 8 拍起，每 0.5 拍一个）
    // 注意：beat 递增而键位递减，才是“听得出来”的下行；两者都递增会变成重复上行。
    for (int i = 0; i < kKeyCount; ++i) {
        const double beat = 8.0 + 0.5 * static_cast<double>(i);
        beats.push_back(Beat(KeyByIndex(kKeyCount - 1 - i), PlayMode::Normal, beat,
                             NoteValue::Eighth));
    }
    return MakeSongFromBeats("C 大调音阶", 120.0, NoteValue::Quarter, TimeSignature{4, 4}, beats);
}

Song ScaleFast() {
    std::vector<BeatNote> beats;
    for (int i = 0; i < 32; ++i) {
        beats.push_back(Beat(KeyByIndex(i), ModeByIndex(i / 8), 0.5 * static_cast<double>(i),
                             NoteValue::Eighth));
    }
    return MakeSongFromBeats("快速音阶 200BPM", 200.0, NoteValue::Quarter, TimeSignature{4, 4},
                             beats);
}

Song Waltz() {
    std::vector<BeatNote> beats;
    // 3/4 拍：每小节 3 拍，低音（中键）落在第一拍，其余用普通按键
    const int bars = 4;
    for (int bar = 0; bar < bars; ++bar) {
        beats.push_back(Beat(Key::M, PlayMode::Middle, static_cast<double>(bar * 3),
                             NoteValue::Quarter));
        beats.push_back(Beat(Key::B, PlayMode::Normal, static_cast<double>(bar * 3 + 1),
                             NoteValue::Quarter));
        beats.push_back(Beat(Key::N, PlayMode::Normal, static_cast<double>(bar * 3 + 2),
                             NoteValue::Quarter));
    }
    return MakeSongFromBeats("圆舞曲 3/4", 100.0, NoteValue::Quarter, TimeSignature{3, 4}, beats);
}

Song Jig() {
    std::vector<BeatNote> beats;
    // 6/8 拍，以八分音符为一拍：一小节 6 拍
    for (int i = 0; i < 12; ++i) {
        beats.push_back(Beat(KeyByIndex(i), PlayMode::Normal, static_cast<double>(i),
                             NoteValue::Eighth));
    }
    return MakeSongFromBeats("吉格舞曲 6/8", 120.0, NoteValue::Eighth, TimeSignature{6, 8}, beats);
}

Song AllModes() {
    std::vector<BeatNote> beats;
    int index = 0;
    for (int mode = 0; mode < kPlayModeCount; ++mode) {
        for (int key = 0; key < kKeyCount; ++key) {
            beats.push_back(Beat(static_cast<Key>(key), static_cast<PlayMode>(mode),
                                 static_cast<double>(index), NoteValue::Eighth));
            ++index;
        }
    }
    return MakeSongFromBeats("32 种操作全覆盖", 120.0, NoteValue::Quarter, TimeSignature{4, 4},
                             beats);
}

Song LongSong(int noteCount, double intervalMs) {
    if (noteCount < 1) {
        noteCount = 1;
    }
    const double intervalSeconds = std::max(1.0, intervalMs) / 1000.0;

    Song song;
    song.name = "长曲 " + std::to_string(noteCount) + " 音";
    song.bpm = 120.0;
    song.beatUnit = NoteValue::Quarter;
    song.timeSignature = TimeSignature{4, 4};

    song.notes.reserve(static_cast<size_t>(noteCount));
    for (int i = 0; i < noteCount; ++i) {
        Note note;
        note.key = KeyByIndex(i);
        note.mode = ModeByIndex(i / 4);   // 每 4 个音换一种演奏方式，覆盖鼠标按键
        note.startTime = static_cast<double>(i) * intervalSeconds;
        note.duration = intervalSeconds * 0.7;   // 留出释放间隙，避免音符互相重叠
        song.notes.push_back(note);
    }
    return song;
}

Song ByName(const std::string& rawName) {
    const std::string name = TrimLowerAscii(rawName);

    // 1) 中文曲名精确匹配
    for (const SongEntry& entry : kSongEntries) {
        if (rawName == entry.display) {
            return entry.build();
        }
    }
    // 2) ASCII 别名（大小写不敏感）
    for (const SongEntry& entry : kSongEntries) {
        if (ContainsWord(entry.ascii, name)) {
            return entry.build();
        }
    }
    // 3) 中文关键词包含匹配（表里越具体的排在越前面）
    for (const SongEntry& entry : kSongEntries) {
        if (rawName.find(entry.keyword) != std::string::npos) {
            return entry.build();
        }
    }

    Song empty;
    empty.name = "未找到：" + rawName;
    empty.bpm = 0.0;   // 制造一个明显的问题，调用方的 Validate 会报出来
    return empty;
}

}  // namespace test_songs
}  // namespace hp
