#include "ui/SettingsWindow.h"

#include <algorithm>

#include <commctrl.h>

#include <imm.h>

#pragma comment(lib, "imm32.lib")   // ImmAssociateContext：给捕获框关掉输入法

#include "hotkey/HotkeyBinding.h"
#include "song/BeatMath.h"
#include "ui/App.h"
#include "ui/UiFormat.h"

namespace hp {
namespace ui {

namespace {

// 底部固定区（按钮行 + 说明两行）的高度：Layout 与 OnPaint 都要用
static int BottomBlockHeight() {
    return S(158);   // 按钮行 + 说明两行 + 最下面那句（长句可能占三行，给足）
}

constexpr int kIdPlayHotkey = 2001;
constexpr int kIdStopHotkey = 2002;
constexpr int kIdRestartHotkey = 2003;
constexpr int kIdPreviousHotkey = 2004;
constexpr int kIdNextHotkey = 2005;
constexpr int kIdPauseHotkey = 2100;   // 新开一段编号：2001-2031 已经用满了
// 这三行的编号原来是 2004/2005/2006，和「上一首/下一首」热键输入框撞了：在输入框里打字
// 会发出 id 相同的 EN_CHANGE，被那条 case 收到。挪到 2101-2103，别复用。
constexpr int kIdKeyGuard = 2101;
constexpr int kIdMouseButtonGuard = 2102;
constexpr int kIdMouseMoveGuard = 2103;
constexpr int kIdMouseWheelGuard = 2108;   // 复选框：鼠标滚轮也能打断演奏（默认开）
constexpr int kIdSpeed = 2007;
constexpr int kIdSpeedCombo = 2021;   // Phase 8：50/75/100/125/150/200% 六档 + 自定义
constexpr int kIdDryRun = 2008;
constexpr int kIdNatural = 2009;
constexpr int kIdForegroundIgnore = 2010;
constexpr int kIdForegroundPause = 2011;
constexpr int kIdForegroundStop = 2012;
constexpr int kIdApply = 2013;
constexpr int kIdCancel = 2014;
constexpr int kIdDefaults = 2015;
constexpr int kIdRunAsAdmin = 2030;      // 复选框：以管理员身份运行
constexpr int kIdExclusiveHotkeys = 2032;  // 复选框：热键独占（默认关闭 = 放行）
constexpr int kIdElevateNow = 2031;      // 按钮：立即以管理员身份重启
constexpr int kIdIgnoreField = 2104;    // 排除键位：按下即加入
constexpr int kIdIgnoreList = 2105;
constexpr int kIdIgnoreRemove = 2106;
constexpr int kIdIgnoreClear = 2107;
constexpr int kIdMessage = 2016;
constexpr int kIdHint = 2017;

const wchar_t* kWindowClass = L"HarmonicaPlayerSettings";
const wchar_t* kGroupHotkeys = L"全局热键";
const wchar_t* kGroupGuard = L"用户输入急停";
const wchar_t* kGroupPlayback = L"播放选项";
const wchar_t* kGroupForeground = L"前台窗口变化";
const wchar_t* kGroupIgnoreKeys = L"排除键位（按下不打断演奏）";
const wchar_t* kIgnoreLabel = L"按键添加：";
const wchar_t* kIgnoreHint =
    L"列表里的键按下时不会打断演奏（游戏里的走路 / 跳跃键放这里）；\n鼠标键、鼠标滚轮也用上面的复选框（滚轮默认就是打断演奏的）。";
const wchar_t* kHotkeyPlay = L"播放：";
const wchar_t* kHotkeyPause = L"暂停：";
const wchar_t* kHotkeyStop = L"停止：";
const wchar_t* kHotkeyRestart = L"重播：";
const wchar_t* kHotkeyPrevious = L"上一首：";
const wchar_t* kHotkeyNext = L"下一首：";
const wchar_t* kKeyGuardText = L"键盘按键也能打断演奏（任意键急停）";
const wchar_t* kMouseButtonText = L"鼠标按键也能打断演奏";
const wchar_t* kMouseMoveText = L"鼠标移动也能打断演奏（默认关闭：游戏里视角一动就停）";
const wchar_t* kSpeedText = L"播放速度：";
const wchar_t* kDryRunText = L"干跑：只走时间轴，不真的按键（排练用）";
const wchar_t* kNaturalText = L"自然模式：每个音符叠加 3 ms 以内的随机抖动";
const wchar_t* kRunAsAdminText = L"以管理员身份运行（游戏以管理员身份运行时必须勾选）";
const wchar_t* kElevateNowText = L"立即以管理员身份重启";
const wchar_t* kForegroundIgnore = L"不处理（继续演奏）";
const wchar_t* kForegroundPause = L"暂停（切回游戏窗口后自动继续）";
const wchar_t* kForegroundStop = L"停止";

std::string ReadText(HWND edit) {
    const int length = GetWindowTextLengthW(edit);
    if (length <= 0) {
        return std::string();
    }
    std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(edit, buffer.data(), length + 1);
    buffer.resize(static_cast<size_t>(length));
    return WideToUtf8(buffer);
}

}  // namespace

SettingsWindow::SettingsWindow(App& app) : app_(app) {}

SettingsWindow::~SettingsWindow() = default;

bool SettingsWindow::Create(HINSTANCE instance, std::string* error) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &SettingsWindow::WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    windowClass.lpszClassName = kWindowClass;
    windowClass.hbrBackground = nullptr;
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        if (error != nullptr) {
            *error = "注册设置窗口类失败（错误码 " + std::to_string(GetLastError()) + "）";
        }
        return false;
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VSCROLL;
    // 客户区高度按内容算（用户输入急停分组现在是 4 行：键盘 / 鼠标按键 / 鼠标移动 / 鼠标滚轮）
    // = 620 DIP 上下，取 620 留一点余量；尺寸固定，不允许拉伸（拉扁了单选按钮会跑出窗口）。
    // 真正的页面高度由 App 用主窗口客户区高度告知（SetPageClientHeight），放不下的靠滚动条看。
    RECT rect{0, 0, S(560), S(620)};
    AdjustWindowRectForDpi(rect, style, false);
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"设置", style, CW_USEDEFAULT,
                            CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                            nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        if (error != nullptr) {
            *error = "创建设置窗口失败（错误码 " + std::to_string(GetLastError()) + "）";
        }
        return false;
    }
    return true;
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    SettingsWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<SettingsWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->HandleMessage(message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT SettingsWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            ApplyDpi(GetDpiForWindow(hwnd_));
            CreateChildren();
            Layout();
            return 0;
        case WM_SIZE:
            Layout();
            return 0;
        case WM_COMMAND:
            OnCommand(LOWORD(wparam));
            return 0;
        case WM_DRAWITEM:
            OnDrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
            return TRUE;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            HWND control = reinterpret_cast<HWND>(lparam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, ToColorRef(control == messageLabel_ && messageIsError_
                                            ? kColorRed
                                            : (control == hintLabel_ ? kColorMuted : kColorText)));
            return reinterpret_cast<LRESULT>(BackgroundBrush());
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetBkColor(dc, ToColorRef(kColorPanel));
            SetTextColor(dc, ToColorRef(kColorText));
            static HBRUSH editBrush = CreateSolidBrush(ToColorRef(kColorPanel));
            return reinterpret_cast<LRESULT>(editBrush);
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            OnPaint();
            return 0;
        case WM_DPICHANGED:
            ApplyDpi(LOWORD(wparam));
            Layout();
            return 0;
        case WM_VSCROLL: {
            RECT client{};
            GetClientRect(hwnd_, &client);
            const int line = S(24);
            int page = (client.bottom - client.top) - BottomBlockHeight() - line;
            if (page < S(40)) {
                page = S(40);
            }
            int target = scrollY_;
            switch (LOWORD(wparam)) {
                case SB_LINEUP: target -= line; break;
                case SB_LINEDOWN: target += line; break;
                case SB_PAGEUP: target -= page; break;
                case SB_PAGEDOWN: target += page; break;
                case SB_TOP: target = 0; break;
                case SB_BOTTOM: target = MaxScroll(); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: {
                    SCROLLINFO info{};
                    info.cbSize = sizeof(info);
                    info.fMask = SIF_TRACKPOS;
                    if (GetScrollInfo(hwnd_, SB_VERT, &info) != FALSE) {
                        target = info.nTrackPos;
                    }
                    break;
                }
                default:
                    return 0;
            }
            ScrollTo(target);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            // 指针压在能自己滚的控件上（排除键列表 / 下拉框）时，让控件自己滚
            const POINT pt{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            HWND under = WindowFromPoint(pt);
            if (under != nullptr && under != hwnd_) {
                wchar_t cls[64] = L"";
                GetClassNameW(under, cls, 64);
                // 只有真能自己滚的控件才转交；只读编辑框之类没有可滚内容，
                // 压在它们上面滚轮应该滚页面（实测压在热键框上滚不动，就是这个原因）
                if (lstrcmpiW(cls, L"ListBox") == 0 || lstrcmpiW(cls, L"ComboBox") == 0) {
                    SendMessageW(under, WM_MOUSEWHEEL, wparam, lparam);
                    return 0;
                }
            }
            const int notches = static_cast<short>(HIWORD(wparam)) / WHEEL_DELTA;
            ScrollTo(scrollY_ - notches * S(72));
            return 0;
        }
        case WM_CLOSE:
            app_.SetHotkeyCaptureActive(false);   // 关窗时若还在录键，把热键注册恢复
            ShowWindow(hwnd_, SW_HIDE);           // 设置窗口只是隐藏，下次打开更快
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

void SettingsWindow::CreateChildren() {
    const HINSTANCE instance = app_.Instance();

    auto makeLabel = [&](const wchar_t* text, int id) {
        HWND control = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       0, 0, 0, 0, hwnd_,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance,
                                       nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(UiFonts().small), TRUE);
        return control;
    };
    auto makeEdit = [&](const wchar_t* text, int id) {
        HWND control = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
                                       WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL, 0, 0, 0, 0,
                                       hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                       instance, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(UiFonts().body), TRUE);
        return control;
    };
    auto makeCheck = [&](const wchar_t* text, int id) {
        HWND control = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                       0, 0, 0, 0, hwnd_,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance,
                                       nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(UiFonts().body), TRUE);
        return control;
    };
    auto makeRadio = [&](const wchar_t* text, int id) {
        HWND control = CreateWindowExW(0, L"BUTTON", text,
                                       WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP, 0, 0, 0,
                                       0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                       instance, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(UiFonts().body), TRUE);
        return control;
    };
    auto makeButton = [&](const wchar_t* text, int id) {
        HWND control = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                       0, 0, 0, 0, hwnd_,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance,
                                       nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(UiFonts().body), TRUE);
        return control;
    };

    // 注意：标签必须紧挨着自己的输入框创建。Windows 给编辑框取可访问名时，用的是
    // Z 序上紧邻它的那个静态控件；先把三个标签全建完再建编辑框，会让三个编辑框
    // 共用最后一个标签的名字（屏幕阅读器会读错），这里成对创建。
    playLabel_ = makeLabel(kHotkeyPlay, 0);
    playHotkey_ = makeEdit(L"", kIdPlayHotkey);
    pauseLabel_ = makeLabel(kHotkeyPause, 0);
    pauseHotkey_ = makeEdit(L"", kIdPauseHotkey);
    stopLabel_ = makeLabel(kHotkeyStop, 0);
    stopHotkey_ = makeEdit(L"", kIdStopHotkey);
    restartLabel_ = makeLabel(kHotkeyRestart, 0);
    restartHotkey_ = makeEdit(L"", kIdRestartHotkey);
    previousLabel_ = makeLabel(kHotkeyPrevious, 0);
    previousHotkey_ = makeEdit(L"", kIdPreviousHotkey);
    nextLabel_ = makeLabel(kHotkeyNext, 0);
    nextHotkey_ = makeEdit(L"", kIdNextHotkey);
    speedLabel_ = makeLabel(kSpeedText, 0);
    // Phase 8（需求书二十九节）：六档预置 + 自定义，编辑框始终是唯一真值来源
    speedCombo_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                  WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0,
                                  S(110), S(200), hwnd_,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSpeedCombo)),
                                  instance, nullptr);
    SendMessageW(speedCombo_, WM_SETFONT, reinterpret_cast<WPARAM>(UiFonts().body), TRUE);
    for (int i = 0; i < beat::kSpeedPresetCount; ++i) {
        const std::wstring text = Utf8ToWide(beat::SpeedPresetText(i));
        SendMessageW(speedCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    {
        const std::wstring custom = Utf8ToWide(labels::kCustomSpeed);
        SendMessageW(speedCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(custom.c_str()));
    }
    speedEdit_ = makeEdit(L"", kIdSpeed);
    for (HWND label : {playLabel_, stopLabel_, restartLabel_, speedLabel_}) {
        SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(UiFonts().body), TRUE);
    }
    ignoreKeyLabel_ = makeLabel(kIgnoreLabel, 0);
    ignoreKeyField_ = makeEdit(L"", kIdIgnoreField);
    ignoreList_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdIgnoreList)), instance, nullptr);
    SendMessageW(ignoreList_, WM_SETFONT, reinterpret_cast<WPARAM>(UiFonts().body), TRUE);
    ignoreRemoveButton_ = makeButton(L"删除选中", kIdIgnoreRemove);
    ignoreClearButton_ = makeButton(L"清空", kIdIgnoreClear);
    ignoreHint_ = makeLabel(kIgnoreHint, 0);
    keyGuard_ = makeCheck(kKeyGuardText, kIdKeyGuard);
    mouseButtonGuard_ = makeCheck(kMouseButtonText, kIdMouseButtonGuard);
    mouseMoveGuard_ = makeCheck(kMouseMoveText, kIdMouseMoveGuard);
    // 文案取自 labels（ui_test 的“禁 emoji / 中文”守卫要扫得到这一条）
    mouseWheelGuard_ = makeCheck(Utf8ToWide(labels::kMouseWheelGuard).c_str(), kIdMouseWheelGuard);
    dryRunCheck_ = makeCheck(kDryRunText, kIdDryRun);
    naturalCheck_ = makeCheck(kNaturalText, kIdNatural);
    runAsAdminCheck_ = makeCheck(kRunAsAdminText, kIdRunAsAdmin);
    exclusiveHotkeysCheck_ = makeCheck(L"热键独占（会把该键从游戏手里抢走）", kIdExclusiveHotkeys);
    foregroundIgnore_ = makeRadio(kForegroundIgnore, kIdForegroundIgnore);
    foregroundPause_ = makeRadio(kForegroundPause, kIdForegroundPause);
    foregroundStop_ = makeRadio(kForegroundStop, kIdForegroundStop);
    applyButton_ = makeButton(Utf8ToWide(labels::kApply).c_str(), kIdApply);
    cancelButton_ = makeButton(Utf8ToWide(labels::kCancel).c_str(), kIdCancel);
    defaultButton_ = makeButton(Utf8ToWide(labels::kRestoreDefaults).c_str(), kIdDefaults);
    elevateButton_ = makeButton(kElevateNowText, kIdElevateNow);
    messageLabel_ = makeLabel(L"", kIdMessage);
    speedHint_ = makeLabel(L"（六档预置，或自定义 50% ~ 200%）", 0);
    hintLabel_ = makeLabel(L"点一下热键框（原来的键会清空），再按你想用的键即可；按 Delete 取消绑定。",
                           kIdHint);

    // 六个热键框 + 排除键框：焦点在框里时"按下哪个键就设成哪个键"。
    // 只读框做不到这件事（还是能选中/复制），所以用子类化把按键全接过来。
    for (HWND field : {playHotkey_, pauseHotkey_, stopHotkey_, restartHotkey_, previousHotkey_, nextHotkey_,
                       ignoreKeyField_}) {
        SetWindowSubclass(field, &SettingsWindow::HotkeyFieldProc, reinterpret_cast<UINT_PTR>(this),
                          reinterpret_cast<DWORD_PTR>(this));
        // 只读：鼠标点上去不再出现闪烁的插入光标（那会让人以为还得手打），
        // 文字仍然由捕获逻辑用 WM_SETTEXT 写进去。
        const LONG_PTR style = GetWindowLongPtrW(field, GWL_STYLE);
        SetWindowLongPtrW(field, GWL_STYLE, style | ES_READONLY);
        // 关键：中文输入法开着的时候，字母键会被输入法先吃掉（我们只收到 VK_PROCESSKEY 0xE5），
        // 于是"按 W 设成 W"这件事对字母完全不成立。把输入法从这个控件上摘掉，
        // 按键就会原样送进来（顺带也避免了弹输入法候选框）。
        ImmAssociateContext(field, nullptr);
    }
}

