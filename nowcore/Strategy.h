#pragma once

#include <cstdint>
#include <atomic>
#include "Master_Logic_Bridge.h"
#include "Executor.h"
#include "Common.h"

namespace Strategy {
    void check_trigger(double current_price);
    void on_order_filled(double fill_price, double fill_qty);
} // namespace Strategy
