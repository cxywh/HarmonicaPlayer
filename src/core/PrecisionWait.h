#pragma once

// 高精度等待（需求书第七节：粗等待 + 精等待的混合策略）。
//
// 三段式：
//   1. 距离目标还远        → 高精度可等待定时器（CreateWaitableTimerEx +
//                            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION，Win10 1803+）
//                            比 Sleep 准得多：Sleep 受系统计时器粒度影响（常见 15.6ms）；
//   2. 距离目标 1.5ms 以内 → std::this_thread::yield()，让出时间片但不睡；
//   3. 距离目标 0.3ms 以内 → 自旋（YieldProcessor），把最后一点误差压到微秒级。
//
// 为什么要三段：全程自旋会吃满一个核；全程 Sleep 精度不够。
// 只在最后 0.3ms 自旋，既能命中目标时刻，CPU 代价又可以忽略。
//
// 注意：本类**不做**误差补偿。调用方每次都用“绝对目标时刻”调用 WaitUntil，
// 所以偶尔一次睡晚了不会影响下一次 —— 这就是不累积误差的来源。

#include <string>
#include <vector>

namespace hp {

struct WaitPolicy {
    // 距目标超过这个毫秒数 → 用定时器粗等待
    double coarseThresholdMs = 1.5;
    // 距目标超过这个毫秒数（且不到粗等待阈值）→ yield 让出时间片
    double yieldThresholdMs = 0.25;
    // 粗等待时故意少睡这么多毫秒，避免睡过头（剩下的用 yield/自旋补齐）
    double coarseReserveMs = 0.9;
};

// 睡醒误差统计（正数 = 晚了，负数 = 早了）
struct WaitStats {
    int count = 0;
    double meanMs = 0.0;      // 平均误差（有正负）
    double meanAbsMs = 0.0;   // 平均绝对误差
    double maxEarlyMs = 0.0;  // 最大提前（负数）
    double maxLateMs = 0.0;   // 最大延后（正数）
    double stdDevMs = 0.0;    // 误差标准差

    // 以下三个是内部累加量（实现用，外部不用管）
    double accumulatedAbs = 0.0;
    double accumulatedM2 = 0.0;
    // 全样本（毫秒，带符号）：用于算分位数与“超限次数”
    std::vector<double> samplesMs;

    void Add(double errorSeconds);
    void Reset();
    // |误差| 的 q 分位（例如 0.5 / 0.95 / 0.99）；没有样本时返回 0
    double QuantileAbsMs(double q) const;
    // |误差| 超过 limitMs 的样本数 / 占比
    int CountAbsOver(double limitMs) const;
    double RatioAbsOver(double limitMs) const;
    std::string Summary() const;
};

class PrecisionWait {
public:
    PrecisionWait();
    ~PrecisionWait();

    PrecisionWait(const PrecisionWait&) = delete;
    PrecisionWait& operator=(const PrecisionWait&) = delete;

    // 是否拿到了高精度定时器（false 表示退化成 Sleep，精度会差一档）
    bool HighResolutionTimerAvailable() const { return highResolutionTimer_; }

    // 睡到指定的绝对时刻（用 core/Time.h 的同一时钟，单位秒）。
    // 返回 实际醒来时刻 - 目标时刻（秒，正数 = 晚）。
    double WaitUntil(double targetSeconds, const WaitPolicy& policy = WaitPolicy());

    // 至少睡 seconds（相对），返回 实际 - 期望
    double WaitSeconds(double seconds, const WaitPolicy& policy = WaitPolicy());

    // ---- 可被唤醒的等待（Phase 3 的高精度调度用它） ----
    // 立刻唤醒正在等待的线程：用于急停、暂停、播放中变速等“必须马上响应”的场景。
    // 语义：唤醒当前这一次等待；若没人正在等，则这一次唤醒被记住，下一次等待会立刻返回。
    void Wake();

    // 睡到 targetSeconds；被 Wake() 打断时返回 false（此时还没到目标时刻）。
    // 调用方拿到 false 应当重新判断命令与目标时刻，再决定是否继续等。
    // 实现上用 WaitForMultipleObjects 同时等“定时器”和“唤醒事件”，
    // 所以急停不需要轮询、也不用把长等待切成小片。
    bool WaitUntilWakeable(double targetSeconds, const WaitPolicy& policy = WaitPolicy());

    // 唤醒事件是否已被触发（会消费掉本次信号）
    bool ConsumeWakeSignal();

private:
    // 粗等待一次；失败时退回 Sleep。watchWake=true 时可被唤醒，返回 false 表示被打断。
    bool WaitOnceMs(double milliseconds, bool watchWake);

    void* timer_ = nullptr;       // HANDLE：高精度可等待定时器
    void* wakeEvent_ = nullptr;   // HANDLE：自动复位的唤醒事件
    bool highResolutionTimer_ = false;
};

// 把当前线程提升到“时间关键（TIME_CRITICAL）”，析构时自动恢复原优先级。
// 用途：实测表明播放时最坏的“晚醒”几乎都来自系统调度抢占，而不是等待算法本身；
// 把播放线程提到高优先级能显著压低最坏值（Phase 3 的播放线程会用）。
// 注意：只在等待期间短时间使用，析构即恢复，避免影响系统整体调度。
class ScopedThreadPriorityBoost {
public:
    ScopedThreadPriorityBoost();
    ~ScopedThreadPriorityBoost();

    ScopedThreadPriorityBoost(const ScopedThreadPriorityBoost&) = delete;
    ScopedThreadPriorityBoost& operator=(const ScopedThreadPriorityBoost&) = delete;

    bool Applied() const { return applied_; }
    int PreviousPriority() const { return previousPriority_; }

private:
    bool applied_ = false;
    int previousPriority_ = 0;
};

// 进程 CPU 时间（用户态 + 内核态，秒），用于量化“精度 vs CPU 占用”
double ProcessCpuSeconds();

}  // namespace hp
