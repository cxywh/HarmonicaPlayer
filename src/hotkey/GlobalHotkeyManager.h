#pragma once

// 全局热键管理器（需求书第十三节）
//
//   F6 = 播放 / 开始　F7 = 停止　F8 = 从头开始　F9 = 上一首　F10 = 下一首
//   必须使用 Windows 全局热键机制（RegisterHotKey）；
//   即使工具窗口没有焦点（游戏全屏）也必须工作；
//   并且允许用户自定义。
//
// 设计要点：
//   1. RegisterHotKey 注册在**哪个线程**上，WM_HOTKEY 就投给哪个线程的消息队列，
//      所以本类自带一条热键线程 + 消息循环，运行期改热键也是把请求
//      PostThreadMessage 给它、在同一线程上重新注册。
//   2. 回调在**热键线程**上执行，因此必须极短、不得阻塞（只允许调用播放器命令，
//      它们是"改意图 + 置标志 + SetEvent"，不含等待）。文件 IO、弹窗、等待一律禁止。
//   3. 加 MOD_NOREPEAT：按住 F6 不重复触发，避免"按住不放＝连点"。
//   4. 十八节"启动热键特殊处理"：本类记住启动热键最近一次触发的时刻，
//      Phase 6 的输入监控据此忽略"刚按下的那一下 F6"，不会刚开播就被自己急停。

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hotkey/HotkeyBinding.h"

namespace hp {

// ---------------------------------------------------------------------------
// 一次热键触发
// ---------------------------------------------------------------------------
struct HotkeyEvent {
    HotkeyAction action = HotkeyAction::Play;
    HotkeyBinding binding{};            // 触发时生效的绑定（可能已被改过）
    unsigned long long tick = 0;        // GetTickCount64()，毫秒
    int id = 0;                         // 注册用的 id，排障用
};

// ---------------------------------------------------------------------------
// 改热键的结果
// ---------------------------------------------------------------------------
struct HotkeyChangeResult {
    bool ok = false;
    std::string error;      // 失败原因（中文，可直接显示）
    std::string warning;    // 成功但有风险（例如抢走字母键）
};

// ---------------------------------------------------------------------------
// 全局热键管理器
// ---------------------------------------------------------------------------
class GlobalHotkeyManager {
public:
    using Callback = std::function<void(const HotkeyEvent&)>;

    GlobalHotkeyManager();
    ~GlobalHotkeyManager();   // 停止线程并注销全部热键

    GlobalHotkeyManager(const GlobalHotkeyManager&) = delete;
    GlobalHotkeyManager& operator=(const GlobalHotkeyManager&) = delete;

    // 启动热键线程并注册默认热键（F6/F7/F8）。
    // 返回 false 只在"线程起不来"时发生；个别热键被别的程序占用不算失败，
    // 那些动作会保持未注册状态，可从 DescribeAll() / RegistrationErrors() 看到。
    // callback 会在热键线程上被调用，必须非阻塞；可以传 nullptr（只注册不响应）。
    bool Start(const Callback& callback, std::string* error = nullptr);

    // 注销全部热键并结束线程（幂等）
    void Stop();

    bool IsRunning() const { return running_.load(); }

    // 热键线程 id（用于排障与"回调是否在自己的线程上"的验证）
    unsigned long HotkeyThreadId() const { return threadId_.load(); }

    // ---- 自定义热键（需求书第十三节"允许用户自定义"）----
    // 运行中改：与已有绑定重复会被拒绝；被别的程序占用会被拒绝且**保留原绑定**。
    // 传一个 vk == 0 的绑定表示取消这个动作的热键。
    HotkeyChangeResult SetHotkey(HotkeyAction action, const HotkeyBinding& binding);

    // 恢复三键默认值（F6/F7/F8）
    HotkeyChangeResult ResetToDefaults();

    HotkeyBinding GetHotkey(HotkeyAction action) const;
    std::vector<HotkeyBinding> AllBindings() const;

