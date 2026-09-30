#pragma once

// InputMonitor —— 用户输入监控（需求书第十四～十八、二十、二十五、二十六、三十八节）
//
// 职责：钩住系统输入，把"用户自己的按键"翻译成一次急停请求。
//
//   十四节 最高优先级：演奏中用户只要主动按任意键 → 立即停止、释放工具持有的输入、交还控制权
//   十五节 任意键：A-Z、0-9、F1-F24、方向键、Shift/Ctrl/Alt、Space/Tab/Enter/Esc/
//          Backspace/Insert/Delete/Home/End/PageUp/PageDown 等，只要新的 KeyDown
//   十六节 自动输入绝不能触发急停（否则播放器会自己把自己停掉）
//   十七节 用 WH_KEYBOARD_LL，检查事件来源：程序注入 → 忽略；真实按键 → 急停
//   十八节 刚按下的启动热键要忽略（否则一按 F6 开始就立刻被自己停掉）
//   二十节 与播放线程的竞态：钩子回调只管"喊停"，"停干净"由播放器的发送锁保证
//   二十五节 鼠标按键也应该能触发（默认开）；鼠标移动默认不触发（太灵敏）
//   二十六节 停止原因要能显示"检测到用户输入：Space"
//   三十八节 线程架构：Input Monitor 是独立的一条线程 + 消息循环
//
// 三条硬的实现约束：
//   1. 低级钩子必须在**装钩子的那条线程**上有消息循环（系统靠它把事件送过来），
//      所以本类自带线程；且卸载钩子必须在同一条线程上做。
//   2. 钩子回调必须极短：Windows 对低级钩子有超时（LowLevelHooksTimeout，默认 300 ms），
//      超时会静默丢掉这个钩子。所以回调里只允许做"分类 + 调一次非阻塞回调"，
//      绝不允许文件 IO、弹窗、加锁等待。本类统计并记录回调最坏耗时（worstCallbackMs）。
//   3. 注入判定有两道：LLKHF_INJECTED 标志 + 本工具的 dwExtraInfo 标记（0x48504C59 "HPLY"）。

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "core/Platform.h"   // 本模块是 Win32 专用的：钩子类型 LRESULT/CALLBACK 都来自这里
#include "monitor/HookTypes.h"

namespace hp {

class InputMonitor {
public:
    InputMonitor();
    ~InputMonitor();   // 停止并卸载钩子

    InputMonitor(const InputMonitor&) = delete;
    InputMonitor& operator=(const InputMonitor&) = delete;

    // 起监控线程并安装 WH_KEYBOARD_LL / WH_MOUSE_LL。
    // 低级钩子是**进程级**的，所以同时只能有一个 InputMonitor 在跑；
    // 已经有别的实例在跑时返回 false 并把原因写进 error。
    bool Start(const InputMonitorOptions& options, const InputMonitorCallbacks& callbacks,
               std::string* error = nullptr);

    // 卸载钩子并结束线程（幂等）
    void Stop();

    bool IsRunning() const { return running_.load(); }
    unsigned long MonitorThreadId() const { return threadId_.load(); }

    // 设置（二十五节的三个开关）。默认：键盘开、鼠标键开、鼠标移动关。
    void SetOptions(const InputMonitorOptions& options);
    InputMonitorOptions Options() const;

    InputMonitorStats Stats() const;
    std::string DescribeStats() const;
    std::string DescribeOptions() const;

    // 最近一次被判为"用户输入"的事件
    UserInputEvent LastEvent() const;

    // 是否检测到过"真人（非注入）按键"。
    // 自检里用它区分"跑的是哪条路"：程序注入永远走不到这里（十六节）。
    bool SawRealUserInput() const { return sawRealInput_.load(); }

    // ---------------------------------------------------------------------
    // 给自检用的两个口子（产品代码不调用）
    // ---------------------------------------------------------------------
    // 1) 判定"是不是注入事件"的纯函数（十六节的判据）
    static bool IsInjectedEvent(bool injectedFlag, unsigned long long extraInfo);

    // 2) 把一条"原始钩子事件"塞进**真实分发路径**（分类 → 忽略判定 → 触发）。
    //    自检用它模拟"真人按了一下硬件键"（带 injected=false），
    //    走的是与真实钩子完全相同的一段代码。
    bool ProcessRawEventForTest(const RawHookEvent& raw);

    // 等触发次数达到 count（自检用）
    bool WaitForTrigger(long long count, int timeoutMs);

private:
    void ThreadMain();
    void ProcessEvent(const RawHookEvent& raw);   // 钩子线程与自检共用
    static LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam);
    static InputMonitor* Instance();
    static void SetInstance(InputMonitor* instance);

    mutable std::mutex mutex_;
    std::condition_variable readyCv_;
    bool ready_ = false;
    std::string startError_;
    // 回调只在 Start() 里设置一次、运行期间不再改，所以钩子回调可以不加锁读取。
    InputMonitorCallbacks callbacks_{};

    // 设置用原子量存：钩子回调里绝不能因为等锁而卡住（Windows 有 300 ms 超时上限）
    std::atomic<bool> optKeyboard_{true};
    std::atomic<bool> optMouseButtons_{true};
    std::atomic<bool> optMouseMove_{false};

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> quit_{false};
    std::atomic<unsigned long> threadId_{0};
    std::atomic<bool> sawRealInput_{false};

    // 统计（钩子线程写、任意线程读）
    std::atomic<long long> keyboardEvents_{0};
    std::atomic<long long> mouseEvents_{0};
    std::atomic<long long> injectedIgnored_{0};
    std::atomic<long long> keyUpIgnored_{0};
    std::atomic<long long> moveIgnored_{0};
    std::atomic<long long> disabledIgnored_{0};
    std::atomic<long long> hotkeyIgnored_{0};
    std::atomic<long long> triggered_{0};
    std::atomic<long long> simulatedForTest_{0};
    std::atomic<long long> worstCallbackTicks_{0};   // QPC 计数（不是纳秒，换算见 Stats）

    // 最近一次"用户输入"的原子快照（钩子回调里不加锁、不分配堆内存）
    std::atomic<int> lastKind_{0};
    std::atomic<unsigned> lastVk_{0};
    std::atomic<int> lastButton_{0};
    std::atomic<unsigned long long> lastTick_{0};
    std::atomic<bool> lastInjected_{false};
};

}  // namespace hp
