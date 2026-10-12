#pragma once

// 应用程序对象（Phase 7）。
//
// 需求书三十八节的线程归属：
//   UI 线程          —— 窗口、设置、曲谱编辑、状态显示（本类的方法都在 UI 线程上跑）
//   播放线程          —— Timeline / PlaybackClock / InputController（SongPlayer 内部）
//   Input Monitor 线程 —— WH_KEYBOARD_LL → EmergencyStop（Phase 6）
//   热键线程          —— RegisterHotKey 回调（Phase 5）
//
// 三条硬规则（不许破坏）：
//   1. 热键回调与钩子回调**跑在别的线程上**：里面只许改播放器意图 + PostMessage 给窗口，
//      绝不许直接碰控件。
//   2. 所有命令（按钮、热键、菜单）都走本类同一个入口，行为只有一份。
//   3. 退出前必须 Stop 监控、Stop 热键、ReleaseAll（异常安全，四十二节）。

#include <atomic>
#include <memory>
#include <string>

#include "core/Platform.h"
#include "hotkey/GlobalHotkeyManager.h"
#include "input/InputController.h"
#include "monitor/InputMonitor.h"
#include "player/SongPlayer.h"
#include "song/Song.h"
#include "song/Timeline.h"
#include "ui/UiModel.h"   // AppSettings / EditorRow / 界面文案
#include "ui/OsdWindow.h"
#include "ui/UiTheme.h"

namespace hp {
namespace ui {

class MainWindow;
class NewSongWindow;
class SettingsWindow;

// 主窗口自定义消息：别的线程只许用 PostMessage 把"请求"丢过来
constexpr UINT kMsgHotkeyAction = WM_APP + 1;   // wParam = HotkeyAction
constexpr UINT kMsgUserInput = WM_APP + 2;      // 钩子线程：已经急停了，请刷新界面
constexpr UINT kMsgForeground = WM_APP + 3;     // 前台窗口变化（四十二节）

class App {
public:
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    bool Initialize(HINSTANCE instance, std::string* error);
    int Run();

    // 以管理员身份重启（游戏以管理员身份运行时，普通权限的 SendInput 会被系统 UIPI 丢掉）：
    // 成功则本实例马上退出、由提权后的新实例接着跑；失败把中文原因写进 *message
    bool RequestElevation(std::string* message);
    // 设置窗口点“立即以管理员身份重启”走这条：提权成功后要用**和启动时同一套退出流程**
    // 结束本实例（置退出标志 + 给主窗口投递关闭消息）。只调 RequestElevation 不退出，
    // 旧实例会继续带着钩子 / 热键活着，于是两个实例同时响应按键、抢注入。
    bool RequestElevationAndQuit(std::string* message);
    // Initialize 是否因为"正在提权重启"而提前返回（主程序据此安静退出）
    bool ElevationRelaunchRequested() const { return elevationRelaunching_; }
    // 本进程当前是不是管理员权限
    bool IsElevated() const;

    HINSTANCE Instance() const { return instance_; }

    SongPlayer& Player() { return *player_; }
    InputMonitor& Monitor() { return *monitor_; }
    GlobalHotkeyManager& Hotkeys() { return *hotkeys_; }
    InputController& Input() { return *input_; }

    const Song& CurrentSong() const { return song_; }
    const Timeline& CurrentTimeline() const { return timeline_; }
    bool HasSong() const { return hasSong_; }
    // 曲谱被换掉（载入/编辑）：重建时间轴；正在演奏就停下来（换谱等于换曲）
    void SetSong(const Song& song, const std::string& source);
    std::vector<std::string> songFiles_;   // 最近一次扫描到的 songs 里的曲谱
    std::string currentSongPath_;          // 当前载入的曲谱文件（空 = 没有曲谱）
    // 启动时关于"当前曲谱"的那句实话（启动流程后面 ApplySettings 会改状态行，
    // 这里记下来在最后再放回去，免得用户看到的是"设置已生效"而不是"还没有曲谱/已载入上次曲谱"）
    std::string startupNotice_;
    bool startupNoticeError_ = false;

    const AppSettings& Settings() const { return settings_; }

