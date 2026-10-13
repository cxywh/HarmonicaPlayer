#include "ui/UiTheme.h"

#include <algorithm>

#include "ui/UiModel.h"      // labels
#include "ui/UiFormat.h"

namespace hp {
namespace ui {

namespace {

unsigned g_dpi = 96;
FontSet g_fonts;
HBRUSH g_backgroundBrush = nullptr;
HBRUSH g_panelBrush = nullptr;

const wchar_t* kUiFontName = L"Microsoft YaHei UI";
const wchar_t* kMonoFontName = L"Consolas";

HFONT MakeFont(const wchar_t* face, int pointSize, bool bold) {
    // 点 → 像素：1 pt = dpi/72 px（用 DPI 缩放，120 DPI 下按钮文字不会变小）
    const int height = -MulDiv(pointSize, static_cast<int>(g_dpi), 72);
    LOGFONTW lf{};
    lf.lfHeight = height;
    lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    return CreateFontIndirectW(&lf);
}

}  // namespace

HBRUSH PanelBrush() {
    return g_panelBrush;
}

COLORREF ToColorRef(Rgb color) {
    return RGB(color.r, color.g, color.b);
}

const Rgb kColorBackground = {246, 247, 249};
const Rgb kColorPanel = {255, 255, 255};
const Rgb kColorText = {32, 36, 41};
const Rgb kColorMuted = {110, 118, 129};
const Rgb kColorBorder = {214, 219, 226};
const Rgb kColorAccent = {38, 108, 214};
const Rgb kColorGreen = {31, 138, 76};
const Rgb kColorOrange = {191, 116, 15};
const Rgb kColorRed = {199, 54, 52};
const Rgb kColorBlue = {56, 92, 158};
const Rgb kColorBorderStrong = {199, 206, 215};

void SetUiDpi(unsigned dpi) {
    if (dpi >= 72 && dpi <= 480) {
        g_dpi = dpi;
    }
}

unsigned UiDpi() {
    return g_dpi;
}

int S(int value) {
    return MulDiv(value, static_cast<int>(g_dpi), 96);
}

void AdjustWindowRectForDpi(RECT& rect, DWORD style, bool hasMenu) {
    // AdjustWindowRectExForDpi 只有 Win10 1607+ 才有；动态取，取不到就用老的
    using AdjustForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static AdjustForDpiFn adjustForDpi = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32 != nullptr) {
            adjustForDpi = reinterpret_cast<AdjustForDpiFn>(
                reinterpret_cast<void*>(GetProcAddress(user32, "AdjustWindowRectExForDpi")));
        }
    }
    if (adjustForDpi != nullptr) {
        adjustForDpi(&rect, style, hasMenu ? TRUE : FALSE, 0, g_dpi);
        return;
    }
    AdjustWindowRectEx(&rect, style, hasMenu ? TRUE : FALSE, 0);
}

FontSet& UiFonts() {
    return g_fonts;
}

void RebuildUiFonts() {
    DestroyUiFonts();
    g_fonts.title = MakeFont(kUiFontName, 15, true);
    g_fonts.body = MakeFont(kUiFontName, 10, false);
    g_fonts.small = MakeFont(kUiFontName, 9, false);
    g_fonts.mono = MakeFont(kMonoFontName, 10, false);
}

void DestroyUiFonts() {
    HFONT fonts[] = {g_fonts.title, g_fonts.body, g_fonts.small, g_fonts.mono};
    for (HFONT font : fonts) {
        if (font != nullptr) {
            DeleteObject(font);
        }
    }
    g_fonts = FontSet{};
}

HBRUSH BackgroundBrush() {
    return g_backgroundBrush;
}

void RebuildThemeBrushes() {
    DestroyThemeBrushes();
    g_backgroundBrush = CreateSolidBrush(ToColorRef(kColorBackground));
    g_panelBrush = CreateSolidBrush(ToColorRef(kColorPanel));
}

void DestroyThemeBrushes() {
    if (g_backgroundBrush != nullptr) {
        DeleteObject(g_backgroundBrush);
        g_backgroundBrush = nullptr;
    }
    if (g_panelBrush != nullptr) {
        DeleteObject(g_panelBrush);
        g_panelBrush = nullptr;
    }
}

// ---------------------------------------------------------------------------
// 按钮文字测量（见 UiTheme.h 的说明）
// ---------------------------------------------------------------------------
namespace {

constexpr int kButtonTextPaddingPerSide = 12;   // 96 DPI 下文字左右各留的像素
constexpr int kMinButtonWidth = 56;             // 极短文字也留个能点的宽度
constexpr int kButtonGap = 8;                   // 按钮之间的横向间隙（与窗口排版一致）

}  // namespace

