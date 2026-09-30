#pragma once

// 设置窗口（Phase 7：“设置”里放的全是需求书要求可配置的东西）。
//
//   十三节 热键允许用户自定义（播放 / 停止 / 重播）
//   二十五节 鼠标移动是否触发急停（默认关）
//   四十二节 前台窗口变化：不处理 / 暂停 / 停止（默认暂停，可配置）
//   播放速度、干跑、自然模式（Phase 2 的时间轴选项）
//
// 约定：窗口里改的是**工作副本**，点"应用"才生效（热键改失败会由 App 回滚并报出来）。

#include <string>

#include "core/Platform.h"
#include "ui/UiModel.h"
#include "ui/UiTheme.h"

namespace hp {
namespace ui {

class App;

class SettingsWindow {
public:
    explicit SettingsWindow(App& app);
    ~SettingsWindow();

    bool Create(HINSTANCE instance, std::string* error);
    void LoadSettings(const AppSettings& settings);   // 打开时把当前设置填进控件
    void Show();
    // 设置页与主页面等高：App 在打开前把主页面客户区高度告进来，放不下的选项用滚动条看
    void SetPageClientHeight(int height);
    HWND Handle() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

    void CreateChildren();
    void Layout();
    void OnCommand(int id);
    void OnDrawItem(const DRAWITEMSTRUCT* item);
    void OnPaint();
    void FillControls(const AppSettings& settings);
    // 从控件读回设置；校验失败时把原因写进 error 返回 false
    bool ReadControls(AppSettings& out, std::string& error);
    void SetMessage(const std::string& text, bool error);
    // Phase 13：热键框 / 排除键框被按下按键时的处理（子类化回调里调）
    static LRESULT CALLBACK HotkeyFieldProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                            UINT_PTR id, DWORD_PTR ref);
    void OnCapturedKey(HWND field, unsigned vk, bool forIgnoreList);
    void RefreshIgnoreList();
    // 录键的开始 / 结束：清空框、给提示、把全局热键临时让出去
    void BeginCapture(HWND field, bool ignoreField);
    void EndCapture(HWND field, bool ignoreField);
    void ApplyDpi(unsigned dpi);
    // 滚动：内容高（Layout 算出的自然高）与页高之差就是可滚范围
    int MaxScroll() const;
    void ScrollTo(int target);

    App& app_;
    HWND hwnd_ = nullptr;
    int pageClientHeight_ = 0;   // 主页面客户区高度（0 = 还没被告知）
    int contentHeight_ = 0;      // 选项总高
    int scrollY_ = 0;            // 当前滚动偏移

    HWND playLabel_ = nullptr;
    HWND pauseLabel_ = nullptr;
    HWND stopLabel_ = nullptr;
    HWND restartLabel_ = nullptr;
    HWND previousLabel_ = nullptr;
    HWND nextLabel_ = nullptr;
    HWND playHotkey_ = nullptr;
    HWND pauseHotkey_ = nullptr;
    HWND stopHotkey_ = nullptr;
    HWND restartHotkey_ = nullptr;
    HWND previousHotkey_ = nullptr;
    HWND nextHotkey_ = nullptr;
    HWND keyGuard_ = nullptr;
    HWND mouseButtonGuard_ = nullptr;
    HWND mouseMoveGuard_ = nullptr;
    HWND speedLabel_ = nullptr;
    HWND speedCombo_ = nullptr;   // Phase 8：六档预置 + 自定义
    HWND speedEdit_ = nullptr;
    HWND speedHint_ = nullptr;
    HWND dryRunCheck_ = nullptr;
    HWND naturalCheck_ = nullptr;
    HWND runAsAdminCheck_ = nullptr;
    HWND exclusiveHotkeysCheck_ = nullptr;
    HWND elevateButton_ = nullptr;
    HWND ignoreKeyLabel_ = nullptr;    // 排除键位：标签
    HWND ignoreKeyField_ = nullptr;    // 排除键位：在这里按键即加入列表
    HWND ignoreList_ = nullptr;        // 已加入的排除键
    HWND ignoreRemoveButton_ = nullptr;
    HWND ignoreClearButton_ = nullptr;
    HWND ignoreHint_ = nullptr;
    RECT ignoreGroup_{};
    HWND captureField_ = nullptr;      // 当前正在录键的框
    std::string captureOriginal_;      // 点开之前的原值（没按键就还原回去）
    bool captureHadOriginal_ = false;
    std::vector<std::string> ignoreKeys_;
    HWND foregroundIgnore_ = nullptr;
    HWND foregroundPause_ = nullptr;
    HWND foregroundStop_ = nullptr;
    HWND applyButton_ = nullptr;
    HWND cancelButton_ = nullptr;
    HWND defaultButton_ = nullptr;
    HWND messageLabel_ = nullptr;
    HWND hintLabel_ = nullptr;

    RECT hotkeyGroup_{};
    RECT guardGroup_{};
    RECT playbackGroup_{};
    RECT foregroundGroup_{};

    unsigned dpi_ = 96;
    bool messageIsError_ = false;
    AppSettings working_{};
};

}  // namespace ui
}  // namespace hp
