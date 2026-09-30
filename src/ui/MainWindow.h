#pragma once

// 主窗口（Phase 7，需求书三十六节那张界面草图）。
//
// 布局（自上而下）：
//   当前歌曲：xxx
//   曲谱：[下拉列表：songs 目录里的 .json]      （Phase 12）
//   [播放] [暂停] [停止] [重播]
//   ┌ 状态 ───────────────────────────────────────────┐
//   │ 状态：演奏中                                     │
//   │ 当前音符：23 / 128                               │
//   │ [进度条] 65%                                     │
//   │ 00:01.500 / 00:06.000      BPM：120　速度：100%　模式：精确 │
//   └─────────────────────────────────────────────────┘
//   ┌ 音符表 ─────────────────────────────────────────┐
//   │ 序号  时间    音符  模式    时值                 │
//   └─────────────────────────────────────────────────┘
//   [打开曲谱] [编辑曲谱] [文本曲谱] [设置]
//   热键：F6 播放　F7 停止　F8 重播　F9 上一首　F10 下一首（可在设置里改）
//   就绪。可以选择曲谱后按播放，或直接用全局热键。

#include "core/Platform.h"
#include "ui/UiTheme.h"

namespace hp {
namespace ui {

class App;

class MainWindow {
public:
    explicit MainWindow(App& app);
    ~MainWindow();

    bool Create(HINSTANCE instance, std::string* error);
    HWND Handle() const { return hwnd_; }
    void Refresh();
    // 只在文字真的变了才 SetWindowTextW（值语义，不动控件就不重绘）
    void SetTextIfChanged(HWND control, std::string& cache, const std::string& text);                 // 状态、进度、按钮可用性（100 ms 定时器调）
    void RebuildNoteList();         // 曲谱换了以后重建表格（虚拟列表：只设条数）
    void RefreshSongList();         // Phase 12：重扫 songs 目录并填曲谱列表
    std::wstring NoteRowCellText(int row, int column) const;   // 控件来问哪一格就给哪一格

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

    void CreateChildren();
    void Layout();
    void OnTimer();
    void OnCommand(int id, int notifyCode);
    void OnDrawItem(const DRAWITEMSTRUCT* item);
    LRESULT OnNotify(const NMHDR* header);
    void OnPaint();
    void OnDpiChanged(unsigned dpi, const RECT* suggested);
    void ApplyDpi(unsigned dpi);

    App& app_;
    HWND hwnd_ = nullptr;
    HWND title_ = nullptr;
    HWND subtitleLabel_ = nullptr;   // 标题下的小字（音符数 / 文件名）
    HWND emptyHint_ = nullptr;       // 空状态提示（叠在音符表上，没曲谱时显示）
    HWND songListTag_ = nullptr;       // "曲谱："（Phase 12）
    HWND songListCombo_ = nullptr;     // 曲谱列表下拉（Phase 12）
    HWND playButton_ = nullptr;
    HWND pauseButton_ = nullptr;
    HWND restartButton_ = nullptr;
    HWND stateLabel_ = nullptr;
    HWND currentLabel_ = nullptr;
    HWND progressBar_ = nullptr;
    HWND timeLabel_ = nullptr;
    HWND infoLabel_ = nullptr;
    HWND barLabel_ = nullptr;          // Phase 8：小节：3 / 8
    HWND tempoLabel_ = nullptr;        // Phase 8：BPM：120（1 拍 = 500 ms）
    HWND signatureLabel_ = nullptr;    // Phase 8：拍号：4/4　每小节 4 拍 = 2.000 秒
    HWND speedLabel_ = nullptr;
    HWND speedCombo_ = nullptr;        // Phase 8：六档速度 + 自定义
    HWND modeLabel_ = nullptr;
    HWND modeCombo_ = nullptr;         // Phase 8：精确 / 自然
    HWND noteList_ = nullptr;
    HWND openButton_ = nullptr;
    HWND editButton_ = nullptr;
    HWND newButton_ = nullptr;
    HWND settingsButton_ = nullptr;
    HWND hintLabel_ = nullptr;
    HWND statusLabel_ = nullptr;

    unsigned dpi_ = 96;
    unsigned long long lastTimerTick_ = 0;
    unsigned noteGeneration_ = 0;      // 表格内容对应的曲谱版本
    int highlightedRow_ = -1;
    mutable std::wstring cellText_;   // 虚拟列表回答 LVN_GETDISPINFO 用的暂存（控件会立刻拷走）
    std::string lastStateText_;
    // 资源：这五处原来每 100 ms 无条件 SetWindowTextW，文字没变也会触发控件重绘，
    // 空转时白烧 CPU（实测 3.28% 单核）。改成"变了才写"。
    std::string lastTitleText_;
    std::string lastCurrentText_;
    std::string lastTimeText_;
    std::string lastBarText_;
    std::string lastTempoText_;
    std::string lastSignatureText_;
    std::string lastSubtitleText_;
    std::string lastHintText_;
    std::string lastEmptyHintText_;
    UINT refreshIntervalMs_ = 100;
    std::string lastStatusText_;
    bool lastStatusError_ = false;

    // Phase 12：曲谱列表（下标与 songItems_ 一一对应）
    std::vector<std::string> songItems_;
    int lastSongSelection_ = -2;      // -2 = 还没同步过；-1 = 无选中
    bool refillingSongList_ = false;   // 填列表期间挡住 CBN_SELCHANGE 自激

    // 布局算出来的分组框（WM_PAINT 里自己画）
    RECT statusGroup_{};
    RECT notesGroup_{};
};

}  // namespace ui
}  // namespace hp
