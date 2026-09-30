#include "ui/MainWindow.h"

#include "song/BeatMath.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include <commctrl.h>

#include "input/InputController.h"
#include "player/SongPlayer.h"
#include "song/SongSerializer.h"
#include "ui/App.h"
#include "ui/UiFormat.h"
#include "ui/UiModel.h"

namespace hp {
namespace ui {

namespace {

// 列表里只显示文件名（带路径太吵）
std::string SongFileName(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Windows 路径不区分大小写
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

}  // namespace

namespace {

// 控件 id
constexpr int kIdTitle = 1001;
constexpr int kIdPlay = 1002;
constexpr int kIdPause = 1003;
// 1004 原为"停止"按钮：用户反馈暂停/停止功能重复，按钮已移除；停止功能保留在 F7 热键
// 与"前台窗口变化"策略里（App::CommandStop 仍在用），所以 id 也不再分配。
constexpr int kIdRestart = 1005;
constexpr int kIdState = 1006;
constexpr int kIdCurrent = 1007;
constexpr int kIdProgress = 1008;
constexpr int kIdTime = 1009;
constexpr int kIdInfo = 1010;
constexpr int kIdNotes = 1011;
constexpr int kIdOpen = 1012;
constexpr int kIdEdit = 1013;
constexpr int kIdNewSong = 1015;   // 新建曲谱（原来是文本曲谱，Phase 12 补换成常驻入口）
constexpr int kIdSettings = 1014;
constexpr int kIdHint = 1022;   // 原来是 1015，和文本曲谱按钮撞了 id，顺手分开
constexpr int kIdStatusLine = 1016;
constexpr int kIdBar = 1017;
constexpr int kIdTempo = 1018;
constexpr int kIdSpeedCombo = 1019;
constexpr int kIdModeCombo = 1020;
constexpr int kIdSongList = 1021;   // Phase 12：曲谱列表（列 songs 目录里的 .json）
constexpr int kIdSubtitle = 1023;    // 标题下的小字（原来和标题挤在一行）
constexpr int kIdEmptyHint = 1024;   // 空状态提示（叠在音符表上）

constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kRefreshIntervalMs = 100;       // 演奏中：进度条要跟手
constexpr UINT kIdleRefreshIntervalMs = 300;   // 空转：降到 3 Hz，进度条反正不动

const wchar_t* kWindowClass = L"HarmonicaPlayerMain";

}  // namespace

MainWindow::MainWindow(App& app) : app_(app) {}

MainWindow::~MainWindow() = default;

bool MainWindow::Create(HINSTANCE instance, std::string* error) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &MainWindow::WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hbrBackground = nullptr;   // 自己画底色（避免闪烁与主题不一致）
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        if (error != nullptr) {
            *error = "注册窗口类失败（错误码 " + std::to_string(GetLastError()) + "）";
        }
        return false;
    }

    const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX;
    RECT rect{0, 0, S(720), S(825)};   // +19：状态行从两行改成三行（[权限] 那句不被切）
    AdjustWindowRectForDpi(rect, style, false);

