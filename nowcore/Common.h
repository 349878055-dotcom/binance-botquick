#pragma once

#include <atomic>
#include <cstdint>
#include <chrono>
#include <sys/time.h> // Linux/Unix specific header for clock_gettime

namespace Common {

// [物理底层] 纳秒级时钟锚点
// 必须使用 CLOCK_REALTIME 以对齐 Unix Epoch (1970-01-01)
// 这是高频交易中所有事件的时间戳真理来源
inline uint64_t get_now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts); 
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

} // namespace Common

// [系统信号] 全局运行状态锁
// 控制主循环 (Network/Executor) 的生命周期
extern std::atomic<bool> g_running;