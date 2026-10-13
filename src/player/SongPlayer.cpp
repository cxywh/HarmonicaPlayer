#include "player/SongPlayer.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>

#include "core/Log.h"
#include "core/Platform.h"
#include "core/Time.h"

namespace hp {
namespace {

constexpr double kInfinite = std::numeric_limits<double>::infinity();

// 两个音符算“紧挨着”的时间容差（秒）：前一个的结束 == 后一个的开始（《父亲》就是这种排布）。
// 紧挨着的同 mode 音符之间保持按住鼠标键，不在边界上做“抬起 → 立刻重按”。
constexpr double kModeKeepEpsilon = 1e-6;

}  // namespace

SongPlayer::SongPlayer(InputController& input, const PlayerOptions& options)
    : input_(input), options_(options) {
    // 播放线程常驻：避免每次按下“播放”都新建线程（那会带来毫秒级的额外抖动）
    worker_ = std::thread(&SongPlayer::WorkerMain, this);
}

SongPlayer::~SongPlayer() {
    Quit();
}

// ---------------------------------------------------------------------------
// 播放线程
// ---------------------------------------------------------------------------
void SongPlayer::WorkerMain() {
    threadId_.store(GetCurrentThreadId());
    SetThreadDescription(GetCurrentThread(), L"HarmonicaPlayback");

    for (;;) {
        std::shared_ptr<const Timeline> timeline;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return quit_.load() || startPending_; });
            if (quit_.load()) {
                break;
            }
            // 新一轮开始：清掉上一轮留下的命令标志
            startPending_ = false;
            stopRequested_ = false;
            pauseRequested_ = false;
            finishRequested_ = false;
            timeline = timeline_;
            loopActive_ = true;
            state_ = PlaybackState::Playing;
            next_ = timeline ? timeline->LowerBound(startPosition_) : 0;
            // 游标是“本曲已起音的音符数”，继续演奏时从暂停位置对应的音符接着算，绝不倒退
            firedNotes_.store(next_);
            clock_.StartAt(startPosition_);
            clock_.SetSpeed(speed_);
        }
        cv_.notify_all();

        if (timeline) {
            if (options_.boostPlaybackThreadPriority) {
                // 只在演奏期间提升优先级，退出这一轮立刻恢复
                ScopedThreadPriorityBoost boost;
                PlayLoop(*timeline);
            } else {
                PlayLoop(*timeline);
            }
        }

        // ---- 本轮收尾 ----
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loopActive_ = false;
            heldCount_.store(0);
            if (finishRequested_) {
                finishRequested_ = false;
                // 只有“自然播放结束”才由这里写状态；停止/暂停的状态已由命令方写好
                if (!stopRequested_.load() && !pauseRequested_.load() && !startPending_ &&
                    !restartRequested_) {
                    state_ = PlaybackState::Stopped;
                    reason_ = StopReason::Finished;
                    reasonDetail_.clear();
                    clock_.Stop();   // 位置归零
                }
            }
            if (restartRequested_) {
                restartRequested_ = false;
                startPosition_ = 0.0;
                startPending_ = true;
            }
        }
        // 兜底：任何退出路径都不允许留下被按住的键（幂等、无等待）
        ReleaseAllGuarded();
        cv_.notify_all();
    }

    // 线程退出前最后一道保险
    ReleaseAllGuarded();
}

void SongPlayer::PlayLoop(const Timeline& timeline) {
    const int count = timeline.NoteCount();
    held_.clear();
    heldCount_.store(0);
    // 新一轮从头开始：手上没有按着任何演奏方式
    modeHeld_ = PlayMode::Normal;

    for (;;) {
        if (Interrupted()) {
            return;   // 急停 / 暂停 / 退出：立刻交还控制权
        }
        const double position = clock_.PositionSeconds();

        // 1. 释放所有已经到点的音符（按绝对时间释放，与起音同一套时间基准）
        ReleaseDueNotes(position, timeline);

        // 2. 起音：一次循环只处理一个音符，保证每个音符都有自己的精确目标时刻
        if (next_ < count) {
            const Note& note = timeline.At(next_);
            if (note.startTime <= position) {
                // 迟到（例如刚被某个事件唤醒）也照样补发，不跳过 —— 下一拍仍按绝对时刻对齐
                const bool pressed = PressNoteGuarded(note);
                const double actualPosition = clock_.PositionSeconds();
                const int index = next_;
                ++next_;
                firedNotes_.store(next_);
                if (pressed) {
                    // 通知 UI（Phase 7 的音符高亮用它）。回调在播放线程上，必须非阻塞。
                    NoteObserver observer;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        observer = observer_;
                    }
                    if (observer) {
                        observer(index, note, actualPosition, actualPosition - note.startTime);
                    }
                }
                continue;   // 回到开头重新读时间
            }
        }

        // 3. 结束判断：所有音符都已起音，且持有的输入都释放完了
        if (next_ >= count && held_.empty()) {
            finishRequested_ = true;
            heldCount_.store(0);
            return;
        }

        // 4. 等到下一个事件（下一个起音 或 最早的释放）
        double eventPosition = 0.0;
        bool hasEvent = false;
        if (next_ < count) {
            eventPosition = timeline.At(next_).startTime;
            hasEvent = true;
        }
        const double releaseAt = EarliestHeldEnd();
        if (releaseAt < kInfinite && (!hasEvent || releaseAt < eventPosition)) {
            eventPosition = releaseAt;
            hasEvent = true;
        }
        if (!hasEvent) {
            finishRequested_ = true;
            return;
        }

        // 由时钟把“歌曲位置”映射成“墙钟时刻”，再可唤醒地精等到那一刻。
        // 变速会改变这个映射关系，所以每次等待都重新换算。
        const double targetWall = clock_.WallClockForPosition(eventPosition);
        if (!waiter_.WaitUntilWakeable(targetWall)) {
            continue;   // 被唤醒（急停/暂停/变速）→ 回开头重新判断
        }
    }
}

