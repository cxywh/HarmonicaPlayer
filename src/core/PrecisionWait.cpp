#include "core/PrecisionWait.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "core/Platform.h"
#include "core/Time.h"

namespace hp {
namespace {

// 自旋提示：告诉 CPU“我在等”，比什么都不做省电一点
inline void SpinHint() {
    YieldProcessor();
}

}  // namespace

// ---------------------------------------------------------------------------
// WaitStats
// ---------------------------------------------------------------------------
void WaitStats::Reset() {
    count = 0;
    meanMs = 0.0;
    meanAbsMs = 0.0;
    maxEarlyMs = 0.0;
    maxLateMs = 0.0;
    stdDevMs = 0.0;
    accumulatedAbs = 0.0;
    accumulatedM2 = 0.0;
    samplesMs.clear();
}

void WaitStats::Add(double errorSeconds) {
    const double errorMs = errorSeconds * 1000.0;
    ++count;
    samplesMs.push_back(errorMs);

    if (count == 1) {
        meanMs = errorMs;
        accumulatedAbs = std::fabs(errorMs);
        meanAbsMs = accumulatedAbs;
        accumulatedM2 = 0.0;
        stdDevMs = 0.0;
        maxEarlyMs = errorMs < 0.0 ? errorMs : 0.0;
        maxLateMs = errorMs > 0.0 ? errorMs : 0.0;
        return;
    }

    // 平均值与标准差的在线更新（Welford）
    const double delta = errorMs - meanMs;
    meanMs += delta / static_cast<double>(count);
    accumulatedM2 += delta * (errorMs - meanMs);
    stdDevMs = std::sqrt(accumulatedM2 / static_cast<double>(count - 1));

    accumulatedAbs += std::fabs(errorMs);
    meanAbsMs = accumulatedAbs / static_cast<double>(count);

    if (errorMs < maxEarlyMs) {
        maxEarlyMs = errorMs;
    }
    if (errorMs > maxLateMs) {
        maxLateMs = errorMs;
    }
}

double WaitStats::QuantileAbsMs(double q) const {
    if (samplesMs.empty()) {
        return 0.0;
    }
    std::vector<double> absoluteValues;
    absoluteValues.reserve(samplesMs.size());
    for (double value : samplesMs) {
        absoluteValues.push_back(std::fabs(value));
    }
    std::sort(absoluteValues.begin(), absoluteValues.end());

    double clamped = q;
    if (!(clamped > 0.0)) {
        clamped = 0.0;
    } else if (clamped > 1.0) {
        clamped = 1.0;
    }
    size_t index = static_cast<size_t>(clamped * static_cast<double>(absoluteValues.size() - 1) + 0.5);
    if (index >= absoluteValues.size()) {
        index = absoluteValues.size() - 1;
    }
    return absoluteValues[index];
}

int WaitStats::CountAbsOver(double limitMs) const {
    int over = 0;
    for (double value : samplesMs) {
        if (std::fabs(value) > limitMs) {
            ++over;
        }
    }
    return over;
}

double WaitStats::RatioAbsOver(double limitMs) const {
    if (count <= 0) {
        return 0.0;
    }
    return static_cast<double>(CountAbsOver(limitMs)) / static_cast<double>(count);
}

std::string WaitStats::Summary() const {
    char buffer[384];
    std::snprintf(buffer, sizeof(buffer),
                  "样本 %d 次  平均误差 %+.3f ms  平均绝对误差 %.3f ms  "
                  "|误差| 中位 %.3f ms  p95 %.3f ms  p99 %.3f ms  "
                  "最大延后 %+.3f ms  最大提前 %+.3f ms  标准差 %.3f ms  "
                  "超 1ms %d 次  超 3ms %d 次",
                  count, meanMs, meanAbsMs, QuantileAbsMs(0.50), QuantileAbsMs(0.95),
                  QuantileAbsMs(0.99), maxLateMs, maxEarlyMs, stdDevMs, CountAbsOver(1.0),
                  CountAbsOver(3.0));
    return std::string(buffer);
}

// ---------------------------------------------------------------------------
// PrecisionWait
// ---------------------------------------------------------------------------
PrecisionWait::PrecisionWait() {
    // 高精度可等待定时器（Win10 1803+）。拿不到就退化成 Sleep。
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    if (timer != nullptr) {
        highResolutionTimer_ = true;
    } else {
        timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        highResolutionTimer_ = false;
    }
    timer_ = timer;

    // 唤醒事件：自动复位（等一次消费一次），初始未触发。
    // 这是“急停不需要轮询”的关键——SetEvent 会让正卡在长时间等待里的线程立刻醒来。
    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

PrecisionWait::~PrecisionWait() {
    if (timer_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(timer_));
        timer_ = nullptr;
    }
    if (wakeEvent_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(wakeEvent_));
        wakeEvent_ = nullptr;
    }
}

void PrecisionWait::Wake() {
    if (wakeEvent_ != nullptr) {
        SetEvent(static_cast<HANDLE>(wakeEvent_));
    }
}