LRESULT CALLBACK SettingsWindow::HotkeyFieldProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                                 UINT_PTR id, DWORD_PTR ref) {
    SettingsWindow* self = reinterpret_cast<SettingsWindow*>(ref);
    if (self == nullptr) {
        return DefSubclassProc(hwnd, msg, wparam, lparam);
    }
    const bool isHotkeyField = hwnd == self->playHotkey_ || hwnd == self->pauseHotkey_ ||
                               hwnd == self->stopHotkey_ || hwnd == self->restartHotkey_ ||
                               hwnd == self->previousHotkey_ || hwnd == self->nextHotkey_;
    const bool isIgnoreField = hwnd == self->ignoreKeyField_;
    if (isHotkeyField || isIgnoreField) {
        if (msg == WM_GETDLGCODE) {
            // 连 Tab / Enter / Esc / 方向键都算合法热键，所以全都截下来
            return DLGC_WANTALLKEYS | DLGC_WANTTAB;
        }
        if (msg == WM_CHAR || msg == WM_SYSCHAR || msg == WM_PASTE || msg == WM_CUT || msg == WM_CLEAR) {
            HideCaret(hwnd);
            return 0;   // 不让字符进到框里（框里的文字只由捕获逻辑写）
        }
        if (msg == WM_KEYUP || msg == WM_SYSKEYUP) {
            return 0;
        }
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
            HideCaret(hwnd);   // 编辑框收到按键会想显示光标，压住
            self->OnCapturedKey(hwnd, static_cast<unsigned>(wparam), isIgnoreField);
            return 0;
        }
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_LBUTTONUP) {
            const LRESULT result = DefSubclassProc(hwnd, msg, wparam, lparam);
            HideCaret(hwnd);
            return result;
        }
        if (msg == WM_SETFOCUS) {
            // 故意**不**转给默认过程：DefWindowProc(WM_SETFOCUS) 会给编辑框 CreateCaret()，
            // 于是框里出现闪烁的插入光标，用户就以为还得手动打字。只用 ES_READONLY 挡不住
            // （只读编辑框照样有光标）。键盘消息是发给"焦点窗口"的过程的，跟光标无关，
            // 所以跳过默认过程后按键照样进这里，反而更干净。
            self->BeginCapture(hwnd, isIgnoreField);
            HideCaret(hwnd);
            return 0;
        }
        if (msg == WM_KILLFOCUS) {
            const LRESULT result = DefSubclassProc(hwnd, msg, wparam, lparam);
            self->EndCapture(hwnd, isIgnoreField);
            return result;
        }
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, &SettingsWindow::HotkeyFieldProc, id);
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

