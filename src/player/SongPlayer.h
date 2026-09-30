#pragma once

// SongPlayer：把时间轴变成真正的演奏。
//
// 需求书依据：
//   二十一节 播放状态 PlaybackState { Stopped, Playing, Paused }
//            支持 Play / Pause / Resume / Stop / Restart / EmergencyStop
//   二十二节 暂停：停止产生新音符 → 释放当前 Player-owned 输入 → 保存位置 → Paused；
//            继续时从暂停位置接着弹，不能从头
//   二十三节 停止：停止后续音符 → ReleaseAll() → 播放位置归零 → Stopped
//   二十四节 歌曲结束：停止 → ReleaseAll() → Stopped（绝不让最后一个音符一直按着）
//   二十六节 停止原因要能显示给 UI
//   三十八节 线程架构：播放线程拥有 Timeline / PlaybackClock / InputController；
//            UI 线程不能因为播放而卡死 —— 所以所有命令都是“投递意图后立刻返回”
//   四十一节 急停竞态：stopRequested 在**每一次发送输入之前**再检查，
//            并与“置位 + ReleaseAll”共用一把锁，保证急停之后不再产生新的自动输入
//   四十二节 异常安全：退出 / 播放线程异常都必须 ReleaseAll()

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/PrecisionWait.h"
#include "input/InputController.h"
#include "player/PlaybackClock.h"
#include "player/PlaybackTypes.h"
#include "song/Timeline.h"

namespace hp {

struct PlayerOptions {
    // 播放线程提到“时间关键”优先级（实测可压低被系统抢占造成的偶发晚醒）
    bool boostPlaybackThreadPriority = true;
    // 干跑：只走时间轴与时钟，不注入按键。用于测试调度精度 / 以后的“预览进度”
    bool dryRun = false;
};

class SongPlayer {
public:
    explicit SongPlayer(InputController& input, const PlayerOptions& options = PlayerOptions());
    ~SongPlayer();

    SongPlayer(const SongPlayer&) = delete;
    SongPlayer& operator=(const SongPlayer&) = delete;

    // ---- 命令：任意线程可调用，立即返回，绝不等待播放线程 ----
    bool Play(const Timeline& timeline);                     // 从 0 开始演奏（替换当前曲目）
    bool Pause();                                            // 暂停
    bool Resume();                                           // 从暂停位置继续
    bool Stop(StopReason reason = StopReason::UserStop);     // 停止（位置归零）
    bool Restart();                                          // 位置归零，从头再弹
    bool EmergencyStop(const std::string& detail);           // 急停（Phase 6 的钩子调它）
    void SetSpeed(double speed);                             // 播放中变速（只改时间映射）
    void SetDryRun(bool dryRun);
    bool Quit();                                             // 结束播放线程（析构也会调）

    // ---- 状态查询：任意线程，快速返回 ----
    PlaybackState State() const;
    bool IsPlaying() const;
    bool IsPaused() const;
    bool IsStopped() const;
    double PositionSeconds() const;
    double TotalDurationSeconds() const;
    double ProgressRatio() const;      // 0 ~ 1
    double Speed() const;
    int FiredNoteCount() const;        // 已经起音的音符数（曲内序号）
    int NoteCount() const;
    int HeldInputCount() const;        // 播放线程当前持有（按下未释放）的音符数
    StopReason LastStopReason() const;
    std::string LastStopDetail() const;
    std::string StatusText() const;    // "状态：已停止　原因：用户手动停止"
    std::string ProgressText() const;  // "位置 …／速度 …／已起音 …"
    unsigned long PlaybackThreadId() const;
    bool PlaybackLoopActive() const;   // 播放循环是否正在跑
    bool DryRun() const;

    // ---- 测试辅助：等状态变化（带超时） ----
    bool WaitForState(PlaybackState state, int timeoutMs);
    bool WaitForPlaying(int timeoutMs);
    bool WaitForStopped(int timeoutMs);
    bool WaitForFiredNotes(int count, int timeoutMs);

    // ---- 给 UI 的音符观察者 ----
    // 每起音一个音符就回调一次（Phase 7 的音符高亮 / 进度显示会用它）。
    // 参数：音符下标、音符本身、起音时的歌曲位置（秒）、迟到量（秒，负数表示提前）。
    // 注意：回调发生在**播放线程**上，必须非阻塞（不要在里面 Sleep 或弹窗）。
    using NoteObserver = std::function<void(int noteIndex, const Note& note, double actualPosition,
                                            double latenessSeconds)>;
    void SetNoteObserver(NoteObserver observer);

private:
    void WorkerMain();
    void PlayLoop(const Timeline& timeline);

    // 下面三个都在“发送锁”保护下工作，见 sendMutex_ 的注释
    bool PressNoteGuarded(const Note& note);
    void ReleaseNoteGuarded(Key key, PlayMode mode);
    void ReleaseAllGuarded();

    void ReleaseDueNotes(double position);
    double EarliestHeldEnd() const;
    bool Interrupted() const;

    struct HeldNote {
        Key key;
        PlayMode mode;
        double endTime;
    };

    InputController& input_;
    PlayerOptions options_;

    // 保护“曲目 + 状态 + 命令意图 + 播放位置”
    mutable std::mutex mutex_;
    std::condition_variable cv_;

    // 发送锁：把“检查停止标志 + 发送输入”和“置位停止标志 + ReleaseAll”串成原子对，
    // 这是需求书第四十一节那条不变量的实现方式（急停后不允许再出现新的自动输入）。
    mutable std::mutex sendMutex_;

    std::atomic<bool> quit_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> pauseRequested_{false};

    std::shared_ptr<const Timeline> timeline_;
    int noteCount_ = 0;
    double totalDuration_ = 0.0;
    NoteObserver observer_;

    PlaybackState state_ = PlaybackState::Stopped;
    StopReason reason_ = StopReason::None;
    std::string reasonDetail_;

    bool startPending_ = false;
    bool restartRequested_ = false;
    bool finishRequested_ = false;
    bool loopActive_ = false;
    double startPosition_ = 0.0;
    double resumePosition_ = 0.0;
    double speed_ = 1.0;

    std::thread worker_;
    std::atomic<unsigned long> threadId_{0};
    std::atomic<int> firedNotes_{0};
    std::atomic<int> heldCount_{0};

    // 以下只在播放线程里使用
    PlaybackClock clock_;
    PrecisionWait waiter_;
    int next_ = 0;
    std::vector<HeldNote> held_;
};

}  // namespace hp
