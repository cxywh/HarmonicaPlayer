#include "ui/OsdWindow.h"

#include <windows.h>

namespace hp {
namespace ui {

namespace {

const wchar_t* kOsdClass = L"HarmonicaPlayerOsd";
const UINT_PTR kHideTimerId = 1;
const UINT kHoldMs = 3000;            // 几秒后消失
const int kPaddingXDip = 26;
const int kPaddingYDip = 16;
const int kFontDip = 20;
const int kRadiusDip = 10;
const int kVerticalPercent = 62;      // 屏幕高度的 62%：略低于正中，避开准星
const BYTE kCardAlpha = 214;          // 整窗不透明度（约 84%）：看得清，又不糊住画面

std::wstring ToWide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int size = static_cast<int>(utf8.size());
    const int need = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), size, nullptr, 0);
    if (need <= 0) {
        return std::wstring();
    }
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), size, out.data(), need);
    return out;
}

}  // namespace

int OsdWindow::Scale(int dip) const {
    return MulDiv(dip, static_cast<int>(dpi_), 96);
}

bool OsdWindow::Create(HINSTANCE instance, std::string* error) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &OsdWindow::WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kOsdClass;
    windowClass.hbrBackground = nullptr;
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        if (error != nullptr) {
            *error = "注册提示窗口类失败（错误码 " + std::to_string(GetLastError()) + "）";
        }
        return false;
    }

    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                                WS_EX_TOOLWINDOW,
                            kOsdClass, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        if (error != nullptr) {
            *error = "创建提示窗口失败（错误码 " + std::to_string(GetLastError()) + "）";
        }
        return false;
    }
    SetLayeredWindowAttributes(hwnd_, 0, kCardAlpha, LWA_ALPHA);
    dpi_ = GetDpiForWindow(hwnd_);
    if (dpi_ == 0) {
        dpi_ = 96;
    }
    // 字体直接取系统界面字体：不猜字体名，一定创建得出来
    LOGFONTW logFont{};
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) != FALSE) {
        logFont = metrics.lfMessageFont;
    } else {
        lstrcpyW(logFont.lfFaceName, L"Segoe UI");
    }
    logFont.lfHeight = -MulDiv(kFontDip, static_cast<int>(dpi_), 72);
    logFont.lfWeight = FW_SEMIBOLD;
    logFont.lfQuality = CLEARTYPE_QUALITY;
    font_ = CreateFontIndirectW(&logFont);
    cardBrush_ = CreateSolidBrush(RGB(14, 18, 26));
    radius_ = Scale(kRadiusDip);
    return true;
}

void OsdWindow::ShowText(const std::string& utf8Text) {
    if (hwnd_ == nullptr || utf8Text.empty()) {
        return;
    }
    text_ = ToWide(utf8Text);
    if (text_.empty()) {
        return;
    }
    RecalcLayout();
    SetWindowPos(hwnd_, HWND_TOPMOST, card_.left, card_.top, card_.right - card_.left,
                 card_.bottom - card_.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, TRUE);
    // 同一 id 再 SetTimer = 重新计时：连续切歌只会刷新文字与倒计时，不会堆出第二条
    SetTimer(hwnd_, kHideTimerId, kHoldMs, nullptr);
}

void OsdWindow::Hide() {
    if (hwnd_ == nullptr) {
        return;
    }
    KillTimer(hwnd_, kHideTimerId);
    ShowWindow(hwnd_, SW_HIDE);
}

void OsdWindow::RecalcLayout() {
    HDC dc = GetDC(hwnd_);
    if (dc == nullptr) {
        return;
    }
    HGDIOBJ oldFont = SelectObject(dc, font_);
    RECT measure{0, 0, 0, 0};
    DrawTextW(dc, text_.c_str(), -1, &measure, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    if (oldFont != nullptr) {
        SelectObject(dc, oldFont);
    }
    ReleaseDC(hwnd_, dc);

    const int width = (measure.right - measure.left) + Scale(kPaddingXDip) * 2;
    const int height = (measure.bottom - measure.top) + Scale(kPaddingYDip) * 2;

    // 前台窗口（一般就是游戏）所在的显示器；全屏时就是整块屏幕
    HMONITOR monitor = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) == FALSE) {
        return;
    }
    const int monitorWidth = info.rcMonitor.right - info.rcMonitor.left;
    const int monitorHeight = info.rcMonitor.bottom - info.rcMonitor.top;
    const int left = info.rcMonitor.left + (monitorWidth - width) / 2;
    const int top = info.rcMonitor.top + monitorHeight * kVerticalPercent / 100 - height / 2;
    card_ = RECT{left, top, left + width, top + height};

    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius_ * 2, radius_ * 2);
    if (region != nullptr) {
        // 成功后这块区域归系统所有，不要自己 DeleteObject
        SetWindowRgn(hwnd_, region, TRUE);
    }
}

LRESULT CALLBACK OsdWindow::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    OsdWindow* self = reinterpret_cast<OsdWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<OsdWindow*>(create->lpCreateParams);
        if (self != nullptr) {
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
    }
    if (self == nullptr) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    return self->HandleMessage(message, wparam, lparam);
}

LRESULT OsdWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_TIMER:
            if (wparam == kHideTimerId) {
                Hide();
            }
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;   // 绝不抢焦点：正在打游戏时不能被打断
        case WM_ERASEBKGND:
            return 1;               // 自己画，别闪
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd_, &paint);
            RECT client{};
            GetClientRect(hwnd_, &client);
            FillRect(dc, &client, cardBrush_);
            HGDIOBJ oldFont = SelectObject(dc, font_);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            RECT area = client;
            area.left += Scale(kPaddingXDip);
            area.right -= Scale(kPaddingXDip);
            DrawTextW(dc, text_.c_str(), -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (oldFont != nullptr) {
                SelectObject(dc, oldFont);
            }
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd_, kHideTimerId);
            return 0;
        default:
            // lparam 必须原样转给默认过程：WM_NCCREATE 少了 lpCreateStruct，
            // CreateWindowExW 会直接失败（窗口建不出来，且报错很容易被别的提示盖掉）
            return DefWindowProcW(hwnd_, message, wparam, lparam);
    }
}

}  // namespace ui
}  // namespace hp
