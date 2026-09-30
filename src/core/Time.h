#pragma once

// 高精度单调时钟。
// 播放器的整个时间轴必须建立在这个时钟上：
//   - QueryPerformanceCounter 是单调的，不会因为系统对时、夏令时、NTP 调整而跳变；
//   - system_clock（std::chrono::system_clock / time()）绝对不可以当播放时钟用。
// Phase 2 的 PlaybackClock 会直接基于这里的 NowTicks()。

namespace hp {

long long NowTicks();        // QPC 原始计数值（单调递增）
long long TicksPerSecond();  // QPC 频率
double    NowSeconds();      // NowTicks() / TicksPerSecond()，单位秒

// 把秒换算成 QPC 计数，供“绝对时间轴 -> 目标时刻”转换使用
inline long long SecondsToTicks(double seconds) {
    return static_cast<long long>(seconds * static_cast<double>(TicksPerSecond()));
}

// 把 QPC 计数换算成秒
inline double TicksToSeconds(long long ticks) {
    return static_cast<double>(ticks) / static_cast<double>(TicksPerSecond());
}

}  // namespace hp
