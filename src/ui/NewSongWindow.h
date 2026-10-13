#pragma once

// 新建曲谱窗口（Phase 12 补：需求要求"新建"独立成常驻功能，并改用点选式加音符）。
//
// 与"编辑曲谱"的分工：
//   新建曲谱（本窗口）：任何时候都能打开，从空白开始，点按钮加音符，存进 songs\ 就出现在主界面列表里。
//   编辑曲谱（SongEditorWindow）：改已经载入的那首（表格化编辑、改 BPM/拍号、另存为）。
//
// 加音符的方式刻意不用下拉框：口琴一共就 8 个音（1 2 3 4 5 6 7 i）+ 6 种演奏方式
// （本音/左/中/右/左+中/右+中），全部列成按钮，点一下加一个音符——
// 用户不需要知道"下拉框里那一串英文"是什么意思。

#include <string>
#include <vector>

#include "core/Platform.h"
#include "core/Types.h"
#include "song/Song.h"
#include "ui/UiModel.h"
#include "ui/UiTheme.h"

namespace hp {
namespace ui {

class App;

class NewSongWindow {
public:
    explicit NewSongWindow(App& app);
    ~NewSongWindow();

    bool Create(HINSTANCE instance, std::string* error);
    // 清成一首新曲谱：曲名"新曲谱"、120 BPM、没有音符（打开新窗口时调一次）
    void Reset();
    void Show();
    HWND Handle() const { return hwnd_; }
    bool HasNotes() const { return !rows_.empty(); }
    // 装载一首已有曲谱来编辑（「编辑曲谱」走这条）：保存时覆盖原文件
    void LoadSongForEdit(const Song& song, const std::string& sourcePath);
    bool EditingExistingSong() const { return !editPath_.empty(); }
    size_t NoteCount() const { return rows_.size(); }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

    void CreateChildren();
    void Layout();
    void UpdateColumnWidths();   // 表格列宽按文字实测（时值文案变长了，写死会显示不全）
    void OnCommand(int id, int notifyCode, HWND control);
    void OnDrawItem(const DRAWITEMSTRUCT* item);
    void OnPaint();
    void ApplyDpi(unsigned dpi);

    void AddNote(Key key);
    void ExtendLastNote();       // 「延时」：把上一个音符延长一拍（简谱的 -）
    void RefreshList();
    int SelectedRow() const;              // 表格里选中的第几行（-1 = 没选）
    void SelectRow(int index);            // 选中第 index 行并滚到可见
    void SetStatus(const std::string& text, bool error);
    Song Context() const;                       // 只有曲头，用来算小节/毫秒/秒数
    bool BuildSong(Song& out, std::string& error);
    void SaveToSongs();
    void InvalidatePickers();

    App& app_;
    HWND hwnd_ = nullptr;

    HWND nameLabel_ = nullptr;
    HWND nameEdit_ = nullptr;
    HWND bpmLabel_ = nullptr;
    HWND bpmEdit_ = nullptr;
    HWND bpmHint_ = nullptr;
    HWND noteLabel_ = nullptr;
    HWND modeLabel_ = nullptr;
    HWND durationLabel_ = nullptr;
    HWND noteButtons_[kKeyCount] = {};
    HWND modeButtons_[kPlayModeCount] = {};
    HWND durationButtons_[kDurationChoiceCount] = {};
    HWND extendButton_ = nullptr;        // 「延时」（简谱的 -）：把上一个音符延长一拍
    HWND list_ = nullptr;
    HWND hintLabel_ = nullptr;
    HWND statusLabel_ = nullptr;
    HWND editSelectedButton_ = nullptr;
    HWND deleteSelectedButton_ = nullptr;
    HWND clearButton_ = nullptr;
    HWND saveButton_ = nullptr;
    HWND closeButton_ = nullptr;

    RECT infoGroup_{};
    RECT pickGroup_{};
    RECT listGroup_{};

    std::vector<EditorRow> rows_;
    std::string editPath_;                       // 非空 = 正在编辑这个文件（保存就覆盖它）
    NoteValue frameBeatUnit_ = NoteValue::Quarter;   // 曲头拍单位：编辑已有曲谱时沿用它的
    TimeSignature frameSignature_{4, 4};             // 曲头拍号：同上
    double cursorBeats_ = 0.0;          // 下一个音符落在第几拍（休止符只推它、不进表格）
    PlayMode mode_ = PlayMode::Normal;  // 当前选的演奏方式
    int durationChoice_ = kDefaultDurationChoiceIndex;   // 当前选的时值（下标进 kDurationChoices）
    int editingRow_ = -1;               // >=0 表示正在"编辑选中行"：点音调按钮会替换这一行
    unsigned dpi_ = 96;
    bool statusIsError_ = false;
};

}  // namespace ui
}  // namespace hp