// ---------------------------------------------------------------------------
// 发送（都在发送锁保护下）
// ---------------------------------------------------------------------------
bool SongPlayer::PressNoteGuarded(const Note& note) {
    std::lock_guard<std::mutex> sendLock(sendMutex_);
    // 需求书第四十一节：每一次发送输入之前都要再检查一次急停标志
    if (stopRequested_.load() || pauseRequested_.load() || quit_.load()) {
        return false;
    }
    if (options_.dryRun) {
        held_.push_back(HeldNote{note.key, note.mode, note.EndTime()});
        heldCount_.store(static_cast<int>(held_.size()));
        return true;
    }

    // ---- 演奏方式（鼠标键）：换 mode 先抬干净上一个，同一个 mode 连续则保持按住 ----
    //   同 mode 连续时**不重复按下**（原来就是靠 MouseController::Press 的“已按下不重复下发”
    //   兜住的），更要紧的是别在边界上先抬后按：那对事件挨得极近，正是把系统读值搅浑、
    //   让右键被误判成“用户按着”从而永远不抬起的序列。
    bool modeOk = true;
    bool modePressedNow = false;
    if (note.mode != modeHeld_) {
        if (modeHeld_ != PlayMode::Normal) {
            input_.ReleaseMode(modeHeld_);   // 上一个 mode 需要的鼠标键全部抬起来
        }
        modeHeld_ = PlayMode::Normal;        // 此刻手上不再按着任何演奏方式
        modeOk = input_.PressMode(note.mode);
        modePressedNow = true;
        if (modeOk) {
            modeHeld_ = note.mode;
        }
    }

    const bool keyOk = input_.PressKey(note.key);
    if (modeOk && keyOk) {
        held_.push_back(HeldNote{note.key, note.mode, note.EndTime()});
        heldCount_.store(static_cast<int>(held_.size()));
        return true;
    }

    // 失败分支：只回收**这一次真正按下**的部分，任何分支下都不留键。
    if (keyOk) {
        input_.ReleaseKey(note.key);         // 键按下成功了：放掉它
    }
    if (modePressedNow) {
        // 这一次按下去的鼠标键：哪怕只按下了组合模式里的一半，也一并收干净
        // （复用上一个音符的 mode 时绝不走这里 —— 那会把还在响的音变调）。
        input_.ReleaseMode(note.mode);
        modeHeld_ = PlayMode::Normal;
    }
    return false;
}

void SongPlayer::ReleaseNoteGuarded(Key key, PlayMode mode, bool keepMode) {
    std::lock_guard<std::mutex> sendLock(sendMutex_);
    if (options_.dryRun) {
        return;
    }
    input_.ReleaseKey(key);
    if (keepMode) {
        // 下一个音符要的是同一个 mode：保持按住（这就是“连按不重按”的优化）。
        return;
    }
    input_.ReleaseMode(mode);
    if (modeHeld_ == mode) {
        modeHeld_ = PlayMode::Normal;
    }
}

void SongPlayer::ReleaseAllGuarded() {
    std::lock_guard<std::mutex> sendLock(sendMutex_);
    input_.ReleaseAll();
    modeHeld_ = PlayMode::Normal;   // 手上什么都没有了
}

