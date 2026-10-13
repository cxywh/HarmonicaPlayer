// NewSongWindow.cpp —— 新建曲谱（常驻、点选式加音符）
//
// 界面结构（单游标自顶向下，按钮跟着内容走，最后按内容总高把窗口调到刚好）：
//   [曲谱信息]  曲名 / 速度（BPM）
//   [加音符]    音调：1 2 3 4 5 6 7 i （括号里是键盘 Z X C V B N M ,）
//               演奏方式：第一行 不按鼠标（本音）/ 按住左键（降调）/ 按住中键（半音）/ 按住右键（升调）
//                         第二行 按住左键+中键（降调+半音）/ 按住右键+中键（升调+半音）
//                         （后两个是组合模式：同时按住两个鼠标键）
//               时值：全音符 4 拍 / 二分音符 2 拍 / 四分音符 1 拍 / 八分音符 1/2 拍 /
//                     十六分音符 1/4 拍 / 附点四分 1.5 拍 / 附点八分 0.75 拍 /
//                     休止两拍 / 休止一拍 / 休止半拍；再加一个「延时」（简谱的 -，把上一个音符延长一拍）
//   [已加的音符] 表格（序号/小节/拍位置/起始时间/音调/演奏方式/时值）
//   底部：保存到曲谱列表… / 删掉最后一个 / 全部清空 / 关闭 + 状态行
//
// 宽度约定：所有按钮的宽度都用 ui::ButtonWidthForText（内存 DC + GetTextExtentPoint32 实测文字宽）
// 推出来，放不下就折行——**不许**再写死宽度：中文长文案会被截掉（用户报过两次）。
// ui_test 用同一套函数断言"文字宽 + 内边距 ≤ 按钮宽"，改文案时会自动拦住。
//
// 加音符就是"点一下"：不需要理解拍号、时值、毫秒——那些都在表格里替他算好、显示出来。

#include "ui/NewSongWindow.h"

#include <algorithm>
#include <string>
#include <vector>

#include <commctrl.h>

#include "core/Log.h"
#include "ui/App.h"
#include "ui/UiFormat.h"

namespace hp {
namespace ui {

namespace {

const wchar_t* kWindowClass = L"HarmonicaPlayerNewSong";

constexpr int kIdName = 2101;
constexpr int kIdBpm = 2102;
constexpr int kIdNoteBase = 2110;       // 2110..2117 → Key::Z..Comma
constexpr int kIdModeBase = 2120;       // 2120..2125 → PlayMode（6 种：单键 4 + 组合 2）
constexpr int kIdDurationBase = 2130;   // 2130..2139 → kDurationChoices（10 档标准时值）
constexpr int kIdList = 2140;
constexpr int kIdDeleteSelected = 2150;   // 删除选中行（原来是"删掉最后一个"）
constexpr int kIdEditSelected = 2154;     // 编辑选中行
constexpr int kIdExtendLast = 2155;       // 延时（简谱的 -）：把上一个音符延长一拍
constexpr int kIdClear = 2151;
constexpr int kIdSave = 2152;
constexpr int kIdClose = 2153;
constexpr int kIdStatus = 2160;
constexpr int kIdHint = 2161;

std::string ReadText(HWND edit) {
    if (edit == nullptr) {
        return std::string();
    }
    const int length = GetWindowTextLengthW(edit);
    if (length <= 0) {
        return std::string();
    }
    std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(edit, buffer.data(), length + 1);
    buffer.resize(static_cast<size_t>(length));
    return WideToUtf8(buffer);
}

// 表格里的"演奏方式"用短名（按钮上才是完整说法）
const char* ShortModeName(PlayMode mode) {
    switch (mode) {
        case PlayMode::Normal: return "本音";
        case PlayMode::Left:   return "左键";
        case PlayMode::Middle: return "中键";
        case PlayMode::Right:  return "右键";
        case PlayMode::LeftMiddle:  return "左+中";
        case PlayMode::RightMiddle: return "右+中";
    }
    return "?";
}

}  // namespace

NewSongWindow::NewSongWindow(App& app) : app_(app) {}

NewSongWindow::~NewSongWindow() = default;

bool NewSongWindow::Create(HINSTANCE instance, std::string* error) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &NewSongWindow::WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    windowClass.lpszClassName = kWindowClass;
    windowClass.hbrBackground = nullptr;
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        if (error != nullptr) {
            *error = "注册新建曲谱窗口类失败（错误码 " + std::to_string(GetLastError()) + "）";
        }
        return false;
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    // 客户区基准宽度取了 UiTheme.h 的 kNewSongWindowWidth：自检（ui_test）按同一个宽度算按钮
    // 能不能放得下，两边必须用同一个数，否则断言测的不是窗口真正用的宽度。
    RECT rect{0, 0, S(kNewSongWindowWidth), S(620)};
    AdjustWindowRectForDpi(rect, style, false);
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"新建曲谱", style, CW_USEDEFAULT,
                            CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr,
                            nullptr, instance, this);
    if (hwnd_ == nullptr) {
        if (error != nullptr) {
            *error = "创建新建曲谱窗口失败（错误码 " + std::to_string(GetLastError()) + "）";
        }
        return false;
    }
    dpi_ = GetDpiForWindow(hwnd_);
    return true;
}

