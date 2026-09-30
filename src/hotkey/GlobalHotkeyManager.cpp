#include "hotkey/GlobalHotkeyManager.h"

#include <chrono>

#include "core/Log.h"
#include "core/Platform.h"

namespace hp {
namespace {

// 热键 id：WM_HOTKEY 的 wParam 就是它。用一个不常见的基数，避免与别的模块撞号。
constexpr int kHotkeyIdBase = 0x4B50;   // "KP"

// 发给热键线程的自定义消息
constexpr UINT kMessageApplyRequest = WM_APP + 0x41;
constexpr UINT kMessageQuit = WM_APP + 0x42;

int IndexOf(HotkeyAction action) {
    const int index = static_cast<int>(action);
    return (index >= 0 && index < kHotkeyActionCount) ? index : -1;
}

// RegisterHotKey 失败时的中文解释
std::string ExplainRegisterFailure(unsigned long code) {
    switch (code) {
        case ERROR_HOTKEY_ALREADY_REGISTERED:
            return "这个组合已经被别的程序（或本工具别的地方）占用了";
        case ERROR_INVALID_PARAMETER:
            return "系统不接受这个组合";
        case ERROR_ACCESS_DENIED:
            return "权限不足，无法注册这个组合";
        default:
            return "注册失败（错误码 " + std::to_string(code) + "）";
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 构造与析构
// ---------------------------------------------------------------------------
GlobalHotkeyManager::GlobalHotkeyManager() {
    bindings_.resize(kHotkeyActionCount);
    for (int i = 0; i < kHotkeyActionCount; ++i) {
        bindings_[static_cast<size_t>(i)] = DefaultHotkey(static_cast<HotkeyAction>(i));
        eventCounts_[i].store(0);
    }
}

GlobalHotkeyManager::~GlobalHotkeyManager() {
    Stop();
}

int GlobalHotkeyManager::IdOf(HotkeyAction action) {
    const int index = IndexOf(action);
    return index < 0 ? -1 : kHotkeyIdBase + index;
}

HotkeyAction GlobalHotkeyManager::ActionOfId(int id, bool* known) {
    const int index = id - kHotkeyIdBase;
    if (index >= 0 && index < kHotkeyActionCount) {
        if (known != nullptr) {
            *known = true;
        }
        return static_cast<HotkeyAction>(index);
    }
    if (known != nullptr) {
        *known = false;
    }
    return HotkeyAction::Play;
}

// ---------------------------------------------------------------------------
// 启动 / 停止
// ---------------------------------------------------------------------------
bool GlobalHotkeyManager::Start(const Callback& callback, std::string* error) {
    if (running_.load()) {
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = callback;
        ready_ = false;
        registrationErrors_.clear();
        startError_.clear();
    }
    quit_.store(false);

    thread_ = std::thread(&GlobalHotkeyManager::ThreadMain, this);

    // 等热键线程就绪（消息队列已建、默认热键已尝试注册）
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool started = readyCv_.wait_for(lock, std::chrono::seconds(3), [this] { return ready_; });
        if (!started) {
            if (error != nullptr) {
                *error = "热键线程在 3 秒内没有就绪";
            }
            quit_.store(true);
            lock.unlock();
            if (thread_.joinable()) {
                thread_.join();
            }
            return false;
        }
    }
    running_.store(true);
    return true;
}

void GlobalHotkeyManager::Stop() {
    if (!thread_.joinable()) {
        running_.store(false);
        return;
    }
    quit_.store(true);
    const unsigned long id = threadId_.load();
    if (id != 0) {
        // 让消息循环醒过来并退出（注销热键由热键线程自己做，Register/Unregister 必须同线程）
        if (!PostThreadMessageW(id, kMessageQuit, 0, 0)) {
            // 消息队列已经消失（线程已退出），交给 join 收尾
        }
    }
    thread_.join();
    running_.store(false);
    threadId_.store(0);
}

// ---------------------------------------------------------------------------
// 热键线程
// ---------------------------------------------------------------------------
void GlobalHotkeyManager::ThreadMain() {
    threadId_.store(GetCurrentThreadId());

    // 必须先把消息队列建出来，否则 PostThreadMessage 会失败/丢消息
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    // 在该线程上注册当前绑定（默认 F6/F7/F8）
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registrationErrors_.clear();
        for (int i = 0; i < kHotkeyActionCount; ++i) {
            const HotkeyAction action = static_cast<HotkeyAction>(i);
            const HotkeyBinding binding = bindings_[static_cast<size_t>(i)];
            if (binding.IsEmpty()) {
                continue;
            }
            const int id = IdOf(action);
            if (!RegisterHotKey(nullptr, id, binding.modifiers | MOD_NOREPEAT,
                                static_cast<UINT>(binding.vk))) {
                const unsigned long code = GetLastError();
                registrationErrors_.push_back(binding.ToString() + "（" +
                                              HotkeyActionDisplayName(action) + "）" +
                                              ExplainRegisterFailure(code));
            }
        }
        ready_ = true;
    }
    readyCv_.notify_all();

    while (!quit_.load()) {
        const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
        if (result <= 0) {
            break;   // WM_QUIT 或出错
        }
        switch (msg.message) {
            case WM_HOTKEY:
                HandleHotkeyMessage(static_cast<int>(msg.wParam));
                break;
            case kMessageApplyRequest: {
                auto* request = reinterpret_cast<ChangeRequest*>(msg.lParam);
                if (request != nullptr) {
                    ApplyRequest(request);
                }
                break;
            }
            case kMessageQuit:
                quit_.store(true);
                break;
            default:
                break;
        }
    }

    // 退出前在**本线程**注销全部热键（跨线程注销是无效的）
    std::lock_guard<std::mutex> lock(mutex_);
    UnregisterAllLocked();
}

void GlobalHotkeyManager::SetSuspended(bool suspended) {
    if (!running_.load()) {
        suspended_.store(suspended);
        return;
    }
    ChangeRequest request;
    request.suspendRequest = true;
    request.suspendValue = suspended;
    std::unique_lock<std::mutex> lock(request.mutex);
    if (!PostThreadMessageW(threadId_.load(), kMessageApplyRequest, 0,
                            reinterpret_cast<LPARAM>(&request))) {
        suspended_.store(suspended);   // 线程没了：至少把标志位对上
        return;
    }
    request.cv.wait_for(lock, std::chrono::seconds(2), [&request] { return request.done; });
    lock.unlock();
    suspended_.store(suspended);
}

void GlobalHotkeyManager::UnregisterAllLocked() {
    for (int i = 0; i < kHotkeyActionCount; ++i) {
        UnregisterHotKey(nullptr, IdOf(static_cast<HotkeyAction>(i)));
    }
}

// ---------------------------------------------------------------------------
// 热键触发
// ---------------------------------------------------------------------------
void GlobalHotkeyManager::HandleHotkeyMessage(int id) {
    bool known = false;
    const HotkeyAction action = ActionOfId(id, &known);
    if (!known) {
        return;
    }

    HotkeyEvent event;
    event.action = action;
    event.id = id;
    event.tick = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        event.binding = bindings_[static_cast<size_t>(static_cast<int>(action))];
    }

