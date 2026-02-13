#include "Strategy.h"
#include "Executor.h" // 假设你有 Executor 发单的头文件
#include "Master_Logic_Bridge.h"
#include <algorithm> // for std::max
#include <string>
#include <cmath> // For std::round and std::pow

namespace Strategy {

#include <unordered_map>
#include <mutex> // For std::mutex

    // 本地订单结构体
    struct LocalOrder {
        std::string cl_ord_id;       // 客户端订单 ID
        std::string exch_ord_id;     // 交易所订单 ID
        std::string symbol;          // 交易对
        double      orig_qty;        // 原始数量
        double      filled_qty;      // 已成交数量
        double      price;           // 订单价格
        int         side;            // 1=Buy, -1=Sell
        int         type;            // 订单类型 (Limit/Market)
        int         tif;             // 时间有效方式
        int         status;          // 订单状态 (NEW, PARTIALLY_FILLED, FILLED, CANCELED, REJECTED)
        int         last_event_type; // 记录上一次接收到的事件类型，用于去重
        // ... 可以添加更多字段，例如创建时间，更新时间等
    };

    // 本地 OMS (Order Management System)
    static std::unordered_map<std::string, LocalOrder> g_active_orders;
    static std::mutex g_oms_mutex; // 保护 g_active_orders

    // 根据用户指示，C++ 端不再做策略决策，因此删除所有本地策略状态变量。
    // double local_entry_price = 0.0;
    // double local_max_price = 0.0; // 记录持仓期间的最高价
    // double local_hard_stop = 0.0;
    // bool is_holding = false;      // 是否持仓中
    // bool ratchet_active = false;  // 棘轮是否激活

    void write_order_event(const char* client_order_id, const char* exch_order_id, int event_type, 
                         double fill_price, double fill_qty, double remaining_qty, 
                         int error_code, const char* error_msg) {
        uint64_t write_idx = g_master_bridge->account_feed.write_idx.load(std::memory_order_relaxed);
        int pos = write_idx & EVENT_RING_BUFFER_MASK;

        OrderEventFrame& frame = g_master_bridge->account_feed.frames[pos];
        frame.timestamp = Common::get_now_ns();
        strncpy(frame.client_order_id, client_order_id, sizeof(frame.client_order_id) - 1);
        frame.client_order_id[sizeof(frame.client_order_id) - 1] = '\0';
        strncpy(frame.exch_order_id, exch_order_id, sizeof(frame.exch_order_id) - 1);
        frame.exch_order_id[sizeof(frame.exch_order_id) - 1] = '\0';
        frame.event_type = event_type;
        frame.fill_price = fill_price;
        frame.fill_qty = fill_qty;
        frame.remaining_qty = remaining_qty;
        frame.error_code = error_code;
        strncpy(frame.error_msg, error_msg, sizeof(frame.error_msg) - 1);
        frame.error_msg[sizeof(frame.error_msg) - 1] = '\0';

        g_master_bridge->account_feed.write_idx.store(write_idx + 1, std::memory_order_release);
    }

    // 根据用户指示，C++ 端不再做策略决策，因此删除 check_trigger 函数的定义。
    // 所有触发和决策逻辑将由 Python 端处理。
    // void check_trigger(double current_price) {
    //     // ... 原始 check_trigger 函数内容 ...
    // }

    void on_order_filled(const char* client_order_id, const char* exch_order_id, double fill_px, double fill_qty, double remaining_qty, int order_status_event) {
        std::lock_guard<std::mutex> lock(g_oms_mutex);
        auto it = g_active_orders.find(client_order_id);
        if (it != g_active_orders.end()) {
            // 避免重复处理最终状态的订单事件

            // 更新本地订单状态
            it->second.exch_ord_id = exch_order_id;
            it->second.filled_qty += fill_qty; // 累加成交量
            it->second.status = order_status_event; // 更新为新的订单状态
            it->second.last_event_type = order_status_event; // 更新上一次事件类型

            // 更新共享内存中的持仓信息 (如果这是最终成交)
            // C++ 端仅如实反映成交，不进行决策
            if (order_status_event == EVT_FULL_FILL || order_status_event == EVT_PARTIAL_FILL) { // 只有在实际成交时才更新持仓
                g_master_bridge->account_feed.position_amt.fetch_add(fill_qty, std::memory_order_release);
                // 均价计算需要更复杂的逻辑，且应由 Python 端决策，C++ 端只负责更新持仓量
            }

            // 推送事件到 AccountRingBuffer
            write_order_event(client_order_id, exch_order_id, order_status_event, fill_px, fill_qty, remaining_qty, 0, "");
            
            // 如果是完全成交，从活动订单中移除
            if (order_status_event == EVT_FULL_FILL || order_status_event == EVT_CANCELED || order_status_event == EVT_REJECTED) {
                g_active_orders.erase(it);
            }


            // 根据用户指示，C++ 端不再做策略决策，因此移除 strategy_status 的原子更新逻辑，
            // 以及 is_holding, local_entry_price, local_max_price, local_hard_stop, ratchet_active 等状态的更新。
            // 这些决策和状态管理将由 Python 端完成。

        } else {
            // 如果是交易所推送的未知订单事件，也写入反馈队列，但本地OMS不管理
            write_order_event(client_order_id, exch_order_id, order_status_event, fill_px, fill_qty, remaining_qty, 0, "Unknown order event from exchange.");
        }
    }

} // namespace Strategy