    // ---- 按键捕获（设置窗口正在录键）----
    // 录制期间必须把全局热键**让出去**：RegisterHotKey 注册的键会在到达焦点窗口之前
    // 就被系统吞掉，于是"把 F6 改成别的功能"根本录不到，反而直接把曲子弹起来。
    // 挂起只注销注册、绑定本身不动；恢复时按当前绑定重新注册。
    // 两者都幂等，并且是同步的（返回时已经生效），因为紧接着用户就要按键了。
    void SetSuspended(bool suspended);
    bool IsSuspended() const { return suspended_.load(); }

    // ---- 查看状态 ----
    // 多行文本："播放 / 开始  F6        已注册"
    std::string DescribeAll() const;
    // 同一把键被两个动作占用（正常操作下不会出现，设置界面用来自查）
    std::vector<std::string> Conflicts() const;
    int RegisteredCount() const;
    // 启动时注册失败的动作及原因（例如 "F7 已被其它程序占用"）
    std::vector<std::string> RegistrationErrors() const;
    std::string LastError() const;

    // ---- 给 Phase 6（用户输入监控）用：十八节"启动热键特殊处理" ----
    // 启动热键（播放动作）最近一次触发的时刻；withinMs 内按过就返回 true，
    // 输入监控看到"这次按键就是启动热键本身"时应当忽略。
    bool StartHotkeyPressedRecently(int withinMs) const;
    unsigned long long LastStartHotkeyTick() const { return lastStartTick_.load(); }
    unsigned StartHotkeyVirtualKey() const;

    // 这个键是不是**任何一个动作**的绑定主键（不分修饰键）。
    // Phase 6 的"用户输入急停"靠它把所有绑定的热键排除掉：用户按的是热键，
    // 就该发挥热键作用，不能被当成"用户输入打断演奏"（否则按 F8 会先急停再重播）。
    bool IsBoundKey(unsigned vk) const;

    // ---- 测试/排障辅助 ----
    int EventCount(HotkeyAction action) const;
    HotkeyEvent LastEvent() const;
    // 等到某个动作累计触发 count 次（自检用；最多等 timeoutMs 毫秒）
    bool WaitForEvent(HotkeyAction action, int count, int timeoutMs);

private:
    struct ChangeRequest {
        HotkeyAction action = HotkeyAction::Play;
        HotkeyBinding binding{};
        HotkeyChangeResult result{};
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        bool resetAll = false;
        bool suspendRequest = false;   // 这一个请求是"挂起 / 恢复"而不是改键
        bool suspendValue = false;
    };

    static int IdOf(HotkeyAction action);
    static HotkeyAction ActionOfId(int id, bool* known);

    void ThreadMain();
    void HandleHotkeyMessage(int id);
    void ApplyRequest(ChangeRequest* request);   // 只在热键线程上执行
    void UnregisterAllLocked();                  // 只在热键线程上执行

    mutable std::mutex mutex_;
    std::condition_variable readyCv_;
    bool ready_ = false;                                     // 由热键线程置位
    std::string startError_;                                 // 线程起不来时的原因
    std::vector<std::string> registrationErrors_;            // 注册失败的动作
    std::vector<HotkeyBinding> bindings_;                    // 与 HotkeyAction 一一对应
    Callback callback_;

    std::thread thread_;
    std::atomic<bool> suspended_{false};   // 挂起中：绑定保留，但不注册（按键捕获用）
    std::atomic<bool> running_{false};
    std::atomic<bool> quit_{false};
    std::atomic<unsigned long> threadId_{0};

    // 事件簿（原子，读取不加锁）
    std::atomic<int> eventCounts_[kHotkeyActionCount] = {};
    std::atomic<unsigned long long> lastEventTick_{0};
    std::atomic<unsigned long long> lastStartTick_{0};
    std::atomic<unsigned> lastEventVk_{0};
    std::atomic<int> lastEventAction_{-1};

    mutable std::mutex lastEventMutex_;
    HotkeyEvent lastEvent_{};
};

}  // namespace hp
