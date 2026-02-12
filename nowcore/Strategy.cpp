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
        // ... 可以添加更多字段，例如创建时间，更新时间等
    };

    // 本地 OMS (Order Management System)
    static std::unordered_map<std::string, LocalOrder> g_active_orders;
    static std::mutex g_oms_mutex; // 保护 g_active_orders

    // 本地状态缓存
    double local_entry_price = 0.0;
    double local_max_price = 0.0; // 记录持仓期间的最高价
    double local_hard_stop = 0.0;
    bool is_holding = false;      // 是否持仓中
    bool ratchet_active = false;  // 棘轮是否激活

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

    void check_trigger(double current_price) {
        // 1. 读取 C++ 自己的总体策略状态
        int current_strategy_status = g_master_bridge->account_feed.strategy_status.load(std::memory_order_relaxed);

        // 读取交易对 symbol (从 CommandFrame 中获取，或者从 SHM 的公共区获取，这里假设从公共区获取)
        std::string symbol_str(g_master_bridge->command_ring.frames[0].symbol); // 临时方案，后续从 CommandFrame 中获取

        // --- 紧急撤单逻辑 (优先级最高) ---
        if (current_strategy_status == 99) { 
            // 如果处于持仓状态，市价全抛
            if (is_holding) {
                Executor::place_market_order(symbol_str, "SELL", g_master_bridge->account_feed.position_amt.load(std::memory_order_relaxed));
            }
            Executor::cancel_all_orders(symbol_str); // 撤销所有挂单
            is_holding = false;
            ratchet_active = false; // 重置棘轮状态
            g_master_bridge->account_feed.strategy_status.store(0, std::memory_order_release); // 核心重置
            // 这里不需要 g_master_bridge->cpp_status，因为现在反馈通过 AccountRingBuffer
            write_order_event("SYSTEM", "N/A", EVT_CANCELED, 0, 0, 0, 0, "Emergency Cancel All orders.");
            return;
        }

        // --- 轮询 CommandRingBuffer 处理 Python 指令 ---
        uint64_t current_cmd_read_idx = g_master_bridge->command_ring.read_idx.load(std::memory_order_relaxed);
        uint64_t current_cmd_write_idx = g_master_bridge->command_ring.write_idx.load(std::memory_order_acquire);

        while (current_cmd_read_idx < current_cmd_write_idx) {
            const CommandFrame& cmd_frame = g_master_bridge->command_ring.frames[current_cmd_read_idx & COMMAND_RING_BUFFER_MASK];
            
            // 确保处理的是新指令
            if (cmd_frame.request_id == 0) { // request_id 为 0 通常表示无效或已处理
                current_cmd_read_idx++;
                continue;
            }

            std::string cmd_symbol(cmd_frame.symbol);
            std::string client_order_id(cmd_frame.client_order_id);

            switch (cmd_frame.action) {
                case ACT_NEW: {
                    // 检查是否空闲状态，避免重复下单
                    if (current_strategy_status == 0) { // IDLE
                        // 计算数量 (假设 CommandFrame.quantity 是 USDT 金额)
                        double target_price_raw = cmd_frame.price;
                        double target_usdt_amount_raw = cmd_frame.quantity;

                        // 从共享内存获取精度信息
                        int price_prec = g_master_bridge->price_precision.load(std::memory_order_relaxed);
                        int qty_prec = g_master_bridge->quantity_precision.load(std::memory_order_relaxed);

                        // 修正价格和数量到正确的精度
                        double target_price_corrected = std::round(target_price_raw * std::pow(10, price_prec)) / std::pow(10, price_prec);
                        double target_quantity_calculated = target_usdt_amount_raw / target_price_corrected;
                        double target_quantity_corrected = std::round(target_quantity_calculated * std::pow(10, qty_prec)) / std::pow(10, qty_prec);

                        // 从共享内存获取可用 USDT 余额
                        double available_usdt = g_master_bridge->account_feed.usdt_balance.load(std::memory_order_relaxed);
                        
                        // 计算下单所需的 USDT 估算值 (price * quantity)
                        double estimated_cost = target_price_corrected * target_quantity_corrected; 

                        if (available_usdt >= estimated_cost) {
                            std::lock_guard<std::mutex> lock(g_oms_mutex);
                            g_active_orders[client_order_id] = {
                                client_order_id, "", cmd_symbol, 
                                target_quantity_corrected, 0.0, target_price_corrected, 
                                cmd_frame.side, cmd_frame.type, cmd_frame.tif, EVT_SUBMITTED
                            };
                            Executor::place_limit_order(client_order_id.c_str(), cmd_symbol.c_str(), 
                                                        (cmd_frame.side == 1 ? "BUY" : "SELL"), 
                                                        cmd_frame.type, cmd_frame.tif,
                                                        target_price_corrected, target_quantity_corrected);
                            g_master_bridge->account_feed.strategy_status.store(1, std::memory_order_release); // WAITING
                            write_order_event(client_order_id.c_str(), "", EVT_SUBMITTED, 0, 0, target_quantity_corrected, 0, "Order submitted.");
                            printf("[Strategy] 资金充足，尝试挂单: %s %s %.4f 数量 (对应 %.2f USDT) @ %.2f (修正后). 可用 USDT: %.2f\\n", 
                                   cmd_symbol.c_str(), (cmd_frame.side == 1 ? "BUY" : "SELL"), target_quantity_corrected, target_usdt_amount_raw, target_price_corrected, available_usdt);
                        } else {
                            // 资金不足，直接反馈拒绝
                            write_order_event(client_order_id.c_str(), "", EVT_REJECTED, 0, 0, 0, -1, "Insufficient funds.");
                            g_master_bridge->account_feed.strategy_status.store(0, std::memory_order_release); // 回退到空闲状态
                            printf("[Strategy] 资金不足，无法挂单。所需 %.2f USDT，可用 %.2f USDT。\\n", estimated_cost, available_usdt);
                        }
                    } else {
                        // 策略非空闲，拒绝新订单
                        write_order_event(client_order_id.c_str(), "", EVT_REJECTED, 0, 0, 0, -2, "Strategy not in IDLE state.");
                        printf("[Strategy] 策略非空闲，拒绝新订单 %s。\\n", client_order_id.c_str());
                    }
                    break;
                }
                case ACT_CANCEL: {
                    std::lock_guard<std::mutex> lock(g_oms_mutex);
                    auto it = g_active_orders.find(client_order_id);
                    if (it != g_active_orders.end()) {
                        // 检查订单状态是否允许取消
                        if (it->second.status == EVT_SUBMITTED || it->second.status == EVT_PARTIAL_FILL) {
                            Executor::cancel_order(client_order_id.c_str(), cmd_symbol.c_str());
                            printf("[Strategy] 尝试取消订单: %s\\n", client_order_id.c_str());
                        } else {
                            write_order_event(client_order_id.c_str(), it->second.exch_ord_id.c_str(), EVT_REJECTED, 0, 0, it->second.orig_qty - it->second.filled_qty, -3, "Order state does not allow cancellation.");
                            printf("[Strategy] 订单 %s 状态不允许取消。\\n", client_order_id.c_str());
                        }
                    } else {
                        write_order_event(client_order_id.c_str(), "", EVT_REJECTED, 0, 0, 0, -4, "Order not found for cancellation.");
                        printf("[Strategy] 未找到订单 %s 进行取消。\\n", client_order_id.c_str());
                    }
                    break;
                }
                case ACT_AMEND: {
                    std::lock_guard<std::mutex> lock(g_oms_mutex);
                    auto it = g_active_orders.find(client_order_id);
                    if (it != g_active_orders.end()) {
                        // 检查订单状态是否允许修改
                        if (it->second.status == EVT_SUBMITTED || it->second.status == EVT_PARTIAL_FILL) {
                            Executor::amend_order(client_order_id.c_str(), cmd_symbol.c_str(), 
                                                 cmd_frame.new_price, cmd_frame.new_quantity);
                            printf("[Strategy] 尝试修改订单 %s: 新价格 %.2f, 新数量 %.4f\\n", 
                                   client_order_id.c_str(), cmd_frame.new_price, cmd_frame.new_quantity);
                        } else {
                            write_order_event(client_order_id.c_str(), it->second.exch_ord_id.c_str(), EVT_REJECTED, 0, 0, it->second.orig_qty - it->second.filled_qty, -5, "Order state does not allow amendment.");
                            printf("[Strategy] 订单 %s 状态不允许修改。\\n", client_order_id.c_str());
                        }
                    } else {
                        write_order_event(client_order_id.c_str(), "", EVT_REJECTED, 0, 0, 0, -6, "Order not found for amendment.");
                        printf("[Strategy] 未找到订单 %s 进行修改。\\n", client_order_id.c_str());
                    }
                    break;
                }
                case ACT_CANCEL_ALL: {
                    Executor::cancel_all_orders(cmd_symbol.c_str()); // 撤销所有挂单
                    // 清理本地活动订单中状态为 SUBMITTED 或 PARTIAL_FILL 的订单
                    std::lock_guard<std::mutex> lock(g_oms_mutex);
                    for (auto it = g_active_orders.begin(); it != g_active_orders.end(); ) {
                        if (it->second.status == EVT_SUBMITTED || it->second.status == EVT_PARTIAL_FILL) {
                            write_order_event(it->second.cl_ord_id.c_str(), it->second.exch_ord_id.c_str(), EVT_CANCELED, 0, 0, it->second.orig_qty - it->second.filled_qty, 0, "Cancel All triggered.");
                            it = g_active_orders.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    printf("[Strategy] 收到全撤指令，撤销所有挂单 for %s。\\n", cmd_symbol.c_str());
                    break;
                }
                default:
                    printf("[Strategy] 收到未知指令类型: %d\\n", cmd_frame.action);
                    break;
            }
            // 标记为已处理 (清除 request_id 或其他方式)
            // 这里为了简单，我们假定 Python 会通过 read_idx 推进来确认
            // 实际可能需要将 request_id 归零或者用一个 flag 标记
            // g_master_bridge->command_ring.frames[current_cmd_read_idx & COMMAND_RING_BUFFER_MASK].request_id = 0; // 清零表示已处理
            current_cmd_read_idx++;
        }
        g_master_bridge->command_ring.read_idx.store(current_cmd_read_idx, std::memory_order_release);


        // --- 棘轮止盈逻辑 (持仓后的自治) ---
        if (is_holding) {
            // 1. 更新历史最高价
            local_max_price = std::max(local_max_price, current_price);

            // 2. 检查硬止损 (保命)
            if (current_price <= local_hard_stop) {
                Executor::place_market_order(symbol_str, "SELL", g_master_bridge->account_feed.position_amt.load(std::memory_order_relaxed)); // 市价全抛
                is_holding = false;
                ratchet_active = false; // 重置棘轮状态
                g_master_bridge->account_feed.strategy_status.store(0, std::memory_order_release); // 核心重置
                // 这里不需要 g_master_bridge->cpp_status
                write_order_event("SYSTEM", "N/A", EVT_FULL_FILL, current_price, g_master_bridge->account_feed.position_amt.load(), 0, 0, "Hard stop triggered.");
                return;
            }

            // 3. 检查是否激活棘轮 (利润奔跑)
            double activate_threshold = g_master_bridge->ratchet_active_gap.load(std::memory_order_relaxed);
            if (!ratchet_active && current_price > activate_threshold) {
                ratchet_active = true;
                local_hard_stop = local_entry_price * (1 + 0.001); // 止损上移到保本位 + 0.1%
            }

            // 4. 棘轮追踪
            if (ratchet_active) {
                double callback_ratio = g_master_bridge->ratchet_callback.load(std::memory_order_relaxed);
                double dynamic_stop = local_max_price * (1 - callback_ratio); // 回撤比例从共享内存获取
                // 止损线只能上移
                if (dynamic_stop > local_hard_stop) {
                    local_hard_stop = dynamic_stop;
                }
            }
        }
    }

    void on_order_filled(const char* client_order_id, const char* exch_order_id, double fill_price, double fill_qty, double remaining_qty, int order_status) {
        std::lock_guard<std::mutex> lock(g_oms_mutex);
        auto it = g_active_orders.find(client_order_id);
        if (it != g_active_orders.end()) {
            // 更新本地订单状态
            it->second.exch_ord_id = exch_order_id;
            it->second.filled_qty += fill_qty; // 累加成交量
            it->second.status = order_status; // 更新为新的订单状态

            // 更新共享内存中的持仓信息 (如果这是最终成交)
            if (order_status == EVT_FULL_FILL || order_status == EVT_PARTIAL_FILL) { // 只有在实际成交时才更新持仓
                g_master_bridge->account_feed.position_amt.fetch_add(fill_qty, std::memory_order_release);
                // 均价计算需要更复杂的逻辑，这里暂时简化，或等待 Python 端的处理
                // g_master_bridge->account_feed.avg_price.store(fill_price, std::memory_order_release); 
            }

            // 推送事件到 AccountRingBuffer
            write_order_event(client_order_id, exch_order_id, order_status, fill_price, fill_qty, remaining_qty, 0, "");
            
            // 如果是完全成交，从活动订单中移除
            if (order_status == EVT_FULL_FILL || order_status == EVT_CANCELED || order_status == EVT_REJECTED) {
                g_active_orders.erase(it);
                printf("[Strategy] Order %s removed from active orders.\\n", client_order_id);
            }

            printf("[Strategy] Order %s event: ExchOrderID=%s, Status=%d, FillPx=%.2f, FillQty=%.4f, Remaining=%.4f.\\n", 
                   client_order_id, exch_order_id, order_status, fill_price, fill_qty, remaining_qty);

            // 如果当前策略是等待状态，并且订单完全成交，则更新策略状态
            if (g_master_bridge->account_feed.strategy_status.load(std::memory_order_relaxed) == 1 && order_status == EVT_FULL_FILL) {
                g_master_bridge->account_feed.strategy_status.store(2, std::memory_order_release); // FILLED
                is_holding = true;
                local_entry_price = fill_price; // 假设这是首次成交的均价
                local_max_price = fill_price;
                local_hard_stop = g_master_bridge->hard_stop_price.load(std::memory_order_relaxed);
                ratchet_active = false; // 成交后重置棘轮激活状态
                printf("[Strategy] Strategy status updated to FILLED.\\n");
            }
        } else {
            // 如果是交易所推送的未知订单事件，也写入反馈队列，但本地OMS不管理
            write_order_event(client_order_id, exch_order_id, order_status, fill_price, fill_qty, remaining_qty, 0, "Unknown order event from exchange.");
            printf("[Strategy] Received event for unknown order %s (ExchOrderID: %s). Status=%d, FillPx=%.2f, FillQty=%.4f.\\n", 
                   client_order_id, exch_order_id, order_status, fill_price, fill_qty);
        }
    }

} // namespace Strategy