void SettingsWindow::OnCapturedKey(HWND field, unsigned vk, bool forIgnoreList) {
    // 单独按修饰键：不算设定（等用户按主键）
    if (vk == 0xE5) {   // VK_PROCESSKEY：输入法吞掉了这个键（理论上已经摘了输入法，兜个底）
        SetMessage("这个键被输入法吃掉了，请再按一次。", true);
        return;
    }
    if (vk == 0xFF) {
        return;
    }
    // 单独按修饰键：热键字段不算设定（等用户按主键）；排除键位字段是单键语义，要放行。
    if (!forIgnoreList && (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN ||
                           vk == VK_RWIN || vk == VK_LCONTROL || vk == VK_RCONTROL ||
                           vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LMENU || vk == VK_RMENU)) {
        SetMessage("请连主键一起按（例如 F5、Ctrl+Shift+F9）；光按 Ctrl / Shift / Alt / Win 不算。", false);
        return;
    }

    unsigned modifiers = kModifierNone;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        modifiers |= kModifierCtrl;
    }
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        modifiers |= kModifierShift;
    }
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
        modifiers |= kModifierAlt;
    }
    if (((GetKeyState(VK_LWIN) & 0x8000) != 0) || ((GetKeyState(VK_RWIN) & 0x8000) != 0)) {
        modifiers |= kModifierWin;
    }

    if (forIgnoreList) {
        // 排除列表是单键语义：按哪个键就加哪个键。
        // 单独按 Ctrl / Shift / Alt / Win 时 modifiers 里正好是它自己，早先这里会误判成组合键而拒绝。
        std::string message;
        unsigned ignoreVk = vk;   // 左右修饰键归一到通用 VK，名字才是 Ctrl / Shift / Alt
        if (ignoreVk == VK_LCONTROL || ignoreVk == VK_RCONTROL) {
            ignoreVk = VK_CONTROL;
        } else if (ignoreVk == VK_LSHIFT || ignoreVk == VK_RSHIFT) {
            ignoreVk = VK_SHIFT;
        } else if (ignoreVk == VK_LMENU || ignoreVk == VK_RMENU) {
            ignoreVk = VK_MENU;
        }
        if (AddIgnoreKey(ignoreKeys_, VirtualKeyName(ignoreVk), &message)) {
            SetWindowTextW(ignoreKeyField_, L"");
            RefreshIgnoreList();
            SetMessage(message, false);
        } else {
            SetMessage(message, true);
        }
        return;
    }

    // 热键框：Delete = 取消绑定（写成 none），其余按下什么键就是什么键
    if (vk == VK_DELETE && modifiers == kModifierNone) {
        captureHadOriginal_ = false;
        SetWindowTextW(field, L"none");
        SetMessage("已设为取消绑定（none）。再按一个键就能重新设。", false);
        return;
    }
    HotkeyBinding binding;
    binding.vk = vk;
    binding.modifiers = modifiers;
    if (!binding.IsValid()) {
        SetMessage("这个键不能当热键：" + VirtualKeyName(vk), true);
        return;
    }
    captureHadOriginal_ = false;   // 已经按了键，结束录制时不要再把旧值还原
    const std::wstring text = Utf8ToWide(binding.ToString());
    SetWindowTextW(field, text.c_str());
    std::string message = "已设为 " + binding.ToString();
    if (binding.IsRisky()) {
        message += "（提醒：它会被全局占用，别的程序里按它也会被工具接走）";
    }
    SetMessage(message, false);
}

