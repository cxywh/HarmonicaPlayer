#include "core/Time.h"

#include "core/Platform.h"

namespace hp {
namespace {

// 进程启动时取一次频率即可，QPC 频率在运行期恒定。
const long long kFrequency = []() -> long long {
    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
        return frequency.QuadPart;
    }
    return 1000000LL;   // 理论上不会走到这里
}();

}  // namespace

long long TicksPerSecond() {
    return kFrequency;
}

long long NowTicks() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

double NowSeconds() {
    return static_cast<double>(NowTicks()) / static_cast<double>(kFrequency);
}

}  // namespace hp