void NewSongWindow::Reset() {
    rows_.clear();
    cursorBeats_ = 0.0;
    mode_ = PlayMode::Normal;
    durationChoice_ = kDefaultDurationChoiceIndex;
    editingRow_ = -1;
    if (nameEdit_ != nullptr) {
        SetWindowTextW(nameEdit_, Utf8ToWide(labels::kNewSongName).c_str());
        SetWindowTextW(bpmEdit_, L"120");
    }
    // 回到"新建"语义：不再是编辑某个文件，曲头拍号/拍单位也回到默认（4/4、四分音符）
    editPath_.clear();
    frameBeatUnit_ = NoteValue::Quarter;
    frameSignature_ = TimeSignature{4, 4};
    if (hwnd_ != nullptr) {
        SetWindowTextW(hwnd_, L"新建曲谱");
    }
    if (saveButton_ != nullptr) {
        SetWindowTextW(saveButton_, L"保存到曲谱列表");
    }
    RefreshList();
    InvalidatePickers();
    SetStatus("新曲谱：点下面的音调按钮加音符（先选好演奏方式和时值），加完点「保存到曲谱列表」就出现在主界面。",
              false);
}

void NewSongWindow::LoadSongForEdit(const Song& song, const std::string& sourcePath) {
    rows_ = BuildEditorRows(song);   // 含休止行
    editPath_ = sourcePath;
    frameBeatUnit_ = song.beatUnit;      // 曲头跟着被编辑的曲谱走
    frameSignature_ = song.timeSignature;
    editingRow_ = -1;
    durationChoice_ = kDefaultDurationChoiceIndex;
    mode_ = PlayMode::Normal;
    cursorBeats_ = 0.0;
    for (const EditorRow& row : rows_) {
        cursorBeats_ = std::max(cursorBeats_, row.beat + row.durationBeats);
    }
    if (nameEdit_ != nullptr) {
        SetWindowTextW(nameEdit_, Utf8ToWide(song.name).c_str());
        SetWindowTextW(bpmEdit_, Utf8ToWide(FormatBeats(song.bpm)).c_str());
    }
    if (hwnd_ != nullptr) {
        SetWindowTextW(hwnd_, Utf8ToWide("编辑曲谱 —— " + song.name).c_str());
    }
    if (saveButton_ != nullptr) {
        SetWindowTextW(saveButton_, L"保存（覆盖原曲谱）");
    }
    InvalidatePickers();
    RefreshList();
    SetStatus("已打开《" + song.name + "》（" + std::to_string(rows_.size()) +
                  " 项，含休止）：点音调按钮往后加；在表格里点一行选中它，再点「编辑选中行」改它、"
                  "「删除选中行」删掉它；改完点「保存（覆盖原曲谱）」。",
              false);
}

