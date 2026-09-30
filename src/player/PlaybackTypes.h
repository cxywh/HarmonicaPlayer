#pragma once

// 播放状态与停止原因（需求书第二十一、二十六节）。
//
// 单独一个头文件，方便 UI / 测试只包含枚举而不牵扯播放器实现。
// 注意枚举名与取值都按需求书原文：PlaybackState { Stopped, Playing, Paused }。

#include <string>

namespace hp {

enum class PlaybackState {
    Stopped,
    Playing,
    Paused,
};

// 需求书第二十六节：UI 要显示“为什么停了”
enum class StopReason {
    None,            // 还没有停过（初始状态）
    UserStop,        // 用户手动停止
    Finished,        // 歌曲播放完成
    UserInput,       // 检测到用户输入（Phase 6 的 Input Monitor 触发）
    WindowChanged,   // 前台窗口变化（Phase 11 的可配置项）
    Shutdown,        // 程序退出
    Error,           // 内部错误
};

inline const char* PlaybackStateName(PlaybackState state) {
    switch (state) {
        case PlaybackState::Playing: return "演奏中";
        case PlaybackState::Paused:  return "已暂停";
        case PlaybackState::Stopped:
        default:                     return "已停止";
    }
}

inline const char* StopReasonName(StopReason reason) {
    switch (reason) {
        case StopReason::UserStop:      return "用户手动停止";
        case StopReason::Finished:      return "歌曲播放完成";
        case StopReason::UserInput:     return "检测到用户输入";
        case StopReason::WindowChanged: return "前台窗口变化";
        case StopReason::Shutdown:      return "程序退出";
        case StopReason::Error:         return "内部错误";
        case StopReason::None:
        default:                        return "无";
    }
}

// 中文、无 emoji。detail 里放补充说明（例如“Space”“播放线程异常”）。
inline std::string StopReasonText(StopReason reason, const std::string& detail) {
    std::string text = StopReasonName(reason);
    if (!detail.empty()) {
        text += "：";
        text += detail;
    }
    return text;
}

}  // namespace hp
