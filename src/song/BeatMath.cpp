#include "song/BeatMath.h"

#include <cmath>
#include <cstdio>

namespace hp {
namespace beat {

namespace {

const double kPresets[kSpeedPresetCount] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};

// 拍数 → 两位小数（整数不补零以外的修饰）
std::string Trimmed(double value, int decimals) {
    char buffer[64] = "";
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    std::string text(buffer);
    // 去掉尾随 0 和小数点："2.000" → "2"，"1.500" → "1.5"
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    return text;
}

}  // namespace

double SpeedPreset(int index) {
    if (index < 0 || index >= kSpeedPresetCount) {
        return 1.0;
    }
    return kPresets[index];
}

std::string SpeedPresetText(int index) {
    const double speed = SpeedPreset(index);
    return Trimmed(speed * 100.0, 0) + "%";
}

bool ClampSpeed(double input, double& out, std::string& error) {
    if (!(input == input)) {   // NaN
        error = "速度填的不是一个数字";
        return false;
    }
    // 容差：0.4999… 视为 50%
    if (input < kSpeedMin - 1e-9) {
        error = "速度不能低于 50%（需求书二十九节的范围是 50% ~ 200%）";
        return false;
    }
    if (input > kSpeedMax + 1e-9) {
        error = "速度不能高于 200%（需求书二十九节的范围是 50% ~ 200%）";
        return false;
    }
    if (input < kSpeedMin) {
        input = kSpeedMin;
    }
    if (input > kSpeedMax) {
        input = kSpeedMax;
    }
    out = input;
    return true;
}

double SecondsPerBeat(double bpm) {
    if (!(bpm > 0.0)) {
        return 0.0;
    }
    return 60.0 / bpm;
}

double BeatMs(const Song& song) {
    return song.SecondsPerBeat() * 1000.0;
}

double EffectiveBpm(const Song& song, double speed) {
    return song.bpm * speed;
}

std::string BeatMsText(const Song& song) {
    const double perBar = BeatMs(song);
    if (!(perBar > 0.0)) {
        return "1 拍 = 无法计算（BPM 不合法）";
    }
    // 500 ms / 666.667 ms 这种：整数不显示小数，非整数保留三位
    const bool whole = std::fabs(perBar - std::floor(perBar + 0.5)) < 1e-9;
    return "1 拍 = " + Trimmed(perBar, whole ? 0 : 3) + " ms";
}

double BeatsPerBar(const Song& song) {
    return song.timeSignature.BeatsPerBar();
}

double SecondsPerBar(const Song& song) {
    return song.SecondsPerBar();
}

int BarCount(const Song& song) {
    const double perBar = SecondsPerBar(song);
    const double total = song.TotalDuration();
    if (!(perBar > 0.0) || !(total > 0.0)) {
        return 0;
    }
    return static_cast<int>(std::ceil(total / perBar - 1e-9));
}

std::string SignatureText(const Song& song) {
    return song.timeSignature.ToString() + "　每小节 " + Trimmed(BeatsPerBar(song), 0) + " 拍 = " +
           Trimmed(SecondsPerBar(song), 3) + " 秒";
}

BarSpot LocateBarByBeat(const Song& song, double beat) {
    BarSpot spot;
    const double perBar = BeatsPerBar(song);
    if (!(perBar > 0.0) || !(beat > 0.0)) {
        return spot;   // 第 1 小节第 0 拍
    }
    const int index = static_cast<int>(std::floor(beat / perBar + 1e-9));
    spot.bar = index + 1;
    spot.beatInBar = beat - static_cast<double>(index) * perBar;
    if (spot.beatInBar < 0.0) {
        spot.beatInBar = 0.0;
    }
    return spot;
}

BarSpot LocateBar(const Song& song, double songSeconds) {
    const double perBeat = song.SecondsPerBeat();
    if (!(perBeat > 0.0) || !(songSeconds > 0.0)) {
        return BarSpot{};
    }
    return LocateBarByBeat(song, songSeconds / perBeat);
}

int BarOfBeat(const Song& song, double beat) {
    return LocateBarByBeat(song, beat).bar;
}

double NoteDurationMs(const Song& song, NoteValue value) {
    return NoteValueBeats(value, song.beatUnit) * BeatMs(song);
}

std::string NoteFractionText(NoteValue value) {
    const int denominator = static_cast<int>(value);
    if (denominator <= 0) {
        return "?";
    }
    // 全音符 = 1/1，二分 = 1/2，四分 = 1/4 …
    return "1/" + std::to_string(denominator);
}

std::string NoteValueCellText(const Song& song, NoteValue value) {
    const double ms = NoteDurationMs(song, value);
    const bool whole = std::fabs(ms - std::floor(ms + 0.5)) < 1e-6;
    return std::string(NoteValueDisplayName(value)) + " " + NoteFractionText(value) + "（" +
           Trimmed(ms, whole ? 0 : 1) + " ms）";
}

}  // namespace beat
}  // namespace hp
