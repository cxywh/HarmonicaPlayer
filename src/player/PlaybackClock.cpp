#include "player/PlaybackClock.h"

#include <cmath>
#include <cstdio>

#include "core/Time.h"

namespace hp {
namespace {

constexpr double kMinSpeed = 0.01;
constexpr double kMaxSpeed = 100.0;

}  // namespace

double PlaybackClock::WallSinceBaseLocked() const {
    // 已停止：墙钟不再推进。若继续用“现在 - 基准点”算，会因为基准点已被清零
    // 而算出开机以来的巨大数值 —— 这是时钟语义上必须守住的边界。
    if (!running_) {
        return 0.0;
    }
    // 暂停期间墙钟不推进：用暂停开始时刻代替“现在”
    const long long now = paused_ ? pauseStartTicks_ : NowTicks();
    double wall = TicksToSeconds(now - baseTicks_) - pausedWall_;
    return wall > 0.0 ? wall : 0.0;
}

double PlaybackClock::PositionLocked() const {
    // 已停止：停在基准位置（Stop 会把它归零，Seek 则保留跳转后的位置）
    if (!running_) {
        return positionBase_ > 0.0 ? positionBase_ : 0.0;
    }
    const double position = positionBase_ + WallSinceBaseLocked() * speed_;
    return position > 0.0 ? position : 0.0;
}

void PlaybackClock::RebaseLocked(double positionSeconds) {
    // 把当前位置固化成新的基准点，之后的位置仍然从基准点算出（不累加）
    positionBase_ = positionSeconds > 0.0 ? positionSeconds : 0.0;
    baseTicks_ = NowTicks();
    pausedWall_ = 0.0;
    if (paused_) {
        pauseStartTicks_ = baseTicks_;
    }
}

void PlaybackClock::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = true;
    paused_ = false;
    RebaseLocked(0.0);
}

void PlaybackClock::StartAt(double positionSeconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = true;
    paused_ = false;
    RebaseLocked(positionSeconds);
}

void PlaybackClock::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    paused_ = false;
    positionBase_ = 0.0;
    baseTicks_ = 0;
    pausedWall_ = 0.0;
    pauseStartTicks_ = 0;
}

void PlaybackClock::Pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || paused_) {
        return;
    }
    pauseStartTicks_ = NowTicks();
    paused_ = true;
}

void PlaybackClock::Resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || !paused_) {
        return;
    }
    // 把刚刚这段暂停时长结算进“累计暂停”，位置因此在暂停前后保持连续
    pausedWall_ += TicksToSeconds(NowTicks() - pauseStartTicks_);
    paused_ = false;
}

void PlaybackClock::Seek(double positionSeconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    RebaseLocked(positionSeconds);
}

bool PlaybackClock::IsRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ && !paused_;
}

bool PlaybackClock::IsPaused() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ && paused_;
}

bool PlaybackClock::IsStopped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !running_;
}

double PlaybackClock::PositionSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return PositionLocked();
}

double PlaybackClock::ElapsedWallSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return WallSinceBaseLocked();
}

double PlaybackClock::Speed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return speed_;
}

void PlaybackClock::SetSpeed(double speed) {
    double clamped = speed;
    if (!(clamped > kMinSpeed)) {
        clamped = kMinSpeed;
    } else if (clamped > kMaxSpeed) {
        clamped = kMaxSpeed;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (std::fabs(clamped - speed_) < 1e-12) {
        return;
    }
    // 先取当前位置（用旧速度算），再以该位置为新基准 —— 变速瞬间位置连续
    const double position = PositionLocked();
    speed_ = clamped;
    RebaseLocked(position);
}

double PlaybackClock::WallClockForPosition(double positionSeconds) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const double currentPosition = PositionLocked();
    const double remaining = positionSeconds - currentPosition;
    if (speed_ <= 0.0) {
        return NowSeconds();
    }
    // 剩余歌曲时间 ÷ 速度 = 需要等待的墙钟时间
    return NowSeconds() + remaining / speed_;
}

std::string PlaybackClock::Describe() const {
    std::lock_guard<std::mutex> lock(mutex_);
    char buffer[192];
    const char* state = !running_ ? "已停止" : (paused_ ? "已暂停" : "演奏中");
    std::snprintf(buffer, sizeof(buffer), "状态 %s  位置 %.4f s  速度 %.0f%%  墙钟 %.4f s", state,
                  PositionLocked(), speed_ * 100.0, WallSinceBaseLocked());
    return std::string(buffer);
}

}  // namespace hp