    hwnd_ = CreateWindowExW(0, kWindowClass, Utf8ToWide(labels::kAppTitle).c_str(), style,
                            CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                            rect.bottom - rect.top, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        if (error != nullptr) {
            *error = "创建主窗口失败（错误码 " + std::to_string(GetLastError()) + "）";
        }
        return false;
    }
    return true;
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    MainWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->HandleMessage(message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT MainWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE: {
            // 按需求书的 DPI 流程：先把窗口显示出来，拿到真实 DPI 再定尺寸
            ApplyDpi(GetDpiForWindow(hwnd_));
            CreateChildren();
            SetTimer(hwnd_, kRefreshTimer, kRefreshIntervalMs, nullptr);
            Refresh();
            RebuildNoteList();
            return 0;
        }
        case WM_SIZE:
            Layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            // 下限：状态区（含 BPM/拍号/速度/模式四行）+ 音符表至少一行 + 底部按钮
            RECT rect{0, 0, S(720), S(805)};   // 最小高度也留出三行状态行
            AdjustWindowRectForDpi(rect, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, false);
            info->ptMinTrackSize.x = rect.right - rect.left;
            info->ptMinTrackSize.y = rect.bottom - rect.top;
            return 0;
        }
        case WM_TIMER:
            if (wparam == kRefreshTimer) {
                OnTimer();
            }
            return 0;
        case WM_COMMAND:
            OnCommand(LOWORD(wparam), HIWORD(wparam));
            return 0;
        case WM_DRAWITEM:
            OnDrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
            return TRUE;
        case WM_NOTIFY:
            // 必须把自定义绘制的返回值传回系统（DWLP_MSGRESULT），否则表格不会按我们说的画
            return OnNotify(reinterpret_cast<const NMHDR*>(lparam));
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            HWND control = reinterpret_cast<HWND>(lparam);
            SetBkMode(dc, TRANSPARENT);
            if (control == stateLabel_) {
                SetTextColor(dc, ToColorRef(StatusColor(static_cast<int>(app_.Player().State()),
                                                        static_cast<int>(app_.Player().LastStopReason()))));
            } else if (control == statusLabel_) {
                SetTextColor(dc, ToColorRef(app_.StatusIsError() ? kColorRed : kColorMuted));
            } else if (control == subtitleLabel_ || control == hintLabel_ || control == infoLabel_) {
                SetTextColor(dc, ToColorRef(kColorMuted));
            } else if (control == emptyHint_) {
                // 叠在表格上：底色要跟表格一样是白的，否则一块灰补丁
                SetTextColor(dc, ToColorRef(kColorMuted));
                return reinterpret_cast<LRESULT>(PanelBrush());
            } else {
                SetTextColor(dc, ToColorRef(kColorText));
            }
            return reinterpret_cast<LRESULT>(BackgroundBrush());
        }
        case WM_PAINT:
            OnPaint();
            return 0;
        case WM_ERASEBKGND:
            return 1;   // WM_PAINT 里整块重画，避免闪烁
        case WM_DPICHANGED: {
            const unsigned dpi = LOWORD(wparam);
            OnDpiChanged(dpi, reinterpret_cast<const RECT*>(lparam));
            return 0;
        }
        case kMsgHotkeyAction: {
            // 热键线程只负责把动作丢过来，真正的命令在 UI 线程执行
            switch (static_cast<HotkeyAction>(wparam)) {
                case HotkeyAction::Play:     app_.CommandPlay(); break;
                case HotkeyAction::Stop:     app_.CommandStop(); break;
                case HotkeyAction::Pause:    app_.CommandPauseOrResume(); break;
                case HotkeyAction::Restart:  app_.CommandRestart(); break;
                case HotkeyAction::Previous: app_.CommandPreviousSong(); break;
                case HotkeyAction::Next:     app_.CommandNextSong(); break;
                default: break;
            }
            return 0;
        }
        case kMsgUserInput: {
            // 钩子线程已经调过 EmergencyStop，这里只负责（在 UI 线程上）把话说清楚。
            // 再确认一次停止原因：不是"检测到用户输入"就只是刷新，不弹红字（防止误报）。
            if (app_.Player().LastStopReason() != StopReason::UserInput) {
                Refresh();
                return 0;
            }
            const std::string detail = app_.Player().LastStopDetail();
            std::string text = "演奏被用户输入打断";
            if (!detail.empty()) {
                text += "（" + detail + "）";
            }
            text += "，已交还控制权。";
            app_.SetStatus(text, true);
            Refresh();
            return 0;
        }
        case kMsgForeground:
            Refresh();
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, kRefreshTimer);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