    // 配置保存（Phase 11）：默认落在 %APPDATA%\HarmonicaPlayer\settings.json
    const std::string& SettingsPath() const { return settingsPath_; }
    bool SaveSettingsNow(std::string* error = nullptr);
    // 把"当前曲谱"立刻落盘：下次启动要按它载入（换曲/存曲后调，不等退出）
    void PersistSongPath();
    // 应用设置（热键重绑、监控开关、速度、干跑、自然模式、前台策略）；返回可显示的结果文本
    std::string ApplySettings(const AppSettings& settings);

    // 设置窗口正在录热键：把全局热键临时让出去（否则按 F6 会被系统直接喂给老绑定）
    void SetHotkeyCaptureActive(bool active);

    // ---- 命令（按钮 / 热键 / 菜单共用） ----
    void CommandPlay();
    void CommandPauseOrResume();
    void CommandStop();
    void CommandRestart();
    void CommandOpenSong();
    // Phase 12：上/下一首（热键 F9/F10 + 主界面曲谱列表共用同一条路径）
    void CommandPreviousSong();
    void CommandNextSong();
    // 切歌后在屏幕上提示"当前歌曲：xxx"（全屏游戏里也能看到）
    void ShowSongOsd();
    void SwitchSong(int delta);   // delta = -1 上一首 / +1 下一首（首尾循环）
    // 按路径载入曲谱（打开对话框、曲谱列表、换曲热键、启动载入上次曲谱都走它）
    bool OpenSongFile(const std::string& path, std::string* message);
    // 扫 <程序目录>\songs\*.json，返回排序后的完整路径列表（每次都重新扫盘）
    const std::vector<std::string>& RefreshSongList();
    const std::vector<std::string>& SongFiles() const { return songFiles_; }
    // 真正载入成功的那首曲谱的路径（配置里"上次打开的"可能已经失效，不能当当前曲谱用）
    const std::string& CurrentSongPath() const { return currentSongPath_; }
    void CommandNewSong();                        // 新建曲谱（常驻：任何时候都能开）
    // 新窗口点"保存"走到这里：弹另存为、写盘、设成当前曲谱、刷新列表；成功返回 true
    bool SaveSongAsNew(const Song& song, std::string* message);
    // 存到指定路径（覆盖）：编辑已有曲谱时用（没有文件对话框）
    bool SaveSongInPlace(const Song& song, const std::string& path, std::string* message);
    // （CommandSaveSongAs 已并入 SaveSongAsNew：新建窗口直接保存，主界面不再需要单独的另存为入口）
    // Phase 8：主界面上的速度 / 模式下拉（需求书二十九、三十节）
    void CommandSetSpeed(double speed);
    void CommandSetNaturalMode(bool natural);

    // 编辑当前曲谱：用「新建曲谱」同款点选式窗口打开，保存时覆盖原文件（两个界面统一）
    void EditCurrentSong();
    void ShowSettings();

    // ---- 给窗口用的状态 ----
    MainWindow* Main() const { return mainWindow_; }
    void SetMainWindow(MainWindow* window) { mainWindow_ = window; }
    void PostToMain(UINT message, WPARAM wparam = 0, LPARAM lparam = 0);

    void SetStatus(const std::string& text, bool error = false);
    const std::string& StatusText() const { return statusText_; }
    bool StatusIsError() const { return statusError_; }

    // 状态区要显示的一整块（都由 ui:: 的纯函数算出来）
    std::string SongTitleText() const;
    // 标题下面那行小字：音符数 / 文件名（或"没有曲谱"的说明）。
    // 原来都挤在一行大字里，主标题被撑得又大又杂，看不出重点。
    std::string SongSubtitleText() const;
    std::string StateText() const;
    std::string CurrentNoteText() const;
    std::string TimeText() const;
    std::string InfoText() const;          // BPM / 速度 / 模式（一行摘要）
    std::string HotkeyHintText() const;

    // Phase 8：BPM / 拍号 / 小节 / 模式（主界面分行显示，需求书三十六节）
    std::string TempoText() const;         // "BPM：120（1 拍 = 500 ms）"
    std::string SignatureText() const;     // "拍号：4/4（每小节 4 拍 = 2.000 秒）"
    std::string BarText() const;           // "小节：3 / 8"
    std::string ModeText() const;          // "模式：精确" / "模式：自然（抖动 3 ms 以内）"
    double Speed() const;                  // 当前速度（1.0 = 100%）
    int SpeedPresetIndex() const;          // 命中六档返回 0..5，否则 -1（自定义）
    bool NaturalMode() const;
    int ProgressPercent() const;
    int CurrentNoteIndex() const;          // 高亮用；没有则 -1

