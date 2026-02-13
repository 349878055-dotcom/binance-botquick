#pragma once

#include <cstdint>
#include <atomic>
#include "Master_Logic_Bridge.h"
#include "Executor.h"
#include "Common.h"

namespace Strategy {
    // 根据用户指示，C++ 端不再做策略决策，因此删除 check_trigger 函数的声明。
    // void check_trigger(double current_price);

    // 重新定义 on_order_filled 函数签名，以接收完整的订单事件信息
    void on_order_filled(const char* client_order_id, const char* exch_order_id, double fill_px, double fill_qty, double remaining_qty, int order_status_event);

} // namespace Strategy