bool PrecisionWait::ConsumeWakeSignal() {
    if (wakeEvent_ == nullptr) {
        return false;
    }
    return WaitForSingleObject(static_cast<HANDLE>(wakeEvent_), 0) == WAIT_OBJECT_0;
}

bool PrecisionWait::WaitOnceMs(double milliseconds, bool watchWake) {
    if (milliseconds <= 0.0) {
        return true;
    }
    if (watchWake && ConsumeWakeSignal()) {
        return false;
    }

    const HANDLE timer = static_cast<HANDLE>(timer_);
    if (timer != nullptr && milliseconds >= 0.5) {
        // 相对时间用负数表示（单位 100 纳秒）；周期 0 = 只触发一次
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(milliseconds * 10000.0);
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            if (watchWake && wakeEvent_ != nullptr) {
                // 同时等“定时器到点”和“被唤醒”，谁先来算谁 —— 急停延迟因此是微秒级
                HANDLE handles[2] = {timer, static_cast<HANDLE>(wakeEvent_)};
                const DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                if (result == WAIT_OBJECT_0 + 1) {
                    return false;   // 被唤醒（定时器没到）
                }
                return true;
            }
            WaitForSingleObject(timer, INFINITE);
            return true;
        }
    }

    // 退路：普通 Sleep（系统计时器粒度，常见 15.6ms，精度差一档）
    if (!watchWake) {
        std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(milliseconds));
        return true;
    }
    // 可唤醒版本：Sleep 无法被打断，所以切成小块睡（只在没有高精度定时器时才会走到这里）
    double remaining = milliseconds;
    while (remaining > 0.0) {
        const double chunk = remaining > 2.0 ? 2.0 : remaining;
        std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(chunk));
        remaining -= chunk;
        if (ConsumeWakeSignal()) {
            return false;
        }
    }
    return true;
}

double PrecisionWait::WaitUntil(double targetSeconds, const WaitPolicy& policy) {
    for (;;) {
        const double remainingMs = (targetSeconds - NowSeconds()) * 1000.0;
        if (remainingMs <= 0.0) {
            break;
        }
        if (remainingMs > policy.coarseThresholdMs) {
            // 粗等待：少睡一点（预留 yield/自旋的余量），避免一次性睡过头
            double sleepMs = remainingMs - policy.coarseReserveMs;
            if (sleepMs < 0.0) {
                sleepMs = 0.0;
            }
            WaitOnceMs(sleepMs, false);
        } else if (remainingMs > policy.yieldThresholdMs) {
            // 精等待：让出时间片，但不放弃 CPU
            std::this_thread::yield();
        } else {
            // 亚毫秒：自旋，把最后一点误差压到最小
            SpinHint();
        }
    }
    return NowSeconds() - targetSeconds;
}

bool PrecisionWait::WaitUntilWakeable(double targetSeconds, const WaitPolicy& policy) {
    for (;;) {
        // 先看有没有急停/暂停/变速的唤醒信号；有就立刻交还控制权
        if (ConsumeWakeSignal()) {
            return false;
        }
        const double remainingMs = (targetSeconds - NowSeconds()) * 1000.0;
        if (remainingMs <= 0.0) {
            return true;
        }
        if (remainingMs > policy.coarseThresholdMs) {
            double sleepMs = remainingMs - policy.coarseReserveMs;
            if (sleepMs < 0.0) {
                sleepMs = 0.0;
            }
            if (!WaitOnceMs(sleepMs, true)) {
                return false;
            }
        } else if (remainingMs > policy.yieldThresholdMs) {
            std::this_thread::yield();
        } else {
            SpinHint();
        }
    }
}

double PrecisionWait::WaitSeconds(double seconds, const WaitPolicy& policy) {
    const double target = NowSeconds() + seconds;
    return WaitUntil(target, policy);
}

// ---------------------------------------------------------------------------
// 播放线程优先级（RAII 提升 + 自动恢复）
// ---------------------------------------------------------------------------
ScopedThreadPriorityBoost::ScopedThreadPriorityBoost() {
    const HANDLE thread = GetCurrentThread();
    const int priority = GetThreadPriority(thread);
    if (priority == THREAD_PRIORITY_ERROR_RETURN) {
        applied_ = false;
        return;
    }
    previousPriority_ = priority;
    // TIME_CRITICAL：比普通线程先被调度，用来压低“被系统抢占”造成的偶发晚醒
    applied_ = SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL) != FALSE;
}

ScopedThreadPriorityBoost::~ScopedThreadPriorityBoost() {
    if (applied_) {
        SetThreadPriority(GetCurrentThread(), previousPriority_);
        applied_ = false;
    }
}

// ---------------------------------------------------------------------------
// 进程 CPU 时间
// ---------------------------------------------------------------------------
double ProcessCpuSeconds() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return 0.0;
    }
    auto toSeconds = [](const FILETIME& time) -> double {
        ULARGE_INTEGER value{};
        value.LowPart = time.dwLowDateTime;
        value.HighPart = time.dwHighDateTime;
        // FILETIME 单位是 100 纳秒
        return static_cast<double>(value.QuadPart) / 1e7;
    };
    return toSeconds(kernel) + toSeconds(user);
}

}  // namespace hp