void MainWindow::CreateChildren() {
    const HINSTANCE instance = app_.Instance();

    auto makeStatic = [&](const wchar_t* text, int id, HFONT font) {
        HWND control = CreateWindowExW(0, L"STATIC", text,
                                       WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0,
                                       hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                       instance, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return control;
    };

    title_ = makeStatic(L"", kIdTitle, UiFonts().title);
    subtitleLabel_ = makeStatic(L"", kIdSubtitle, UiFonts().small);

    // Phase 12：曲谱列表——直接把 songs 目录里的 .json 列出来，选一个就换曲
    songListTag_ = makeStatic(L"曲谱：", kIdInfo + 103, UiFonts().body);
    songListCombo_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                     WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0,
                                     S(420), S(260), hwnd_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSongList)),
                                     instance, nullptr);
    SendMessageW(songListCombo_, WM_SETFONT, reinterpret_cast<WPARAM>(UiFonts().body), TRUE);
    stateLabel_ = makeStatic(L"", kIdState, UiFonts().body);
    currentLabel_ = makeStatic(L"", kIdCurrent, UiFonts().body);
    timeLabel_ = makeStatic(L"", kIdTime, UiFonts().mono);
    infoLabel_ = makeStatic(L"", kIdInfo, UiFonts().small);
    barLabel_ = makeStatic(L"", kIdBar, UiFonts().body);
    tempoLabel_ = makeStatic(L"", kIdTempo, UiFonts().small);
    signatureLabel_ = makeStatic(L"", kIdInfo + 100, UiFonts().small);

    auto makeCombo = [&](int id, HFONT font) {
        HWND control = CreateWindowExW(0, L"COMBOBOX", L"",
                                       WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0,
                                       S(150), S(200), hwnd_,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance,
                                       nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return control;
    };
    speedLabel_ = makeStatic(L"", kIdInfo + 101, UiFonts().body);
    speedCombo_ = makeCombo(kIdSpeedCombo, UiFonts().body);
    for (int i = 0; i < beat::kSpeedPresetCount; ++i) {
        const std::wstring text = Utf8ToWide(beat::SpeedPresetText(i));
        SendMessageW(speedCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    {
        const std::wstring custom = Utf8ToWide(labels::kCustomSpeed);
        SendMessageW(speedCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(custom.c_str()));
    }
    modeLabel_ = makeStatic(L"", kIdInfo + 102, UiFonts().body);
    modeCombo_ = makeCombo(kIdModeCombo, UiFonts().body);
    {
        const std::wstring exact = Utf8ToWide(labels::kExactMode);
        const std::wstring natural = Utf8ToWide(std::string(labels::kNaturalMode) + "（抖动 3 ms 以内）");
        SendMessageW(modeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(exact.c_str()));
        SendMessageW(modeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(natural.c_str()));
    }
    SetWindowTextW(barLabel_, L"小节：0 / 0");
    SetWindowTextW(speedLabel_, Utf8ToWide(labels::kSpeedPrefix).c_str());
    SetWindowTextW(modeLabel_, Utf8ToWide(labels::kModePrefix).c_str());
    hintLabel_ = makeStatic(L"", kIdHint, UiFonts().small);
    statusLabel_ = makeStatic(L"", kIdStatusLine, UiFonts().small);
    // 空状态提示：先不显示，Refresh 里按"有没有曲谱"决定（叠在表格上，白底）
    emptyHint_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_CENTER | SS_NOPREFIX, 0, 0, 0, 0,
                                 hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdEmptyHint)),
                                 instance, nullptr);
    SendMessageW(emptyHint_, WM_SETFONT, reinterpret_cast<WPARAM>(UiFonts().small), TRUE);

    auto makeButton = [&](const wchar_t* text, int id) {
        HWND button = CreateWindowExW(0, L"BUTTON", text,
                                      WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd_,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance,
                                      nullptr);
        SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(UiFonts().body), TRUE);
        return button;
    };

    playButton_ = makeButton(Utf8ToWide(labels::kPlay).c_str(), kIdPlay);
    pauseButton_ = makeButton(Utf8ToWide(labels::kPause).c_str(), kIdPause);
    restartButton_ = makeButton(Utf8ToWide(labels::kRestart).c_str(), kIdRestart);
    openButton_ = makeButton(Utf8ToWide(labels::kOpenSong).c_str(), kIdOpen);
    editButton_ = makeButton(Utf8ToWide(labels::kEditSong).c_str(), kIdEdit);
    newButton_ = makeButton(Utf8ToWide(labels::kNewSong).c_str(), kIdNewSong);
    settingsButton_ = makeButton(Utf8ToWide(labels::kSettings).c_str(), kIdSettings);

    progressBar_ = CreateWindowExW(0, L"STATIC", L"",
                                   WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 0, 0, 0, 0, hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdProgress)),
                                   instance, nullptr);

    noteList_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                                    LVS_OWNERDATA,   // Phase 11：虚拟列表，十万音符也能秒开
                                0, 0, 0, 0, hwnd_,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdNotes)), instance,
                                nullptr);
    ListView_SetExtendedListViewStyle(noteList_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SendMessageW(noteList_, WM_SETFONT, reinterpret_cast<WPARAM>(UiFonts().body), TRUE);

    struct Column {
        const wchar_t* text;
        int width;
    };
    const Column columns[] = {
        {L"序号", 56}, {L"时间", 88}, {L"音符", 72}, {L"模式", 96}, {L"时值", 160},
    };
    for (int i = 0; i < static_cast<int>(sizeof(columns) / sizeof(columns[0])); ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<LPWSTR>(columns[i].text);
        column.cx = S(columns[i].width);
        column.iSubItem = i;
        ListView_InsertColumn(noteList_, i, &column);
    }
    // 注意：不要在这里设最后一列“吃掉剩余宽度”。此时控件还没定位、宽度是 0，
    // ListView 会按“剩余 0 - 其他列”算出一个负值，把列压成表头那么宽、文字全被截断。
    // 真正的拉伸放在 Layout() 里、控件定位之后（Layout 末尾那段）。
}

