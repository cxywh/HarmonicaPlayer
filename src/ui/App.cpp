#include "App.h"
#include <shellapi.h>   // ShellExecuteW：申请管理员权限（runas）

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <vector>

#include <commctrl.h>
#include <commdlg.h>   // GetOpenFileNameW / GetSaveFileNameW（OPENFILENAMEW）

#include "core/Log.h"
#include "song/SongSerializer.h"
#include "song/TestSongs.h"
#include "ui/MainWindow.h"
#include "ui/SettingsWindow.h"
#include "ui/SettingsStore.h"
#include "ui/NewSongWindow.h"
#include "song/BeatMath.h"
#include "ui/UiFormat.h"

namespace hp {
namespace ui {

namespace {

App* g_app = nullptr;   // 前台窗口钩子是静态回调，需要拿到当前应用

std::string JoinText(const std::vector<std::string>& parts, const std::string& separator) {
    std::string text;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            text += separator;
        }
        text += parts[i];
    }
    return text;
}

// 本进程是不是以管理员身份运行（决定 SendInput 会不会被系统 UIPI 拦：游戏提权了工具没提权，注入会被丢弃）
bool IsProcessElevatedNow() {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

// 窗口是不是本程序自己的（主界面/设置/编辑/文件对话框）：不算"切出游戏"
bool IsOwnProcessWindow(HWND window) {
    if (window == nullptr) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    return pid == GetCurrentProcessId();
}

// 文件对话框：打开期间把前台窗口变化压住（对话框也是"自己人"，不该被当成切出游戏）
struct DialogGuard {
    explicit DialogGuard(bool& flag) : flag_(flag) { flag_ = true; }
    ~DialogGuard() { flag_ = false; }
    bool& flag_;
};

std::string OpenSongDialog(HWND owner) {
    wchar_t buffer[MAX_PATH * 2] = L"";
    const std::wstring directory = Utf8ToWide(DefaultSongDirectory());
    OPENFILENAMEW options{};
    options.lStructSize = sizeof(options);
    options.hwndOwner = owner;
    options.lpstrFilter = L"曲谱文件 (*.json)\\0*.json\\0所有文件 (*.*)\\0*.*\\0\\0";
    options.lpstrFile = buffer;
    options.nMaxFile = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    options.lpstrTitle = L"打开曲谱";
    options.lpstrInitialDir = directory.c_str();
    options.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    options.lpstrDefExt = L"json";
    if (GetOpenFileNameW(&options) == FALSE) {
        return std::string();
    }
    return WideToUtf8(buffer);
}

std::string SaveSongDialog(HWND owner, const std::string& suggestedName) {
    wchar_t buffer[MAX_PATH * 2] = L"";
    const std::wstring suggested = Utf8ToWide(SanitizeFileName(suggestedName) + ".json");
    lstrcpynW(buffer, suggested.c_str(), static_cast<int>(sizeof(buffer) / sizeof(buffer[0])));
    const std::wstring directory = Utf8ToWide(DefaultSongDirectory());
    OPENFILENAMEW options{};
    options.lStructSize = sizeof(options);
    options.hwndOwner = owner;
    options.lpstrFilter = L"曲谱文件 (*.json)\\0*.json\\0所有文件 (*.*)\\0*.*\\0\\0";
    options.lpstrFile = buffer;
    options.nMaxFile = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    options.lpstrTitle = L"曲谱另存为";
    options.lpstrInitialDir = directory.c_str();
    options.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    options.lpstrDefExt = L"json";
    if (GetSaveFileNameW(&options) == FALSE) {
        return std::string();
    }
    return WideToUtf8(buffer);
}

// 路径比较（Windows 不区分大小写）——"当前这首"是不是列表里的那一条，靠它认
bool SamePathText(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty() || a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) {
            return false;
        }
    }
    return true;
}

// 只取文件名（状态行里显示长路径太吵）
std::string FileNameText(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// 单实例守护：把已经在跑的那个实例叫到最前面（找不到就什么也不做）
void ActivateExistingInstance() {
    const HWND existing = FindWindowW(L"HarmonicaPlayerMain", nullptr);
    if (existing == nullptr) {
        return;
    }
    if (IsIconic(existing) != FALSE) {
        ShowWindow(existing, SW_RESTORE);
    }
    SetForegroundWindow(existing);
}

}  // namespace

App::App() = default;

App::~App() {
    if (foregroundHook_ != nullptr) {
        UnhookWinEvent(foregroundHook_);
        foregroundHook_ = nullptr;
    }
    if (instanceMutex_ != nullptr) {
        CloseHandle(instanceMutex_);   // 释放单实例守护，下次能再开
        instanceMutex_ = nullptr;
    }
    if (monitor_) {
        monitor_->Stop();
    }
    if (hotkeys_) {
        hotkeys_->Stop();
    }
    if (player_) {
        player_->Stop(StopReason::Shutdown);
        player_->Quit();
    }
    if (input_) {
        // 异常安全（四十二节）：退出前一定把工具按下的键放掉
        input_->ReleaseAll();
    }
    // Phase 11 配置保存：退出前落盘（ReleaseAll 已在上方完成，顺序上"先放键、再存盘"）
    if (!settingsPath_.empty()) {
        std::string ignored;
        SaveSettingsNow(&ignored);
    }

    delete settingsWindow_;
    delete newSongWindow_;
    delete mainWindow_;
    settingsWindow_ = nullptr;
    newSongWindow_ = nullptr;
    mainWindow_ = nullptr;
    if (g_app == this) {
        g_app = nullptr;
    }
}

bool App::IsElevated() const {
    return IsProcessElevatedNow();
}

