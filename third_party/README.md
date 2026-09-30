# third_party —— 第三方库（单头文件，直接进仓库）

## nlohmann/json

- 文件：`nlohmann/json.hpp`
- 版本：**v3.11.3**（单头文件版，24,765 行，919,975 字节）
- 来源：<https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp>
- 校验：`md5 = 30b66ababceb852ec47ecf93fd370b8d`
- 许可：MIT（见 <https://github.com/nlohmann/json/blob/develop/LICENSE.MIT>）

### 为什么放在这个目录而不是用包管理器

需求书第三十九节明确要求 JSON 用 `nlohmann/json`。它是**单头文件**库，直接放进仓库就能编译，
不需要 vcpkg / CMake FetchContent / 网络下载，符合本项目“双击 build.bat 就能编译、无外部依赖”
的取向（这样别人拿到源码也能一次编过）。

### 使用时必须注意

`nlohmann/json.hpp` 必须在 `windows.h` **之前**包含，否则 Windows 的 `min`/`max` 宏会破坏
头文件里的 `std::numeric_limits<T>::max()` 这类写法（宏展开会报“宏需要 2 个参数”）。
本项目只有 `src/song/SongSerializer.cpp` 会包含它，并且已经在文件第一行包含。

编译时头文件搜索路径加 `third_party`（`build.bat` 里的 `/Ithird_party`，
`CMakeLists.txt` 里用 `/external:I` + `/external:W0` 把第三方头的警告关掉，不影响本项目自身的 `/W4`）。

### 升级方法

换版本时只需替换这一个文件，并同步更新本文件里的版本号、行数、md5。