void MainWindow::Layout() {
    if (hwnd_ == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int margin = S(16);
    const int gap = S(10);
    const int inner = S(14);

    auto move = [&](HWND control, int x, int y, int w, int h) {
        if (control != nullptr) {
            SetWindowPos(control, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    };

    int y = S(14);
    move(title_, margin, y, width - margin * 2, S(22));
    move(subtitleLabel_, margin, y + S(21), width - margin * 2, S(17));
    y += S(42);

    // Phase 12：曲谱列表（标题下面一行：曲谱：[下拉]）
    {
        const int tagWidth = S(46);
        const int comboWidth = std::min(width - margin * 2 - tagWidth, S(420));
        move(songListTag_, margin, y + S(4), tagWidth, S(20));
        move(songListCombo_, margin + tagWidth, y, comboWidth, S(260));
        y += S(34) + S(10);
    }

    const int buttonWidth = S(96);
    const int buttonHeight = S(34);
    move(playButton_, margin, y, buttonWidth, buttonHeight);
    move(pauseButton_, margin + buttonWidth + gap, y, buttonWidth, buttonHeight);
    move(restartButton_, margin + (buttonWidth + gap) * 2, y, buttonWidth, buttonHeight);
    y += buttonHeight + S(14);

    // 需求书三十六节：状态 / 当前音符 / 进度 / BPM / 速度 / 模式 各自成行
    const int statusGroupHeight = S(236);
    statusGroup_ = RECT{margin, y, width - margin, y + statusGroupHeight};
    const int statusLeft = margin + inner;
    const int statusWidth = width - (margin + inner) * 2;
    int innerY = y + S(30);
    move(stateLabel_, statusLeft, innerY, statusWidth, S(24));
    innerY += S(30);
    move(currentLabel_, statusLeft, innerY, S(200), S(20));
    move(barLabel_, statusLeft + S(216), innerY, statusWidth - S(216), S(20));
    innerY += S(28);
    move(progressBar_, statusLeft, innerY, statusWidth, S(26));
    innerY += S(34);
    move(timeLabel_, statusLeft, innerY, S(216), S(20));
    move(tempoLabel_, statusLeft + S(216), innerY, statusWidth - S(216), S(20));
    innerY += S(32);
    // 速度与模式在主界面上就能改（需求书二十九、三十节）
    const int tagWidth = S(46);
    const int speedComboWidth = S(150);
    move(speedLabel_, statusLeft, innerY + S(3), tagWidth, S(20));
    move(speedCombo_, statusLeft + tagWidth, innerY, speedComboWidth, S(200));
    const int modeLeft = statusLeft + tagWidth + speedComboWidth + S(20);
    move(modeLabel_, modeLeft, innerY + S(3), tagWidth, S(20));
    move(modeCombo_, modeLeft + tagWidth, innerY, S(210), S(200));
    innerY += S(38);
    move(signatureLabel_, statusLeft, innerY, statusWidth, S(20));
    move(infoLabel_, statusLeft, innerY, statusWidth, S(20));   // 保留旧控件（同一行，暂不显示）
    ShowWindow(infoLabel_, SW_HIDE);
    y += statusGroupHeight + S(14);

    const int bottom = height - S(115);   // 状态行三行高：底部基准上移
    const int notesGroupHeight = std::max(S(120), bottom - y - S(4));
    notesGroup_ = RECT{margin, y, width - margin, y + notesGroupHeight};
    move(noteList_, margin + S(10), y + S(26), width - (margin + S(10)) * 2,
         notesGroupHeight - S(34));
    // 控件这时才有真实宽度：最后一列（时值）吃掉剩余宽度，窗口变宽也跟着变
    if (noteList_ != nullptr) {
        ListView_SetColumnWidth(noteList_, 4, LVSCW_AUTOSIZE_USEHEADER);
    }

    move(openButton_, margin, bottom, S(110), buttonHeight);
    move(editButton_, margin + S(110) + gap, bottom, S(110), buttonHeight);
    move(newButton_, margin + (S(110) + gap) * 2, bottom, S(110), buttonHeight);
    // 设置靠右：左边三个是"曲谱相关"，设置是全局的，分开放右边，顺带把右边的大片空白用起来
    move(settingsButton_, width - margin - S(110), bottom, S(110), buttonHeight);
    move(hintLabel_, margin, bottom + buttonHeight + S(6), width - margin * 2, S(20));
    // 状态行可以是长句子（"已保存到 …"、"上次打开的曲谱现在打不开了 …"）：
    // 给三行高。原来两行高，[权限] 那段话被切掉一半（实测肉眼可见）
    move(statusLabel_, margin, bottom + buttonHeight + S(28), width - margin * 2, S(57));
    // 空状态提示摆在表格正中间（表格是白色，提示用白底，不能留灰框）
    move(emptyHint_, margin + S(10), y + S(26) + (notesGroupHeight - S(34)) / 2 - S(18),
         width - (margin + S(10)) * 2, S(36));
    if (emptyHint_ != nullptr) {
        SetWindowPos(emptyHint_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        // 这个静态控件是"先隐藏、拿到文字和尺寸后再显示"的：显示那一下的重画会被吞掉，
        // 结果只在离屏渲染里有字、真屏幕上什么都没有（实测屏幕取色 0 个墨迹点）。
        // 定位完强制重画一次，屏幕才和离屏一致。
        if (IsWindowVisible(emptyHint_) != FALSE) {
            RedrawWindow(emptyHint_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        }
    }

    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OnPaint() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd_, &paint);

    RECT client{};
    GetClientRect(hwnd_, &client);
    FillRect(dc, &client, BackgroundBrush());

    // 分组框：1 px 边框 + 标题（标题处的底色先擦干净，做出"框线被标题截断"的效果）
    auto drawGroup = [&](const RECT& rect, const char* title) {
        HPEN pen = CreatePen(PS_SOLID, 1, ToColorRef(kColorBorder));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, S(8), S(8));
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);

        const std::wstring wide = Utf8ToWide(title);
        HFONT font = UiFonts().small;
        HGDIOBJ oldFont = (font != nullptr) ? SelectObject(dc, font) : nullptr;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ToColorRef(kColorMuted));
        RECT textRect{rect.left + S(12), rect.top - S(9), rect.right - S(12), rect.top + S(11)};
        const int textWidth = static_cast<int>(wide.size()) * S(9) + S(8);
        RECT clearRect{rect.left + S(10), rect.top - S(9), rect.left + S(10) + textWidth,
                       rect.top + S(11)};
        FillRect(dc, &clearRect, BackgroundBrush());
        DrawTextW(dc, wide.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (oldFont != nullptr) {
            SelectObject(dc, oldFont);
        }
    };

    if (statusGroup_.bottom > statusGroup_.top) {
        drawGroup(statusGroup_, labels::kGroupStatus);
    }
    if (notesGroup_.bottom > notesGroup_.top) {
        drawGroup(notesGroup_, labels::kGroupNotes);
    }

    // 空曲谱：在音符表区域中央写一句人话（表格已隐藏，不会互相打架）
    if (!app_.HasSong() && notesGroup_.bottom > notesGroup_.top) {
        const std::wstring hint =
            L"还没有曲谱：点下面的「新建曲谱」建一首，或从上面的「曲谱」列表里选一首";
        HFONT font = UiFonts().small;
        HGDIOBJ oldFont = (font != nullptr) ? SelectObject(dc, font) : nullptr;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ToColorRef(kColorMuted));
        RECT area = notesGroup_;
        area.left += S(12);
        area.right -= S(12);
        DrawTextW(dc, hint.c_str(), -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (oldFont != nullptr) {
            SelectObject(dc, oldFont);
        }
    }

    EndPaint(hwnd_, &paint);
}

void MainWindow::OnCommand(int id, int notifyCode) {
    switch (id) {
        case kIdSongList: {
            // 重新填列表时控件自己会发通知，这里要挡住，否则边填边"选中"→ 连着换曲
            if (refillingSongList_) {
                break;
            }
            if (notifyCode == CBN_DROPDOWN) {
                RefreshSongList();   // 拉开前重扫一次：刚存进去的曲谱立刻能看到
            } else if (notifyCode == CBN_SELCHANGE) {
                const int index = static_cast<int>(SendMessageW(songListCombo_, CB_GETCURSEL, 0, 0));
                if (index >= 0 && index < static_cast<int>(songItems_.size())) {
                    std::string message;
                    if (!app_.OpenSongFile(songItems_[index], &message)) {
                        app_.SetStatus("打开失败（" + SongFileName(songItems_[index]) + "）：" + message,
                                       true);
                    }
                }
            }
            break;
        }
        case kIdPlay:     app_.CommandPlay(); break;
        case kIdPause:    app_.CommandPauseOrResume(); break;
        case kIdRestart:  app_.CommandRestart(); break;
        case kIdOpen:     app_.CommandOpenSong(); break;
        case kIdEdit:     app_.EditCurrentSong(); break;
        case kIdNewSong:   app_.CommandNewSong(); break;
        case kIdSettings: app_.ShowSettings(); break;
        case kIdSpeedCombo: {
            const int index = static_cast<int>(SendMessageW(speedCombo_, CB_GETCURSEL, 0, 0));
            if (index >= 0 && index < beat::kSpeedPresetCount) {
                app_.CommandSetSpeed(beat::SpeedPreset(index));
            } else if (index >= beat::kSpeedPresetCount) {
                app_.SetStatus("自定义速度请在设置里填（范围 50% ~ 200%）。", false);
                app_.ShowSettings();
            }
            break;
        }
        case kIdModeCombo: {
            const int index = static_cast<int>(SendMessageW(modeCombo_, CB_GETCURSEL, 0, 0));
            app_.CommandSetNaturalMode(index == 1);
            break;
        }
        default: break;
    }
}

void MainWindow::OnDrawItem(const DRAWITEMSTRUCT* item) {
    if (item == nullptr) {
        return;
    }
    switch (static_cast<int>(item->CtlID)) {
        case kIdPlay: {
            ButtonPaintInfo info;
            info.kind = ButtonKind::Primary;
            info.text = Utf8ToWide(labels::kPlay);
            info.enabled = (item->itemState & ODS_DISABLED) == 0;
            PaintFlatButton(*item, info);
            break;
        }
        case kIdPause: {
            ButtonPaintInfo info;
            info.kind = ButtonKind::Secondary;
            info.text = Utf8ToWide(app_.Player().IsPaused() ? "继续" : labels::kPause);
            info.enabled = (item->itemState & ODS_DISABLED) == 0;
            PaintFlatButton(*item, info);
            break;
        }
        case kIdRestart: {
            ButtonPaintInfo info;
            info.kind = ButtonKind::Secondary;
            info.text = Utf8ToWide(labels::kRestart);
            info.enabled = (item->itemState & ODS_DISABLED) == 0;
            PaintFlatButton(*item, info);
            break;
        }
        case kIdOpen:
        case kIdNewSong:
        case kIdEdit:
        case kIdSettings: {
            ButtonPaintInfo info;
            info.kind = ButtonKind::Neutral;
            info.text = Utf8ToWide(item->CtlID == kIdOpen      ? labels::kOpenSong
                                   : item->CtlID == kIdNewSong ? labels::kNewSong
                                   : item->CtlID == kIdEdit    ? labels::kEditSong
                                                                : labels::kSettings);
            info.enabled = (item->itemState & ODS_DISABLED) == 0;
            PaintFlatButton(*item, info);
            break;
        }
        case kIdProgress: {
            const int percent = app_.ProgressPercent();
            const std::wstring caption = Utf8ToWide(FormatPercent(percent));
            PaintProgress(*item, percent, caption, kColorAccent);
            break;
        }
        default:
            break;
    }
}

LRESULT MainWindow::OnNotify(const NMHDR* header) {
    if (header == nullptr || header->hwndFrom != noteList_) {
        return FALSE;
    }
    if (header->code == LVN_GETDISPINFO) {
        // 虚拟列表：控件来要某一格文字时才现算（一次只问一格）
        auto* info = reinterpret_cast<NMLVDISPINFOW*>(const_cast<NMHDR*>(header));
        if ((info->item.mask & LVIF_TEXT) != 0) {
            cellText_ = NoteRowCellText(info->item.iItem, info->item.iSubItem);
            info->item.pszText = cellText_.data();
        }
        return FALSE;
    }
    if (header->code == NM_CUSTOMDRAW) {
        auto* draw = reinterpret_cast<LPNMLVCUSTOMDRAW>(const_cast<NMHDR*>(header));
        switch (draw->nmcd.dwDrawStage) {
            case CDDS_PREPAINT:
                SetWindowLongPtrW(hwnd_, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
                return TRUE;
            case CDDS_ITEMPREPAINT: {
                const int row = static_cast<int>(draw->nmcd.dwItemSpec);
                if (row == highlightedRow_) {
                    // 当前正在演奏的音符：用强调色底 + 深色字（不用符号）
                    draw->clrTextBk = ToColorRef(Rgb{222, 232, 248});
                    draw->clrText = ToColorRef(kColorText);
                    SetWindowLongPtrW(hwnd_, DWLP_MSGRESULT, CDRF_NEWFONT);
                } else {
                    SetWindowLongPtrW(hwnd_, DWLP_MSGRESULT, CDRF_DODEFAULT);
                }
                return TRUE;
            }
            default:
                break;
        }
    }
    return FALSE;
}

void MainWindow::OnTimer() {
    const unsigned long long now = GetTickCount64();
    if (now == lastTimerTick_) {
        return;
    }
    lastTimerTick_ = now;
    Refresh();
}

void MainWindow::RefreshSongList() {
    if (songListCombo_ == nullptr) {
        return;
    }
    const std::vector<std::string>& files = app_.RefreshSongList();

    refillingSongList_ = true;
    SendMessageW(songListCombo_, CB_RESETCONTENT, 0, 0);
    songItems_.clear();
    for (const std::string& file : files) {
        songItems_.push_back(file);
        const std::wstring label = Utf8ToWide(SongFileName(file));
        SendMessageW(songListCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    // 当前这首不在 songs 里（例如从别处打开的）：也列进去，标一下，免得看着像没选中
    const std::string current = app_.CurrentSongPath();
    int wanted = -1;
    if (!current.empty()) {
        for (size_t i = 0; i < songItems_.size(); ++i) {
            if (SamePathText(songItems_[i], current)) {
                wanted = static_cast<int>(i);
                break;
            }
        }
        if (wanted < 0) {
            songItems_.push_back(current);
            const std::wstring label = Utf8ToWide(SongFileName(current) + "（不在 songs 里）");
            SendMessageW(songListCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            wanted = static_cast<int>(songItems_.size()) - 1;
        }
    }
    if (songItems_.empty()) {
        const std::wstring empty = Utf8ToWide("（songs 文件夹里还没有曲谱）");
        SendMessageW(songListCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(empty.c_str()));
        SendMessageW(songListCombo_, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
        wanted = -1;
    } else if (wanted < 0) {
        // 有曲谱可选，但当前一首都没载入（启动后的空状态）：别假装选中了第一项
        SendMessageW(songListCombo_, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    } else {
        SendMessageW(songListCombo_, CB_SETCURSEL, static_cast<WPARAM>(wanted), 0);
    }
    lastSongSelection_ = wanted;
    refillingSongList_ = false;
}

void MainWindow::SetTextIfChanged(HWND control, std::string& cache, const std::string& text) {
    if (control == nullptr || text == cache) {
        return;
    }
    cache = text;
    SetWindowTextW(control, Utf8ToWide(text).c_str());
    // 光 SetWindowTextW 不够：控件如果在 0×0 或被隐藏时收到文字，那次失效就丢了，
    // 之后给了尺寸也不会自动补画（实测空状态提示因此一直看不见）。显式重画一次。
    InvalidateRect(control, nullptr, TRUE);
}

void MainWindow::Refresh() {
    if (hwnd_ == nullptr) {
        return;
    }
    SongPlayer& player = app_.Player();

    SetTextIfChanged(title_, lastTitleText_, app_.SongTitleText());
    SetTextIfChanged(subtitleLabel_, lastSubtitleText_, app_.SongSubtitleText());

    // 曲谱列表：只在当前曲谱真的变了才改选中项（免得用户正拉着列表被重置）
    if (songListCombo_ != nullptr && !refillingSongList_) {
        const std::string current = app_.CurrentSongPath();
        int wanted = -1;
        for (size_t i = 0; i < songItems_.size(); ++i) {
            if (SamePathText(songItems_[i], current)) {
                wanted = static_cast<int>(i);
                break;
            }
        }
        if (wanted >= 0 && wanted != lastSongSelection_) {
            SendMessageW(songListCombo_, CB_SETCURSEL, static_cast<WPARAM>(wanted), 0);
            lastSongSelection_ = wanted;
        }
    }

    const std::string stateText = app_.StateText();
    if (stateText != lastStateText_) {
        lastStateText_ = stateText;
        SetWindowTextW(stateLabel_, Utf8ToWide(stateText).c_str());
        InvalidateRect(stateLabel_, nullptr, TRUE);
    }
    // 资源：这五处原来每 100 ms 无条件写一次，文字没变也会让控件重绘（空转 3.28% 单核的元凶）
    SetTextIfChanged(currentLabel_, lastCurrentText_, app_.CurrentNoteText());
    SetTextIfChanged(timeLabel_, lastTimeText_, app_.TimeText());
    SetTextIfChanged(barLabel_, lastBarText_, app_.BarText());
    SetTextIfChanged(tempoLabel_, lastTempoText_, app_.TempoText());
    SetTextIfChanged(signatureLabel_, lastSignatureText_, app_.SignatureText());
    // 速度 / 模式下拉：只在和当前值不一致时改，免得用户正在拉列表时被重置
    const int preset = app_.SpeedPresetIndex();
    const int wantedSpeed = (preset >= 0) ? preset : beat::kSpeedPresetCount;   // 未命中六档 = 最后一项"自定义"
    if (speedCombo_ != nullptr &&
        static_cast<int>(SendMessageW(speedCombo_, CB_GETCURSEL, 0, 0)) != wantedSpeed) {
        SendMessageW(speedCombo_, CB_SETCURSEL, static_cast<WPARAM>(wantedSpeed), 0);
    }
    const int wantedMode = app_.NaturalMode() ? 1 : 0;
    if (modeCombo_ != nullptr &&
        static_cast<int>(SendMessageW(modeCombo_, CB_GETCURSEL, 0, 0)) != wantedMode) {
        SendMessageW(modeCombo_, CB_SETCURSEL, static_cast<WPARAM>(wantedMode), 0);
    }
    SetTextIfChanged(hintLabel_, lastHintText_, app_.HotkeyHintText());

    // 空状态提示：没曲谱时表格中央给一句人话（原来只有一片空白，像坏了）
    // 空状态：表格先藏起来，提示交给窗口自己在 OnPaint 里画。
    // 试过用静态控件做这件事：离屏渲染（PrintWindow）里画得出来，真屏幕上却是空的
    // （屏幕取色 0 个墨迹点，对照组标题 595 / 状态行 1095），"先隐藏后显示"这条路的
    // 重画会被吞掉。父窗口自己画则一定随 WM_PAINT 出现，没有这类坑。
    const bool emptySong = !app_.HasSong();
    if (noteList_ != nullptr && (IsWindowVisible(noteList_) != FALSE) == emptySong) {
        ShowWindow(noteList_, emptySong ? SW_HIDE : SW_SHOW);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (emptyHint_ != nullptr && IsWindowVisible(emptyHint_) != FALSE) {
        ShowWindow(emptyHint_, SW_HIDE);   // 旧控件不再使用，留着也不显示
    }

    const std::string status = app_.StatusText();
    if (status != lastStatusText_ || app_.StatusIsError() != lastStatusError_) {
        lastStatusText_ = status;
        lastStatusError_ = app_.StatusIsError();
        // 长句子自己折行：STATIC 对中文长句不折行，会把右边切掉（用户反馈"字多就显示不全"）
        RECT statusRect{};
        GetClientRect(statusLabel_, &statusRect);
        const double charsPerLine =
            statusRect.right > statusRect.left
                ? static_cast<double>(statusRect.right - statusRect.left) / static_cast<double>(S(14))
                : 60.0;
        // 三行：标签已经给到三行高，[权限] 那句长提示原来是两行被"…"截掉尾巴
        const std::string wrapped = WrapLabelText(status, charsPerLine, 3);
        SetWindowTextW(statusLabel_, Utf8ToWide(wrapped).c_str());
        InvalidateRect(statusLabel_, nullptr, TRUE);
    }

    InvalidateRect(progressBar_, nullptr, TRUE);

    // 按钮可用性：演奏中不能再按播放，停止状态不能按暂停（按钮说人话，不允许乱按）
    EnableWindow(playButton_, !player.IsPlaying() && app_.HasSong());
    EnableWindow(pauseButton_, player.IsPlaying() || player.IsPaused());
    EnableWindow(restartButton_, app_.HasSong());
    // 新建曲谱是**常驻功能**：任何时候都能点（Phase 12 补：新建与编辑彻底分成两个按钮）
    EnableWindow(newButton_, TRUE);
    // 编辑曲谱要有东西可编辑才让点；真点了没曲谱时 App 会如实告诉他先去新建或选一首
    EnableWindow(editButton_, app_.HasSong());

    // 演奏到哪个音符了：高亮那一行（只在变化时刷新，避免闪烁）
    const int row = app_.CurrentNoteIndex();
    if (row != highlightedRow_) {
        const int previous = highlightedRow_;
        highlightedRow_ = row;
        if (noteList_ != nullptr) {
            // Phase 11 大型曲谱：只重画变化的这两行（十万行的表不能整体重画）
            if (previous >= 0) {
                ListView_RedrawItems(noteList_, previous, previous);
            }
            if (row >= 0) {
                ListView_RedrawItems(noteList_, row, row);
                ListView_EnsureVisible(noteList_, row, FALSE);
            }
        }
    }

    if (app_.CurrentTimeline().NoteCount() != ListView_GetItemCount(noteList_)) {
        RebuildNoteList();
    }

    // 资源：演奏/暂停时按 10 Hz 刷（进度条要跟手），空转降到 3 Hz。
    // Refresh 每次用户操作也会跑一遍，所以一按播放间隔立刻就切回 100 ms。
    const bool active = player.IsPlaying() || player.IsPaused();
    const UINT wanted = active ? kRefreshIntervalMs : kIdleRefreshIntervalMs;
    if (wanted != refreshIntervalMs_) {
        refreshIntervalMs_ = wanted;
        SetTimer(hwnd_, kRefreshTimer, wanted, nullptr);
    }
}

std::wstring MainWindow::NoteRowCellText(int row, int column) const {
    return Utf8ToWide(
        ui::NoteRowCellText(app_.CurrentTimeline(), app_.CurrentSong(), row, column));
}

void MainWindow::RebuildNoteList() {
    if (noteList_ == nullptr) {
        return;
    }
    // Phase 11 大型曲谱：虚拟列表 —— 只告诉控件“有多少行”，
    // 具体每格的文字等控件来问（LVN_GETDISPINFO）时才生成。
    // 十万音符不再需要十万次 InsertItem，也不再为每一行常驻内存。
    ListView_SetItemCountEx(noteList_, app_.CurrentTimeline().NoteCount(), LVSICF_NOSCROLL);
    highlightedRow_ = -1;
}

void MainWindow::ApplyDpi(unsigned dpi) {
    if (dpi < 72 || dpi > 480) {
        dpi = 96;
    }
    dpi_ = dpi;
    SetUiDpi(dpi);
    RebuildUiFonts();
    RebuildThemeBrushes();

    HFONT body = UiFonts().body;
    HWND controls[] = {title_, playButton_, pauseButton_, restartButton_,
                       stateLabel_, currentLabel_, timeLabel_, infoLabel_, noteList_,
                       openButton_, newButton_, editButton_, settingsButton_, hintLabel_, statusLabel_,
                       songListTag_, songListCombo_};
    HFONT fonts[] = {UiFonts().title, body, body, body,
                     body, body, UiFonts().mono, UiFonts().small, body,
                     body, body, body, body, UiFonts().small, UiFonts().small,
                     body, body};
    for (int i = 0; i < static_cast<int>(sizeof(controls) / sizeof(controls[0])); ++i) {
        if (controls[i] != nullptr) {
            SendMessageW(controls[i], WM_SETFONT, reinterpret_cast<WPARAM>(fonts[i]), TRUE);
        }
    }

    // 列宽按新 DPI 重算
    if (noteList_ != nullptr) {
        const int widths[] = {56, 88, 72, 96};
        for (int i = 0; i < 4; ++i) {
            ListView_SetColumnWidth(noteList_, i, S(widths[i]));
        }
        ListView_SetColumnWidth(noteList_, 4, LVSCW_AUTOSIZE_USEHEADER);
    }
}

void MainWindow::OnDpiChanged(unsigned dpi, const RECT* suggested) {
    ApplyDpi(dpi);
    if (suggested != nullptr) {
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    Layout();
    Refresh();
}

}  // namespace ui
}  // namespace hp