// 申请管理员权限并重启自己。游戏以管理员身份运行时，普通权限进程的 SendInput 会被 UIPI 丢掉，
// 用户看到的是"按了播放、游戏里没反应"——所以这件事必须能在界面里一键做完，不能让他去右键菜单找。
bool App::RequestElevation(std::string* message) {
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD capacity = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    if (GetModuleFileNameW(nullptr, path, capacity) == 0) {
        if (message != nullptr) {
            *message = "取不到本程序的路径";
        }
        return false;
    }
    if (elevationDryRun_) {
        // 排错/自检用：只记一行日志，不真的弹 UAC
        LogInfo("（干跑）本来会申请管理员权限重启：HarmonicaPlayer --elevated-attempted");
        if (message != nullptr) {
            *message = "干跑模式：没有真的申请";
        }
        return false;
    }
    // shellapi 的约定：返回值 <= 32 是错误码、不是句柄
    const INT_PTR result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"runas", path, L"--elevated-attempted", nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        const bool cancelled = result == ERROR_CANCELLED;
        const std::string code = std::to_string(static_cast<long long>(result));
        LogWarn(std::string("申请管理员权限") + (cancelled ? "被取消（用户点了否）"
                                                        : ("失败，系统错误码 " + code)));
        if (message != nullptr) {
            *message = cancelled ? "你在系统弹窗里点了「否」" : ("系统报错（代码 " + code + "）");
        }
        return false;
    }
    LogInfo("正在以管理员权限重启（新实例带 --elevated-attempted）");
    return true;
}

bool App::RequestElevationAndQuit(std::string* message) {
    if (!RequestElevation(message)) {
        return false;
    }
    // 提权成功：本实例必须真的退出，否则会跟新实例一起活着（双份钩子 / 双份热键 / 抢注入）。
    // 走**和启动时同一套退出流程**：置退出标志 + 给主窗口投递 WM_CLOSE
    //（WM_CLOSE → DestroyWindow → WM_DESTROY → PostQuitMessage → Run 的消息循环结束）。
    elevationRelaunching_ = true;
    if (mainWindow_ != nullptr && mainWindow_->Handle() != nullptr) {
        PostMessageW(mainWindow_->Handle(), WM_CLOSE, 0, 0);
    }
    return true;
}

// 单实例守护（具名 Mutex，Local\ = 当前登录会话）。
//   谁先建谁持有；后来的实例拿到 ERROR_ALREADY_EXISTS。
//   - 提权重启（带 --elevated-attempted）：旧实例正在退出，等它把互斥量交出来（最多 5 秒）；
//   - 否则：把已有窗口叫到前面、给一句提示、本次启动失败（Initialize 返回 false，主程序弹提示后退出）。
bool App::AcquireSingleInstance(std::string* error) {
    // 初始拥有：谁先 CreateMutexW 成功谁持有，进程退出时自动释放（被放弃 → WAIT_ABANDONED）
    instanceMutex_ = CreateMutexW(nullptr, TRUE, L"Local\\HarmonicaPlayer_SingleInstance_v1");
    if (instanceMutex_ == nullptr) {
        // 极端情况拿不到互斥量：不该因此挡住程序，照常启动（记录一笔）
        LogWarn("单实例守护：创建具名互斥量失败，本次不做单实例检查。");
        return true;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        return true;   // 我们是第一个实例
    }
    if (elevationAttempted_) {
        // 提权重启：旧实例马上会退出，等它把互斥量交出来（WAIT_ABANDONED = 旧实例已终止）
        const DWORD waited = WaitForSingleObject(instanceMutex_, 5000);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) {
            LogInfo("单实例守护：提权重启，旧实例已退出，由本实例接手。");
            return true;
        }
        LogWarn("单实例守护：提权重启时等旧实例退出超时（5 秒），仍继续启动。");
        return true;
    }
    // 已经有一个实例在跑：叫到前面 + 提示 + 退出
    ActivateExistingInstance();
    CloseHandle(instanceMutex_);
    instanceMutex_ = nullptr;
    if (error != nullptr) {
        *error = "已经有一个 Harmonica Player 在运行了（同一个程序只开一个，免得两个实例抢按键）。"
                 "已试着把它切到最前面。";
    }
    return false;
}