void SongPlayer::ReleaseDueNotes(double position, const Timeline& timeline) {
    bool changed = false;
    for (size_t i = 0; i < held_.size();) {
        if (held_[i].endTime <= position) {
            // 紧挨着的下一个音符要用同一个演奏方式 → 保持按住，不做“抬起→重按”。
            // 否则就在这个边界上把上一个 mode 的鼠标键全部抬起来（换调式不得漏抬）。
            bool keepMode = false;
            if (next_ < timeline.NoteCount()) {
                const Note& upcoming = timeline.At(next_);
                keepMode = (upcoming.mode == held_[i].mode) &&
                           (upcoming.startTime <= held_[i].endTime + kModeKeepEpsilon);
            }
            ReleaseNoteGuarded(held_[i].key, held_[i].mode, keepMode);
            held_[i] = held_.back();
            held_.pop_back();
            changed = true;
        } else {
            ++i;
        }
    }
    if (changed) {
        heldCount_.store(static_cast<int>(held_.size()));
    }
}

double SongPlayer::EarliestHeldEnd() const {
    double earliest = kInfinite;
    for (const HeldNote& note : held_) {
        if (note.endTime < earliest) {
            earliest = note.endTime;
        }
    }
    return earliest;
}

bool SongPlayer::Interrupted() const {
    return stopRequested_.load() || pauseRequested_.load() || quit_.load();
}

// ---------------------------------------------------------------------------
// 命令（任意线程调用，立即返回）
// ---------------------------------------------------------------------------
bool SongPlayer::Play(const Timeline& timeline) {
    if (timeline.Empty()) {
        return false;
    }
    // 先按“用户停止”收掉上一轮（释放输入、位置归零）—— 与手动 Stop 共用同一条路径
    Stop(StopReason::UserStop);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        timeline_ = std::make_shared<const Timeline>(timeline);
        noteCount_ = timeline.NoteCount();
        totalDuration_ = timeline.TotalDuration();
        startPosition_ = 0.0;
        resumePosition_ = 0.0;
        startPending_ = true;
        state_ = PlaybackState::Playing;
        reason_ = StopReason::None;
        reasonDetail_.clear();
        firedNotes_.store(0);
        next_ = 0;
    }
    cv_.notify_all();
    waiter_.Wake();
    return true;
}

bool SongPlayer::Pause() {
    double pausePosition = 0.0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != PlaybackState::Playing) {
            return false;
        }
        // 保存当前播放位置（继续时要用它），并把时钟冻住 —— 暂停期间位置必须纹丝不动
        pausePosition = clock_.PositionSeconds();
        clock_.Pause();
        resumePosition_ = pausePosition;
        state_ = PlaybackState::Paused;
    }
    {
        // 需求书第二十二节：暂停要释放当前 Player-owned 输入。
        // 置位与释放放在同一把锁里，保证此后播放线程不会再按下任何键。
        std::lock_guard<std::mutex> sendLock(sendMutex_);
        pauseRequested_ = true;
        input_.ReleaseAll();
    }
    heldCount_.store(0);
    waiter_.Wake();
    cv_.notify_all();
    return true;
}

bool SongPlayer::Resume() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != PlaybackState::Paused) {
            return false;
        }
        // 从暂停位置继续，不是从头（需求书第二十二节）
        startPosition_ = resumePosition_;
        clock_.StartAt(startPosition_);
        clock_.SetSpeed(speed_);
        startPending_ = true;
        state_ = PlaybackState::Playing;
    }
    cv_.notify_all();
    waiter_.Wake();
    return true;
}

bool SongPlayer::Stop(StopReason reason) {
    {
        // 置位 + 释放放在同一把锁里：急停之后不可能再出现新的自动输入
        std::lock_guard<std::mutex> sendLock(sendMutex_);
        stopRequested_ = true;
        input_.ReleaseAll();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reason_ = reason;
        reasonDetail_.clear();
        state_ = PlaybackState::Stopped;
        clock_.Stop();          // 播放位置归零（需求书第二十三节）
        heldCount_.store(0);
        // 已奏音符计数也要归零：界面上的"当前音符 x / y"、进度条、音符表高亮全看它，
        // 不归零就会出现"时间 00:00.000 但进度条停在 16%"这种自相矛盾（用户反馈）
        firedNotes_.store(0);
        startPending_ = false;
        restartRequested_ = false;
    }
    waiter_.Wake();
    cv_.notify_all();
    return true;
}

bool SongPlayer::Restart() {
    {
        std::lock_guard<std::mutex> sendLock(sendMutex_);
        stopRequested_ = true;
        input_.ReleaseAll();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!timeline_) {
            return false;   // 还没有曲目
        }
        clock_.Stop();
        startPosition_ = 0.0;
        reason_ = StopReason::None;
        reasonDetail_.clear();
        firedNotes_.store(0);
        heldCount_.store(0);
        state_ = PlaybackState::Playing;
        if (loopActive_) {
            restartRequested_ = true;   // 本轮结束后立刻重新开始
        } else {
            startPending_ = true;
        }
    }
    cv_.notify_all();
    waiter_.Wake();
    return true;
}