int ButtonTextPadding() {
    return S(kButtonTextPaddingPerSide) * 2;
}

int MeasureTextWidth(HFONT font, const std::wstring& text) {
    if (text.empty()) {
        return 0;
    }
    HDC dc = CreateCompatibleDC(nullptr);   // 内存 DC：不需要窗口就能量
    if (dc == nullptr) {
        return 0;
    }
    HGDIOBJ oldFont = nullptr;
    if (font != nullptr) {
        oldFont = SelectObject(dc, font);
    }
    SIZE size{};
    int width = 0;
    if (GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size) != 0) {
        width = size.cx;
    }
    if (oldFont != nullptr) {
        SelectObject(dc, oldFont);
    }
    DeleteDC(dc);
    return width;
}

int ButtonWidthForText(const std::wstring& text) {
    if (UiFonts().body == nullptr) {
        // 还没建字体（例如自检程序刚启动）：先按当前 DPI 建一份，别拿 0 当宽度量
        RebuildUiFonts();
    }
    const int measured = MeasureTextWidth(UiFonts().body, text);
    return std::max(S(kMinButtonWidth), measured + ButtonTextPadding());
}

int NewSongButtonsAvailableWidth(int clientWidth) {
    // 必须与 NewSongWindow::Layout 的边距一致：左边距 margin + 分组内边距 inner、
    // 左边一列标签 labelWidth、右边距 margin + inner
    const int margin = S(14);
    const int inner = S(14);
    const int rowLeft = margin + inner;
    const int labelWidth = S(72);
    const int available = clientWidth - (rowLeft + labelWidth) - margin - inner;
    return std::max(available, S(120));
}

ButtonRowPlan PlanButtonRows(const std::vector<std::wstring>& labels, int availableWidth,
                             const std::vector<int>& forceNewRowAt) {
    ButtonRowPlan plan;
    plan.widths.reserve(labels.size());
    plan.rows.reserve(labels.size());
    const int gap = S(kButtonGap);
    int row = 0;
    int used = 0;   // 当前行已占宽度（第一个按钮之后每个都含前面的间隙）
    for (size_t i = 0; i < labels.size(); ++i) {
        const int width = ButtonWidthForText(labels[i]);
        plan.widths.push_back(width);
        if (width > availableWidth) {
            plan.overflow = true;   // 一个按钮就撑破整行：必须让调用方/自检看到
        }
        bool forceBreak = false;
        for (const int index : forceNewRowAt) {
            if (index == static_cast<int>(i)) {
                forceBreak = true;
                break;
            }
        }
        if (used > 0 && (forceBreak || used + gap + width > availableWidth)) {
            ++row;            // 这一行放不下了（或指定要换行），换行
            used = width;
        } else {
            used += (used > 0 ? gap + width : width);
        }
        plan.rows.push_back(row);
    }
    plan.rowCount = labels.empty() ? 0 : (row + 1);
    return plan;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), size,
                        nullptr, nullptr);
    return utf8;
}

