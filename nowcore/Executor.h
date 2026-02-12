// Executor.h
#pragma once
#include <string>

struct GenericShmStruct; // 前置声明

namespace Executor {

    // 1. 初始化：把 API Key 传进来
    void init(const std::string& api_key, const std::string& api_secret);

    // 2. 挂单 (Limit)
    // side: "BUY" or "SELL"
    void place_limit_order(const std::string& symbol, const std::string& side, double price, double quantity);

    // 3. 市价全平 (Market)
    // 用于止损或止盈
    void place_market_order(const std::string& symbol, const std::string& side, double quantity);

    // 4. 撤销所有单 (Cancel All)
    void cancel_all_orders(const std::string& symbol);

    // 5. 查询账户信息并写入共享内存
    void fetch_account_info(const std::string& symbol, GenericShmStruct* shm_bridge);

    // 6. 获取交易对精度信息并写入共享内存
    void fetch_and_set_precision(const std::string& symbol, GenericShmStruct* shm_bridge);

}