LRESULT CALLBACK NewSongWindow::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        // 必须在 WM_NCCREATE 就把句柄存下来：WM_CREATE 里建子控件要用它当父窗口，
        // 否则 hwnd_ 还是 nullptr，子控件创建失败（错误码 1400），整个窗口都建不起来。
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        auto* self = static_cast<NewSongWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<NewSongWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) {
        return self->HandleMessage(message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT NewSongWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            CreateChildren();
            Layout();
            return 0;
        case WM_SIZE:
            Layout();
            return 0;
        case WM_DPICHANGED: {
            const unsigned dpi = HIWORD(wparam);
            const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
            if (suggested != nullptr) {
                SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            ApplyDpi(dpi);
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            const int notify = HIWORD(wparam);
            HWND control = reinterpret_cast<HWND>(lparam);
            OnCommand(id, notify, control);
            return 0;
        }
        case WM_DRAWITEM:
            OnDrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
            return TRUE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            OnPaint();
            return 0;
        case WM_CLOSE:
            // 常驻窗口：关掉只是收起来，下次点"新建曲谱"接着用（里面的音符不丢）
            ShowWindow(hwnd_, SW_HIDE);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

void NewSongWindow::CreateChildren() {
    auto makeEdit = [&](int id, int width) {
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, S(width), S(24),
                                    hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                    GetModuleHandleW(nullptr), nullptr);
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(UiFonts().body), TRUE);
        return edit;
    };
    auto makeLabel = [&](const wchar_t* text, int id) {
        HWND label = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0,
                                     hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                     GetModuleHandleW(nullptr), nullptr);
        SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(UiFonts().body), TRUE);
        return label;
    };
    auto makeButton = [&](const wchar_t* text, int id) {
        HWND button = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0,
                                      S(90), S(34), hwnd_,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                      GetModuleHandleW(nullptr), nullptr);
        SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(UiFonts().body), TRUE);
        return button;
    };

    // 标签紧挨着自己的输入框创建：Windows 用 Z 序上紧邻的 STATIC 给控件取可访问名
    nameLabel_ = makeLabel(L"曲名：", 0);
    nameEdit_ = makeEdit(kIdName, 260);
    bpmLabel_ = makeLabel(L"速度：", 0);
    bpmEdit_ = makeEdit(kIdBpm, 70);
    bpmHint_ = makeLabel(L"（每分钟多少拍：填 120 就行）", 0);

    // 音调按钮：1（Z） 2（X） … i（,）
    noteLabel_ = makeLabel(L"音调：", 0);
    for (int i = 0; i < kKeyCount; ++i) {
        const Key key = static_cast<Key>(i);
        const std::string text = KeyNoteName(key) + "（" + KeyName(key) + "）";
        noteButtons_[i] = makeButton(Utf8ToWide(text).c_str(), kIdNoteBase + i);
    }

    modeLabel_ = makeLabel(L"演奏方式：", 0);
    for (int i = 0; i < kPlayModeCount; ++i) {
        const PlayMode mode = static_cast<PlayMode>(i);
        modeButtons_[i] = makeButton(Utf8ToWide(PlayModeButtonName(mode)).c_str(), kIdModeBase + i);
    }

    durationLabel_ = makeLabel(L"时值：", 0);
    for (int i = 0; i < kDurationChoiceCount; ++i) {
        durationButtons_[i] =
            makeButton(Utf8ToWide(kDurationChoices[i].label).c_str(), kIdDurationBase + i);
    }
    // 延时（简谱的 -）：不是"时值档位"，是一个动作按钮——点它把上一个音符延长一拍
    const std::wstring extendText = Utf8ToWide(labels::kExtendAction);
    extendButton_ = makeButton(extendText.c_str(), kIdExtendLast);

    hintLabel_ = makeLabel(
        L"点音调按钮加音符；「延时」把上一个音符延长一拍；表格里点一行选中，再点「编辑选中行」换音或「删除选中行」删掉。",
        kIdHint);

    list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                            0, 0, 0, 0, hwnd_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdList)),
                            GetModuleHandleW(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(UiFonts().body), TRUE);
    struct Column {
        const wchar_t* text;
        int width;
    };
    const Column columns[] = {
        {L"序号", 46}, {L"小节", 46},   {L"拍位置", 66}, {L"起始时间", 78},
        {L"音调", 62}, {L"演奏方式", 78}, {L"时值", 200},
    };
    for (int i = 0; i < static_cast<int>(sizeof(columns) / sizeof(columns[0])); ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<LPWSTR>(columns[i].text);
        column.cx = S(columns[i].width);
        column.iSubItem = i;
        ListView_InsertColumn(list_, i, &column);
    }
    UpdateColumnWidths();   // 时值列按文字实测再定一次宽（"十六分音符 1/4 拍（0.125 秒）"更长）

    saveButton_ = makeButton(L"保存到曲谱列表", kIdSave);
    editSelectedButton_ = makeButton(L"编辑选中行", kIdEditSelected);
    deleteSelectedButton_ = makeButton(L"删除选中行", kIdDeleteSelected);
    clearButton_ = makeButton(L"全部清空", kIdClear);
    closeButton_ = makeButton(L"关闭", kIdClose);
    saveButton_ = saveButton_;   // 语义提示：主按钮是保存
    statusLabel_ = makeLabel(L"", kIdStatus);
}

void NewSongWindow::ApplyDpi(unsigned dpi) {
    dpi_ = dpi;
    SetUiDpi(dpi);
    RebuildUiFonts();
    RebuildThemeBrushes();
    for (HWND control : {nameEdit_, bpmEdit_, list_, hintLabel_, statusLabel_, saveButton_,
                         editSelectedButton_, deleteSelectedButton_, clearButton_, closeButton_,
                         extendButton_}) {
        if (control != nullptr) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(UiFonts().body), TRUE);
        }
    }
    for (HWND button : noteButtons_) {
        SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(UiFonts().body), TRUE);
    }
    for (HWND button : modeButtons_) {
        SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(UiFonts().body), TRUE);
    }
    for (HWND button : durationButtons_) {
        SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(UiFonts().body), TRUE);
    }
    Layout();
}

// 表格列宽：时值列的文字变长了（标准乐理名字 + 秒数），写死的宽度会显示不全。
// 取所有时值档位的**实测**文字宽再定宽，字体/DPI 变了也跟得上。
void NewSongWindow::UpdateColumnWidths() {
    if (list_ == nullptr) {
        return;
    }
    const double bpm = 120.0;   // 只用来估"（0.125 秒）"那一段的宽度，取最长的写法即可
    int widest = S(120);
    for (int i = 0; i < kDurationChoiceCount; ++i) {
        const std::wstring cell = Utf8ToWide(DurationUserCell(kDurationChoices[i].beats, bpm));
        widest = std::max(widest, MeasureTextWidth(UiFonts().body, cell) + S(24));
    }
    // 前面 6 列的固定宽度合计（序号/小节/拍位置/起始时间/音调/演奏方式）
    const int leadingColumns = S(46 + 46 + 66 + 78 + 62 + 78);
    const int listWidth = std::max(S(200), S(kNewSongWindowWidth) - (S(14) + S(10)) * 2);
    const int cap = std::max(S(160), listWidth - leadingColumns - S(20));
    ListView_SetColumnWidth(list_, 6, std::min(widest, cap));
}