    const int index = IndexOf(action);
    if (index >= 0) {
        eventCounts_[index].fetch_add(1);
    }
    lastEventTick_.store(event.tick);
    lastEventVk_.store(event.binding.vk);
    lastEventAction_.store(static_cast<int>(action));
    if (action == HotkeyAction::Play) {
        // 十八节：记下启动热键这一下，供 Phase 6 的输入监控忽略
        lastStartTick_.store(event.tick);
    }
    {
        std::lock_guard<std::mutex> lock(lastEventMutex_);
        lastEvent_ = event;
    }

    Callback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = callback_;
    }
    if (callback) {
        callback(event);   // 约定：这里必须极短、不得阻塞
    }
}

// ---------------------------------------------------------------------------
// 运行期改热键
// ---------------------------------------------------------------------------
void GlobalHotkeyManager::ApplyRequest(ChangeRequest* request) {
    HotkeyChangeResult result;

    std::lock_guard<std::mutex> lock(mutex_);

    // 统一的"交回结果"出口：写结果 + 置完成 + 唤醒调用者
    auto finish = [request](const HotkeyChangeResult& outcome) {
        {
            std::lock_guard<std::mutex> requestLock(request->mutex);
            request->result = outcome;
            request->done = true;
        }
        request->cv.notify_all();
    };

    auto registerOne = [this, &result](HotkeyAction action, const HotkeyBinding& binding) -> bool {
        const int id = IdOf(action);
        if (id < 0) {
            result.error = "未知的热键动作";
            return false;
        }
        if (binding.IsEmpty()) {
            UnregisterHotKey(nullptr, id);
            return true;
        }
        if (!RegisterHotKey(nullptr, id, binding.modifiers | MOD_NOREPEAT,
                            static_cast<UINT>(binding.vk))) {
            result.error = binding.ToString() + " " + ExplainRegisterFailure(GetLastError());
            return false;
        }
        return true;
    };

    if (request->suspendRequest) {
        if (request->suspendValue) {
            UnregisterAllLocked();
            suspended_.store(true);
        } else {
            suspended_.store(false);
            // 按当前绑定全部重新注册。挂起期间绑定可能被改过（改了但没注册），
            // 所以这里不看别的状态，直接按 bindings_ 来一遍。
            registrationErrors_.clear();
            for (int i = 0; i < kHotkeyActionCount; ++i) {
                const HotkeyAction action = static_cast<HotkeyAction>(i);
                const HotkeyBinding binding = bindings_[static_cast<size_t>(i)];
                if (binding.IsEmpty()) {
                    continue;
                }
                if (!registerOne(action, binding)) {
                    registrationErrors_.push_back(binding.ToString() + "（" +
                                                  HotkeyActionDisplayName(action) + "）" +
                                                  result.error);
                }
            }
        }
        result.ok = true;
        finish(result);
        return;
    }

    // 挂起中改绑定：只记下来，不注册（等恢复时统一注册）
    if (suspended_.load() && !request->resetAll) {
        const int suspendIndex = IndexOf(request->action);
        if (suspendIndex >= 0) {
            bindings_[static_cast<size_t>(suspendIndex)] = request->binding;
            result.ok = true;
            finish(result);
            return;
        }
    }

    if (request->resetAll) {
        // 恢复默认：先把自己全部注销，再逐个注册默认值。
        // 注册失败的动作用"默认绑定"记录（用户看到的设置就是默认值），
        // 失败原因进 registrationErrors_，不会假装成功。
        UnregisterAllLocked();
        registrationErrors_.clear();
        for (int i = 0; i < kHotkeyActionCount; ++i) {
            const HotkeyAction action = static_cast<HotkeyAction>(i);
            const HotkeyBinding binding = DefaultHotkey(action);
            bindings_[static_cast<size_t>(i)] = binding;
            if (binding.IsEmpty()) {
                continue;
            }
            if (!registerOne(action, binding)) {
                registrationErrors_.push_back(binding.ToString() + "（" +
                                              HotkeyActionDisplayName(action) + "）" +
                                              result.error);
            }
        }
        result.ok = registrationErrors_.empty();
        if (!result.ok) {
            result.error = registrationErrors_.front();
        }
        finish(result);
        return;
    }

    const int actionIndex = IndexOf(request->action);
    if (actionIndex < 0) {
        result.error = "未知的热键动作";
        finish(result);
        return;
    }

    // 与其它动作重复？直接拒绝（不猜用户意图）
    if (!request->binding.IsEmpty()) {
        for (int i = 0; i < kHotkeyActionCount; ++i) {
            if (i == actionIndex) {
                continue;
            }
            const HotkeyBinding& other = bindings_[static_cast<size_t>(i)];
            if (!other.IsEmpty() && other.Equals(request->binding)) {
                result.error = request->binding.ToString() + " 已经给了「" +
                               HotkeyActionDisplayName(static_cast<HotkeyAction>(i)) +
                               "」，两个动作不能共用同一把键";
                finish(result);
                return;
            }
        }
    }

    const HotkeyBinding oldBinding = bindings_[static_cast<size_t>(actionIndex)];
    const int id = IdOf(request->action);
    // 先注销旧的，再注册新的；新注册失败就回滚到旧绑定（失败不改变现状）
    UnregisterHotKey(nullptr, id);
    if (!registerOne(request->action, request->binding)) {
        const std::string failure = result.error;
        result = HotkeyChangeResult{};
        const bool restored = registerOne(request->action, oldBinding);
        result.ok = false;
        result.error = failure;
        if (!restored) {
            result.error += "；而且原绑定 " + oldBinding.ToString() + " 也恢复失败，这个动作目前没有热键";
        }
        finish(result);
        return;
    }

    bindings_[static_cast<size_t>(actionIndex)] = request->binding;
    result.ok = true;
    if (request->binding.IsRisky()) {
        result.warning = request->binding.ToString() +
                         " 没有修饰键，注册成全局热键后这个键在游戏里会被本工具抢走，"
                         "建议换成 F1-F24 或加 Ctrl/Alt/Shift";
    }
    finish(result);
}

