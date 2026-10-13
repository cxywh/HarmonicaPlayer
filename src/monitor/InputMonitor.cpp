#include "monitor/InputMonitor.h"

#include <chrono>
#include <cstdio>

#include "core/Log.h"
#include "core/Platform.h"
#include "core/Time.h"

namespace hp {
namespace {

// 本工具的注入标记（与 KeyboardController/MouseController 里的一致）：ASCII "HPLY"
constexpr unsigned long long kSyntheticSignature = 0x48504C59ULL;

// 唯一的实例指针：低级钩子是**进程级**的，同时只能有一个监控在跑。
std::atomic<InputMonitor*> g_instance{nullptr};

std::string ToMillisecondsText(long long nanoseconds) {
    const double ms = static_cast<double>(nanoseconds) / 1000000.0;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", ms);
    return std::string(buffer);
}

// 滚轮事件的显示名（二十六节的停止原因要能读懂是哪个方向）
//   delta < 0 = 朝用户方向滚（下滚）；>= 0 = 朝远离用户方向滚（上滚）。
//   水平滚轮（WM_MOUSEHWHEEL）没有单独的文案，按同一个“上滚 / 下滚”报，
//   避免再分出一类只在极少数鼠标上出现的事件。
std::string WheelEventName(int delta) {
    return (delta < 0) ? std::string("鼠标滚轮下滚") : std::string("鼠标滚轮上滚");
}

}  // namespace

const char* UserInputKindName(UserInputKind kind) {
    switch (kind) {
        case UserInputKind::KeyDown:
            return "键盘按键";
        case UserInputKind::MouseButtonDown:
            return "鼠标按键";
        case UserInputKind::MouseMove:
            return "鼠标移动";
        case UserInputKind::Wheel:
            return "鼠标滚轮";
        default:
            return "未知输入";
    }
}

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------
InputMonitor::InputMonitor() = default;

InputMonitor::~InputMonitor() {
    Stop();
}

InputMonitor* InputMonitor::Instance() {
    return g_instance.load();
}

void InputMonitor::SetInstance(InputMonitor* instance) {
    g_instance.store(instance);
}

// ---------------------------------------------------------------------------
// 启动 / 停止
// ---------------------------------------------------------------------------
bool InputMonitor::Start(const InputMonitorOptions& options, const InputMonitorCallbacks& callbacks,
                         std::string* error) {
    if (running_.load()) {
        return true;
    }
    if (g_instance.load() != nullptr) {
        if (error != nullptr) {
            *error = "已经有另一个输入监控在运行（低级钩子是进程级的，一次只能装一份）";
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_ = callbacks;   // 只在启动前赋值，运行期间不再改（钩子回调读它不加锁）
        ready_ = false;
        startError_.clear();
        optKeyboard_.store(options.keyboard);
        optMouseButtons_.store(options.mouseButtons);
        optMouseMove_.store(options.mouseMove);
        optMouseWheel_.store(options.mouseWheel);
    }
    quit_.store(false);
    SetInstance(this);

    thread_ = std::thread(&InputMonitor::ThreadMain, this);

    {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool ok = readyCv_.wait_for(lock, std::chrono::seconds(5), [this] { return ready_; });
        if (!ok || !startError_.empty()) {
            const std::string reason = ok ? startError_ : std::string("监控线程在 5 秒内没有就绪");
            lock.unlock();
            quit_.store(true);
            const unsigned long id = threadId_.load();
            if (id != 0) {
                PostThreadMessageW(id, WM_QUIT, 0, 0);
            }
            if (thread_.joinable()) {
                thread_.join();
            }
            SetInstance(nullptr);
            if (error != nullptr) {
                *error = reason;
            }
            return false;
        }
    }
    running_.store(true);
    return true;
}

void InputMonitor::Stop() {
    if (!thread_.joinable()) {
        running_.store(false);
        if (g_instance.load() == this) {
            SetInstance(nullptr);
        }
        return;
    }
    quit_.store(true);
    const unsigned long id = threadId_.load();
    if (id != 0) {
        PostThreadMessageW(id, WM_QUIT, 0, 0);
    }
    thread_.join();
    running_.store(false);
    threadId_.store(0);
    if (g_instance.load() == this) {
        SetInstance(nullptr);
    }
}

// ---------------------------------------------------------------------------
// 监控线程：装钩子必须在这里，卸载也必须在同一条线程
// ---------------------------------------------------------------------------
void InputMonitor::ThreadMain() {
    threadId_.store(GetCurrentThreadId());

    // 先把消息队列建出来（低级钩子要靠它把事件送进来）
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    const HINSTANCE module = GetModuleHandleW(nullptr);
    HHOOK keyboardHook = nullptr;
    HHOOK mouseHook = nullptr;
    if (optKeyboard_.load()) {
        keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, &InputMonitor::KeyboardProc, module, 0);
        if (keyboardHook == nullptr) {
            std::lock_guard<std::mutex> lock(mutex_);
            startError_ = "装键盘钩子失败（错误码 " + std::to_string(GetLastError()) + "）";
        }
    }
    if (startError_.empty() && (optMouseButtons_.load() || optMouseMove_.load() || optMouseWheel_.load())) {
        mouseHook = SetWindowsHookExW(WH_MOUSE_LL, &InputMonitor::MouseProc, module, 0);
        if (mouseHook == nullptr) {
            std::lock_guard<std::mutex> lock(mutex_);
            startError_ = "装鼠标钩子失败（错误码 " + std::to_string(GetLastError()) + "）";
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready_ = true;
    }
    readyCv_.notify_all();

    if (!startError_.empty()) {
        if (keyboardHook != nullptr) {
            UnhookWindowsHookEx(keyboardHook);
        }
        if (mouseHook != nullptr) {
            UnhookWindowsHookEx(mouseHook);
        }
        return;
    }

    while (!quit_.load()) {
        const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
        if (result <= 0) {
            break;
        }
        // 低级钩子的事件不经过消息队列（系统直接调用钩子回调），
        // 所以这里只负责把消息循环转起来。
    }

    if (keyboardHook != nullptr) {
        UnhookWindowsHookEx(keyboardHook);
    }
    if (mouseHook != nullptr) {
        UnhookWindowsHookEx(mouseHook);
    }
}

// ---------------------------------------------------------------------------
// 钩子回调：只做"翻译 + 分发"，绝不做别的事
// ---------------------------------------------------------------------------
LRESULT CALLBACK InputMonitor::KeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    InputMonitor* self = Instance();
    // 注意：这里不查 running_（它在钩子装好之后才置位，会漏掉启动瞬间的事件），
    // 只看实例在不在、有没有开始退出。
    if (code == HC_ACTION && self != nullptr && !self->quit_.load()) {
        const auto* data = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (data != nullptr) {
            RawHookEvent raw;
            raw.kind = UserInputKind::KeyDown;
            raw.keyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
            raw.vk = data->vkCode;
            raw.injected = (data->flags & LLKHF_INJECTED) != 0 ||
                           (data->flags & LLKHF_LOWER_IL_INJECTED) != 0;
            raw.extraInfo = data->dwExtraInfo;
            raw.tick = GetTickCount64();

            const long long begin = NowTicks();
            self->ProcessEvent(raw);
            const long long elapsed = NowTicks() - begin;
            long long worst = self->worstCallbackTicks_.load();
            while (elapsed > worst &&
                   !self->worstCallbackTicks_.compare_exchange_weak(worst, elapsed)) {
                // 自旋重试：极少数情况下另一个事件在同时更新
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK InputMonitor::MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    InputMonitor* self = Instance();
    if (code == HC_ACTION && self != nullptr && !self->quit_.load()) {
        const auto* data = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if (data != nullptr) {
            RawHookEvent raw;
            raw.injected = (data->flags & LLMHF_INJECTED) != 0 ||
                           (data->flags & LLMHF_LOWER_IL_INJECTED) != 0;
            raw.extraInfo = data->dwExtraInfo;
            raw.tick = GetTickCount64();
            bool relevant = true;
            switch (wParam) {
                case WM_LBUTTONDOWN:
                    raw.kind = UserInputKind::MouseButtonDown;
                    raw.button = MouseButton::Left;
                    break;
                case WM_MBUTTONDOWN:
                    raw.kind = UserInputKind::MouseButtonDown;
                    raw.button = MouseButton::Middle;
                    break;
                case WM_RBUTTONDOWN:
                    raw.kind = UserInputKind::MouseButtonDown;
                    raw.button = MouseButton::Right;
                    break;
                case WM_MOUSEMOVE:
                    raw.kind = UserInputKind::MouseMove;
                    break;
                case WM_MOUSEWHEEL:
                case WM_MOUSEHWHEEL:
                    // 滚轮：独立分类（不是移动、也不是按键）。
                    // 滚轮消息在 MSLLHOOKSTRUCT 里**没有按键位**：mouseData 的高字放的是
                    // 滚动量，所以不能靠“有没有按键按下”去判断，必须按消息号单独归一类。
                    // >= 120（WHEEL_DELTA 的整数倍）上滚，负数下滚。
                    raw.kind = UserInputKind::Wheel;
                    raw.wheelDelta =
                        static_cast<int>(static_cast<short>(HIWORD(data->mouseData)));
                    if (raw.wheelDelta == 0) {
                        // 兜底：个别驱动把滚动量只放在 wParam 里
                        raw.wheelDelta = static_cast<int>(static_cast<short>(HIWORD(wParam)));
                    }
                    break;
                default:
                    // 抬起（WM_LBUTTONUP 等）：不参与急停（二十五节只要按下）
                    relevant = false;
                    break;
            }
            if (relevant) {
                const long long begin = NowTicks();
                self->ProcessEvent(raw);
                const long long elapsed = NowTicks() - begin;
                long long worst = self->worstCallbackTicks_.load();
                while (elapsed > worst &&
                       !self->worstCallbackTicks_.compare_exchange_weak(worst, elapsed)) {
                }
            }
        }
    }
    // 无论是哪一类事件（含滚轮）：**原样放行**给后面的钩子 / 游戏。
    // 本钩子只做“分类 + 判断 + 喊停”，从不吞事件，所以滚轮该怎么滚还怎么滚。
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 唯一的分类与分发路径（真实钩子与自检都走它）
// ---------------------------------------------------------------------------
void InputMonitor::ProcessEvent(const RawHookEvent& raw) {
    if (raw.kind == UserInputKind::MouseMove || raw.kind == UserInputKind::MouseButtonDown ||
        raw.kind == UserInputKind::Wheel) {
        mouseEvents_.fetch_add(1);
    } else {
        keyboardEvents_.fetch_add(1);
    }

    // ---- 十六节：注入的输入一律忽略（否则播放器会自己把自己停掉）----
    if (IsInjectedEvent(raw.injected, raw.extraInfo)) {
        injectedIgnored_.fetch_add(1);
        return;
    }

    // ---- 十五节：只有"新的按下"才算；松开不触发 ----
    if (raw.kind == UserInputKind::KeyDown && raw.keyUp) {
        keyUpIgnored_.fetch_add(1);
        return;
    }

    // ---- 二十五节：按设置过滤 ----
    if (raw.kind == UserInputKind::KeyDown && !optKeyboard_.load()) {
        disabledIgnored_.fetch_add(1);
        return;
    }
    if (raw.kind == UserInputKind::MouseButtonDown && !optMouseButtons_.load()) {
        disabledIgnored_.fetch_add(1);
        return;
    }
    if (raw.kind == UserInputKind::MouseMove) {
        if (!optMouseMove_.load()) {
            moveIgnored_.fetch_add(1);
            return;
        }
    }
    // 滚轮（二十五节 + 用户要求）：默认开。关掉时与键盘/鼠标按键一样记在
    // disabledIgnored_（“这一类在设置里被关掉”）。
    if (raw.kind == UserInputKind::Wheel && !optMouseWheel_.load()) {
        disabledIgnored_.fetch_add(1);
        return;
    }

    UserInputEvent event;
    event.kind = raw.kind;
    event.vk = raw.vk;
    event.button = raw.button;
    event.tick = raw.tick != 0 ? raw.tick : GetTickCount64();
    event.injected = false;
    switch (raw.kind) {
        case UserInputKind::KeyDown:
            event.name = VirtualKeyDisplayName(raw.vk);
            break;
        case UserInputKind::MouseButtonDown:
            event.name = MouseButtonDisplayName(raw.button);
            break;
        case UserInputKind::MouseMove:
            event.name = "鼠标移动";
            break;
        case UserInputKind::Wheel:
            event.name = WheelEventName(raw.wheelDelta);
            break;
        default:
            event.name = "未知输入";
            break;
    }

    // ---- 热键放行模式：真实按键先交给上层认一遍热键 ----
    // 这里**不吞键**：不 return、不改事件，游戏照样收到这个键。
    // 放在 shouldIgnore 之前，是为了让"绑成热键的键"同样能被认出来。
    if (callbacks_.onHookKey) {
        callbacks_.onHookKey(event);
    }

    // ---- 十八节：刚按下的启动热键要忽略（否则按 F6 开始就被自己停掉）----
    if (callbacks_.shouldIgnore && callbacks_.shouldIgnore(event)) {
        hotkeyIgnored_.fetch_add(1);
        return;
    }

    // ---- 二十六节：把"最近一次用户输入"记下来，供停止原因显示 ----
    lastKind_.store(static_cast<int>(event.kind));
    lastVk_.store(event.vk);
    lastButton_.store(static_cast<int>(event.button));
    lastWheelDelta_.store(raw.wheelDelta);
    lastTick_.store(event.tick);
    lastInjected_.store(event.injected);
    sawRealInput_.store(true);
    triggered_.fetch_add(1);

    // ---- 十四节：喊停（回调必须非阻塞；真正的"停干净"由播放器的发送锁保证）----
    if (callbacks_.onUserInput) {
        callbacks_.onUserInput(event);
    }
}

bool InputMonitor::ProcessRawEventForTest(const RawHookEvent& raw) {
    if (!running_.load()) {
        return false;
    }
    RawHookEvent event = raw;
    if (event.tick == 0) {
        event.tick = GetTickCount64();
    }
    simulatedForTest_.fetch_add(1);
    ProcessEvent(event);
    return true;
}

// ---------------------------------------------------------------------------
// 判据与查询
// ---------------------------------------------------------------------------
bool InputMonitor::IsInjectedEvent(bool injectedFlag, unsigned long long extraInfo) {
    if (injectedFlag) {
        return true;   // LLKHF_INJECTED / LLMHF_INJECTED（含 LOWER_IL 变体）
    }
    // 第二道保险：本工具自己的注入标记（万一标志位被过滤掉）
    return extraInfo == kSyntheticSignature;
}

void InputMonitor::SetOptions(const InputMonitorOptions& options) {
    optKeyboard_.store(options.keyboard);
    optMouseButtons_.store(options.mouseButtons);
    optMouseMove_.store(options.mouseMove);
    optMouseWheel_.store(options.mouseWheel);
}

InputMonitorOptions InputMonitor::Options() const {
    InputMonitorOptions options;
    options.keyboard = optKeyboard_.load();
    options.mouseButtons = optMouseButtons_.load();
    options.mouseMove = optMouseMove_.load();
    options.mouseWheel = optMouseWheel_.load();
    return options;
}

InputMonitorStats InputMonitor::Stats() const {
    InputMonitorStats stats;
    stats.keyboardEvents = keyboardEvents_.load();
    stats.mouseEvents = mouseEvents_.load();
    stats.injectedIgnored = injectedIgnored_.load();
    stats.keyUpIgnored = keyUpIgnored_.load();
    stats.moveIgnored = moveIgnored_.load();
    stats.disabledIgnored = disabledIgnored_.load();
    stats.hotkeyIgnored = hotkeyIgnored_.load();
    stats.triggered = triggered_.load();
    stats.simulatedForTest = simulatedForTest_.load();
    // worstCallbackTicks_ 是 QPC 计数，必须按时钟频率换算（本机 10 MHz → 1 计数 = 100 ns）
    stats.worstCallbackMs = TicksToSeconds(worstCallbackTicks_.load()) * 1000.0;
    return stats;
}

std::string InputMonitor::DescribeOptions() const {
    const InputMonitorOptions options = Options();
    std::string text = "用户输入停止：键盘按键 ";
    text += options.keyboard ? "开" : "关";
    text += "　鼠标按键 ";
    text += options.mouseButtons ? "开" : "关";
    text += "　鼠标移动 ";
    text += options.mouseMove ? "开（不推荐：鼠标一动就停）" : "关（默认）";
    text += "　鼠标滚轮 ";
    text += options.mouseWheel ? "开（默认）" : "关";
    return text;
}

std::string InputMonitor::DescribeStats() const {
    const InputMonitorStats stats = Stats();
    std::string text;
    text += "钩子事件：键盘 " + std::to_string(stats.keyboardEvents) + " 条、鼠标 " +
            std::to_string(stats.mouseEvents) + " 条\n";
    text += "忽略原因：注入 " + std::to_string(stats.injectedIgnored) + "、松开 " +
            std::to_string(stats.keyUpIgnored) + "、鼠标移动 " + std::to_string(stats.moveIgnored) +
            "、设置关闭 " + std::to_string(stats.disabledIgnored) + "、启动热键 " +
            std::to_string(stats.hotkeyIgnored) + "\n";
    text += "触发急停：" + std::to_string(stats.triggered) + " 次";
    if (stats.simulatedForTest > 0) {
        text += "（其中自检模拟 " + std::to_string(stats.simulatedForTest) + " 次）";
    }
    text += "\n钩子回调最坏耗时：";
    const double worstMs = TicksToSeconds(worstCallbackTicks_.load()) * 1000.0;
    if (worstMs < 0.001) {
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "%.0f ns（不到 0.001 ms）", worstMs * 1000000.0);
        text += buffer;
    } else {
        text += ToMillisecondsText(static_cast<long long>(worstMs * 1000000.0)) + " ms";
    }
    return text;
}

UserInputEvent InputMonitor::LastEvent() const {
    UserInputEvent event;
    event.kind = static_cast<UserInputKind>(lastKind_.load());
    event.vk = lastVk_.load();
    event.button = static_cast<MouseButton>(lastButton_.load());
    event.tick = lastTick_.load();
    event.injected = lastInjected_.load();
    switch (event.kind) {
        case UserInputKind::KeyDown:
            event.name = VirtualKeyDisplayName(event.vk);
            break;
        case UserInputKind::MouseButtonDown:
            event.name = MouseButtonDisplayName(event.button);
            break;
        case UserInputKind::MouseMove:
            event.name = "鼠标移动";
            break;
        case UserInputKind::Wheel:
            event.name = WheelEventName(lastWheelDelta_.load());
            break;
        default:
            event.name = "未知输入";
            break;
    }
    return event;
}

bool InputMonitor::WaitForTrigger(long long count, int timeoutMs) {
    const unsigned long long deadline =
        GetTickCount64() + static_cast<unsigned long long>(timeoutMs < 0 ? 0 : timeoutMs);
    while (triggered_.load() < count) {
        if (GetTickCount64() >= deadline) {
            return false;
        }
        Sleep(1);
    }
    return true;
}

}  // namespace hp