void PaintFlatButton(const DRAWITEMSTRUCT& item, const ButtonPaintInfo& info) {
    Rgb base = kColorPanel;
    Rgb text = kColorText;
    switch (info.kind) {
        case ButtonKind::Primary:   base = kColorGreen;  text = {255, 255, 255}; break;
        case ButtonKind::Secondary: base = kColorBlue;   text = {255, 255, 255}; break;
        case ButtonKind::Danger:    base = kColorRed;    text = {255, 255, 255}; break;
        case ButtonKind::Neutral:
        default:                    base = kColorPanel;  text = kColorText;      break;
    }
    if (!info.enabled) {
        // 禁用：整体压暗，仍能看清文字（不用灰到看不见）
        base = Rgb{238, 240, 243};
        text = Rgb{168, 176, 187};
    } else if ((item.itemState & ODS_SELECTED) != 0) {
        base = Rgb{static_cast<unsigned char>(base.r * 85 / 100),
                   static_cast<unsigned char>(base.g * 85 / 100),
                   static_cast<unsigned char>(base.b * 85 / 100)};
    } else if ((item.itemState & ODS_HOTLIGHT) != 0) {
        base = Rgb{static_cast<unsigned char>(std::min(255, base.r * 108 / 100)),
                   static_cast<unsigned char>(std::min(255, base.g * 108 / 100)),
                   static_cast<unsigned char>(std::min(255, base.b * 108 / 100))};
    }
    if ((item.itemState & ODS_FOCUS) != 0) {
        // 焦点：加一圈深色边，键盘用户也能看出选中的是哪个按钮
        text = (info.kind == ButtonKind::Neutral) ? kColorAccent : Rgb{30, 30, 30};
    }

    HDC dc = item.hDC;
    RECT rect = item.rcItem;
    SetBkMode(dc, TRANSPARENT);
    HBRUSH brush = CreateSolidBrush(ToColorRef(base));
    HPEN pen = CreatePen(PS_SOLID, 1, ToColorRef(kColorBorder));
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    const int radius = S(6);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    SetTextColor(dc, ToColorRef(text));
    HFONT font = UiFonts().body;
    HGDIOBJ oldFont = (font != nullptr) ? SelectObject(dc, font) : nullptr;
    RECT textRect = rect;
    DrawTextW(dc, info.text.c_str(), -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (oldFont != nullptr) {
        SelectObject(dc, oldFont);
    }
}

void PaintGroupFrame(HDC dc, const RECT& rect, const wchar_t* title) {
    HPEN pen = CreatePen(PS_SOLID, 1, ToColorRef(kColorBorderStrong));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, S(8), S(8));
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);

    const std::wstring wide = (title != nullptr) ? title : L"";
    if (wide.empty()) {
        return;
    }
    HFONT font = UiFonts().body;
    HGDIOBJ oldFont = (font != nullptr) ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ToColorRef(kColorText));
    SIZE size{};
    GetTextExtentPoint32W(dc, wide.c_str(), static_cast<int>(wide.size()), &size);
    RECT clearRect{rect.left + S(12), rect.top - S(9), rect.left + S(12) + size.cx + S(8),
                   rect.top + S(11)};
    FillRect(dc, &clearRect, BackgroundBrush());
    RECT textRect{rect.left + S(16), rect.top - S(9), rect.right - S(12), rect.top + S(11)};
    DrawTextW(dc, wide.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (oldFont != nullptr) {
        SelectObject(dc, oldFont);
    }
}

void PaintProgress(const DRAWITEMSTRUCT& item, int percent, const std::wstring& caption, Rgb fill) {
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    HDC dc = item.hDC;
    RECT rect = item.rcItem;
    SetBkMode(dc, TRANSPARENT);

    HBRUSH track = CreateSolidBrush(ToColorRef(Rgb{229, 232, 237}));
    HPEN pen = CreatePen(PS_SOLID, 1, ToColorRef(kColorBorder));
    HGDIOBJ oldBrush = SelectObject(dc, track);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, S(8), S(8));

    const int width = rect.right - rect.left;
    const int filled = (width * percent) / 100;
    if (filled > 0) {
        HBRUSH bar = CreateSolidBrush(ToColorRef(fill));
        HPEN barPen = CreatePen(PS_SOLID, 1, ToColorRef(fill));
        SelectObject(dc, bar);
        SelectObject(dc, barPen);
        RECT barRect = rect;
        barRect.right = rect.left + std::max(filled, S(8));
        if (barRect.right > rect.right) {
            barRect.right = rect.right;
        }
        RoundRect(dc, barRect.left, barRect.top, barRect.right, barRect.bottom, S(8), S(8));
        SelectObject(dc, bar);
        DeleteObject(bar);
        DeleteObject(barPen);
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(track);
    DeleteObject(pen);

    if (!caption.empty()) {
        SetTextColor(dc, ToColorRef(kColorText));
        HFONT font = UiFonts().mono;
        HGDIOBJ oldFont = (font != nullptr) ? SelectObject(dc, font) : nullptr;
        RECT textRect = rect;
        textRect.left += S(8);
        textRect.right -= S(8);
        DrawTextW(dc, caption.c_str(), -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (oldFont != nullptr) {
            SelectObject(dc, oldFont);
        }
    }
}

Rgb StatusColor(int playbackState, int stopReason) {
    switch (static_cast<hp::PlaybackState>(playbackState)) {
        case hp::PlaybackState::Playing:
            return kColorGreen;
        case hp::PlaybackState::Paused:
            return kColorOrange;
        case hp::PlaybackState::Stopped:
        default:
            break;
    }
    switch (static_cast<hp::StopReason>(stopReason)) {
        case hp::StopReason::UserInput:
            return kColorRed;
        case hp::StopReason::WindowChanged:
        case hp::StopReason::Error:
            return kColorOrange;
        case hp::StopReason::UserStop:
        case hp::StopReason::Finished:
        case hp::StopReason::Shutdown:
        case hp::StopReason::None:
        default:
            return kColorMuted;
    }
}

}  // namespace ui
}  // namespace hp