HotkeyChangeResult GlobalHotkeyManager::SetHotkey(HotkeyAction action, const HotkeyBinding& binding) {
    HotkeyChangeResult result;
    if (!binding.IsEmpty() && !binding.IsValid()) {
        result.error = binding.ToString() + " 不是本工具支持的键（" + HotkeyHelpText() + "）";
        return result;
    }
    if (!running_.load()) {
        result.error = "热键管理器还没有启动";
        return result;
    }

    ChangeRequest request;
    request.action = action;
    request.binding = binding;

    // 在热键线程上调用（例如回调里改热键）：直接做，避免自己等自己
    if (GetCurrentThreadId() == threadId_.load()) {
        ApplyRequest(&request);
        return request.result;
    }

    std::unique_lock<std::mutex> lock(request.mutex);
    if (!PostThreadMessageW(threadId_.load(), kMessageApplyRequest, 0,
                            reinterpret_cast<LPARAM>(&request))) {
        result.error = "无法把请求交给热键线程（错误码 " + std::to_string(GetLastError()) + "）";
        return result;
    }
    const bool done = request.cv.wait_for(lock, std::chrono::seconds(3), [&request] { return request.done; });
    if (!done) {
        result.error = "热键线程没有响应（等待超时）";
        return result;
    }
    return request.result;
}