bool SongPlayer::EmergencyStop(const std::string& detail) {
    {
        std::lock_guard<std::mutex> sendLock(sendMutex_);
        stopRequested_ = true;
        input_.ReleaseAll();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = PlaybackState::Stopped;
        reason_ = StopReason::UserInput;
        reasonDetail_ = detail;
        clock_.Stop();
        heldCount_.store(0);
        startPending_ = false;
        restartRequested_ = false;
    }
    waiter_.Wake();
    cv_.notify_all();
    return true;
}

void SongPlayer::SetSpeed(double speed) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        speed_ = speed;
        clock_.SetSpeed(speed);   // 时间映射立刻生效，位置保持连续
    }
    // 唤醒播放线程，让它用新速度重算下一个目标时刻
    waiter_.Wake();
}

void SongPlayer::SetDryRun(bool dryRun) {
    std::lock_guard<std::mutex> lock(mutex_);
    options_.dryRun = dryRun;
}

void SongPlayer::SetNoteObserver(NoteObserver observer) {
    std::lock_guard<std::mutex> lock(mutex_);
    observer_ = std::move(observer);
}

bool SongPlayer::Quit() {
    {
        std::lock_guard<std::mutex> sendLock(sendMutex_);
        quit_ = true;
        input_.ReleaseAll();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = PlaybackState::Stopped;
        if (reason_ == StopReason::None) {
            reason_ = StopReason::Shutdown;
        }
    }
    waiter_.Wake();
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    return true;
}

// ---------------------------------------------------------------------------
// 状态查询
// ---------------------------------------------------------------------------
PlaybackState SongPlayer::State() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool SongPlayer::IsPlaying() const { return State() == PlaybackState::Playing; }
bool SongPlayer::IsPaused() const { return State() == PlaybackState::Paused; }
bool SongPlayer::IsStopped() const { return State() == PlaybackState::Stopped; }

double SongPlayer::PositionSeconds() const {
    return clock_.PositionSeconds();
}

double SongPlayer::TotalDurationSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalDuration_;
}

double SongPlayer::ProgressRatio() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (totalDuration_ <= 0.0) {
        return 0.0;
    }
    const double position = clock_.PositionSeconds();
    double ratio = position / totalDuration_;
    if (ratio < 0.0) {
        ratio = 0.0;
    } else if (ratio > 1.0) {
        ratio = 1.0;
    }
    return ratio;
}

double SongPlayer::Speed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return speed_;
}

int SongPlayer::FiredNoteCount() const { return firedNotes_.load(); }

int SongPlayer::NoteCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return noteCount_;
}

int SongPlayer::HeldInputCount() const { return heldCount_.load(); }

StopReason SongPlayer::LastStopReason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reason_;
}

std::string SongPlayer::LastStopDetail() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reasonDetail_;
}

std::string SongPlayer::StatusText() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string text = "状态：";
    text += PlaybackStateName(state_);
    if (state_ == PlaybackState::Stopped && reason_ != StopReason::None) {
        text += "　原因：";
        text += StopReasonText(reason_, reasonDetail_);
    }
    return text;
}

std::string SongPlayer::ProgressText() const {
    std::lock_guard<std::mutex> lock(mutex_);
    char buffer[192];
    std::snprintf(buffer, sizeof(buffer), "位置 %.2f / %.2f 秒（%.0f%%）　速度 %.0f%%　已起音 %d/%d",
                  clock_.PositionSeconds(), totalDuration_,
                  totalDuration_ > 0.0 ? clock_.PositionSeconds() / totalDuration_ * 100.0 : 0.0,
                  speed_ * 100.0, firedNotes_.load(), noteCount_);
    return std::string(buffer);
}

unsigned long SongPlayer::PlaybackThreadId() const { return threadId_.load(); }

bool SongPlayer::PlaybackLoopActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loopActive_;
}

bool SongPlayer::DryRun() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return options_.dryRun;
}

// ---------------------------------------------------------------------------
// 测试辅助
// ---------------------------------------------------------------------------
bool SongPlayer::WaitForState(PlaybackState state, int timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                        [this, state] { return state_ == state; });
}

bool SongPlayer::WaitForPlaying(int timeoutMs) {
    return WaitForState(PlaybackState::Playing, timeoutMs);
}

bool SongPlayer::WaitForStopped(int timeoutMs) {
    return WaitForState(PlaybackState::Stopped, timeoutMs);
}

bool SongPlayer::WaitForFiredNotes(int count, int timeoutMs) {
    const double deadline = NowSeconds() + static_cast<double>(timeoutMs) / 1000.0;
    while (NowSeconds() < deadline) {
        if (firedNotes_.load() >= count) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return firedNotes_.load() >= count;
}

}  // namespace hp