bool App::Initialize(HINSTANCE instance, std::string* error) {
    instance_ = instance;
    g_app = this;

    // ---- 命令行开关：要在单实例守护之前知道“本次是不是提权重启”----
    // --no-elevate 强制不提权（排错/自检）；--elevated-attempted = 这一次是申请后重启的；
    // --dry-elevate 只记日志、不真申请。
    {
        const wchar_t* rawCommandLine = GetCommandLineW();
        const std::wstring commandLine = rawCommandLine != nullptr ? rawCommandLine : L"";
        elevationSuppressed_ = commandLine.find(L"--no-elevate") != std::wstring::npos;
        elevationAttempted_ = commandLine.find(L"--elevated-attempted") != std::wstring::npos;
        elevationDryRun_ = commandLine.find(L"--dry-elevate") != std::wstring::npos;
    }

    // ---- 单实例守护（具名 Mutex）：已经有实例在跑就别再建第二套钩子/热键 ----
    if (!AcquireSingleInstance(error)) {
        return false;
    }

    // ---- 配置保存（Phase 11）：先读盘，坏文件也照常启动 ----
    settingsPath_ = DefaultSettingsPath();
    const SettingsLoadResult loaded = LoadSettings(AppSettings::Default(), settingsPath_);
    settings_ = loaded.settings;
    if (!loaded.ok) {
        SetStatus(loaded.error + "（会在退出时写一份新的）", false);
    } else if (!loaded.warnings.empty()) {
        SetStatus("配置已读取，但有 " + std::to_string(loaded.warnings.size()) + " 处问题：" +
                      loaded.warnings.front(),
                  false);
    }

    input_ = std::make_unique<InputController>(InputSendOptions());

    PlayerOptions playerOptions;
    playerOptions.dryRun = settings_.dryRun;
    player_ = std::make_unique<SongPlayer>(*input_, playerOptions);

    // 曲谱目录：打包给别人时程序目录里可能没有 songs 文件夹，这里启动就建好
    // （放在程序自己旁边，跟着程序走；建不出来就如实提示，不影响演奏）
    const std::string songDirectory = DefaultSongDirectory();
    if (!songDirectory.empty() && !EnsureDirectory(songDirectory)) {
        SetStatus("没法在程序目录里建 songs 文件夹（" + songDirectory +
                      "）：保存曲谱时请另选一个能写的位置。",
                  true);
    }

    // Phase 12：不再默认塞一首内置曲。启动时要么载入"上次打开的那首"，要么就是空曲谱，
    // 等用户自己编辑/保存（内置曲还留在 TestSongs 里，文本曲谱窗口的示例照旧能用）。
    song_ = Song();
    hasSong_ = false;
    // 配置里"上次打开的曲谱"只在真能载入成功时才成为"当前曲谱"；
    // 载入失败时这里必须留空，否则界面上会出现"标题说没有曲谱、下拉却选中某一首"的矛盾
    currentSongPath_.clear();
    RebuildTimeline();     // 空曲谱也要建一次时间轴：界面拿到的是一份合法的空时间轴
    RefreshSongList();

    auto notice = [&](const std::string& text, bool isError) {
        startupNotice_ = text;
        startupNoticeError_ = isError;
        SetStatus(text, isError);
    };

    if (!settings_.songPath.empty()) {
        std::string message;
        if (OpenSongFile(settings_.songPath, &message)) {
            notice("已载入上次打开的曲谱：" + song_.name + "（" +
                       std::to_string(timeline_.NoteCount()) + " 个音符）",
                   false);
        } else {
            notice("上次打开的曲谱现在打不开了（" + settings_.songPath + "）：" + message +
                       "　现在是空曲谱，编辑一首保存后就能从列表里选。",
                   true);
        }
    } else {
        notice(std::string("还没有曲谱：点「") + labels::kNewSong +
                   "」建一首（点按钮选音调，存好就出现在曲谱列表里）。",
               false);
    }

    // ---- 管理员权限（游戏以管理员身份运行时，普通权限的 SendInput 会被 UIPI 悄悄丢掉）----
    // 命令行开关已在上方（单实例守护之前）解析：--no-elevate 强制不提权（排错/自检）；
    // --elevated-attempted = 这一次是申请后重启的；--dry-elevate 只记日志、不真申请。
    {
        const bool elevated = IsElevated();
        if (!elevated && settings_.runAsAdmin && !elevationSuppressed_ && !elevationAttempted_) {
            std::string reason;
            if (RequestElevation(&reason)) {
                elevationRelaunching_ = true;   // 新实例接着跑，本实例交给 main 静默退出
                return true;
            }
            notice(statusText_ + "　[权限] 申请管理员权限没成功（" + reason +
                       "）：仍以普通权限运行；游戏以管理员身份运行时按键会被系统拦掉。",
                   true);
        } else if (elevated) {
            notice(statusText_ + "　[权限] 已用管理员权限运行：游戏以管理员身份运行时也能正常按键。",
                   startupNoticeError_);
        } else if (elevationAttempted_) {
            notice(statusText_ + "　[权限] 提权没有生效（系统弹窗里点了否？）：现在仍是普通权限，"
                                 "游戏以管理员身份运行时按键会被拦掉。「设置」→"
                                 "「立即以管理员身份重启」可以再试一次。",
                   startupNoticeError_);
        } else {
            notice(statusText_ + "　[权限] 现在是普通权限：游戏以管理员身份运行时按键会被系统拦掉。"
                                 "点「设置」勾选「以管理员身份运行」后重启即可（也可以直接点"
                                 "「立即以管理员身份重启」）。",
                   startupNoticeError_);
        }
    }

    // ---- 全局热键（十三节）----
    // exe 同目录的 debug.log（记录错误用；写不出去就静默降级）
    LogStartSession();

    hotkeys_ = std::make_unique<GlobalHotkeyManager>();
    std::string hotkeyError;
    // 屏幕提示窗口：切歌时显示当前曲名。创建失败不影响主功能，但要说出来
    if (osd_ == nullptr) {
        auto osd = std::make_unique<OsdWindow>();
        std::string osdError;
        if (osd->Create(instance, &osdError)) {
            osd_ = std::move(osd);
        } else {
            SetStatus("提示窗口没创建起来（按上一首/下一首不会在屏幕上提示）：" + osdError, true);
        }
    }

    // 放行模式（默认）**不注册**全局热键：那个键在系统里没人独占，游戏照样收到；
    // 热键改由 InputMonitor 的低层钩子认（见 OnHookKey）。独占模式才是老行为。
    hookHotkeysActive_.store(!settings_.exclusiveHotkeys);
    if (settings_.exclusiveHotkeys &&
        !hotkeys_->Start([this](const HotkeyEvent& event) { OnHotkey(event); }, &hotkeyError)) {
        SetStatus("全局热键暂时不可用（" + hotkeyError + "），可以先用界面按钮。", true);
    }

    // ---- 用户输入急停（Phase 6）----
    monitor_ = std::make_unique<InputMonitor>();
    InputMonitorOptions monitorOptions;
    monitorOptions.keyboard = settings_.monitorKeyboard;
    monitorOptions.mouseButtons = settings_.monitorMouseButtons;
    monitorOptions.mouseMove = settings_.monitorMouseMove;
    InputMonitorCallbacks callbacks;
    callbacks.onUserInput = [this](const UserInputEvent& event) { OnUserInput(event); };
    callbacks.shouldIgnore = [this](const UserInputEvent& event) { return ShouldIgnoreUserInput(event); };
    callbacks.onHookKey = [this](const UserInputEvent& event) { OnHookKey(event); };
    std::string monitorError;
    if (!monitor_->Start(monitorOptions, callbacks, &monitorError)) {
        SetStatus("用户输入急停暂时不可用（" + monitorError + "）。", true);
    }

    // ---- 主窗口 ----
    mainWindow_ = new MainWindow(*this);
    if (!mainWindow_->Create(instance, error)) {
        return false;
    }
    ShowWindow(mainWindow_->Handle(), SW_SHOW);
    UpdateWindow(mainWindow_->Handle());
    // 启动就把曲谱列表填好（含启动时载入的那首），并让音符表显示当前曲谱
    mainWindow_->RefreshSongList();
    mainWindow_->RebuildNoteList();

    // ---- 前台窗口变化监听（四十二节）----
    foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                      &App::ForegroundProc, 0, 0, WINEVENT_OUTOFCONTEXT);

    // ---- 设置先应用一次（速度、监控开关、自然模式）----
    const AppSettings initial = settings_;
    const std::string applied = ApplySettings(initial);
    if (!startupNotice_.empty()) {
        // 曲谱那句实话放回状态行（ApplySettings 的"设置已生效"让位）
        SetStatus(startupNotice_, startupNoticeError_);
    } else if (statusText_.empty()) {
        // 启动总会留下一句实话（已载入上次曲谱 / 还没有曲谱 / 上次那首打不开了），
        // 这里只在真的什么都没说时才兜底显示"就绪"，别再拿"就绪"盖掉失败原因。
        SetStatus(labels::kReady, false);
    }
    (void)applied;

    // Phase 12 补：权限提示已挪到上面的"管理员权限"分支——那里的提示能直接告诉用户点哪儿，
    // 不用再去右键菜单里找"以管理员身份运行"。
    return true;
}

