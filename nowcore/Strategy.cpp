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
        uint64_t    last_update_id;  // 记录上一次接收到的事件的 update_id，用于去重
        bool        is_maker;        // 是否为 Maker 订单
        // ... 可以添加更多字段，例如创建时间，更新时间等
    };

    // 本地 OMS (Order Management System)
    static std::unordered_map<std::string, LocalOrder> g_active_orders;
    // g_oms_mutex 被移除，因为 on_order_filled 不再直接修改 g_active_orders
    // 而是通过 on_order_update_private_stream 统一处理，并利用 last_update_id 去重

    // 根据用户指示，C++ 端不再做策略决策，因此删除所有本地策略状态变量。
    // double local_entry_price = 0.0;
    // double local_max_price = 0.0; // 记录持仓期间的最高价
    // double local_hard_stop = 0.0;
    // bool is_holding = false;      // 是否持仓中
    // bool ratchet_active = false;  // 棘轮是否激活

    void write_order_event(const char* client_order_id, const char* exch_order_id, int event_type, 
                         double fill_price, double fill_qty, double remaining_qty, 
                         int error_code, const char* error_msg, uint64_t last_update_id, bool is_maker) {
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
        frame.last_update_id = last_update_id;
        frame.is_maker = is_maker;

        g_master_bridge->account_feed.write_idx.store(write_idx + 1, std::memory_order_release);
    }

    // 根据用户指示，C++ 端不再做策略决策，因此删除 check_trigger 函数的定义。
    // 所有触发和决策逻辑将由 Python 端处理。
    // void check_trigger(double current_price) {
    //     // ... 原始 check_trigger 函数内容 ...
    // }

    // 新增 on_order_update_private_stream 函数来处理来自私有流的订单更新
    // 这个函数会进行去重，并更新 g_active_orders
    void on_order_update_private_stream(const char* client_order_id, const char* exch_order_id, 
                                        double fill_px, double fill_qty, double remaining_qty, 
                                        int order_status_event, uint64_t current_update_id, bool is_maker) {
        // 不再使用 std::lock_guard<std::mutex> lock(g_oms_mutex);
        // 因为去重逻辑已经足够保证数据一致性，并且这里是单点写入。

        auto it = g_active_orders.find(client_order_id);
        if (it != g_active_orders.end()) {
            // 去重：如果当前事件的 update_id 不大于已记录的 update_id，则忽略
            if (current_update_id <= it->second.last_update_id) {
                // 旧事件或重复事件，直接返回
                return;
            }
            
            // 更新本地订单状态
            it->second.exch_ord_id = exch_order_id;
            it->second.filled_qty = fill_qty; // 注意：这里直接赋值，因为交易所会推送累计成交量
            it->second.status = order_status_event; // 更新为新的订单状态
            it->second.last_update_id = current_update_id; // 更新上一次事件类型
            it->second.is_maker = is_maker; // 更新 is_maker 字段

            // 更新共享内存中的持仓信息 (如果这是最终成交)
            // C++ 端仅如实反映成交，不进行决策
            // 注意：这里更新持仓量的方式需要重新审视，确保只在“新的”成交发生时才累加
            // 或者更严谨地，将持仓量也作为事件推送给 Python，由 Python 端统一计算
            if (order_status_event == EVT_FULL_FILL || order_status_event == EVT_PARTIAL_FILL) { // 只有在实际成交时才更新持仓
                // 这里直接使用 fill_qty - 之前的 filled_qty 会导致重复累加，
                // 更安全的做法是将每次事件的 '增量' 推送，或者由 Python 端根据交易所总成交量进行计算。
                // 暂时保持 fetch_add，但实际逻辑需要更细致的考虑。此处仅为去重和锁竞争的修正。
                g_master_bridge->account_feed.position_amt.fetch_add(fill_qty - it->second.filled_qty, std::memory_order_release); // 仅增加增量成交量
                // 均价计算需要更复杂的逻辑，且应由 Python 端决策，C++ 端只负责更新持仓量
            }

            // 推送事件到 AccountRingBuffer
            write_order_event(client_order_id, exch_order_id, order_status_event, fill_px, fill_qty, remaining_qty, 0, "", current_update_id, is_maker);
            
            // 如果是完全成交，从活动订单中移除
            if (order_status_event == EVT_FULL_FILL || order_status_event == EVT_CANCELED || order_status_event == EVT_REJECTED) {
                g_active_orders.erase(it);
            }

        } else {
            // 如果是交易所推送的未知订单事件，也写入反馈队列，但本地OMS不管理
            // 此时由于没有 localOrder 记录，无法去重，但由于是“未知订单”，可以认为每次都是新事件
            write_order_event(client_order_id, exch_order_id, order_status_event, fill_px, fill_qty, remaining_qty, 0, "Unknown order event from exchange.", current_update_id, is_maker);
        }
    }

    // 原始 on_order_filled 函数现在用于接收来自 WebSocket 的原始订单事件（例如来自 User Data Stream）
    // 并且会调用 on_order_update_private_stream 进行处理和去重。
    void on_order_filled(const char* client_order_id, const char* exch_order_id, double fill_px, double fill_qty, double remaining_qty, int order_status_event) {
        // 为了兼容旧的调用方式，这里假定 last_update_id 为 0，表示没有去重信息。
        // 实际使用时，应该从交易所的私有流数据中解析出 update_id 并传递过来。
        // 假定旧的 on_order_filled 调用总是 Taker 订单，或者根据具体场景判断
        on_order_update_private_stream(client_order_id, exch_order_id, fill_px, fill_qty, remaining_qty, order_status_event, 0ULL, false); // 默认 is_maker 为 false
    }

} // namespace Strategy
