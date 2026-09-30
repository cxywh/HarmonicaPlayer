# Harmonica Player

Windows 上的**游戏内口琴自动演奏工具**：按时间轴模拟按键，让角色在游戏里"自己吹口琴"。
原生 Win32 C++17（MSVC），单个 exe，无运行库依赖。

## 下载

到 [Releases](https://github.com/cxywh/HarmonicaPlayer/releases) 下载
`HarmonicaPlayer-v1.0.0-win64.zip`，解压后双击 `HarmonicaPlayer.exe`
（内含主程序、12 首曲谱、使用说明与声明）。

## 功能

- 精确时间轴播放：单调时钟（QPC），实测最大漂移 0.235 ms
- 全局热键：播放 / 暂停 / 停止 / 重播 / 上一首 / 下一首，全部可自由改键
- 热键两种模式：**放行**（默认——不抢键，游戏照样收到那个键）与**独占**
- 切歌时屏幕中央浮出"当前歌曲：xxx"，几秒自动消失；不抢焦点、点击穿透
- 演奏中按下任意真实按键立即急停并释放所有按键；可设"排除键位"避免游戏内误停
- 曲谱为 JSON 格式并可内置编辑；`settings.json` 存设置、`debug.log` 记错误，两者误删会自动重建

## 运行要求

- Windows 10 及以上
- **游戏以管理员身份运行时，本程序也必须提权**（设置里勾选后会自动申请）
- 只模拟人手按键：不注入游戏进程、不读写游戏内存、不修改游戏文件

## 默认热键

| 按键 | 作用 | 按键 | 作用 |
|---|---|---|---|
| F6 | 播放 / 开始 | F5 | 暂停 / 继续 |
| F7 | 停止 | F8 | 重新播放 |
| F9 | 上一首 | F10 | 下一首 |

热键都能改（设置 -> 直接按你想用的键），支持小键盘、方向键、Insert、Home / End / PageUp / PageDown 等。

## 自己编译

```bat
build.bat        :: 主程序 + 11 个自检工具（输出到 dist\）
build.bat app    :: 只编主程序
```

需要 MSVC（Visual Studio 2022 / 2026 BuildTools）与 Windows SDK。

## 曲谱

`songs\` 下是 JSON 格式曲谱，可以直接加；`tools\jianpu2score.py` 是简谱转曲谱的脚本。

## 使用须知

请先阅读 [NOTICE.md](NOTICE.md)：本工具可能违反游戏用户协议，**账号风险自负**；
仅限个人学习与娱乐，**禁止商业用途**。

## 开发文档

逐 Phase 的实现记录与自检结论见 [docs/开发文档.md](docs/开发文档.md)。