void SettingsWindow::BeginCapture(HWND field, bool ignoreField) {
    captureField_ = field;
    captureOriginal_.clear();
    captureHadOriginal_ = false;
    if (!ignoreField) {
        captureOriginal_ = ReadText(field);
        captureHadOriginal_ = true;
    }
    // 清空框：用户点进来就是"要重设"，空框 + 提示比留着旧值更好懂
    SetWindowTextW(field, L"");
    if (ignoreField) {
        SetMessage("现在按下要排除的键（单键；Ctrl、Shift、Alt、CapsLock、Tab 这类也能单独按）。", false);
    } else {
        SetMessage("现在按下你想用的键就设好了；按 Delete = 取消绑定（none）。录键期间原来的热键会让位。",
                   false);
    }
    app_.SetHotkeyCaptureActive(true);   // 把全局热键让出去，否则按 F6 会被老绑定直接吃掉
}

void SettingsWindow::EndCapture(HWND field, bool ignoreField) {
    // 点进来又没按键（比如点去别的框了）：把原值放回去，别把用户的设置弄丢
    if (!ignoreField && captureHadOriginal_ && field == captureField_ && ReadText(field).empty()) {
        SetWindowTextW(field, Utf8ToWide(captureOriginal_).c_str());
    }
    captureHadOriginal_ = false;
    captureField_ = nullptr;
    app_.SetHotkeyCaptureActive(false);
}

