    1|#pragma once
     2|#include <atomic>
     3|#include <cstdint>
     4|
     5|// ==========================================
     6|// 1. 物理常量定义
     7|// ==========================================
     8|// 2^13 = 8192。足够存 3-5 秒的高频数据。
     9|// 核心优势：总大小 512KB，完美锁定在 CPU L2 Cache 中，极速读写。
    10|constexpr int RING_BUFFER_SIZE = 8192; 
    11|constexpr int RING_BUFFER_MASK = RING_BUFFER_SIZE - 1; // 掩码 = 8191 (0x1FFF)
    12|
    13|// 指令队列和事件队列的大小
    14|constexpr int COMMAND_RING_BUFFER_CAPACITY = 128; // 2^7，足够应对突发高频指令
    15|constexpr int COMMAND_RING_BUFFER_MASK = COMMAND_RING_BUFFER_CAPACITY - 1;
    16|
    17|constexpr int EVENT_RING_BUFFER_CAPACITY = 1024; // 2^10，足够应对高频成交事件
    18|constexpr int EVENT_RING_BUFFER_MASK = EVENT_RING_BUFFER_CAPACITY - 1;
    19|
    20|// ==========================================
    21|// 2. 基础数据帧 (MarketFrame) - 64字节
    22|// ==========================================
    23|struct alignas(64) MarketFrame {
    24|    uint64_t timestamp;      // 交易所时间 (ns)
    25|    double   price;          // 成交价
    26|    double   quantity;       // 成交量
    27|    
    28|    // BBO (盘口) -- 【注释】不再使用
    29|    // double   bid_p;          // 买一价
    30|    // double   ask_p;          // 卖一价
    31|    // double   bid_q;          // 买一量
    32|    // double   ask_q;          // 卖一量
    33|    
    34|    int      type;           // 1=Trade, 2=Depth
    35|    int      side;
    36|    // 自动填充到 64 字节，无需手动 padding
    37|};
    38|
    39|// ==========================================
    40|// 3. 环形缓冲区 (MarketRingBuffer)
    41|// ==========================================
    42|struct alignas(64) MarketRingBuffer {
    43|    // 这是一个一直累加的序号 (0 -> ∞)
    44|    // C++ 写入位置 = write_index & RING_BUFFER_MASK
    45|    std::atomic<uint64_t> write_index; 
    46|    char padding[56]; // 显式填充以避免伪共享
    47|    
    48|    // 仓库本体：512KB 的连续内存
    49|    MarketFrame frames[RING_BUFFER_SIZE]; 
    50|};
    51|
    52|// ==========================================
    53|// 4. 指令区 (CommandRingBuffer) - Python -> C++
    54|// ==========================================
    55|
    56|// 动作类型宏定义
    57|#define ACT_NONE    0
    58|#define ACT_NEW     1
    59|#define ACT_CANCEL  2
    60|#define ACT_AMEND   3
    61|#define ACT_CANCEL_ALL 4 // 注意：这里我将 ACT_CLEAR 改名为 ACT_CANCEL_ALL 以保持语义清晰
    62|
    63|// 订单类型
    64|#define ORD_LIMIT   1
    65|#define ORD_MARKET  2
    66|
    67|// TIF
    68|#define TIF_GTC     1
    69|#define TIF_IOC     2
    70|#define TIF_FOK     3
    71|
    72|struct alignas(64) CommandFrame {
    73|    uint64_t request_id;       // 全局递增的请求ID，用于去重和排序
    74|    uint64_t trigger_ms;       // 触发这一单的那一帧行情的交易所时间 (ms)
    75|    char     client_order_id[32]; // 必须有，用于全链路追踪
    76|    char     parent_order_id[32]; // 【新增】父订单 ID，用于平仓单追踪
    77|    char     symbol[16];
    78|    
    79|    int      action;           // 1=New, 2=Cancel, 3=Amend, 4=Cancel All
    80|    int      type;             // Limit/Market (ORD_LIMIT, ORD_MARKET)
    81|    int      side;             // 1=Buy, -1=Sell
    82|    // int      tif;              // Time in Force (TIF_GTC, TIF_IOC, TIF_FOK) -- 【注释】不再使用 TIF
    83|    
    84|    // double   price;            // 【注释】不再使用价格
    85|    double   quantity; // 在 CommandFrame 中，quantity 表示交易币的数量，而不是 USDT 金额
    86|
    87|    // For AMEND_ORDER
    88|    char padding[16]; // 112 + 32 = 144 bytes，192 - 144 = 48 bytes 填充
    89|};
    90|
    91|struct alignas(64) CommandRingBuffer {
    92|    // 读写索引 (单生产者 Python -> 单消费者 C++)
    93|    std::atomic<uint64_t> write_idx; // Python 写
    94|    std::atomic<uint64_t> read_idx;  // C++ 读
    95|    
    96|    // 比如 128 个槽位，足够应对突发高频指令
    97|    CommandFrame frames[COMMAND_RING_BUFFER_CAPACITY]; 
    98|};
    99|
   100|// ==========================================
   101|// 5. 账户反馈区 (AccountRingBuffer) - C++ -> Python
   102|// ==========================================
   103|
   104|// 事件类型
   105|#define EVT_SUBMITTED    1  // 已提交
   106|#define EVT_PARTIAL_FILL 2  // 部分成交
   107|#define EVT_FULL_FILL    3  // 完全成交
   108|#define EVT_CANCELED     4  // 已取消
   109|#define EVT_REJECTED     5  // 已拒绝
   110|#define EVT_AMENDED      6  // 已改单
   111|
   112|struct alignas(64) OrderEventFrame {
   113|    uint64_t timestamp;        // 事件发生时间 (纳秒)
   114|    char     client_order_id[32]; 
   115|    // char     exch_order_id[32];    // 【删除】交易所订单ID (已废弃)
   116|    int      event_type;       // 事件类型 (EVT_SUBMITTED, EVT_PARTIAL_FILL 等)
    int      side;             // 【新增】订单方向 (1=Buy, -1=Sell)
   117|    double   fill_price;       // 仅在 Fill 时有效
   118|    double   fill_qty;         // 仅在 Fill 时有效
   119|    double   remaining_qty;    // 剩余未成交量
   120|    
   121|    // 错误信息 (如果 Rejected)
   122|    int      error_code;       // 交易所返回的错误码
   123|    char     error_msg[64];    // 错误信息描述
   124|    uint64_t last_update_id;   // 交易所推送的订单更新ID，用于去重
   125|    bool     is_maker;         // 是否为 Maker 订单 (true = Maker, false = Taker)
   126|    uint64_t trigger_ms;       // 触发这一单的那一帧行情的交易所时间 (ms)
   127|    char     parent_order_id[32]; // 【新增】父订单 ID，用于平仓单追踪
   128|};
   129|
   130|struct alignas(64) AccountRingBuffer {
   131|    // 读写索引 (单生产者 C++ -> 单消费者 Python)
   132|    std::atomic<uint64_t> write_idx; // C++ 写
   133|    std::atomic<uint64_t> read_idx;  // Python 读
   134|    
   135|    // 必须足够大，防止高频成交时爆满
   136|    OrderEventFrame frames[EVENT_RING_BUFFER_CAPACITY]; 
   137|    
   138|    // 原有的账户快照 (Snapshot) 依然保留，用于定期对账
   139|    std::atomic<double> usdt_balance;
   140|    std::atomic<double> position_amt;
   141|    // strategy_status 已移到 GenericShmStruct 中
   142|};
   143|
   144|// ==========================================
   145|// 6. 总线结构体 (GenericShmStruct)
   146|// ==========================================
   147|struct alignas(64) GenericShmStruct {
   148|    // 区域 1: 市场数据流 (传送带)
   149|    MarketRingBuffer market_ring; 
   150|
   151|    // 区域 2: 指令队列 (控制台)
   152|    CommandRingBuffer command_ring; // 替换 CommandArea
   153|
   154|    // 区域 3: 事件反馈队列 + 账户快照 (仪表盘)
   155|    AccountRingBuffer account_feed; // 替换 AccountArea
   156|    
   157|    // 系统状态心跳
   158|    std::atomic<bool> cpp_alive;
   159|    std::atomic<bool> py_alive;
   160|
   161|    // 交易对价格和数量精度
   162|    std::atomic<int>  price_precision;
   163|    std::atomic<int>  quantity_precision;
   164|
   165|    // 棘轮止损参数
   166|    std::atomic<double> ratchet_active_gap;
   167|    std::atomic<double> ratchet_callback;
   168|    std::atomic<double> hard_stop_price;
   169|    std::atomic<uint64_t> system_health; // 新增：系统健康时间戳
   170|    std::atomic<int>    strategy_status; // 从 AccountRingBuffer 移动到这里
   171|};
   172|
   173|// 全局指针声明
   174|extern GenericShmStruct* g_master_bridge;
