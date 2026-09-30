#pragma once

// 内置测试曲目。
//
// 用途：
//   1. 验证 BPM / 拍号 / 时值的换算（Metronome 的时间是最整的：60 BPM → 每个四分音符正好 1.000 秒）；
//   2. 验证长时间播放不跑拍（LongSong 可以生成几千个音符）；
//   3. 覆盖 8 键 × 4 种演奏方式（AllModes）。
//
// Phase 9 的文本谱、Phase 10 的录制也会拿这些曲子当基准。

#include <string>
#include <vector>

#include "song/Song.h"

namespace hp {
namespace test_songs {

// 内置曲目的名字列表（顺序即推荐顺序）
std::vector<std::string> Names();

// 按名字取曲目；名字不认识时返回一个带问题的空曲（可查 Validate）
Song ByName(const std::string& name);

// 60 BPM、4/4、纯四分音符，全部用 Z 键 → 每个音符正好间隔 1.000 秒，共 16 个音符（16 秒）
Song Metronome();

// 120 BPM、4/4：前 8 个四分音符上行音阶，后 8 个八分音符下行 → 混合时值
Song Scale();

// 200 BPM、4/4、八分音符密集跑动（每秒 6.67 个音），用来压时间轴
Song ScaleFast();

// 3/4 拍圆舞曲：100 BPM，左手/中键交替
Song Waltz();

// 6/8 拍：120 BPM 且以八分音符为一拍（beatsPerBar = 6）
Song Jig();

// 8 键 × 4 种演奏方式 = 32 个音符全覆盖
Song AllModes();

// 长时间曲子：共 noteCount 个音符，间隔 intervalMs 毫秒，模式轮换
Song LongSong(int noteCount, double intervalMs = 20.0);

}  // namespace test_songs
}  // namespace hp
