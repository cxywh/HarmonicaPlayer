#pragma once

// 极简控制台日志。
// 约定（用户偏好）：中文文案、无 emoji、状态用文字 + 颜色表达、不刷屏。
// 输出格式：[   1.234] [警告] 消息…
// 时间前缀是“程序启动以来的秒数”，且基于 QPC，因此日志里的时间差可以直接当作
// 精度证据使用（Phase 2/3 调时间轴时会靠它读数）。

#include <cstddef>
#include <string>

namespace hp {

enum class LogLevel {
    Info,    // 普通信息
    Ok,      // 通过 / 成功（绿色）
    Warn,    // 警告（黄色）
    Error,   // 失败 / 错误（红色）
    Plain    // 原样输出，无前缀无颜色
};

void LogLine(LogLevel level, const std::string& message);

// 打开 exe 同目录的 debug.log 并写一行启动横幅。
// 兜底：写不出去（目录只读 / 被占用）就静默降级成"只输出控制台"，绝不抛异常。
void LogStartSession();

void LogInfo(const std::string& message);
void LogOk(const std::string& message);
void LogWarn(const std::string& message);
void LogError(const std::string& message);
void LogPlain(const std::string& message);   // 无前缀，无换行处理之外的行为
void LogBlank();                             // 输出一个空行

// 不换行输出（用于 "3 秒后开始…" 这种同一行的倒计时）
void LogInline(const std::string& message);

double SinceStartSeconds();

// Phase 11“日志”项：已输出的日志行数（原子计数）。
// 自检用它验证“演奏十万音符也不会刷屏”——前后差值就是这期间写了几行日志。
std::size_t LogLineCount();
void ResetLogLineCount();

}  // namespace hp
