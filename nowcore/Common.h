#pragma once

#include <atomic>
#include <cstdint>
#include <chrono>
#include <sys/time.h> // Linux/Unix specific header

namespace Common {
inline uint64_t get_now_ns() {
    struct timespec ts;
    // 必须使用 REALTIME 才能对齐 1970 年基准
    clock_gettime(CLOCK_REALTIME, &ts); 
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
} // namespace Common

struct Constitution {
    static constexpr double MAX_ORDER_USDT = 20.0;    // 单笔资金熔断线
    static constexpr double MIN_ORDER_USDT = 5.5;     // 交易所门槛
    static constexpr int RING_BUFFER_SIZE = 4096;     // 必须是 2^N
};

// 在这里定义 BurstFrame
struct BurstFrame {
    int64_t timestamp;      // 发生时间
    int event_type;         // 事件类型 (信号/下单/成交/报错)
    double current_price;   // 当时价格
    double trigger_val;     // 当时的触发值 (比如跌幅)
    double gene_threshold;  // 当时用的阈值基因 (重要！进化要对比这个)
    uint64_t order_id;      // 订单号
    double fill_price;      // 实际成交价 (计算滑点用)
    char padding[256 - sizeof(int64_t) - sizeof(int) - sizeof(double)*3 - sizeof(uint64_t) - sizeof(double)];
};

// 在这里定义 BlackBoxFrame
struct alignas(64) BlackBoxFrame {
    uint64_t t_local;      // 本地接收时间 (偏移 0)
    uint64_t t_exch;       // 交易所时间 (偏移 8)
    double price;          // 价格 (偏移 16)
    double quantity;       // 成交量 (偏移 24)
    char event_type;       // 事件类型 (偏移 32)
    char padding[31];      // 补齐到 64 字节
};

extern std::atomic<bool> g_running; // 声明