void SettingsWindow::RefreshIgnoreList() {
    if (ignoreList_ == nullptr) {
        return;
    }
    SendMessageW(ignoreList_, LB_RESETCONTENT, 0, 0);
    for (const std::string& key : ignoreKeys_) {
        const std::wstring text = Utf8ToWide(key);
        SendMessageW(ignoreList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
}

// 底部固定区（按钮行 + 说明两行）的高度，Layout 与 OnPaint 都要用
int SettingsWindow::MaxScroll() const {
    if (hwnd_ == nullptr) {
        return 0;
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    int view = (client.bottom - client.top) - BottomBlockHeight();
    if (view < S(120)) {
        view = S(120);
    }
    return (contentHeight_ > view) ? (contentHeight_ - view) : 0;
}

void SettingsWindow::ScrollTo(int y) {
    if (hwnd_ == nullptr) {
        return;
    }
    const int maxScroll = MaxScroll();
    const int target = (y < 0) ? 0 : ((y > maxScroll) ? maxScroll : y);
    if (target == scrollY_) {
        return;
    }
    scrollY_ = target;
    Layout();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void SettingsWindow::SetPageClientHeight(int height) {
    if (height <= 0) {
        return;
    }
    pageClientHeight_ = height;
    scrollY_ = 0;   // 每次打开设置都从顶部看起
    if (hwnd_ != nullptr) {
        Layout();
    }
}

void SettingsWindow::Layout() {
    if (hwnd_ == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int margin = S(16);
    const int inner = S(16);
    const int rowHeight = S(24);
    const int rowsUnit = rowHeight + S(4);
    const int groupHead = S(28);
    const int groupFoot = S(12);
    const int groupGap = S(14);
    const int rowLeft = margin + inner;
    const int fullRow = width - margin * 2 - inner * 2;
    const int labelHeight = S(20);

    // move 给"跟着页面滚"的控件（纵向减掉滚动偏移）；pin 给底部固定行（按钮 + 说明）
    auto move = [&](HWND control, int x, int y, int w, int h) {
        if (control != nullptr) {
            SetWindowPos(control, nullptr, x, y - scrollY_, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    };
    auto pin = [&](HWND control, int x, int y, int w, int h) {
        if (control != nullptr) {
            SetWindowPos(control, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    };
    // 一组的高度 = 组头 + 行数 * 行距 - 最后一个行间距 + 组尾
    auto groupHeight = [&](int rows) { return groupHead + rows * rowsUnit - S(4) + groupFoot; };
    const int hotkeyHeight = groupHeight(6);
    const int guardHeight = groupHeight(4);   // +1 行：鼠标滚轮打断演奏
    const int playbackHeight = groupHeight(5);   // +1 行：热键独占
    const int foregroundHeight = groupHeight(3);

    // 关键：整块界面只有一个自顶向下的游标，按钮也跟在内容后面。
    // （早先按钮锚在窗口底部，结果和上半部分的分组算出重叠，单选按钮盖在按钮上。）
    int y = margin;

    // 热键
    hotkeyGroup_ = RECT{margin, y, width - margin, y + hotkeyHeight};
    int rowY = y + groupHead;
    const int labelWidth = S(64);
    const int editWidth = S(150);
    move(playLabel_, rowLeft, rowY + (rowHeight - labelHeight) / 2, labelWidth, labelHeight);
    move(playHotkey_, rowLeft + labelWidth, rowY, editWidth, rowHeight);
    rowY += rowsUnit;
    move(pauseLabel_, rowLeft, rowY + (rowHeight - labelHeight) / 2, labelWidth, labelHeight);
    move(pauseHotkey_, rowLeft + labelWidth, rowY, editWidth, rowHeight);
    rowY += rowsUnit;
    move(stopLabel_, rowLeft, rowY + (rowHeight - labelHeight) / 2, labelWidth, labelHeight);
    move(stopHotkey_, rowLeft + labelWidth, rowY, editWidth, rowHeight);
    rowY += rowsUnit;
    move(restartLabel_, rowLeft, rowY + (rowHeight - labelHeight) / 2, labelWidth, labelHeight);
    move(restartHotkey_, rowLeft + labelWidth, rowY, editWidth, rowHeight);
    rowY += rowsUnit;
    move(previousLabel_, rowLeft, rowY + (rowHeight - labelHeight) / 2, labelWidth, labelHeight);
    move(previousHotkey_, rowLeft + labelWidth, rowY, editWidth, rowHeight);
    rowY += rowsUnit;
    move(nextLabel_, rowLeft, rowY + (rowHeight - labelHeight) / 2, labelWidth, labelHeight);
    move(nextHotkey_, rowLeft + labelWidth, rowY, editWidth, rowHeight);
    y += hotkeyHeight + groupGap;

    // 用户输入急停
    guardGroup_ = RECT{margin, y, width - margin, y + guardHeight};
    rowY = y + groupHead;
    move(keyGuard_, rowLeft, rowY, fullRow, rowHeight);
    rowY += rowsUnit;
    move(mouseButtonGuard_, rowLeft, rowY, fullRow, rowHeight);
    rowY += rowsUnit;
    move(mouseMoveGuard_, rowLeft, rowY, fullRow, rowHeight);
    rowY += rowsUnit;
    move(mouseWheelGuard_, rowLeft, rowY, fullRow, rowHeight);
    y += guardHeight + groupGap;

    // 排除键位：一行输入（按下即添加）+ 列表 + 两个按钮 + 提示
    const int ignoreListHeight = S(72);
    const int ignoreHintHeight = S(54);   // 提示加了一句（鼠标滚轮），给三行高，别截字
    const int ignoreHeight = groupHead + rowsUnit * 3 - S(4) + groupFoot +
                             (ignoreListHeight - rowsUnit) + S(18) + (ignoreHintHeight - S(36));
    ignoreGroup_ = RECT{margin, y, width - margin, y + ignoreHeight};
    rowY = y + groupHead;
    move(ignoreKeyLabel_, rowLeft, rowY + (rowHeight - labelHeight) / 2, labelWidth, labelHeight);
    move(ignoreKeyField_, rowLeft + labelWidth, rowY, editWidth, rowHeight);
    move(ignoreRemoveButton_, rowLeft + labelWidth + editWidth + S(12), rowY, S(96), rowHeight);
    move(ignoreClearButton_, rowLeft + labelWidth + editWidth + S(114), rowY, S(72), rowHeight);
    rowY += rowsUnit;
    move(ignoreList_, rowLeft, rowY, fullRow, ignoreListHeight);
    rowY += ignoreListHeight;
    move(ignoreHint_, rowLeft, rowY + S(2), fullRow, ignoreHintHeight);   // 句子长，留三行
    y += ignoreHeight + groupGap;

    // 播放选项
    playbackGroup_ = RECT{margin, y, width - margin, y + playbackHeight};
    rowY = y + groupHead;
    const int speedLabelWidth = S(74);
    move(speedLabel_, rowLeft, rowY + (rowHeight - labelHeight) / 2, speedLabelWidth, labelHeight);
    move(speedCombo_, rowLeft + speedLabelWidth, rowY, S(110), S(200));
    move(speedEdit_, rowLeft + speedLabelWidth + S(120), rowY, S(90), rowHeight);
    move(speedHint_, rowLeft + speedLabelWidth + S(216), rowY + S(3), S(320), S(20));
    rowY += rowsUnit;
    move(dryRunCheck_, rowLeft, rowY, fullRow, rowHeight);
    rowY += rowsUnit;
    move(naturalCheck_, rowLeft, rowY, fullRow, rowHeight);
    rowY += rowsUnit;
    move(runAsAdminCheck_, rowLeft, rowY, fullRow, rowHeight);
    rowY += rowsUnit;
    move(exclusiveHotkeysCheck_, rowLeft, rowY, fullRow, rowHeight);
    y += playbackHeight + groupGap;

    // 前台窗口变化
    foregroundGroup_ = RECT{margin, y, width - margin, y + foregroundHeight};
    rowY = y + groupHead;
    move(foregroundIgnore_, rowLeft, rowY, fullRow, rowHeight);
    rowY += rowsUnit;
    move(foregroundPause_, rowLeft, rowY, fullRow, rowHeight);
    rowY += rowsUnit;
    move(foregroundStop_, rowLeft, rowY, fullRow, rowHeight);
    y += foregroundHeight + groupGap;

    // 分组框坐标也要跟着滚（OnPaint 直接拿它们画框）
    RECT* scrollGroups[] = {&hotkeyGroup_, &guardGroup_, &ignoreGroup_, &playbackGroup_, &foregroundGroup_};
    for (RECT* group : scrollGroups) {
        group->top -= scrollY_;
        group->bottom -= scrollY_;
    }

    // 设置页与主页面等高：选项放不下就滚动看，底部按钮行固定在下沿、不跟着滚
    contentHeight_ = y;
    const int pageHeight = (client.bottom > client.top) ? client.bottom : y;
    const int bottomBlock = BottomBlockHeight();
    int viewHeight = pageHeight - bottomBlock;
    if (viewHeight < S(120)) {
        viewHeight = S(120);
    }
    const int maxScroll = (contentHeight_ > viewHeight) ? (contentHeight_ - viewHeight) : 0;
    scrollY_ = (scrollY_ < 0) ? 0 : ((scrollY_ > maxScroll) ? maxScroll : scrollY_);

    const int barTop = pageHeight - bottomBlock;
    const int buttonHeight = S(34);
    pin(applyButton_, margin, barTop + S(4), S(96), buttonHeight);
    pin(cancelButton_, margin + S(106), barTop + S(4), S(96), buttonHeight);
    pin(defaultButton_, margin + S(212), barTop + S(4), S(110), buttonHeight);
    pin(elevateButton_, margin + S(330), barTop + S(4), S(150), buttonHeight);
    // 这两行都可能是长句子（"设置没全部生效：…"、"点一下热键框…"）：各给两行高，别截断
    pin(messageLabel_, margin, barTop + S(48), width - margin * 2, S(38));
    pin(hintLabel_, margin, barTop + S(88), width - margin * 2, S(54));   // 长句给三行，别切字

    // 子窗口不受父窗口裁剪：滚到可视区外的控件必须自己藏起来，
    // 否则会画到下面固定的按钮行上（实测"停止"复选框就露在窗口下沿外）。
    // 不用写控件名单：除了底部固定那六个，其余一律按滚动区处理。
    for (HWND child = GetWindow(hwnd_, GW_CHILD); child != nullptr;
         child = GetWindow(child, GW_HWNDNEXT)) {
        if (child == applyButton_ || child == cancelButton_ || child == defaultButton_ ||
            child == elevateButton_ || child == messageLabel_ || child == hintLabel_) {
            continue;
        }
        RECT childRect{};
        GetWindowRect(child, &childRect);
        MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&childRect), 2);
        const bool inView = (childRect.bottom > 0) && (childRect.top < barTop);
        ShowWindow(child, inView ? SW_SHOWNOACTIVATE : SW_HIDE);
    }

    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = (contentHeight_ > 0) ? contentHeight_ - 1 : 0;
    info.nPage = static_cast<UINT>(viewHeight);
    info.nPos = scrollY_;
    SetScrollInfo(hwnd_, SB_VERT, &info, TRUE);
    // 内容比可视区高才显示滚动条（显隐会改客户区宽度，所以只在状态真的变了才动）
    const bool needBar = (maxScroll > 0);
    if (((GetWindowLongPtrW(hwnd_, GWL_STYLE) & WS_VSCROLL) != 0) != needBar) {
        ShowScrollBar(hwnd_, SB_VERT, needBar ? TRUE : FALSE);
    }

    // 高度：与主页面一致（App 打开前告知）；没告知就用设计高度，并且不许高过工作区
    int wantedHeight = (pageClientHeight_ > 0) ? pageClientHeight_ : S(620);
    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) != FALSE) {
        const int workHeight = work.bottom - work.top;
        if (workHeight > 0 && wantedHeight > workHeight - S(48)) {
            wantedHeight = workHeight - S(48);
        }
    }
    if (wantedHeight < S(360)) {
        wantedHeight = S(360);
    }
    if (client.bottom != wantedHeight) {
        RECT desired{0, 0, width, wantedHeight};
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
        AdjustWindowRectForDpi(desired, style, false);
        SetWindowPos(hwnd_, nullptr, 0, 0, desired.right - desired.left, desired.bottom - desired.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;   // 客户区尺寸会变，再来一次 WM_SIZE -> Layout，别拿旧尺寸接着算
    }

    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::OnPaint() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd_, &paint);
    RECT client{};
    GetClientRect(hwnd_, &client);
    FillRect(dc, &client, BackgroundBrush());
    // 分组框只画在可滚动区里，别画到下面固定的按钮行上
    const int savedDc = SaveDC(dc);
    IntersectClipRect(dc, 0, 0, client.right, client.bottom - BottomBlockHeight());
    if (hotkeyGroup_.bottom > hotkeyGroup_.top) {
        PaintGroupFrame(dc, hotkeyGroup_, kGroupHotkeys);
    }
    if (guardGroup_.bottom > guardGroup_.top) {
        PaintGroupFrame(dc, guardGroup_, kGroupGuard);
    }
    if (ignoreGroup_.bottom > ignoreGroup_.top) {
        PaintGroupFrame(dc, ignoreGroup_, kGroupIgnoreKeys);
    }
    if (playbackGroup_.bottom > playbackGroup_.top) {
        PaintGroupFrame(dc, playbackGroup_, kGroupPlayback);
    }
    if (foregroundGroup_.bottom > foregroundGroup_.top) {
        PaintGroupFrame(dc, foregroundGroup_, kGroupForeground);
    }
    if (savedDc != 0) {
        RestoreDC(dc, savedDc);
    }
    EndPaint(hwnd_, &paint);
}

void SettingsWindow::OnDrawItem(const DRAWITEMSTRUCT* item) {
    if (item == nullptr) {
        return;
    }
    ButtonPaintInfo info;
    switch (static_cast<int>(item->CtlID)) {
        case kIdApply:
            info.kind = ButtonKind::Primary;
            info.text = Utf8ToWide(labels::kApply);
            break;
        case kIdCancel:
            info.kind = ButtonKind::Neutral;
            info.text = Utf8ToWide(labels::kCancel);
            break;
        case kIdDefaults:
            info.kind = ButtonKind::Neutral;
            info.text = Utf8ToWide(labels::kRestoreDefaults);
            break;
        case kIdElevateNow:
            info.kind = ButtonKind::Secondary;
            info.text = kElevateNowText;
            break;
        // 排除键位的两个按钮漏了分支：自绘按钮没 case 就走到 default: return，
        // 结果是"按钮存在、也能点（BM_CLICK 有效），但永远不画出来"——看着像界面没加载完
        case kIdIgnoreRemove:
            info.kind = ButtonKind::Neutral;
            info.text = L"删除选中";
            break;
        case kIdIgnoreClear:
            info.kind = ButtonKind::Neutral;
            info.text = L"清空";
            break;
        default:
            return;
    }
    info.enabled = (item->itemState & ODS_DISABLED) == 0;
    PaintFlatButton(*item, info);
}

void SettingsWindow::OnCommand(int id) {
    if (id == kIdSpeedCombo) {
        const int index = static_cast<int>(SendMessageW(speedCombo_, CB_GETCURSEL, 0, 0));
        if (index >= 0 && index < beat::kSpeedPresetCount) {
            SetWindowTextW(speedEdit_, Utf8ToWide(beat::SpeedPresetText(index)).c_str());
        } else if (index >= beat::kSpeedPresetCount) {
            SetFocus(speedEdit_);
            SendMessageW(speedEdit_, EM_SETSEL, 0, -1);
            SetMessage("自定义速度填 50% ~ 200% 之间的值（例如 90%）。", false);
        }
        return;
    }
    switch (id) {
        case kIdIgnoreRemove: {
            const int index = static_cast<int>(SendMessageW(ignoreList_, LB_GETCURSEL, 0, 0));
            if (index < 0) {
                SetMessage("先在列表里点一个键，再点「删除选中」。", true);
                break;
            }
            const std::string removed = ignoreKeys_[static_cast<size_t>(index)];
            RemoveIgnoreKeyAt(ignoreKeys_, static_cast<size_t>(index));
            RefreshIgnoreList();
            SetMessage("已从排除列表里删掉：" + removed, false);
            break;
        }
        case kIdIgnoreClear: {
            if (ignoreKeys_.empty()) {
                SetMessage("排除列表本来就是空的。", false);
                break;
            }
            ignoreKeys_.clear();
            RefreshIgnoreList();
            SetMessage("已清空排除键（所有键都会照常打断演奏）。", false);
            break;
        }
        case kIdApply: {
            app_.SetHotkeyCaptureActive(false);   // 应用前先把热键注册恢复，免得改了绑定却没注册上
            AppSettings result;
            std::string error;
            if (!ReadControls(result, error)) {
                SetMessage(error, true);
                return;
            }
            working_ = result;
            const std::string text = app_.ApplySettings(result);
            SetMessage(text, app_.StatusIsError());
            break;
        }
        case kIdCancel:
            app_.SetHotkeyCaptureActive(false);
            FillControls(app_.Settings());   // 丢掉本次修改
            SetMessage("已放弃本次修改。", false);
            ShowWindow(hwnd_, SW_HIDE);
            break;
        case kIdDefaults:
            FillControls(AppSettings::Default());
            SetMessage("已填回默认值，点应用生效。", false);
            break;
        case kIdElevateNow: {
            // 立即以管理员身份重启：这个按钮本身就表示"以后也自动申请"，所以先把配置写下来，
            // 再请系统提权（runas）。成功的话本实例就退出，新实例接着跑。
            AppSettings wanted = working_;
            std::string error;
            if (!ReadControls(wanted, error)) {
                SetMessage(error, true);
                break;
            }
            wanted.runAsAdmin = true;
            const std::string applied = app_.ApplySettings(wanted);
            std::string reason;
            // 用 RequestElevationAndQuit：提权成功后本实例会立刻退出（置退出标志 + 投递关闭消息），
            // 不能只提权不退出 —— 那会留下两个实例抢钩子 / 抢注入。
            if (app_.RequestElevationAndQuit(&reason)) {
                SetMessage("正在用管理员权限重启…（本窗口会关闭，由新的管理员实例接着跑）" + applied, false);
            } else {
                SetMessage("申请管理员权限没成功（" + reason + "）。" + applied, true);
            }
            break;
        }
        default:
            break;
    }
}

void SettingsWindow::FillControls(const AppSettings& settings) {
    auto setText = [](HWND edit, const std::string& text) {
        SetWindowTextW(edit, Utf8ToWide(text).c_str());
    };
    setText(playHotkey_, settings.playHotkey);
    setText(pauseHotkey_, settings.pauseHotkey);
    setText(stopHotkey_, settings.stopHotkey);
    setText(restartHotkey_, settings.restartHotkey);
    setText(previousHotkey_, settings.previousHotkey);
    setText(nextHotkey_, settings.nextHotkey);
    setText(speedEdit_, FormatSpeedText(settings.speed));
    if (speedCombo_ != nullptr) {
        const int preset = SpeedPresetIndexFor(settings.speed);
        const int wanted = (preset >= 0) ? preset : beat::kSpeedPresetCount;   // 未命中 = 自定义
        SendMessageW(speedCombo_, CB_SETCURSEL, static_cast<WPARAM>(wanted), 0);
    }

    auto check = [](HWND control, bool value) {
        SendMessageW(control, BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
    };
    check(keyGuard_, settings.monitorKeyboard);
    check(mouseButtonGuard_, settings.monitorMouseButtons);
    check(mouseMoveGuard_, settings.monitorMouseMove);
    check(mouseWheelGuard_, settings.stopOnMouseWheel);
    check(dryRunCheck_, settings.dryRun);
    check(naturalCheck_, settings.naturalMode);
    check(runAsAdminCheck_, settings.runAsAdmin);
    check(exclusiveHotkeysCheck_, settings.exclusiveHotkeys);

    HWND radio = settings.foregroundPolicy == ForegroundPolicy::Ignore   ? foregroundIgnore_
                 : settings.foregroundPolicy == ForegroundPolicy::Stop ? foregroundStop_
                                                                       : foregroundPause_;
    SendMessageW(foregroundIgnore_, BM_SETCHECK, radio == foregroundIgnore_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(foregroundPause_, BM_SETCHECK, radio == foregroundPause_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(foregroundStop_, BM_SETCHECK, radio == foregroundStop_ ? BST_CHECKED : BST_UNCHECKED, 0);
}

bool SettingsWindow::ReadControls(AppSettings& out, std::string& error) {
    out = working_;
    out.playHotkey = ReadText(playHotkey_);
    out.pauseHotkey = ReadText(pauseHotkey_);
    out.stopHotkey = ReadText(stopHotkey_);
    out.restartHotkey = ReadText(restartHotkey_);
    out.previousHotkey = ReadText(previousHotkey_);
    out.nextHotkey = ReadText(nextHotkey_);

    // 热键文本先在这里校验一遍（真正注册的结果由 App 报回来）
    struct Field {
        const char* name;
        const std::string& text;
    };
    const Field fields[] = {
        {"播放", out.playHotkey},
        {"暂停", out.pauseHotkey},
        {"停止", out.stopHotkey},
        {"重播", out.restartHotkey},
        {"上一首", out.previousHotkey},
        {"下一首", out.nextHotkey},
    };
    for (const Field& field : fields) {
        HotkeyBinding binding;
        std::string hotkeyError;
        if (!ParseHotkey(field.text, binding, &hotkeyError)) {
            error = std::string(field.name) + "热键有问题：" + hotkeyError;
            return false;
        }
    }

    double speed = 1.0;
    if (!ParseSpeedText(ReadText(speedEdit_), speed, error)) {
        return false;
    }
    out.speed = speed;

    out.ignoredKeys = ignoreKeys_;
    out.monitorKeyboard = SendMessageW(keyGuard_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    out.monitorMouseButtons = SendMessageW(mouseButtonGuard_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    out.monitorMouseMove = SendMessageW(mouseMoveGuard_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    out.stopOnMouseWheel = SendMessageW(mouseWheelGuard_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    out.dryRun = SendMessageW(dryRunCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    out.naturalMode = SendMessageW(naturalCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    out.runAsAdmin = SendMessageW(runAsAdminCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    out.exclusiveHotkeys = SendMessageW(exclusiveHotkeysCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;

    if (SendMessageW(foregroundIgnore_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        out.foregroundPolicy = ForegroundPolicy::Ignore;
    } else if (SendMessageW(foregroundStop_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        out.foregroundPolicy = ForegroundPolicy::Stop;
    } else {
        out.foregroundPolicy = ForegroundPolicy::Pause;
    }
    return true;
}

void SettingsWindow::SetMessage(const std::string& text, bool error) {
    messageIsError_ = error;
    SetWindowTextW(messageLabel_, Utf8ToWide(text).c_str());
    InvalidateRect(messageLabel_, nullptr, TRUE);
}

void SettingsWindow::LoadSettings(const AppSettings& settings) {
    working_ = settings;
    ignoreKeys_ = working_.ignoredKeys;
    const size_t dropped = SanitizeIgnoredKeys(ignoreKeys_);
    FillControls(working_);
    RefreshIgnoreList();
    if (dropped > 0) {
        SetMessage("排除键位里有 " + std::to_string(dropped) + " 项读不出来，已忽略。", true);
    } else {
        SetMessage("改完点应用生效。点一下热键框（原来的键会清空），再按你想用的键即可；按 Delete 取消绑定。",
                   false);
    }
}

void SettingsWindow::Show() {
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void SettingsWindow::ApplyDpi(unsigned dpi) {
    if (dpi < 72 || dpi > 480) {
        dpi = 96;
    }
    dpi_ = dpi;
    SetUiDpi(dpi);
    RebuildUiFonts();
    RebuildThemeBrushes();
    HWND controls[] = {playLabel_, pauseLabel_, stopLabel_, restartLabel_, previousLabel_, nextLabel_, playHotkey_, pauseHotkey_, stopHotkey_, restartHotkey_, previousHotkey_, nextHotkey_, ignoreKeyLabel_, ignoreKeyField_, ignoreList_, ignoreRemoveButton_, ignoreClearButton_, ignoreHint_, keyGuard_, mouseButtonGuard_,
                       mouseMoveGuard_, mouseWheelGuard_, speedEdit_, speedCombo_, speedHint_, dryRunCheck_, naturalCheck_,
                       runAsAdminCheck_, exclusiveHotkeysCheck_, elevateButton_, foregroundIgnore_,
                       foregroundPause_, foregroundStop_, applyButton_, cancelButton_,
                       defaultButton_, messageLabel_, hintLabel_};
    for (HWND control : controls) {
        if (control != nullptr) {
            SendMessageW(control, WM_SETFONT,
                         reinterpret_cast<WPARAM>(control == messageLabel_ || control == hintLabel_
                                                      ? UiFonts().small
                                                      : UiFonts().body),
                         TRUE);
        }
    }
}

}  // namespace ui
}  // namespace hp
