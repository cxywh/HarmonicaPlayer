#pragma once

// 统一的 Windows 头文件入口。
// 所有需要 Win32 API 的文件都必须先包含本文件，理由：
//   1. 关掉 min/max 宏（NOMINMAX），否则会和 std::min/std::max 打架；
//   2. WIN32_LEAN_AND_MEAN 让 windows.h 少拖一堆用不到的头，编译更快；
//   3. 统一 _WIN32_WINNT（Windows 10），避免不同翻译单元拿到的声明不一致。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00   // Windows 10
#endif

#include <windows.h>
