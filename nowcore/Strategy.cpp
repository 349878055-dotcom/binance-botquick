#include "Strategy.h"
#include "Executor.h" // 假设你有 Executor 发单的头文件
#include "Master_Logic_Bridge.h"
#include <algorithm> // for std::max
#include <string>
#include <cmath> // For std::round and std::pow
#include <unordered_map>
#include <mutex> // For std::mutex

namespace Strategy {

    // 本地订单结构体 -- 【删除】不再使用 LocalOrder
    // struct LocalOrder {
    //     std::string cl_ord_id;       // 客户端订单 ID
    //     std::string exch_ord_id;     // 交易所订单 ID
    //     std::string symbol;          // 交易对
    //     double      orig_qty;        // 原始数量
    //     double      filled_qty;      // 已成交数量
    //     double      price;           // 订单价格
    //     int         side;            // 1=Buy, -1=Sell
    //     int         type;            // 订单类型 (Limit/Market)
    //     int         tif;             // 时间有效方式
    //     int         status;          // 订单状态 (NEW, PARTIALLY_FILLED, FILLED, CANCELED, REJECTED)
    //     uint64_t    last_update_id;  // 记录上一次接收到的事件的 update_id，用于去重
    //     bool        is_maker;        // 是否为 Maker 订单
    //     // ... 可以添加更多字段，例如创建时间，更新时间等
    // };

    // 本地 OMS (Order Management System) -- 【删除】不再在 C++ 端管理本地订单状态
    // static std::unordered_map<std::string, LocalOrder> g_active_orders;
    // g_oms_mutex 被移除，因为 on_order_filled 不再直接修改 g_active_orders
    // 而是通过 on_order_update_private_stream 统一处理，并利用 last_update_id 去重

    // 根据用户指示，C++ 端不再做策略决策，因此删除所有本地策略状态变量。
    // double local_entry_price = 0.0;
    // double local_max_price = 0.0; // 记录持仓期间的最高价
    // double local_hard_stop = 0.0;
    // bool is_holding = false;      // 是否持仓中
    // bool ratchet_active = false;  // 棘轮是否激活

    // 新增 write_order_event 函数签名，以包含 transact_time 和 trigger_ms
    void write_order_event(const char* client_order_id, const char* parent_order_id, const char* exch_order_id, int event_type, int side,
                           double fill_price, double fill_qty, double remaining_qty, 
                           int error_code, const char* error_msg, uint64_t last_update_id, bool is_maker,
                           uint64_t transact_time, uint64_t trigger_ms) {
        uint64_t write_idx = g_master_bridge->account_feed.write_idx.load(std::memory_order_relaxed);
        int pos = write_idx & EVENT_RING_BUFFER_MASK;

        OrderEventFrame& frame = g_master_bridge->account_feed.frames[pos];
        frame.timestamp = transact_time; // 使用交易所成交时间作为事件时间
        strncpy(frame.client_order_id, client_order_id, sizeof(frame.client_order_id) - 1);
        frame.client_order_id[sizeof(frame.client_order_id) - 1] = '\0';
        strncpy(frame.parent_order_id, parent_order_id, sizeof(frame.parent_order_id) - 1);
        frame.parent_order_id[sizeof(frame.parent_order_id) - 1] = '\0';
        // strncpy(frame.exch_order_id, exch_order_id, sizeof(frame.exch_order_id) - 1); // 【注释】不再使用 exch_order_id
        // frame.exch_order_id[sizeof(frame.exch_order_id) - 1] = '\0'; // 【注释】不再使用 exch_order_id
        frame.event_type = event_type;
        frame.side = side; // 写入订单方向
        frame.fill_price = fill_price;
        frame.fill_qty = fill_qty;
        frame.remaining_qty = remaining_qty;
        frame.error_code = error_code;
        strncpy(frame.error_msg, error_msg, sizeof(frame.error_msg) - 1);
        frame.error_msg[sizeof(frame.error_msg) - 1] = '\0';
        frame.last_update_id = last_update_id;
        frame.is_maker = is_maker;
        frame.trigger_ms = trigger_ms; // 写入行情起点时间戳

        g_master_bridge->account_feed.write_idx.store(write_idx + 1, std::memory_order_release);

        // 【核心修改】直接原子更新持仓量，因为现在只有市价单且即时成交
        if (event_type == EVT_FULL_FILL || event_type == EVT_PARTIAL_FILL) {
            g_master_bridge->account_feed.position_amt.fetch_add(fill_qty * side, std::memory_order_release);
        }
    }

    // 根据用户指示，C++ 端不再做策略决策，因此删除 check_trigger 函数的定义。
    // 所有触发和决策逻辑将由 Python 端处理。
    // void check_trigger(double current_price) {
    //     // ... 原始 check_trigger 函数内容 ...
    // }

    // 新增 on_order_update_private_stream 函数来处理来自私有流的订单更新
    // 这个函数会进行去重，并更新 g_active_orders
    void on_order_update_private_stream(const char* client_order_id, const char* parent_order_id, const char* exch_order_id, 
                                        double fill_px, double fill_qty, double remaining_qty, int order_status_event, 
                                        uint64_t current_update_id, bool is_maker, uint64_t transact_time, 
                                        uint64_t trigger_ms, int side) {
        // 不再使用 std::lock_guard<std::mutex> lock(g_oms_mutex);
        // 因为去重逻辑已经足够保证数据一致性，并且这里是单点写入。

        // 由于简化模型只处理市价单，并且 C++ 端不再管理本地订单状态 (g_active_orders)
        // 所有订单事件都直接写入共享内存，由 Python 端进行去重和状态管理。
        // 因此，这里的去重逻辑和 g_active_orders 的使用被移除。
        write_order_event(client_order_id, parent_order_id, exch_order_id, order_status_event, side, fill_px, fill_qty, remaining_qty, 0, "", current_update_id, is_maker, transact_time, trigger_ms);
    }

    // 原始 on_order_filled 函数现在用于接收来自 WebSocket 的原始订单事件（例如来自 User Data Stream）
    // 并且会调用 on_order_update_private_stream 进行处理和去重。
    void on_order_filled(const char* client_order_id, const char* parent_order_id, const char* exch_order_id, double fill_px, double fill_qty, double remaining_qty, int order_status_event, uint64_t transact_time, uint64_t last_update_id, bool is_maker, uint64_t trigger_ms, int side) {
        on_order_update_private_stream(client_order_id, parent_order_id, exch_order_id, fill_px, fill_qty, remaining_qty, order_status_event, last_update_id, is_maker, transact_time, trigger_ms, side);
    }

} // namespace Strategy