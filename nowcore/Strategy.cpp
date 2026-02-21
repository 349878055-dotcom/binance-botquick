     1|#include "Strategy.h"
     2|#include "Executor.h" // 假设你有 Executor 发单的头文件
     3|#include "Master_Logic_Bridge.h"
     4|#include <algorithm> // for std::max
     5|#include <string>
     6|#include <cmath> // For std::round and std::pow
     7|
     8|namespace Strategy {
     9|
    10|#include <unordered_map>
    11|#include <mutex> // For std::mutex
    12|
    13|    // 本地订单结构体 -- 【删除】不再使用 LocalOrder
    14|    // struct LocalOrder {
    15|    //     std::string cl_ord_id;       // 客户端订单 ID
    16|    //     std::string exch_ord_id;     // 交易所订单 ID
    17|    //     std::string symbol;          // 交易对
    18|    //     double      orig_qty;        // 原始数量
    19|    //     double      filled_qty;      // 已成交数量
    20|    //     double      price;           // 订单价格
    21|    //     int         side;            // 1=Buy, -1=Sell
    22|    //     int         type;            // 订单类型 (Limit/Market)
    23|    //     int         tif;             // 时间有效方式
    24|    //     int         status;          // 订单状态 (NEW, PARTIALLY_FILLED, FILLED, CANCELED, REJECTED)
    25|    //     uint64_t    last_update_id;  // 记录上一次接收到的事件的 update_id，用于去重
    26|    //     bool        is_maker;        // 是否为 Maker 订单
    27|    //     // ... 可以添加更多字段，例如创建时间，更新时间等
    28|    // };
    29|
    30|    // 本地 OMS (Order Management System) -- 【删除】不再在 C++ 端管理本地订单状态
    31|    // static std::unordered_map<std::string, LocalOrder> g_active_orders;
    32|    // g_oms_mutex 被移除，因为 on_order_filled 不再直接修改 g_active_orders
    33|    // 而是通过 on_order_update_private_stream 统一处理，并利用 last_update_id 去重
    34|
    35|    // 根据用户指示，C++ 端不再做策略决策，因此删除所有本地策略状态变量。
    36|    // double local_entry_price = 0.0;
    37|    // double local_max_price = 0.0; // 记录持仓期间的最高价
    38|    // double local_hard_stop = 0.0;
    39|    // bool is_holding = false;      // 是否持仓中
    40|    // bool ratchet_active = false;  // 棘轮是否激活
    41|
    42|    // 新增 write_order_event 函数签名，以包含 transact_time 和 trigger_ms
    43|    void write_order_event(const char* client_order_id, const char* parent_order_id, const char* exch_order_id, int event_type, 
    44|                           double fill_price, double fill_qty, double remaining_qty, 
    45|                           int error_code, const char* error_msg, uint64_t last_update_id, bool is_maker,
    46|                           uint64_t transact_time, uint64_t trigger_ms) {
    47|        uint64_t write_idx = g_master_bridge->account_feed.write_idx.load(std::memory_order_relaxed);
    48|        int pos = write_idx & EVENT_RING_BUFFER_MASK;

    49|        OrderEventFrame& frame = g_master_bridge->account_feed.frames[pos];
    50|        frame.timestamp = transact_time; // 使用交易所成交时间作为事件时间
    51|        strncpy(frame.client_order_id, client_order_id, sizeof(frame.client_order_id) - 1);
    52|        frame.client_order_id[sizeof(frame.client_order_id) - 1] = '\0';
    53|        strncpy(frame.parent_order_id, parent_order_id, sizeof(frame.parent_order_id) - 1);
    54|        frame.parent_order_id[sizeof(frame.parent_order_id) - 1] = '\0';
    55|        // strncpy(frame.exch_order_id, exch_order_id, sizeof(frame.exch_order_id) - 1); // 【注释】不再使用 exch_order_id
    56|        // frame.exch_order_id[sizeof(frame.exch_order_id) - 1] = '\0'; // 【注释】不再使用 exch_order_id
    57|        frame.event_type = event_type;
    58|        frame.fill_price = fill_price;
    59|        frame.fill_qty = fill_qty;
    60|        frame.remaining_qty = remaining_qty;
    61|        frame.error_code = error_code;
    62|        strncpy(frame.error_msg, error_msg, sizeof(frame.error_msg) - 1);
    63|        frame.error_msg[sizeof(frame.error_msg) - 1] = '\0';
    64|        frame.last_update_id = last_update_id;
    65|        frame.is_maker = is_maker;
    66|        frame.trigger_ms = trigger_ms; // 写入行情起点时间戳
    67|
    68|        g_master_bridge->account_feed.write_idx.store(write_idx + 1, std::memory_order_release);
    69|
    70|        // 【核心修改】直接原子更新持仓量，因为现在只有市价单且即时成交
    71|        if (event_type == EVT_FULL_FILL || event_type == EVT_PARTIAL_FILL) {
    72|            // 如果是买入方向，增加持仓量；如果是卖出方向，减少持仓量
    73|            double qty_change = fill_qty; // 成交量就是变化量
    74|            // 需要判断订单方向，但目前的 on_order_filled 没有 side 参数。
    75|            // 假设我们从 client_order_id 中推断或者后续添加 side 参数。
    76|            // 暂时简化处理，假设 fill_qty 已经包含了方向性。
    77|            // 更严谨的做法是：当接收到订单事件时，需要知道该订单是 BUY 还是 SELL。
    78|            // 在目前的简化模型中，我们只处理市价单，并且知道是全平或反向开仓。
    79|            // 为了简化，我们假设 fill_qty 已经包含了方向。
    80|            // 实际应用中，如果需要精确的 positionAmt 更新，需要将 side 参数传递到这里。
    81|            
    82|            // 由于简化模型只处理市价单全平或反向开仓，这里的 fill_qty 是绝对值。
    83|            // position_amt 的更新应该根据订单的方向来决定是加还是减。
    84|            // 由于在 Executor::perform_binance_request 中，我们已经根据 side 参数发送了订单。
    85|            // 并且 fill_qty 应该是成交的绝对量。我们仍然需要知道原始订单的 side。
    86|            // 这里需要从 Python 端传入一个订单方向。
    87|            
    88|            // 鉴于目前 on_order_filled 已经有 is_maker 参数，但没有直接的 order_side
    89|            // 且 order_status_event 无法直接告诉我们原始订单的方向。
    90|            // 最安全的方式是：Python 端通过共享内存CommandFrame发送订单时，带上 side。
    91|            // C++ 端在处理成交事件时，需要知道这个订单的原始 side。
    92|            // 由于目前 on_order_filled 接口没有 side，我们暂时不能在这里直接更新 position_amt。
    93|            // 相反，我们应该让 Python 端根据接收到的事件和其本地状态来计算 position_amt。
    94|            // 因此，C++ 端只负责如实上报事件，不在这里更新 position_amt。
    95|
    96|            // 【临时注释】C++ 端不再直接更新 position_amt，由 Python 端统一处理
    97|            // g_master_bridge->account_feed.position_amt.fetch_add(qty_change * (is_buy_order ? 1 : -1), std::memory_order_release);
    98|        }
    99|    }
   100|
   101|    // 根据用户指示，C++ 端不再做策略决策，因此删除 check_trigger 函数的定义。
   102|    // 所有触发和决策逻辑将由 Python 端处理。
   103|    // void check_trigger(double current_price) {
   104|    //     // ... 原始 check_trigger 函数内容 ...
   105|    // }
   106|
   107|    // 新增 on_order_update_private_stream 函数来处理来自私有流的订单更新
   108|    // 这个函数会进行去重，并更新 g_active_orders
   109|    void on_order_update_private_stream(const char* client_order_id, const char* parent_order_id, const char* exch_order_id, 
   110|                                        double fill_px, double fill_qty, double remaining_qty, 
   111|                                        int order_status_event, uint64_t current_update_id, bool is_maker,
   112|                                        uint64_t transact_time, uint64_t trigger_ms) {
   113|        // 不再使用 std::lock_guard<std::mutex> lock(g_oms_mutex);
   114|        // 因为去重逻辑已经足够保证数据一致性，并且这里是单点写入。
   115|
   116|        // 由于简化模型只处理市价单，并且 C++ 端不再管理本地订单状态 (g_active_orders)
   117|        // 所有订单事件都直接写入共享内存，由 Python 端进行去重和状态管理。
   118|        // 因此，这里的去重逻辑和 g_active_orders 的使用被移除。
   119|        write_order_event(client_order_id, parent_order_id, exch_order_id, order_status_event, fill_px, fill_qty, remaining_qty, 0, "", current_update_id, is_maker, transact_time, trigger_ms);
   120|    }
   121|
   122|    // 原始 on_order_filled 函数现在用于接收来自 WebSocket 的原始订单事件（例如来自 User Data Stream）
   123|    // 并且会调用 on_order_update_private_stream 进行处理和去重。
   124|    void on_order_filled(const char* client_order_id, const char* parent_order_id, const char* exch_order_id, double fill_px, double fill_qty, double remaining_qty, int order_status_event, uint64_t transact_time, uint64_t last_update_id, bool is_maker, uint64_t trigger_ms) {
   125|        on_order_update_private_stream(client_order_id, parent_order_id, exch_order_id, fill_px, fill_qty, remaining_qty, order_status_event, last_update_id, is_maker, transact_time, trigger_ms);
   126|    }
   127|
   128|} // namespace Strategy
   129|