void NewSongWindow::Layout() {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    const int margin = S(14);
    const int inner = S(14);
    const int rowLeft = margin + inner;
    const int rowHeight = S(24);
    const int rowsUnit = S(28);
    const int groupHead = S(28);
    const int groupFoot = S(12);
    const int groupGap = S(14);
    const int buttonHeight = S(34);
    const int buttonGap = S(8);
    const int buttonRowPitch = S(34) + S(14);   // 按钮行距 = 按钮高 + 空隙（原来用 S(28) 会挤在一起）
    const int labelWidth = S(72);
    auto move = [](HWND control, int x, int y, int cx, int cy) {
        if (control != nullptr) {
            MoveWindow(control, x, y, cx, cy, TRUE);
        }
    };
    auto groupHeight = [&](int rows) { return groupHead + rows * rowsUnit - S(4) + groupFoot; };

    int y = margin;

    // 曲谱信息（两行：曲名 / 速度）
    infoGroup_ = RECT{margin, y, width - margin, y + groupHeight(2)};
    int rowY = y + groupHead;
    move(nameLabel_, rowLeft, rowY + S(3), S(56), S(20));
    move(nameEdit_, rowLeft + S(56), rowY, S(260), rowHeight);
    rowY += rowsUnit;
    move(bpmLabel_, rowLeft, rowY + S(3), S(56), S(20));
    move(bpmEdit_, rowLeft + S(56), rowY, S(70), rowHeight);
    move(bpmHint_, rowLeft + S(56) + S(78), rowY + S(3), S(260), S(20));
    y += groupHeight(2) + groupGap;

    // 加音符（音调 1 行 + 演奏方式若干行 + 时值若干行 + 延时 1 行 + 一行提示）
    //
    // 行数不再写死：按钮宽度全部由 ui::ButtonWidthForText（文字实测）推出来，
    // 再按可用宽度贪心折行。文案一改（例如时值从 4 档扩到 10 档 + 延时），
    // 行数自动跟着变——不会再把"按"字那种长文案截掉。
    const int available = width - (rowLeft + labelWidth) - margin - inner;

    std::vector<std::wstring> modeLabels;
    modeLabels.reserve(kPlayModeCount);
    for (int i = 0; i < kPlayModeCount; ++i) {
        modeLabels.push_back(Utf8ToWide(PlayModeButtonName(static_cast<PlayMode>(i))));
    }
    // 自由折行会把这一对组合模式拆散（一个跟在单键后面挤第一行、一个独占第二行）：
    // 指定从 PlayMode::LeftMiddle（下标 4）起另起一行 → 4 个单键一行、2 个组合模式一行。
    // 窗口与 ui_test 用同一份下标，断言测的就是窗口真正用的排版。
    const std::vector<int> modeBreaks = {static_cast<int>(PlayMode::LeftMiddle)};
    const ButtonRowPlan modePlan = PlanButtonRows(modeLabels, available, modeBreaks);

    std::vector<std::wstring> durationLabels;
    durationLabels.reserve(kDurationChoiceCount + 1);
    for (int i = 0; i < kDurationChoiceCount; ++i) {
        durationLabels.push_back(Utf8ToWide(kDurationChoices[i].label));
    }
    // 「延时」（简谱的 -）跟在时值按钮后面排（它是"把上一个音符延长一拍"，属于时值一类的动作）；
    // 这样能填满时值最后一行的空位，不用为它单独占一行（窗口别长太高）。
    durationLabels.push_back(Utf8ToWide(labels::kExtendAction));
    const ButtonRowPlan durationPlan = PlanButtonRows(durationLabels, available);

    const int modeRows = std::max(1, modePlan.rowCount);
    const int durationRows = std::max(1, durationPlan.rowCount);
    const int pickGroupHeight =
        groupHead + buttonRowPitch * (1 + modeRows + durationRows) + rowsUnit + groupFoot;
    pickGroup_ = RECT{margin, y, width - margin, y + pickGroupHeight};
    rowY = y + groupHead;
    {
        move(noteLabel_, rowLeft, rowY + S(5), S(64), S(20));
        // 8 个音调按钮排一行：等分（整齐好看），但不得窄于文字实测宽度（"1（Z）"也要放得下）
        int widest = S(52);
        for (const HWND button : noteButtons_) {
            wchar_t text[64] = {};
            GetWindowTextW(button, text, 64);
            widest = std::max(widest, ButtonWidthForText(text));
        }
        const int buttonWidth =
            std::max(widest, (available - buttonGap * (kKeyCount - 1)) / kKeyCount);
        int x = rowLeft + labelWidth;
        for (int i = 0; i < kKeyCount; ++i) {
            move(noteButtons_[i], x, rowY, buttonWidth, buttonHeight);
            x += buttonWidth + buttonGap;
        }
    }
    rowY += buttonRowPitch;
    {
        // 演奏方式按钮：宽度 = 文字实测宽度 + 内边距，按可用宽度折行
        move(modeLabel_, rowLeft, rowY + S(5), S(64), S(20));
        std::vector<int> rowX(static_cast<size_t>(modePlan.rowCount), 0);
        for (int i = 0; i < kPlayModeCount; ++i) {
            const int row = modePlan.rows[static_cast<size_t>(i)];
            const int cx = modePlan.widths[static_cast<size_t>(i)];
            move(modeButtons_[i], rowLeft + labelWidth + rowX[static_cast<size_t>(row)],
                 rowY + row * buttonRowPitch, cx, buttonHeight);
            rowX[static_cast<size_t>(row)] += cx + buttonGap;
        }
    }
    rowY += buttonRowPitch * modeRows;
    {
        // 时值按钮 + 「延时」：同样是实测宽度 + 折行（标准时值名字更长：全音符 / 十六分音符 / 附点八分…）
        move(durationLabel_, rowLeft, rowY + S(5), S(64), S(20));
        std::vector<int> rowX(static_cast<size_t>(durationPlan.rowCount), 0);
        for (int i = 0; i < kDurationChoiceCount; ++i) {
            const int row = durationPlan.rows[static_cast<size_t>(i)];
            const int cx = durationPlan.widths[static_cast<size_t>(i)];
            move(durationButtons_[i], rowLeft + labelWidth + rowX[static_cast<size_t>(row)],
                 rowY + row * buttonRowPitch, cx, buttonHeight);
            rowX[static_cast<size_t>(row)] += cx + buttonGap;
        }
        // 延时排在时值按钮之后（同一套排版，最后一个）
        const size_t extendIndex = static_cast<size_t>(kDurationChoiceCount);
        const int row = durationPlan.rows[extendIndex];
        const int cx = durationPlan.widths[extendIndex];
        move(extendButton_, rowLeft + labelWidth + rowX[static_cast<size_t>(row)],
             rowY + row * buttonRowPitch, cx, buttonHeight);
        rowX[static_cast<size_t>(row)] += cx + buttonGap;
    }
    rowY += buttonRowPitch * durationRows;
    move(hintLabel_, rowLeft, rowY + S(2), std::max(S(120), width - rowLeft - margin - inner), S(20));
    y += pickGroupHeight + groupGap;

    // 已加的音符
    const int listHeight = S(238);
    listGroup_ = RECT{margin, y, width - margin, y + listHeight};
    move(list_, margin + S(10), y + S(26), std::max(S(200), width - (margin + S(10)) * 2),
         listHeight - S(36));
    y += listHeight + groupGap;

    // 底部按钮与状态
    int x = margin;
    move(saveButton_, x, y, S(140), buttonHeight);
    x += S(140) + buttonGap;
    move(editSelectedButton_, x, y, S(110), buttonHeight);
    x += S(110) + buttonGap;
    move(deleteSelectedButton_, x, y, S(110), buttonHeight);
    x += S(110) + buttonGap;
    move(clearButton_, x, y, S(90), buttonHeight);
    x += S(90) + buttonGap;
    move(closeButton_, x, y, S(76), buttonHeight);
    y += buttonHeight + S(10);
    // 状态行也可能是长句子（"正在编辑第 N 个音符（现在是 …）：点音调按钮把它换成别的音 …"）：
    // 给两行高，SS_LEFT 会在宽度内自动折行；窗口高度跟着内容走，不用手算
    move(statusLabel_, margin, y, std::max(S(120), width - margin * 2), S(57));
    y += S(57) + margin;

    // 窗口高度按内容定（改完客户区正好等于 y，下一轮算出来相同，不来回抖）
    if (height != y) {
        RECT desired{0, 0, width, y};
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
        AdjustWindowRectForDpi(desired, style, false);
        SetWindowPos(hwnd_, nullptr, 0, 0, desired.right - desired.left, desired.bottom - desired.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void NewSongWindow::OnCommand(int id, int notifyCode, HWND control) {
    if (id >= kIdNoteBase && id < kIdNoteBase + kKeyCount) {
        AddNote(static_cast<Key>(id - kIdNoteBase));
        return;
    }
    if (id >= kIdModeBase && id < kIdModeBase + kPlayModeCount) {
        mode_ = static_cast<PlayMode>(id - kIdModeBase);
        InvalidatePickers();
        SetStatus(std::string("演奏方式改成：") + PlayModeButtonName(mode_), false);
        return;
    }
    if (id >= kIdDurationBase && id < kIdDurationBase + kDurationChoiceCount) {
        durationChoice_ = id - kIdDurationBase;
        InvalidatePickers();
        if (kDurationChoices[durationChoice_].rest) {
            // 点休止档（休止两拍 / 休止一拍 / 休止半拍）= 立刻往列表里加一条休止
            // （用户要求"选中就能看到"；休止只占时间、不出声）
            AddNote(Key::Z);
        } else {
            SetStatus(std::string("时值改成：") + kDurationChoices[durationChoice_].label + "。",
                      false);
        }
        return;
    }
    if (id == kIdExtendLast) {
        ExtendLastNote();
        return;
    }
    if ((id == kIdName || id == kIdBpm) && notifyCode == EN_CHANGE) {
        RefreshList();   // 曲名/速度一变，表格里的小节与时间跟着重算
        return;
    }
    if (id == kIdEditSelected) {
        const int selected = SelectedRow();
        if (editingRow_ >= 0) {
            // 再点一次 = 退出编辑（免得用户不知道自己在编辑哪一行）
            editingRow_ = -1;
            InvalidateRect(editSelectedButton_, nullptr, TRUE);
            SetStatus("退出编辑，接下来点音调按钮就是往后接着加音符。", false);
            return;
        }
        if (selected < 0) {
            SetStatus("先在下面的表格里点一行，选中它才能编辑。", true);
            return;
        }
        editingRow_ = selected;
        const EditorRow& row = rows_[static_cast<size_t>(selected)];
        mode_ = row.mode;
        const int index = DurationChoiceIndexFor(row.durationBeats);
        if (index >= 0) {
            durationChoice_ = index;
        }
        InvalidatePickers();
        InvalidateRect(editSelectedButton_, nullptr, TRUE);
        RefreshList();
        SelectRow(selected);
        SetStatus("正在编辑第 " + std::to_string(selected + 1) + " 个音符（现在是 " +
                      KeyNoteName(row.key) + "（" + KeyName(row.key) + "）· " +
                      ShortModeName(row.mode) + " · " + DurationUserCell(row.durationBeats, Context().bpm) +
                      "）：点音调按钮把它换成别的音；想改方式/时值就先点上面的按钮再点音调按钮。",
                  false);
        return;
    }
    if (id == kIdDeleteSelected) {
        const int selected = SelectedRow();
        if (selected < 0) {
            SetStatus("先在下面的表格里点一行，选中它才能删除。", true);
            return;
        }
        const double removedBeats = rows_[static_cast<size_t>(selected)].durationBeats;
        rows_.erase(rows_.begin() + selected);
        // 后面的音符整体往前挪（保持间隔一致；用户加的休止也一并跟着挪）
        for (size_t j = static_cast<size_t>(selected); j < rows_.size(); ++j) {
            rows_[j].beat -= removedBeats;
            if (rows_[j].beat < 0.0) {
                rows_[j].beat = 0.0;
            }
        }
        cursorBeats_ = std::max(0.0, cursorBeats_ - removedBeats);
        if (editingRow_ == selected) {
            editingRow_ = -1;
            InvalidateRect(editSelectedButton_, nullptr, TRUE);
        } else if (editingRow_ > selected) {
            --editingRow_;
        }
        RefreshList();
        const int next = std::min(selected, static_cast<int>(rows_.size()) - 1);
        if (next >= 0) {
            SelectRow(next);
        }
        SetStatus("删掉第 " + std::to_string(selected + 1) + " 个音符，后面的往前挪，现在还有 " +
                      std::to_string(rows_.size()) + " 个。",
                  false);
        return;
    }
    if (id == kIdClear) {
        rows_.clear();
        cursorBeats_ = 0.0;
        editingRow_ = -1;
        InvalidateRect(editSelectedButton_, nullptr, TRUE);
        RefreshList();
        SetStatus("清空了，从头开始加吧。", false);
        return;
    }
    if (id == kIdSave) {
        SaveToSongs();
        return;
    }
    if (id == kIdClose) {
        ShowWindow(hwnd_, SW_HIDE);
        return;
    }
    (void)control;
}

int NewSongWindow::SelectedRow() const {
    if (list_ == nullptr) {
        return -1;
    }
    return static_cast<int>(SendMessageW(list_, LVM_GETNEXTITEM, static_cast<WPARAM>(-1),
                                         LVNI_SELECTED));
}

void NewSongWindow::SelectRow(int index) {
    if (list_ == nullptr || index < 0 || static_cast<size_t>(index) >= rows_.size()) {
        return;
    }
    LVITEMW item{};
    item.mask = LVIF_STATE;
    item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    item.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(list_, LVM_SETITEMSTATE, static_cast<WPARAM>(index),
                 reinterpret_cast<LPARAM>(&item));
    ListView_EnsureVisible(list_, index, FALSE);
}

void NewSongWindow::AddNote(Key key) {
    const int index = std::min(std::max(durationChoice_, 0), kDurationChoiceCount - 1);
    const DurationChoice choice = kDurationChoices[index];

    if (editingRow_ >= 0 && static_cast<size_t>(editingRow_) < rows_.size()) {
        // 编辑选中行：点音调按钮 = 把选中的那一行换成这个音（休止档对"换音"没意义，沿用原时值）
        EditorRow& row = rows_[static_cast<size_t>(editingRow_)];
        const int edited = editingRow_;
        const double newDuration = choice.beats;
        const double delta = newDuration - row.durationBeats;
        row.rest = choice.rest;   // 时值选"休止"= 把这行变成休止行；点音调 = 变回音符行
        row.key = key;
        row.mode = mode_;
        row.durationBeats = newDuration;
        editingRow_ = -1;
        if (delta < -1e-9 || delta > 1e-9) {
            for (size_t j = static_cast<size_t>(edited) + 1; j < rows_.size(); ++j) {
                rows_[j].beat += delta;
            }
            cursorBeats_ = std::max(0.0, cursorBeats_ + delta);
        }
        InvalidateRect(editSelectedButton_, nullptr, TRUE);
        RefreshList();
        SelectRow(edited);
        if (row.rest) {
            SetStatus("已把第 " + std::to_string(edited + 1) + " 项改成休止（不出声，占 " +
                          DurationUserCell(newDuration, Context().bpm) + "），后面的音符跟着往后挪。",
                      false);
        } else {
            SetStatus("已把第 " + std::to_string(edited + 1) + " 项改成 " + KeyNoteName(key) + "（" +
                          KeyName(key) + "）· " + ShortModeName(mode_) + " · " +
                          DurationUserCell(newDuration, Context().bpm) + "。",
                      false);
        }
        return;
    }

    if (choice.rest) {
        // 休止也是一"项"：进列表看得见，后面的音符跟着往后挪；演奏时它不出声、只占时间
        EditorRow rest;
        rest.rest = true;
        rest.beat = cursorBeats_;
        rest.durationBeats = choice.beats;
        rows_.push_back(rest);
        cursorBeats_ += choice.beats;
        RefreshList();
        SelectRow(static_cast<int>(rows_.size()) - 1);
        SetStatus(std::string("加了一个休止（") + choice.label + "，不出声、只占时间），列表里能看到它；" +
                      "下一个音符从第 " + FormatBeats(cursorBeats_) +
                      " 拍开始。想把它换成音符：选中它、点「编辑选中行」，再点音调按钮。",
                  false);
        return;
    }

    EditorRow row;
    row.key = key;
    row.mode = mode_;
    row.beat = cursorBeats_;
    row.durationBeats = choice.beats;
    rows_.push_back(row);
    cursorBeats_ += choice.beats;

    RefreshList();
    const int last = static_cast<int>(rows_.size()) - 1;
    ListView_EnsureVisible(list_, last, FALSE);
    SetStatus(std::string("加了 ") + KeyNoteName(key) + "（" + KeyName(key) + "）· " +
                  ShortModeName(mode_) + " · " + choice.label + "，现在共 " +
                  std::to_string(rows_.size()) + " 个音符 / " + FormatBeats(cursorBeats_) + " 拍。",
              false);
}

void NewSongWindow::ExtendLastNote() {
    // 「延时」= 简谱里的 "-"：把上一个音符再唱一拍。模型里仍是**一个**音符（duration + 1 拍），
    // 不新增行、不拆成两个音符——所以表格里的行数不变，行上的时值变长。
    if (rows_.empty()) {
        SetStatus("表格里还没有音符：延时（简谱的 -）是把上一个音符延长一拍，先点音调按钮加一个音符。",
                  true);
        return;
    }
    const int index = ExtendLastRow(rows_, &cursorBeats_);
    const EditorRow& row = rows_[static_cast<size_t>(index)];
    RefreshList();
    SelectRow(index);
    SetStatus("已把第 " + std::to_string(index + 1) + " 个" +
                  (row.rest ? std::string("休止") : std::string("音符")) + "延长了一拍（现在是 " +
                  DurationUserCell(row.durationBeats, Context().bpm) +
                  "，下一个音符从第 " + FormatBeats(cursorBeats_) + " 拍接上）。",
              false);
}

void NewSongWindow::RefreshList() {
    if (list_ == nullptr) {
        return;
    }
    ListView_DeleteAllItems(list_);
    const Song context = Context();
    for (size_t i = 0; i < rows_.size(); ++i) {
        const EditorRow& row = rows_[i];
        const EditorRowText text = DescribeRow(row, context);
        // 休止行：音调列写"休止"、演奏方式列写"—"（它不按键，只是占时间）
        const std::string note =
            row.rest ? std::string("休止")
                     : KeyNoteName(row.key) + "（" + KeyName(row.key) + "）";
        const std::string modeText = row.rest ? std::string("—") : ShortModeName(row.mode);
        const std::wstring cells[] = {
            Utf8ToWide(std::to_string(i + 1)), Utf8ToWide(text.bar),  Utf8ToWide(text.beat),
            Utf8ToWide(text.time),             Utf8ToWide(note),      Utf8ToWide(modeText),
            Utf8ToWide(DurationUserCell(row.durationBeats, context.bpm)),
        };
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(cells[0].c_str());
        const int inserted = ListView_InsertItem(list_, &item);
        for (int column = 1; column < 7; ++column) {
            ListView_SetItemText(list_, inserted, column, const_cast<LPWSTR>(cells[column].c_str()));
        }
    }
}

void NewSongWindow::SetStatus(const std::string& text, bool error) {
    statusIsError_ = error;
    if (statusLabel_ == nullptr) {
        return;
    }
    // 长句子自己折行（STATIC 对中文长句不折行，字多就被切掉）：最多三行
    RECT statusRect{};
    GetClientRect(statusLabel_, &statusRect);
    const double charsPerLine =
        statusRect.right > statusRect.left
            ? static_cast<double>(statusRect.right - statusRect.left) / static_cast<double>(S(14))
            : 60.0;
    SetWindowTextW(statusLabel_, Utf8ToWide(WrapLabelText(text, charsPerLine, 3)).c_str());
    // 状态用文字说清楚 + 用颜色区分：错误红色、正常深灰（不刷屏：只是一行文字）
    HDC dc = GetDC(hwnd_);
    if (dc != nullptr) {
        const COLORREF color = error ? RGB(176, 42, 42) : RGB(70, 78, 88);
        const HBRUSH old = static_cast<HBRUSH>(SelectObject(dc, GetStockObject(NULL_BRUSH)));
        SetTextColor(dc, color);
        ReleaseDC(hwnd_, dc);
        (void)old;
    }
    InvalidateRect(statusLabel_, nullptr, TRUE);
}

Song NewSongWindow::Context() const {
    double bpm = 120.0;
    std::string ignored;
    if (bpmEdit_ != nullptr) {
        const std::string text = ReadText(bpmEdit_);
        if (!text.empty() && !ParseBpmText(text, bpm, ignored)) {
            bpm = 120.0;   // 输到一半的数字先按 120 预览，保存时再拦
        }
    }
    const std::string name = nameEdit_ != nullptr ? ReadText(nameEdit_) : std::string();
    return DescribeContext(name, bpm, frameBeatUnit_, frameSignature_);
}

bool NewSongWindow::BuildSong(Song& out, std::string& error) {
    const std::string name = ReadText(nameEdit_);
    if (name.empty()) {
        error = "曲名不能为空：给它起个名字（比如「小星星」）再保存。";
        return false;
    }
    double bpm = 0.0;
    if (!ParseBpmText(ReadText(bpmEdit_), bpm, error)) {
        return false;
    }
    if (rows_.empty()) {
        error = "里面一个音符都没有：先点上面的音调按钮加几个。";
        return false;
    }
    out = BuildSongFromRows(name, bpm, frameBeatUnit_, frameSignature_, rows_);
    if (out.notes.empty()) {
        error = "音符算出来是空的，检查一下有没有只放了休止符。";
        return false;
    }
    return true;
}

void NewSongWindow::SaveToSongs() {
    Song song;
    std::string error;
    if (!BuildSong(song, error)) {
        SetStatus(error, true);
        return;
    }
    if (!editPath_.empty()) {
        // 编辑已有曲谱：不问文件名，直接覆盖它（用户要的就是"改完保存"）
        std::string message;
        if (app_.SaveSongInPlace(song, editPath_, &message)) {
            SetStatus("已保存，覆盖了原曲谱：" + message + "　关掉窗口、按 F6 就能演奏。", false);
            ShowWindow(hwnd_, SW_HIDE);
        } else {
            SetStatus(message.empty() ? "保存失败。" : message, true);
        }
        return;
    }
    std::string message;
    if (app_.SaveSongAsNew(song, &message)) {
        SetStatus("已保存：" + message + "　关掉这个窗口、按 F6 就能演奏。", false);
        ShowWindow(hwnd_, SW_HIDE);   // 存完就收起来，用户直接去按 F6
    } else if (message.empty()) {
        SetStatus("没有保存：你可能在文件对话框里点了取消。再点一次「保存到曲谱列表」即可。", true);
    } else {
        SetStatus(message, true);
    }
}

void NewSongWindow::InvalidatePickers() {
    for (HWND button : modeButtons_) {
        InvalidateRect(button, nullptr, TRUE);
    }
    for (HWND button : durationButtons_) {
        InvalidateRect(button, nullptr, TRUE);
    }
}

void NewSongWindow::OnDrawItem(const DRAWITEMSTRUCT* item) {
    if (item == nullptr) {
        return;
    }
    const int id = static_cast<int>(item->CtlID);
    ButtonPaintInfo info;
    info.enabled = (item->itemState & ODS_DISABLED) == 0;
    info.kind = ButtonKind::Neutral;
    if (id >= kIdNoteBase && id < kIdNoteBase + kKeyCount) {
        const Key key = static_cast<Key>(id - kIdNoteBase);
        info.kind = ButtonKind::Secondary;
        info.text = Utf8ToWide(KeyNoteName(key) + "（" + KeyName(key) + "）");
    } else if (id >= kIdModeBase && id < kIdModeBase + kPlayModeCount) {
        const PlayMode mode = static_cast<PlayMode>(id - kIdModeBase);
        info.kind = (mode == mode_) ? ButtonKind::Primary : ButtonKind::Neutral;
        info.text = Utf8ToWide(PlayModeButtonName(mode));
    } else if (id >= kIdDurationBase && id < kIdDurationBase + kDurationChoiceCount) {
        const int index = id - kIdDurationBase;
        info.kind = (index == durationChoice_) ? ButtonKind::Primary : ButtonKind::Neutral;
        info.text = Utf8ToWide(kDurationChoices[index].label);
    } else if (id == kIdSave) {
        info.kind = ButtonKind::Primary;
        info.text = L"保存到曲谱列表";
    } else if (id == kIdEditSelected) {
        info.kind = (editingRow_ >= 0) ? ButtonKind::Primary : ButtonKind::Secondary;
        info.text = L"编辑选中行";
    } else if (id == kIdDeleteSelected) {
        info.text = L"删除选中行";
    } else if (id == kIdExtendLast) {
        info.text = Utf8ToWide(labels::kExtendAction);
    } else if (id == kIdClear) {
        info.text = L"全部清空";
    } else if (id == kIdClose) {
        info.text = L"关闭";
    } else {
        return;
    }
    PaintFlatButton(*item, info);
}

void NewSongWindow::OnPaint() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd_, &paint);
    RECT client{};
    GetClientRect(hwnd_, &client);
    FillRect(dc, &client, BackgroundBrush());
    if (infoGroup_.bottom > infoGroup_.top) {
        PaintGroupFrame(dc, infoGroup_, L"曲谱信息");
    }
    if (pickGroup_.bottom > pickGroup_.top) {
        PaintGroupFrame(dc, pickGroup_, L"加音符（点一下加一个）");
    }
    if (listGroup_.bottom > listGroup_.top) {
        PaintGroupFrame(dc, listGroup_, L"已加的音符");
    }
    EndPaint(hwnd_, &paint);
}

void NewSongWindow::Show() {
    // 让"曲名/速度/音调/方式/时值"的当前值一眼可见（首次打开时 Reset 已经填好）
    if (rows_.empty() && GetWindowTextLengthW(statusLabel_) == 0) {
        SetStatus("新曲谱：点下面的音调按钮加音符，加完点「保存到曲谱列表」。", false);
    }
    ApplyDpi(dpi_ == 0 ? GetDpiForWindow(hwnd_) : dpi_);
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    RefreshList();
}

}  // namespace ui
}  // namespace hp