HotkeyChangeResult GlobalHotkeyManager::ResetToDefaults() {
    HotkeyChangeResult result;
    if (!running_.load()) {
        result.error = "热键管理器还没有启动";
        return result;
    }
    ChangeRequest request;
    request.resetAll = true;
    if (GetCurrentThreadId() == threadId_.load()) {
        ApplyRequest(&request);
        return request.result;
    }
    std::unique_lock<std::mutex> lock(request.mutex);
    if (!PostThreadMessageW(threadId_.load(), kMessageApplyRequest, 0,
                            reinterpret_cast<LPARAM>(&request))) {
        result.error = "无法把请求交给热键线程（错误码 " + std::to_string(GetLastError()) + "）";
        return result;
    }
    const bool done = request.cv.wait_for(lock, std::chrono::seconds(3), [&request] { return request.done; });
    if (!done) {
        result.error = "热键线程没有响应（等待超时）";
        return result;
    }
    return request.result;
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------
HotkeyBinding GlobalHotkeyManager::GetHotkey(HotkeyAction action) const {
    const int index = IndexOf(action);
    if (index < 0) {
        return HotkeyBinding{};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return bindings_[static_cast<size_t>(index)];
}

std::vector<HotkeyBinding> GlobalHotkeyManager::AllBindings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bindings_;
}

std::string GlobalHotkeyManager::DescribeAll() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string text;
    for (int i = 0; i < kHotkeyActionCount; ++i) {
        const HotkeyAction action = static_cast<HotkeyAction>(i);
        const HotkeyBinding& binding = bindings_[static_cast<size_t>(i)];
        text += HotkeyActionDisplayName(action);
        text += "  ";
        text += binding.ToString();
        if (binding.IsEmpty()) {
            text += "（没有热键）";
        } else if (binding.IsRisky()) {
            text += "（注意：会抢走这个键）";
        }
        text += "\n";
    }
    return text;
}