int App::Run() {
    MSG message{};
    int result = 0;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (mainWindow_ != nullptr && message.hwnd == mainWindow_->Handle()) {
            // 主窗口的快捷键（回车/空格等交给控件自己处理，这里不做额外拦截）
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void App::SetSong(const Song& song, const std::string& source) {
    // 换曲等于换谱：先停下并交还控制权（二十三节）
    if (player_->IsPlaying() || player_->IsPaused()) {
        player_->Stop(StopReason::UserStop);
    }
    song_ = song;
    hasSong_ = true;
    RebuildTimeline();
    if (!source.empty()) {
        settings_.songPath = source;
        currentSongPath_ = source;
    }
    if (mainWindow_ != nullptr) {
        mainWindow_->RebuildNoteList();
        mainWindow_->Refresh();
    }
}

void App::RebuildTimeline() {
    TimelineOptions options;
    options.naturalMode = settings_.naturalMode;
    options.jitterMs = settings_.jitterMs;
    std::vector<std::string> problems;
    timeline_ = Timeline::Build(song_, options, &problems);
    ++timelineGeneration_;
    if (!problems.empty()) {
        SetStatus("曲谱有 " + std::to_string(problems.size()) + " 个问题：" + problems.front(), true);
    }
}

void App::SetStatus(const std::string& text, bool error) {
    statusText_ = text;
    statusError_ = error;
    if (error) {
        LogError(text);   // 弹给用户的错误也落一份到 debug.log
    }
    if (mainWindow_ != nullptr) {
        mainWindow_->Refresh();
    }
}

void App::PostToMain(UINT message, WPARAM wparam, LPARAM lparam) {
    if (mainWindow_ != nullptr && mainWindow_->Handle() != nullptr) {
        PostMessageW(mainWindow_->Handle(), message, wparam, lparam);
    }
}

void App::OnHotkey(const HotkeyEvent& event) {
    // **热键线程**上执行：只投递意图，绝不碰控件（需求书三十八节）
    PostToMain(kMsgHotkeyAction, static_cast<WPARAM>(event.action));
}

void App::OnUserInput(const UserInputEvent& event) {
    // **钩子线程**上执行：只做急停（播放器内部无等待），界面刷新交给 UI 线程（十九节）
    bool interrupted = false;
    if (player_->IsPlaying() || player_->IsPaused()) {
        player_->EmergencyStop(event.name);
        interrupted = true;
    }
    // 没在演奏时按键是用户自己的事（他可能只是在打字/玩游戏）：不要弹"演奏被用户输入打断"。
    // 只有真打断了正在进行的演奏才通知界面。
    if (interrupted) {
        PostToMain(kMsgUserInput);
    }
}

void App::SetHotkeyCaptureActive(bool active) {
    // 放行模式没有 RegisterHotKey 可挂起，靠这个标志拦住（录键时按下的键不认成热键）
    hotkeyCaptureSuspended_.store(active);
    if (hotkeys_ != nullptr) {
        hotkeys_->SetSuspended(active);
    }
}

unsigned App::CurrentModifiers() const {
    unsigned modifiers = kModifierNone;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {
        modifiers |= kModifierCtrl;
    }
    if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
        modifiers |= kModifierAlt;
    }
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
        modifiers |= kModifierShift;
    }
    if (((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0) {
        modifiers |= kModifierWin;
    }
    return modifiers;
}

HotkeyBinding App::BindingFor(HotkeyAction action) const {
    if (hotkeys_ != nullptr && hotkeys_->IsRunning()) {
        return hotkeys_->GetHotkey(action);   // 独占模式：以真正注册的为准
    }
    const int index = static_cast<int>(action);
    if (index >= 0 && index < kHotkeyActionCount) {
        return ownBindings_[index];
    }
    return HotkeyBinding();
}

void App::OnHookKey(const UserInputEvent& event) {
    // 跑在钩子线程上：只允许"匹配 + PostMessage"。加锁/IO/等待都会超时丢钩子。
    if (!hookHotkeysActive_.load() || hotkeyCaptureSuspended_.load()) {
        return;   // 独占模式走 RegisterHotKey；录键中不认热键
    }
    if (event.kind != UserInputKind::KeyDown || event.vk == 0) {
        return;
    }
    MainWindow* window = mainWindow_;
    if (window == nullptr || hotkeys_ == nullptr) {
        return;   // 主窗口还没建好（启动极早期）
    }
    const unsigned modifiers = CurrentModifiers();
    for (int index = 0; index < kHotkeyActionCount; ++index) {
        const HotkeyAction action = static_cast<HotkeyAction>(index);
        const HotkeyBinding binding = BindingFor(action);
        if (!binding.IsEmpty() && binding.vk == event.vk && binding.modifiers == modifiers) {
            // 和 RegisterHotKey 那条路走同一个消息，行为完全一致（含切歌时的屏幕提示）
            PostMessageW(window->Handle(), kMsgHotkeyAction, static_cast<WPARAM>(action), 0);
            return;
        }
    }
}

bool App::ShouldIgnoreUserInput(const UserInputEvent& event) {
    // 十八节：热键键不算"用户输入打断演奏"，得让它发挥热键作用。
    // Phase 12 把范围从"只有启动热键"扩到"任何一把绑定的热键"（F7 停止 / F8 重播 /
    // F9 上一首 / F10 下一首 同样不算）：否则按 F8 会先被急停掐一下再重播，
    // 演奏中按 F9 换曲也会先急停——行为看着对，其实是两股力量在抢发送锁，不能这么留。
    if (event.vk == 0) {
        return false;
    }
    // 热键（放行模式下是自己存的那份）不算"用户输入"，否则按一下换歌就急停了
    for (int index = 0; index < kHotkeyActionCount; ++index) {
        if (ownBindings_[index].vk != 0 && ownBindings_[index].vk == event.vk) {
            return true;
        }
    }
    if (hotkeys_ != nullptr && hotkeys_->IsBoundKey(event.vk)) {
        return true;
    }
    // Phase 13：用户自己加的"排除键位"（游戏里的走路 / 跳跃 / 切枪键）同样不算打断演奏。
    // 热键键已经在上一步挡掉了，这里只管用户手工加进列表的那些键。
    // 无锁快照：钩子线程只做一次原子读，进不了锁，也不会读到 UI 线程正在改的 vector。
    const std::shared_ptr<const std::vector<unsigned>> ignored = std::atomic_load(&ignoredVks_);
    return ignored != nullptr && IsIgnoredKey(event.vk, *ignored);
}

void App::ForegroundProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild,
                         DWORD thread, DWORD time) {
    (void)hook;
    (void)event;
    (void)idObject;
    (void)idChild;
    (void)thread;
    (void)time;
    if (g_app != nullptr) {
        g_app->OnForegroundWindow(hwnd);
    }
}

void App::OnForegroundWindow(HWND window) {
    if (suppressForeground_ || window == nullptr) {
        return;
    }
    // 自己人的窗口（本进程：主窗口、编辑窗口、设置窗口、文件对话框）不算"切走"
    if (IsOwnProcessWindow(window)) {
        return;
    }
    // 切回"开始演奏时所在的那个窗口"（游戏）：按承诺自动继续
    if (window == playbackForeground_) {
        if (pausedByForeground_ && settings_.foregroundPolicy == ForegroundPolicy::Pause &&
            player_->IsPaused()) {
            player_->Resume();
            pausedByForeground_ = false;
            SetStatus("回到原来那个窗口，继续演奏。", false);
            PostToMain(kMsgForeground);
        }
        return;
    }
    // 从界面点"播放"时前台是本程序自己，目标窗口还没定：第一次切到别的窗口就当目标，不要一上来就暂停
    if (player_->IsPlaying() && !pausedByForeground_ && IsOwnProcessWindow(playbackForeground_)) {
        playbackForeground_ = window;
        PostToMain(kMsgForeground);
        return;
    }
    ApplyForegroundPolicy(window);
}

void App::ApplyForegroundPolicy(HWND window) {
    // 只有正在演奏/暂停时才有意义
    if (!player_->IsPlaying() && !player_->IsPaused()) {
        return;
    }
    switch (settings_.foregroundPolicy) {
        case ForegroundPolicy::Ignore:
            return;
        case ForegroundPolicy::Stop: {
            wchar_t title[256] = L"";
            GetWindowTextW(window, title, static_cast<int>(sizeof(title) / sizeof(title[0])));
            player_->Stop(StopReason::WindowChanged);
            SetStatus("前台窗口变化，已停止演奏。", true);
            pausedByForeground_ = false;
            break;
        }
        case ForegroundPolicy::Pause:
        default:
            player_->Pause();
            pausedByForeground_ = true;
            SetStatus("前台窗口变化，已暂停（切回游戏窗口会自动继续）。", false);
            break;
    }
    PostToMain(kMsgForeground);
}

void App::CommandPlay() {
    if (!hasSong_ || timeline_.NoteCount() == 0) {
        SetStatus("还没有可演奏的曲谱：先打开或编辑一首。", true);
        return;
    }
    if (player_->IsPlaying()) {
        SetStatus("已经在演奏了。", false);
        return;
    }
    if (player_->IsPaused()) {
        player_->Resume();
        pausedByForeground_ = false;   // 用户手动接手，不再是"卡在前台变化上"
        SetStatus("继续演奏。", false);
        return;
    }
    playbackForeground_ = GetForegroundWindow();
    pausedByForeground_ = false;
    if (player_->Play(timeline_)) {
        SetStatus("开始演奏：" + song_.name + "　速度 " + FormatSpeedText(player_->Speed()) +
                      (player_->DryRun() ? "　干跑（不注入按键）" : ""),
                  false);
    } else {
        SetStatus("播放器没有接受这次播放请求。", true);
    }
}

void App::CommandPauseOrResume() {
    if (player_->IsPlaying()) {
        player_->Pause();
        SetStatus("已暂停（再按一次继续）。", false);
    } else if (player_->IsPaused()) {
        player_->Resume();
        pausedByForeground_ = false;
        SetStatus("继续演奏。", false);
    } else {
        SetStatus("当前没有在演奏。", false);
    }
}

void App::CommandStop() {
    if (player_->IsPlaying() || player_->IsPaused()) {
        player_->Stop(StopReason::UserStop);
        SetStatus("已停止，位置已归零。", false);
    } else {
        SetStatus("当前没有在演奏。", false);
    }
}

void App::CommandRestart() {
    if (!hasSong_ || timeline_.NoteCount() == 0) {
        SetStatus("还没有可演奏的曲谱。", true);
        return;
    }
    if (player_->IsStopped()) {
        CommandPlay();
        return;
    }
    player_->Restart();
    SetStatus("从头重播。", false);
}

bool App::OpenSongFile(const std::string& path, std::string* message) {
    Song loaded;
    const SongIoResult result = LoadSongFromJson(path, loaded);
    if (!result.ok) {
        if (message != nullptr) {
            *message = result.error;
        }
        return false;
    }
    SetSong(loaded, path);
    PersistSongPath();   // 记住这首：下次启动载入它
    std::string text = "已打开：" + song_.name + "（" + std::to_string(timeline_.NoteCount()) +
                       " 个音符，" + FormatClock(timeline_.TotalDuration()) + "）";
    if (result.HasWarnings()) {
        text += "　注意：" + result.warnings.front();
    }
    if (message != nullptr) {
        *message = text;
    }
    SetStatus(text, result.HasWarnings());
    return true;
}

const std::vector<std::string>& App::RefreshSongList() {
    songFiles_.clear();
    const std::string directory = DefaultSongDirectory();
    if (!directory.empty()) {
        std::error_code code;
        // 路径按 UTF-8 转（窄字符串直接喂 std::filesystem 会在中文路径上抛异常）
        const std::filesystem::path root = std::filesystem::u8path(directory);
        if (std::filesystem::is_directory(root, code)) {
            std::filesystem::directory_iterator it(root, code);
            const std::filesystem::directory_iterator end;
            while (!code && it != end) {
                std::error_code fileCode;
                if (it->is_regular_file(fileCode)) {
                    const std::string name = it->path().filename().u8string();
                    if (name.size() > 5) {
                        std::string tail = name.substr(name.size() - 5);
                        for (char& ch : tail) {
                            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        }
                        if (tail == ".json") {
                            songFiles_.push_back(it->path().u8string());
                        }
                    }
                }
                it.increment(code);
            }
        }
    }
    std::sort(songFiles_.begin(), songFiles_.end(),
              [](const std::string& a, const std::string& b) {
                  return FileNameText(a) < FileNameText(b);
              });
    return songFiles_;
}

void App::SwitchSong(int delta) {
    const std::vector<std::string>& files = RefreshSongList();
    if (files.empty()) {
        SetStatus("songs 文件夹里还没有曲谱（" + DefaultSongDirectory() +
                      "）：先编辑一首并保存，列表里就会出现它。",
                  true);
        return;
    }
    const std::string current = currentSongPath_;
    int index = -1;
    for (size_t i = 0; i < files.size(); ++i) {
        if (SamePathText(files[i], current)) {
            index = static_cast<int>(i);
            break;
        }
    }
    const int count = static_cast<int>(files.size());
    int target = 0;
    if (index < 0) {
        target = delta >= 0 ? 0 : count - 1;   // 当前这首不在列表里：下一首=第一首，上一首=最后一首
    } else {
        target = ((index + delta) % count + count) % count;   // 首尾循环
    }
    const std::string name = FileNameText(files[target]);
    const bool wasPlaying = player_->IsPlaying() || player_->IsPaused();
    std::string message;
    if (!OpenSongFile(files[target], &message)) {
        SetStatus("换曲失败（" + name + "）：" + message, true);
        return;
    }
    if (wasPlaying) {
        // 演奏中换曲：换完接着往下演，中间不经过急停、不停顿
        CommandPlay();
        SetStatus("已切到 " + name + "，接着演奏（" + std::to_string(timeline_.NoteCount()) +
                      " 个音符）",
                  false);
    } else {
        SetStatus("已切到 " + name + "（" + std::to_string(timeline_.NoteCount()) +
                      " 个音符，" + FormatClock(timeline_.TotalDuration()) + "）。",
                  false);
    }
}

void App::CommandPreviousSong() {
    SwitchSong(-1);
    ShowSongOsd();
}

void App::CommandNextSong() {
    SwitchSong(1);
    ShowSongOsd();
}

void App::ShowSongOsd() {
    if (osd_ == nullptr) {
        return;
    }
    if (!hasSong_ || song_.name.empty()) {
        return;   // 没有曲谱可提示（状态行里已经说清楚了），别在游戏里弹一句没用的
    }
    osd_->ShowText(std::string(labels::kSongPrefix) + song_.name);
}

void App::CommandOpenSong() {
    DialogGuard guard(suppressForeground_);
    const std::string path = OpenSongDialog(mainWindow_ != nullptr ? mainWindow_->Handle() : nullptr);
    if (path.empty()) {
        return;   // 用户取消了，什么也不做（不改状态、不打断演奏）
    }
    OpenSongFile(path, nullptr);
}

void App::EditCurrentSong() {
    if (!hasSong_) {
        // 新建已经独立成常驻按钮了：没有曲谱时这里如实说清楚该去哪
        SetStatus(std::string("还没有曲谱可以编辑：先点「") + labels::kNewSong +
                      "」建一首，或从上面的曲谱列表里选一首。",
                  true);
        return;
    }
    // 编辑曲谱和新建曲谱**共用同一个窗口**（点选式加音符）：打开时把现有曲谱装进去，
    // 保存时覆盖原文件（用户要求：编辑界面改成和新建曲谱一样）
    if (newSongWindow_ == nullptr) {
        newSongWindow_ = new NewSongWindow(*this);
        std::string error;
        if (!newSongWindow_->Create(instance_, &error)) {
            delete newSongWindow_;
            newSongWindow_ = nullptr;
            SetStatus("打开编辑窗口失败：" + error, true);
            return;
        }
    }
    newSongWindow_->LoadSongForEdit(song_, currentSongPath_);
    newSongWindow_->Show();
}

void App::PersistSongPath() {
    // 只在真换了曲谱时写一次小 JSON；失败不打扰用户（写不进配置不该拦着人家演奏），
    // 但要留日志痕迹，免得"下次启动没载入上次曲谱"变成查不出的怪事。
    if (settingsPath_.empty()) {
        return;
    }
    std::string error;
    if (!SaveSettingsNow(&error)) {
        LogWarn("记不住“上次打开的曲谱”：" + error);
    }
}

bool App::SaveSettingsNow(std::string* error) {
    std::string message;
    const bool ok = SaveSettings(settings_, settingsPath_, message);
    if (!ok && error != nullptr) {
        *error = message;
    }
    return ok;
}

void App::CommandNewSong() {
    if (newSongWindow_ == nullptr) {
        newSongWindow_ = new NewSongWindow(*this);
        std::string error;
        if (!newSongWindow_->Create(instance_, &error)) {
            delete newSongWindow_;
            newSongWindow_ = nullptr;
            SetStatus("打开新建曲谱窗口失败：" + error, true);
            return;
        }
        newSongWindow_->Reset();   // 打开就是一张白纸（曲名"新曲谱"、120 BPM）
    } else if (newSongWindow_->EditingExistingSong()) {
        // 刚才在用它编辑别的曲谱：换成一张白纸（那个曲谱文件本身不受影响）
        newSongWindow_->Reset();
        SetStatus("换成了空白新曲谱（刚才编辑的曲谱文件没有改动）。", false);
    } else if (newSongWindow_->HasNotes()) {
        // 常驻窗口里可能还留着没保存的音符：不擅自清掉，只说清楚想从头开始该点哪
        SetStatus("新建曲谱窗口还开着（里面有 " + std::to_string(newSongWindow_->NoteCount()) +
                      " 个音符没保存）：想从头开始就点「全部清空」，想接着加就继续点音调按钮。",
                  false);
    }
    newSongWindow_->Show();
}

bool App::SaveSongAsNew(const Song& song, std::string* message) {
    if (message != nullptr) {
        message->clear();
    }
    DialogGuard guard(suppressForeground_);
    const std::string path =
        SaveSongDialog(mainWindow_ != nullptr ? mainWindow_->Handle() : nullptr, song.name);
    if (path.empty()) {
        return false;   // 用户取消：message 留空，调用方据此给"你取消了"的提示
    }
    return SaveSongInPlace(song, path, message);
}

bool App::SaveSongInPlace(const Song& song, const std::string& path, std::string* message) {
    if (message != nullptr) {
        message->clear();
    }
    if (path.empty()) {
        if (message != nullptr) {
            *message = "没有可写入的文件名。";
        }
        return false;
    }
    const SongIoResult result = SaveSongToJson(song, path);
    if (!result.ok) {
        if (message != nullptr) {
            *message = "保存失败：" + result.error;
        }
        return false;
    }
    // 存下来的就是"当前曲谱"：主界面、曲谱列表、下次启动都用它
    SetSong(song, path);
    settings_.songPath = path;
    currentSongPath_ = path;
    PersistSongPath();
    RefreshSongList();
    if (mainWindow_ != nullptr) {
        mainWindow_->RefreshSongList();
        mainWindow_->Refresh();
    }
    if (message != nullptr) {
        *message = path;
    }
    return true;
}

void App::ShowSettings() {
    if (settingsWindow_ == nullptr) {
        settingsWindow_ = new SettingsWindow(*this);
        std::string error;
        if (!settingsWindow_->Create(instance_, &error)) {
            delete settingsWindow_;
            settingsWindow_ = nullptr;
            SetStatus("打开设置窗口失败：" + error, true);
            return;
        }
    }
    settingsWindow_->LoadSettings(settings_);
    if (settingsWindow_ != nullptr && mainWindow_ != nullptr && mainWindow_->Handle() != nullptr) {
        RECT mainClient{};
        GetClientRect(mainWindow_->Handle(), &mainClient);
        // 与主页面等高：设置页客户区和主页面一样高，多出来的选项靠滚动看
        settingsWindow_->SetPageClientHeight(mainClient.bottom - mainClient.top);
    }
    settingsWindow_->Show();
}

void App::NotifySongEdited(const Song& song) {
    SetSong(song, std::string());
    if (mainWindow_ != nullptr) {
        mainWindow_->Refresh();
    }
}

std::string App::ApplySettings(const AppSettings& settings) {
    std::vector<std::string> problems;
    std::vector<std::string> warnings;

    // ---- 热键：变了才重新注册（注册失败会回滚，Phase 5）----
    struct Rebind {
        HotkeyAction action;
        std::string text;
    };
    const Rebind rebinds[] = {
        {HotkeyAction::Play, settings.playHotkey},
        {HotkeyAction::Pause, settings.pauseHotkey},
        {HotkeyAction::Stop, settings.stopHotkey},
        {HotkeyAction::Restart, settings.restartHotkey},
        {HotkeyAction::Previous, settings.previousHotkey},
        {HotkeyAction::Next, settings.nextHotkey},
    };
    if (hotkeys_ != nullptr) {
        for (const Rebind& rebind : rebinds) {
            const HotkeyBinding current = hotkeys_->GetHotkey(rebind.action);
            HotkeyBinding wanted;
            std::string error;
            if (!ParseHotkey(rebind.text, wanted, &error)) {
                problems.push_back(std::string(HotkeyActionDisplayName(rebind.action)) +
                                   " 热键： " + error);
                continue;
            }
            if (wanted.Equals(current)) {
                continue;
            }
            const HotkeyChangeResult result = hotkeys_->SetHotkey(rebind.action, wanted);
            if (!result.ok) {
                problems.push_back(std::string(HotkeyActionDisplayName(rebind.action)) +
                                   " 热键： " + result.error);
            } else if (!result.warning.empty()) {
                warnings.push_back(result.warning);
            }
        }
    }

    // ---- 排除键位：解析成键码集合（这些键按下时不打断演奏）----
    // 先构造好整份新表，最后一次性原子发布：钩子线程要么看到旧的完整表、要么看到新的完整表，
    // 绝不会读到被 clear/重分配过的半成品（原来的写法是数据竞争，急停可能因此失效）。
    std::vector<unsigned> ignoredKeys;
    for (const std::string& text : settings.ignoredKeys) {
        unsigned vk = 0;
        std::string ignoreError;
        if (!ParseIgnoredKey(text, vk, &ignoreError)) {
            problems.push_back(std::string("排除键「") + text + "」：" + ignoreError);
            continue;
        }
        ignoredKeys.push_back(vk);
    }
    std::atomic_store(&ignoredVks_,
                      std::make_shared<const std::vector<unsigned>>(std::move(ignoredKeys)));

    // ---- 用户输入急停的三项开关 ----
    if (monitor_ != nullptr) {
        InputMonitorOptions options;
        options.keyboard = settings.monitorKeyboard;
        options.mouseButtons = settings.monitorMouseButtons;
        options.mouseMove = settings.monitorMouseMove;
        monitor_->SetOptions(options);
    }

    // ---- 热键模式（放行 / 独占）----
    // 放行：不注册全局热键，键留给游戏，靠钩子认（需要提权）
    // 独占：RegisterHotKey，键被工具接走
    hookHotkeysActive_.store(!settings.exclusiveHotkeys);
    if (hotkeys_ != nullptr) {
        const bool running = hotkeys_->IsRunning();
        if (settings.exclusiveHotkeys && !running) {
            std::string startError;
            if (!hotkeys_->Start([this](const HotkeyEvent& event) { OnHotkey(event); }, &startError)) {
                problems.push_back("热键独占没启用成功：" + startError);
            }
        } else if (!settings.exclusiveHotkeys && running) {
            hotkeys_->Stop();
        }
    }

    // ---- 绑定表自己存一份（放行模式没有注册表可查）----
    // 顺序必须跟 HotkeyAction 一致：Play/Stop/Restart/Previous/Next/Pause
    for (int index = 0; index < kHotkeyActionCount; ++index) {
        ownBindings_[index] = HotkeyBinding();
    }
    const std::string texts[kHotkeyActionCount] = {
        settings.playHotkey,  settings.stopHotkey, settings.restartHotkey,
        settings.previousHotkey, settings.nextHotkey, settings.pauseHotkey};
    for (int index = 0; index < kHotkeyActionCount; ++index) {
        HotkeyBinding parsed;
        if (ParseHotkey(texts[index], parsed, nullptr)) {
            ownBindings_[index] = parsed;
        }
    }

    // ---- 播放速度与干跑 ----
    // 速度一律过一遍 BeatMath 的范围校验（50% ~ 200%），不让任何调用方绕过去
    double speed = settings.speed;
    std::string speedError;
    if (!beat::ClampSpeed(settings.speed, speed, speedError)) {
        problems.push_back(speedError);
    }
    if (player_ != nullptr) {
        player_->SetSpeed(speed);
        player_->SetDryRun(settings.dryRun);
    }

    // ---- 自然模式（时间轴选项）：变了就重建时间轴 ----
    const bool timelineChanged = (settings.naturalMode != settings_.naturalMode) ||
                                 (settings.jitterMs != settings_.jitterMs);
    settings_ = settings;
    settings_.speed = speed;   // 存的是校验后的值
    if (timelineChanged) {
        RebuildTimeline();
    }

    std::string text;
    if (!problems.empty()) {
        text = "设置没全部生效：" + JoinText(problems, "；");
        SetStatus(text, true);
    } else {
        if (!warnings.empty()) {
            text = "设置已生效，但注意：" + JoinText(warnings, "；");
        } else {
            text = "设置已生效：" + settings_.Summary();
        }
        SetStatus(text, false);
        // Phase 11 配置保存：设置生效即写盘（失败只在状态行提示，不影响播放）。
        // Phase 13 修正：带"提醒"（例如热键抢走字母键）的这次应用**也要写盘**——
        // 否则提示说"已生效"，用户重启后发现改动没了，等于骗人。
        std::string saveError;
        if (!SaveSettingsNow(&saveError)) {
            text += "（配置没能保存：" + saveError + "）";
            SetStatus(text, true);
        }
    }
    if (mainWindow_ != nullptr) {
        mainWindow_->Refresh();
    }
    return text;
}

std::string App::SongTitleText() const {
    if (!hasSong_) {
        return std::string(labels::kSongPrefix) + labels::kNoSong;
    }
    return std::string(labels::kSongPrefix) + song_.name;
}

std::string App::SongSubtitleText() const {
    if (!hasSong_) {
        return std::string();   // 空曲谱时标题那行已经写着"没有曲谱…"，副标题留空，别重复同一句话
    }
    std::string text = "（" + std::to_string(timeline_.NoteCount()) + " 个音符）";
    if (!currentSongPath_.empty()) {
        // 只给文件名：全路径太长，标题下面这行放不下，而且真正有用的信息是"哪一首"
        const size_t slash = currentSongPath_.find_last_of("/\\");
        text += "　文件：" + (slash == std::string::npos ? currentSongPath_
                                                       : currentSongPath_.substr(slash + 1));
    }
    return text;
}

std::string App::StateText() const {
    return StatusLine(player_->State(), player_->LastStopReason(), player_->LastStopDetail());
}

std::string App::CurrentNoteText() const {
    return ui::CurrentNoteText(player_->FiredNoteCount(), timeline_.NoteCount());
}

std::string App::TimeText() const {
    return FormatClock(player_->PositionSeconds()) + " / " + FormatClock(player_->TotalDurationSeconds());
}

std::string App::InfoText() const {
    std::string text = "BPM：" + FormatBeats(song_.bpm);
    text += "　速度：" + FormatSpeedText(player_->Speed());
    text += "　模式：" + std::string(settings_.naturalMode
                                     ? ("自然（抖动 " + FormatBeats(settings_.jitterMs) + " ms）")
                                     : "精确");
    if (settings_.dryRun) {
        text += "　干跑";
    }
    return text;
}

std::string App::TempoText() const {
    return ui::TempoText(song_);
}

std::string App::SignatureText() const {
    return ui::SignatureLine(song_);
}

std::string App::BarText() const {
    // 当前小节由“已播到第几秒”换算：拍号决定一小节多少拍（Phase 8）
    const double position = player_ != nullptr ? player_->PositionSeconds() : 0.0;
    const int bar = hasSong_ ? beat::LocateBar(song_, position).bar : 0;
    const int total = hasSong_ ? beat::BarCount(song_) : 0;
    return ui::BarText(bar, total);
}

std::string App::ModeText() const {
    return ui::ModeText(settings_.naturalMode, settings_.jitterMs);
}

double App::Speed() const {
    return player_ != nullptr ? player_->Speed() : settings_.speed;
}

int App::SpeedPresetIndex() const {
    return ui::SpeedPresetIndexFor(Speed());
}

bool App::NaturalMode() const {
    return settings_.naturalMode;
}

void App::CommandSetSpeed(double speed) {
    double checked = speed;
    std::string error;
    if (!beat::ClampSpeed(speed, checked, error)) {
        SetStatus(error, true);
        return;
    }
    settings_.speed = checked;
    if (player_ != nullptr) {
        player_->SetSpeed(checked);   // 只改时间映射，不动已发的音符（Phase 3）
    }
    SetStatus("速度已改为 " + ui::FormatSpeedText(checked) +
                  "（有效 BPM " + ui::FormatBeats(beat::EffectiveBpm(song_, checked)) + "）。",
              false);
}

void App::CommandSetNaturalMode(bool natural) {
    if (settings_.naturalMode == natural) {
        return;
    }
    settings_.naturalMode = natural;
    RebuildTimeline();   // 抖动写在时间轴里，所以要重建；正在演奏的那一遍不受影响
    SetStatus(natural ? "已切到自然模式（每个音符 ±3 ms 以内，不累积），下次播放生效。"
                      : "已切到精确模式，下次播放生效。",
              false);
}

std::string App::HotkeyHintText() const {
    std::string play = "F6";
    std::string stop = "F7";
    std::string restart = "F8";
    if (hotkeys_ != nullptr) {
        play = BindingFor(HotkeyAction::Play).ToString();
        stop = BindingFor(HotkeyAction::Stop).ToString();
        restart = BindingFor(HotkeyAction::Restart).ToString();
    }
    std::string previous = settings_.previousHotkey;
    std::string next = settings_.nextHotkey;
    std::string pause = settings_.pauseHotkey;
    if (hotkeys_ != nullptr) {
        previous = BindingFor(HotkeyAction::Previous).ToString();
        next = BindingFor(HotkeyAction::Next).ToString();
        pause = BindingFor(HotkeyAction::Pause).ToString();
    }
    return HotkeyHint(play, pause, stop, restart, previous, next);
}

int App::ProgressPercent() const {
    return PercentOf(player_->FiredNoteCount(), timeline_.NoteCount());
}

int App::CurrentNoteIndex() const {
    const int fired = player_->FiredNoteCount();
    if (fired <= 0) {
        return -1;
    }
    return std::min(fired - 1, std::max(0, timeline_.NoteCount() - 1));
}

}  // namespace ui
}  // namespace hp