    // 曲谱被改过（编辑窗口要用最新的）
    void NotifySongEdited(const Song& song);

private:
    static void CALLBACK ForegroundProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject,
                                        LONG idChild, DWORD thread, DWORD time);
    void OnHotkey(const HotkeyEvent& event);
    // 钩子线程上认"热键"（放行模式）：只允许匹配 + 投递，别的都不许做
    void OnHookKey(const UserInputEvent& event);
    // 当前按下的修饰键：钩子线程上即时取，不依赖监视器内部状态
    unsigned CurrentModifiers() const;
    // 某个动作当前生效的绑定：独占模式问注册结果，放行模式用自己解析的那份
    HotkeyBinding BindingFor(HotkeyAction action) const;
    void OnUserInput(const UserInputEvent& event);
    bool ShouldIgnoreUserInput(const UserInputEvent& event);
    void OnForegroundWindow(HWND window);
    void RebuildTimeline();
    void ApplyForegroundPolicy(HWND window);
    // 单实例守护：具名 Mutex。已经有实例在跑就把它叫到前面并让本次启动失败（返回 false）。
    bool AcquireSingleInstance(std::string* error);

    HINSTANCE instance_ = nullptr;

    std::unique_ptr<InputController> input_;
    std::unique_ptr<SongPlayer> player_;
    std::unique_ptr<InputMonitor> monitor_;
    std::unique_ptr<GlobalHotkeyManager> hotkeys_;

    MainWindow* mainWindow_ = nullptr;
    NewSongWindow* newSongWindow_ = nullptr;       // 新建曲谱（常驻窗口）
    SettingsWindow* settingsWindow_ = nullptr;

    Song song_;
    Timeline timeline_;
    bool hasSong_ = false;
    std::unique_ptr<OsdWindow> osd_;   // 屏幕提示窗口（切歌时显示曲名）
    // 放行模式生效中（= 设置里没勾"热键独占"）。UI 线程写、钩子线程读，所以用原子的。
    std::atomic<bool> hookHotkeysActive_{false};
    // 录键期间挂起热键：放行模式没有 RegisterHotKey 可 Suspend，得自己拦
    std::atomic<bool> hotkeyCaptureSuspended_{false};
    // 放行模式自己存一份绑定（没有 RegisterHotKey 就没有管理器里的表，提示行和
    // 钩子匹配都靠它；不存的话会退化成"只有默认键生效"）
    HotkeyBinding ownBindings_[kHotkeyActionCount];
    unsigned timelineGeneration_ = 0;

    AppSettings settings_{};
    std::string settingsPath_;      // Phase 11：配置文件路径

    // Phase 13：排除键位解析出来的键码（按下不打断演奏）。设置一变就重算。
    // 无锁快照：UI 线程构造好新的一份后用 std::atomic_store 一次发布，钩子线程只做
    // std::atomic_load（不进锁）。原来是裸 vector —— UI 线程 clear/push_back 引起重分配的
    // 那一瞬间，钩子线程会读到已释放的内存（数据竞争，急停可能失效）。
    std::shared_ptr<const std::vector<unsigned>> ignoredVks_;

    std::string statusText_;
    bool statusError_ = false;

    HWINEVENTHOOK foregroundHook_ = nullptr;
    HANDLE instanceMutex_ = nullptr;         // 单实例守护用的具名互斥量
    HWND playbackForeground_ = nullptr;      // 开始演奏时的前台窗口（游戏）
    bool elevationRelaunching_ = false;      // 已经发起提权重启，本实例该退出了
    bool elevationAttempted_ = false;        // 命令行带 --elevated-attempted：不再申请第二次
    bool elevationSuppressed_ = false;       // 命令行带 --no-elevate：不提权（排错/自检用）
    bool elevationDryRun_ = false;           // 命令行带 --dry-elevate：只记日志不真申请（排错用）
    bool pausedByForeground_ = false;
    bool suppressForeground_ = false;        // 文件对话框等自己人的窗口，别误判
};

}  // namespace ui
}  // namespace hp