std::vector<std::string> GlobalHotkeyManager::Conflicts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> conflicts;
    for (int i = 0; i < kHotkeyActionCount; ++i) {
        for (int j = i + 1; j < kHotkeyActionCount; ++j) {
            const HotkeyBinding& a = bindings_[static_cast<size_t>(i)];
            const HotkeyBinding& b = bindings_[static_cast<size_t>(j)];
            if (!a.IsEmpty() && a.Equals(b)) {
                conflicts.push_back(std::string(HotkeyActionDisplayName(static_cast<HotkeyAction>(i))) +
                                    " 与 " +
                                    HotkeyActionDisplayName(static_cast<HotkeyAction>(j)) + " 都绑在 " +
                                    a.ToString() + " 上");
            }
        }
    }
    return conflicts;
}

int GlobalHotkeyManager::RegisteredCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const HotkeyBinding& binding : bindings_) {
        if (!binding.IsEmpty()) {
            ++count;
        }
    }
    return count;
}

std::vector<std::string> GlobalHotkeyManager::RegistrationErrors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return registrationErrors_;
}

std::string GlobalHotkeyManager::LastError() const {
    const std::vector<std::string> errors = RegistrationErrors();
    return errors.empty() ? std::string() : errors.front();
}

// ---------------------------------------------------------------------------
// 十八节：启动热键特殊处理
// ---------------------------------------------------------------------------
bool GlobalHotkeyManager::StartHotkeyPressedRecently(int withinMs) const {
    const unsigned long long tick = lastStartTick_.load();
    if (tick == 0) {
        return false;
    }
    const unsigned long long now = GetTickCount64();
    if (now < tick) {
        return false;
    }
    return (now - tick) <= static_cast<unsigned long long>(withinMs < 0 ? 0 : withinMs);
}

unsigned GlobalHotkeyManager::StartHotkeyVirtualKey() const {
    const HotkeyBinding binding = GetHotkey(HotkeyAction::Play);
    return binding.vk;
}

bool GlobalHotkeyManager::IsBoundKey(unsigned vk) const {
    if (vk == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const HotkeyBinding& binding : bindings_) {
        if (binding.vk == vk) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 测试辅助
// ---------------------------------------------------------------------------
int GlobalHotkeyManager::EventCount(HotkeyAction action) const {
    const int index = IndexOf(action);
    return index < 0 ? 0 : eventCounts_[index].load();
}

HotkeyEvent GlobalHotkeyManager::LastEvent() const {
    std::lock_guard<std::mutex> lock(lastEventMutex_);
    return lastEvent_;
}

bool GlobalHotkeyManager::WaitForEvent(HotkeyAction action, int count, int timeoutMs) {
    const unsigned long long deadline =
        GetTickCount64() + static_cast<unsigned long long>(timeoutMs < 0 ? 0 : timeoutMs);
    while (EventCount(action) < count) {
        if (GetTickCount64() >= deadline) {
            return false;
        }
        Sleep(1);
    }
    return true;
}

}  // namespace hp
