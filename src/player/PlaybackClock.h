#pragma once

// 播放时钟：把“墙钟时间”映射成“歌曲时间轴上的位置”。
//
//   position = positionBase + (墙钟耗时 - 累计暂停时长) × speed
//
// 关键点（需求书第五、六、七、二十二、二十九节）：
//   1. 基于 QPC 单调时钟（core/Time.h），不用 system_clock，也不靠 Sleep 计时；
//   2. **位置是算出来的，不是累加出来的** —— 每次读位置都从基准点重算，
//      所以任何一次调度延迟都不会传到下一次，误差不累积；
//   3. 变速是**时间映射**（位置 = 墙钟 × 速度），不是把每次 Sleep 乘一个倍数；
//      变速瞬间位置连续，不会跳变；
//   4. 暂停时位置冻结，继续时从暂停处接着走（不会回到开头）。

#include <mutex>
#include <string>

namespace hp {

class PlaybackClock {
public:
    PlaybackClock() = default;

    PlaybackClock(const PlaybackClock&) = delete;
    PlaybackClock& operator=(const PlaybackClock&) = delete;

    // 从歌曲第 0 秒开始走
    void Start();
    // 从指定位置开始走（重播 / 继续）
    void StartAt(double positionSeconds);
    // 停止并复位到 0（不改速度：速度属于设置，Stop 只复位位置）
    void Stop();

    void Pause();
    void Resume();

    // 直接跳到某个位置（保持当前的“在走 / 暂停”状态）
    void Seek(double positionSeconds);

    bool IsRunning() const;   // 正在推进（不含暂停）
    bool IsPaused() const;
    bool IsStopped() const;

    // 歌曲时间轴上的位置（秒）
    double PositionSeconds() const;
    // 实际墙钟耗时（秒），不含暂停、**未乘速度**
    double ElapsedWallSeconds() const;

    double Speed() const;
    // 时间映射：位置 = 墙钟 × 速度。变速时位置连续。
    void SetSpeed(double speed);

    // 让调度器能算“某个位置对应哪个墙钟时刻”：
    //   目标墙钟 = NowSeconds() + (targetPosition - PositionSeconds()) / Speed
    // 用它配合 PrecisionWait 睡到目标时刻。
    double WallClockForPosition(double positionSeconds) const;

    // 一行状态描述，便于日志与排查
    std::string Describe() const;

private:
    // 调用者必须已持锁
    double WallSinceBaseLocked() const;
    double PositionLocked() const;
    void RebaseLocked(double positionSeconds);

    mutable std::mutex mutex_;
    bool running_ = false;
    bool paused_ = false;
    double speed_ = 1.0;
    double positionBase_ = 0.0;      // 基准位置（秒）
    long long baseTicks_ = 0;        // 基准点对应的 QPC 计数
    double pausedWall_ = 0.0;        // 已结算的累计暂停时长
    long long pauseStartTicks_ = 0;  // 本次暂停开始的 QPC 计数
};

}  // namespace hp
