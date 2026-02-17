#pragma once
#include <atomic>
#include <cstdint>

// ==========================================
// 1. 物理常量定义
// ==========================================
// 2^13 = 8192。足够存 3-5 秒的高频数据。
// 核心优势：总大小 512KB，完美锁定在 CPU L2 Cache 中，极速读写。
constexpr int RING_BUFFER_SIZE = 8192; 
constexpr int RING_BUFFER_MASK = RING_BUFFER_SIZE - 1; // 掩码 = 8191 (0x1FFF)

// 指令队列和事件队列的大小
constexpr int COMMAND_RING_BUFFER_CAPACITY = 128; // 2^7，足够应对突发高频指令
constexpr int COMMAND_RING_BUFFER_MASK = COMMAND_RING_BUFFER_CAPACITY - 1;

constexpr int EVENT_RING_BUFFER_CAPACITY = 1024; // 2^10，足够应对高频成交事件
constexpr int EVENT_RING_BUFFER_MASK = EVENT_RING_BUFFER_CAPACITY - 1;

// ==========================================
// 2. 基础数据帧 (MarketFrame) - 64字节
// ==========================================
struct alignas(64) MarketFrame {
    uint64_t timestamp;      // 交易所时间 (ns)
    uint64_t local_timestamp; // 本地接收时间 (ns)
    double   price;          // 成交价
    double   quantity;       // 成交量
    
    // BBO (盘口)
    double   bid_p;          // 买一价
    double   ask_p;          // 卖一价
    double   bid_q;          // 买一量
    double   ask_q;          // 卖一量
    
    int      type;           // 1=Trade, 2=Depth
    int      side;
    // 自动填充到 64 字节，无需手动 padding
};

// ==========================================
// 3. 环形缓冲区 (MarketRingBuffer)
// ==========================================
struct alignas(64) MarketRingBuffer {
    // 这是一个一直累加的序号 (0 -> ∞)
    // C++ 写入位置 = write_index & RING_BUFFER_MASK
    std::atomic<uint64_t> write_index; 
    char padding[56]; // 显式填充以避免伪共享
    
    // 仓库本体：512KB 的连续内存
    MarketFrame frames[RING_BUFFER_SIZE]; 
};

// ==========================================
// 4. 指令区 (CommandRingBuffer) - Python -> C++
// ==========================================

// 动作类型宏定义
#define ACT_NONE    0
#define ACT_NEW     1
#define ACT_CANCEL  2
#define ACT_AMEND   3
#define ACT_CANCEL_ALL 4 // 注意：这里我将 ACT_CLEAR 改名为 ACT_CANCEL_ALL 以保持语义清晰

// 订单类型
#define ORD_LIMIT   1
#define ORD_MARKET  2

// TIF
#define TIF_GTC     1
#define TIF_IOC     2
#define TIF_FOK     3

struct alignas(64) CommandFrame {
    uint64_t request_id;       // 全局递增的请求ID，用于去重和排序
    char     client_order_id[32]; // 必须有，用于全链路追踪
    char     symbol[16];
    
    int      action;           // 1=New, 2=Cancel, 3=Amend, 4=Cancel All
    int      type;             // Limit/Market (ORD_LIMIT, ORD_MARKET)
    int      side;             // 1=Buy, -1=Sell
    int      tif;              // Time in Force (TIF_GTC, TIF_IOC, TIF_FOK)
    
    double   price;
    double   quantity; // 在 CommandFrame 中，quantity 表示交易币的数量，而不是 USDT 金额

    // For AMEND_ORDER
    double   new_price;        // 修改订单时的新价格
    double   new_quantity;     // 修改订单时的新数量
};

struct alignas(64) CommandRingBuffer {
    // 读写索引 (单生产者 Python -> 单消费者 C++)
    std::atomic<uint64_t> write_idx; // Python 写
    std::atomic<uint64_t> read_idx;  // C++ 读
    
    // 比如 128 个槽位，足够应对突发高频指令
    CommandFrame frames[COMMAND_RING_BUFFER_CAPACITY]; 
};

// ==========================================
// 5. 账户反馈区 (AccountRingBuffer) - C++ -> Python
// ==========================================

// 事件类型
#define EVT_SUBMITTED    1  // 已提交
#define EVT_PARTIAL_FILL 2  // 部分成交
#define EVT_FULL_FILL    3  // 完全成交
#define EVT_CANCELED     4  // 已取消
#define EVT_REJECTED     5  // 已拒绝
#define EVT_AMENDED      6  // 已改单

struct alignas(64) OrderEventFrame {
    uint64_t timestamp;        // 事件发生时间 (纳秒)
    char     client_order_id[32]; 
    char     exch_order_id[32]; // 交易所的单号 (用于查单)
    
    int      event_type;       // 事件类型 (EVT_SUBMITTED, EVT_PARTIAL_FILL 等)
    double   fill_price;       // 仅在 Fill 时有效
    double   fill_qty;         // 仅在 Fill 时有效
    double   remaining_qty;    // 剩余未成交量
    
    // 错误信息 (如果 Rejected)
    int      error_code;       // 交易所返回的错误码
    char     error_msg[64];    // 错误信息描述
    uint64_t last_update_id;   // 交易所推送的订单更新ID，用于去重
    bool     is_maker;         // 是否为 Maker 订单 (true = Maker, false = Taker)
};

struct alignas(64) AccountRingBuffer {
    // 读写索引 (单生产者 C++ -> 单消费者 Python)
    std::atomic<uint64_t> write_idx; // C++ 写
    std::atomic<uint64_t> read_idx;  // Python 读
    
    // 必须足够大，防止高频成交时爆满
    OrderEventFrame frames[EVENT_RING_BUFFER_CAPACITY]; 
    
    // 原有的账户快照 (Snapshot) 依然保留，用于定期对账
    std::atomic<double> usdt_balance;
    std::atomic<double> bnb_balance; // 保留 BNB 余额
    std::atomic<double> position_amt;
    std::atomic<double> avg_price;
    // strategy_status 已移到 GenericShmStruct 中
};

// ==========================================
// 6. 总线结构体 (GenericShmStruct)
// ==========================================
struct alignas(64) GenericShmStruct {
    // 区域 1: 市场数据流 (传送带)
    MarketRingBuffer market_ring; 

    // 区域 2: 指令队列 (控制台)
    CommandRingBuffer command_ring; // 替换 CommandArea

    // 区域 3: 事件反馈队列 + 账户快照 (仪表盘)
    AccountRingBuffer account_feed; // 替换 AccountArea
    
    // 系统状态心跳
    std::atomic<bool> cpp_alive;
    std::atomic<bool> py_alive;

    // 交易对价格和数量精度
    std::atomic<int>  price_precision;
    std::atomic<int>  quantity_precision;

    // 棘轮止损参数
    std::atomic<double> ratchet_active_gap;
    std::atomic<double> ratchet_callback;
    std::atomic<double> hard_stop_price;
    std::atomic<uint64_t> system_health; // 新增：系统健康时间戳
    std::atomic<int>    strategy_status; // 从 AccountRingBuffer 移动到这里
};

// 全局指针声明
extern GenericShmStruct* g_master_bridge;
