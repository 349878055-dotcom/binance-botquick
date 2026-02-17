// Executor.cpp
// 交易执行器模块：负责与交易所 API 进行实际的交易操作（下单、撤单、查询账户等）。
// 它充当了策略层和网络层之间的桥梁，将策略指令转换为具体的 API 请求，并处理 API 响应。
#include "Executor.h" // 引入本模块的头文件
#include "Network.h" // 引用 Network 模块，用于执行 HTTP/HTTPS 请求与币安 API 通信
#include "Master_Logic_Bridge.h" // 引入共享内存结构体定义，用于与主逻辑模块进行数据交互
#include "Common.h" // 引入 Common 模块，用于获取时间戳等通用功能

#include <iostream> // 标准输入输出流
#include <map> // 键值对容器，用于构建 HTTP 请求参数
#include <string> // 字符串操作
#include <cstring> // 用于 strstr (查找子字符串), strncpy (安全复制字符串)
#include <cmath> // 用于 strtod (字符串转 double)
#include <thread> // 引入线程库，用于异步发送订单，避免阻塞主线程

namespace Executor { // 定义 Executor 命名空间，封装所有交易执行相关功能

    // 静态变量：用于存储币安 API Key 和 Secret。
    // 注意：在生产环境中，g_api_secret 不应硬编码，而应通过更安全的方式加载（例如环境变量、配置文件或加密存储）。
    static std::string g_api_key;
    static std::string g_api_secret;

    /**
     * @brief 初始化 Executor 模块。
     *        接收并存储币安 API Key 和 Secret，供后续 API 请求使用。
     * @param key 币安 API Key。
     * @param secret 币安 API Secret。
     */
    void init(const std::string& key, const std::string& secret) {
        g_api_key = key;
        g_api_secret = secret;
        // 打印调试信息，确认 API Key 已加载。
        // [冷数据日志]：此日志只在程序启动时输出一次，不影响核心交易路径性能，可以保留。
        std::cout << "[Executor] API Key loaded." << std::endl;
    }

    /**
     * @brief 辅助函数：从 JSON 字符串中提取指定键的 double 类型数值。
     *        这是一个简易的 JSON 解析方法，依赖 strstr 查找子字符串和 strtod 转换。
     *        在生产环境中，强烈建议使用更健壮的 JSON 解析库（如 nlohmann/json），以提高代码的鲁棒性。
     * @param json_str 包含 JSON 数据的字符串。
     * @param key 要查找的 JSON 键。
     * @return 如果找到键并成功转换，返回对应的 double 值；否则返回 0.0。
     */
    static double get_json_double(const std::string& json_str, const std::string& key) {
        std::string search_key = "\"" + key + "\":"; // 构建要查找的字符串，例如 """key":"""
        const char* key_ptr = strstr(json_str.c_str(), search_key.c_str()); // 查找键的起始位置
        if (key_ptr) { // 如果找到了键
            // 跳过键字符串，并将后续的数值字符串转换为 double 类型
            return std::strtod(key_ptr + search_key.length(), nullptr);
        }
        return 0.0; // 如果未找到键，返回默认值 0.0
    }

    /**
     * @brief 异步发送订单的内部实现函数。
     *        此函数在一个独立的线程中执行，避免阻塞主线程，提高系统响应性。
     *        它将订单指令构建成币安 API 请求，发送给交易所，并处理响应。
     *        所有订单事件（成功、部分成交、失败等）都将通过 Strategy::on_order_filled 回调处理。
     * @param client_order_id 客户端生成的唯一订单 ID，用于订单追踪。
     * @param symbol 交易对符号（例如 "BNBUSDT"）。
     * @param side 订单方向（"BUY" 或 "SELL"）。
     * @param order_type 订单类型（ORD_LIMIT=限价单, ORD_MARKET=市价单）。
     * @param tif 时间有效方式（TIF_GTC=Good Till Cancel, TIF_IOC=Immediate Or Cancel, TIF_FOK=Fill Or Kill）。
     * @param price 订单价格（仅限价单有效）。
     * @param quantity 订单数量。
     */
    void async_send_order_internal(const char* client_order_id, const std::string& symbol, const std::string& side, int order_type, int tif, double price, double quantity) {
        // 在一个新线程中执行异步操作，并将所有参数按值捕获（[=]）
        std::thread([=]() {
            std::string cid_str = client_order_id; // 为了线程安全，转存为 std::string
            std::map<std::string, std::string> params; // 用于构建 API 请求参数的映射表
            params["symbol"] = symbol; // 设置交易对
            params["side"] = side;     // 设置订单方向
            params["quantity"] = std::to_string(quantity); // 设置订单数量

            std::string type_str; // 订单类型字符串
            if (order_type == ORD_MARKET) { // 如果是市价单
                type_str = "MARKET";
            } else { // 默认为限价单 (ORD_LIMIT)
                type_str = "LIMIT";
                params["price"] = std::to_string(price); // 限价单需要指定价格

                // 根据 TIF (Time In Force) 类型设置参数
                if (tif == TIF_IOC) params["timeInForce"] = "IOC";       // Immediate Or Cancel (立即成交或取消)
                else if (tif == TIF_FOK) params["timeInForce"] = "FOK";  // Fill Or Kill (全部成交或取消)
                else params["timeInForce"] = "GTC";                    // Good Till Cancel (一直有效直到取消)
            }
            params["type"] = type_str; // 设置订单类型

            // 设置客户端自定义订单 ID，用于订单追踪
            params["newClientOrderId"] = cid_str;

            std::string response; // 用于存储 API 响应的缓冲区
            // 调用 Network 模块发送币安 API 请求
            long code = Network::perform_binance_request(
                Network::g_curl_handle, // 全局 CURL 句柄
                "POST",                 // HTTP 方法为 POST (用于下单)
                "/fapi/v1/order",       // 币安下单 API 路径
                params,                 // 请求参数
                response,               // 响应缓冲区
                g_api_key,              // API Key
                g_api_secret,           // API Secret
                true                    // 需要签名 (因为是私有交易请求)
            );

            if (code == 200) { // 如果 HTTP 请求成功 (HTTP 状态码 200)
                // 【重要】这里不再直接修改 strategy_status，所有订单状态的更新和决策都通过 Strategy::on_order_filled 推送事件。
                // Strategy 层将负责处理状态机去重和更新共享内存 (GenericShmStruct) 中的账户状态。
                // 
                // 解析 API 响应，提取交易所订单 ID (orderId), 客户端订单 ID (clientOrderId), 
                // 订单状态 (status), 成交价 (avgPrice), 成交量 (executedQty) 等关键信息。
                // 假设 response 是类似 {"symbol":"BTCUSDT","orderId":12345,"clientOrderId":"my_client_id","status":"FILLED","avgPrice":"10000.0","executedQty":"0.001",...}
                // 注意：这里仍然使用简易的 strstr/strtod 进行 JSON 解析，在生产环境中，强烈建议使用更健壮的 JSON 解析库。
                
                std::string exch_order_id_str; // 交易所订单 ID
                std::string order_status_str;  // 订单状态字符串
                double fill_px = 0.0;          // 实际成交价格
                double fill_qty = 0.0;         // 实际成交数量
                double remaining_qty = quantity; // 简化处理，实际剩余数量需要从响应中解析
                int event_type = EVT_SUBMITTED; // 默认事件类型为“已提交”

                // 简易解析 exch_order_id (交易所订单 ID)
                const char* order_id_ptr = strstr(response.c_str(), "\"orderId\":");
                if (order_id_ptr) {
                    exch_order_id_str = std::to_string((long long)std::strtod(order_id_ptr + 11, nullptr));
                }

                // 简易解析 status (订单状态)
                const char* status_ptr = strstr(response.c_str(), "\"status\":\"");
                if (status_ptr) {
                    status_ptr += 9; // 跳过 """status":"""
                    const char* status_end = strstr(status_ptr, "\""); // 查找状态字符串的结束引号
                    if (status_end) {
                        order_status_str = std::string(status_ptr, status_end - status_ptr); // 提取订单状态字符串
                    }
                }

                // 简易解析 executedQty (已成交数量) 和 avgPrice (平均成交价格) (如果订单已成交或部分成交)
                if (order_status_str == "FILLED" || order_status_str == "PARTIALLY_FILLED") {
                    const char* executed_qty_ptr = strstr(response.c_str(), "\"executedQty\":\"");
                    if (executed_qty_ptr) {
                        fill_qty = std::strtod(executed_qty_ptr + 14, nullptr); // 跳过 """executedQty":"""，转换为 double
                    }
                    const char* avg_price_ptr = strstr(response.c_str(), "\"avgPrice\":\"");
                    if (avg_price_ptr) {
                        fill_px = std::strtod(avg_price_ptr + 12, nullptr); // 跳过 """avgPrice":"""，转换为 double
                    }
                }

                // 根据解析到的订单状态字符串，映射到内部定义的事件类型
                if (order_status_str == "FILLED") event_type = EVT_FULL_FILL;         // 完全成交
                else if (order_status_str == "PARTIALLY_FILLED") event_type = EVT_PARTIAL_FILL; // 部分成交
                else if (order_status_str == "CANCELED") event_type = EVT_CANCELED;       // 已取消
                else if (order_status_str == "REJECTED") event_type = EVT_REJECTED;       // 已拒绝
                // 否则默认为 EVT_SUBMITTED (已提交)

                // 通知 Strategy 模块有订单事件发生。Strategy 层负责去重和更新共享内存中的账户状态。
                // C++ 侧只负责将事件写入共享内存，Python 侧消费共享内存后才进行日志打印。
                Strategy::on_order_filled(client_order_id, exch_order_id_str.c_str(), fill_px, fill_qty, remaining_qty - fill_qty, event_type);
 

            } else { // 如果 HTTP 请求失败 (HTTP 状态码非 200)
                // 下单失败，通知 Strategy 模块订单被拒绝。
                // 提取错误信息 (如果有)，并通过 Strategy::on_order_filled 推送拒绝事件。
                // C++ 侧只负责将事件写入共享内存，Python 侧消费共享内存后才进行日志打印。
                int error_code = -1; // 默认错误码
                std::string error_msg = "Order placement failed."; // 默认错误信息
                // 简易解析错误消息中的 "msg" 字段
                const char* msg_ptr = strstr(response.c_str(), "\"msg\":\"");
                if (msg_ptr) {
                    msg_ptr += 6; // 跳过 """msg":"""
                    const char* msg_end = strstr(msg_ptr, "\"");
                    if (msg_end) {
                        error_msg = std::string(msg_ptr, msg_end - msg_ptr);
                    }
                }
                // 简易解析错误消息中的 "code" 字段
                const char* code_ptr = strstr(response.c_str(), "\"code\":");
                if (code_ptr) {
                    error_code = (int)std::strtod(code_ptr + 7, nullptr);
                }
                // 通知 Strategy 模块订单被拒绝
                Strategy::on_order_filled(client_order_id, "", EVT_REJECTED, 0, 0, quantity, error_code, error_msg.c_str());
 
            }
        }).detach(); // 分离线程，让它独立运行。一旦线程被分离，其生命周期将不受 std::thread 对象的管理。
    }

    // --- 对外接口：供 Strategy 模块调用的公共函数 ---

    /**
     * @brief 发送一个限价订单到交易所。
     *        此函数会生成一个唯一的客户端订单 ID，并调用内部的异步发单函数。
     * @param symbol 交易对符号。
     * @param side 订单方向（"BUY" 或 "SELL"）。
     * @param price 订单价格。
     * @param quantity 订单数量。
     */
    void place_limit_order(const char* client_order_id, const std::string& symbol, const std::string& side, double price, double quantity, int tif) {
        // 调用异步内部发单函数，订单类型为限价单 (ORD_LIMIT)，现在把 tif 传进去。
        async_send_order_internal(client_order_id, symbol, side, ORD_LIMIT, tif, price, quantity);
    }

    /**
     * @brief 发送一个市价订单到交易所。
     *        此函数会生成一个唯一的客户端订单 ID，并调用内部的异步发单函数。
     *        市价单通常用于止损或止盈，以最快速度成交。
     * @param symbol 交易对符号。
     * @param side 订单方向（"BUY" 或 "SELL"）。
     * @param quantity 订单数量。
     */
    void place_market_order(const char* client_order_id, const std::string& symbol, const std::string& side, double quantity) {
        // 调用异步内部发单函数，订单类型为市价单 (ORD_MARKET)。市价单不需要指定价格，价格参数设为 0.0。
        async_send_order_internal(client_order_id, symbol, side, ORD_MARKET, TIF_GTC, 0.0, quantity);
    }

    /**
     * @brief 撤销指定交易对的所有挂单。
     *        通常用于紧急情况或清仓操作。
     * @param symbol 交易对符号。
     */
    void cancel_all_orders(const std::string& symbol) {
        std::map<std::string, std::string> params; // 请求参数
        params["symbol"] = symbol; // 设置要撤销订单的交易对

        std::string response; // 用于存储 API 响应的缓冲区
        // 调用 Network 模块发送币安 API 请求，方法为 DELETE。
        long code = Network::perform_binance_request(
            Network::g_curl_handle,
            "DELETE", // HTTP 方法为 DELETE (用于撤销订单)
            "/fapi/v1/allOpenOrders", // 币安撤销所有挂单的 API 路径
            params,
            response,
            g_api_key,
            g_api_secret,
            true // 需要签名
        );

        if (code == 200) {
            // [冷数据日志]：撤单成功日志，不影响核心交易路径性能，可以保留。
            std::cout << "[Executor] 撤单成功 (Cancel All) for " << symbol << "." << std::endl;
        } else {
            // [冷数据日志]：撤单失败日志，不影响核心交易路径性能，可以保留，用于排查问题。
            std::cerr << "[Executor] 撤单失败 for " << symbol << ": " << response << std::endl;
        }
    }

    /**
     * @brief 辅助函数：根据币安 API 返回的 stepSize 或 tickSize 字符串计算精度（小数位数）。
     *        例如，"0.001" 的精度是 3，"1.0" 的精度是 0。
     * @param step_size_str 步长或最小变动单位的字符串表示。
     * @return 精度（小数位数）。
     */
    static int get_precision_from_step(const std::string& step_size_str) {
        size_t dot_pos = step_size_str.find('.'); // 查找小数点的位置
        if (dot_pos == std::string::npos) { // 如果没有小数点
            return 0; // 精度为 0
        }
        // 查找小数点后第一个非零数字的位置
        size_t first_nonzero = step_size_str.find_first_not_of('0', dot_pos + 1);
        if (first_nonzero == std::string::npos) { // 如果小数点后全是零
            return 0; // 小数点后全是零，精度为 0
        }
        // 精度 = 第一个非零数字的位置 - 小数点的位置
        return first_nonzero - dot_pos;
    }

    /**
     * @brief 获取币安账户信息（余额、持仓等）并写入共享内存。
     *        此函数通过币安 REST API 查询账户信息，并将关键数据解析后更新到 GenericShmStruct 共享内存中。
     *        注意：这里仍然使用简易的 strstr/strtod 进行 JSON 解析，在生产环境中，强烈建议使用更健壮的 JSON 解析库。
     * @param symbol 相关的交易对符号（用于查询持仓信息）。
     * @param shm_bridge 指向 GenericShmStruct 共享内存的指针。
     */
    void fetch_account_info(const std::string& symbol, GenericShmStruct* shm_bridge) {
        if (!shm_bridge) { // 检查共享内存指针是否有效
            std::cerr << "[Executor] Error: Shared memory bridge is null." << std::endl;
            return;
        }

        std::string response_buffer; // 用于存储 API 响应的缓冲区
        std::map<std::string, std::string> params; // GET /fapi/v2/account API 不需要额外的查询参数，但为了签名，会内部注入 timestamp

        // 调用 Network 模块发送币安 API 请求，方法为 GET。
        long http_code = Network::perform_binance_request(
            Network::g_curl_handle,
            "GET",                  // HTTP 方法为 GET (用于查询)
            "/fapi/v2/account",     // 币安 U 本位合约账户信息 API 路径
            params,
            response_buffer,
            g_api_key,
            g_api_secret,
            true // 需要签名
        );

        if (http_code == 200) { // 如果 HTTP 请求成功
            // 解析 JSON 响应，提取 USDT 余额、BNB 余额和持仓信息。
            // [冷数据日志]：此日志在循环中不会被调用，属于冷数据日志，可以保留。
            const char* balance_cursor = response_buffer.c_str();
            double usdt_free_balance = 0.0; // USDT 可用余额
            double bnb_free_balance = 0.0;  // BNB 可用余额
            // double btc_free_balance = 0.0; // 已删除

            // 查找 USDT 余额
            const char* usdt_asset_ptr = strstr(balance_cursor, "\"asset\":\"USDT\"");
            if (usdt_asset_ptr) {
                const char* free_balance_ptr = strstr(usdt_asset_ptr, "\"free\":\"");
                if (free_balance_ptr) {
                    usdt_free_balance = std::strtod(free_balance_ptr + 8, nullptr);
                }
            }
            // 原子地更新共享内存中的 USDT 余额
            shm_bridge->account.usdt_balance.store(usdt_free_balance, std::memory_order_release);

            // 查找 BNB 余额
            const char* bnb_asset_ptr = strstr(balance_cursor, "\"asset\":\"BNB\"");
            if (bnb_asset_ptr) {
                const char* free_balance_ptr = strstr(bnb_asset_ptr, "\"free\":\"");
                if (free_balance_ptr) {
                    bnb_free_balance = std::strtod(free_balance_ptr + 8, nullptr);
                }
            }
            // 原子地更新共享内存中的 BNB 余额
            shm_bridge->account.bnb_balance.store(bnb_free_balance, std::memory_order_release);

            // 查找指定交易对的合约持仓信息
            const char* pos_ptr = strstr(response_buffer.c_str(), ("\"symbol\":\"" + symbol + "\"").c_str());
            if (pos_ptr) {
                const char* amt_ptr = strstr(pos_ptr, "\"positionAmt\":\"");
                if (amt_ptr) {
                    double pos_amt = std::strtod(amt_ptr + 15, nullptr);
                    shm_bridge->account.position_amt.store(pos_amt, std::memory_order_release); // 原子地更新持仓量
                }
                const char* entry_ptr = strstr(pos_ptr, "\"entryPrice\":\"");
                if (entry_ptr) {
                    double entry_px = std::strtod(entry_ptr + 14, nullptr);
                    shm_bridge->account.avg_price.store(entry_px, std::memory_order_release); // 原子地更新持仓均价
                }
            }


            // [冷数据日志]：成功获取账户信息日志，可以保留。
            std::cout << "[Executor] 成功获取账户信息。USDT 可用余额: " << usdt_free_balance 
                      << ", BNB 可用余额: " << bnb_free_balance 
                      << ", 持仓量: " << shm_bridge->account.position_amt.load() << ", 均价: " << shm_bridge->account.avg_price.load() << std::endl;
        } else {
            // [冷数据日志]：获取账户信息失败日志，可以保留，用于排查问题。
            std::cerr << "[Executor] 获取账户信息失败 (HTTP " << http_code << "): " << response_buffer << std::endl;
        }
    }

    void fetch_and_set_precision(const std::string& symbol, GenericShmStruct* shm_bridge) {
        if (!shm_bridge) {
            std::cerr << "[Executor] Error: Shared memory bridge is null." << std::endl;
            return;
        }

        std::string response_buffer;
        long http_code = Network::fetch_exchange_info(Network::g_curl_handle, symbol, response_buffer);

        if (http_code == 200) {
            //printf("[DEBUG] fetch_and_set_precision - Full response_buffer: %s\n", response_buffer.c_str());

            // 解析 JSON 响应
            // 查找 "symbols" 数组
            const char* symbols_start = strstr(response_buffer.c_str(), "\"symbols\":[");
            //printf("[DEBUG] symbols_start: %p\n", (void*)symbols_start);

            if (symbols_start) {
                const char* symbol_entry_start = strstr(symbols_start, ("\"symbol\":\"" + symbol + "\"").c_str());
                //printf("[DEBUG] symbol_entry_start: %p\n", (void*)symbol_entry_start);

                if (symbol_entry_start) {
                    // 找到对应的交易对
                    const char* filters_start = strstr(symbol_entry_start, "\"filters\":[");
                    //printf("[DEBUG] filters_start: %p\n", (void*)filters_start);

                    if (filters_start) {
                        const char* filters_end = strstr(filters_start, "]}");
                        //printf("[DEBUG] filters_end: %p\n", (void*)filters_end);

                        if (filters_end) {
                            std::string filters_str(filters_start, filters_end - filters_start + 2);
                            //printf("[DEBUG] filters_str: %s\n", filters_str.c_str());

                            // 查找 PRICE_FILTER
                            const char* price_filter_start = strstr(filters_str.c_str(), "\"filterType\":\"PRICE_FILTER\"");
                            //printf("[DEBUG] price_filter_start: %p\n", (void*)price_filter_start);
                            if (price_filter_start) {
                                const char* tick_size_ptr = strstr(price_filter_start, "\"tickSize\":\"");
                                //printf("[DEBUG] tick_size_ptr: %p\n", (void*)tick_size_ptr);
                                if (tick_size_ptr) {
                                    tick_size_ptr += 12; // 跳过 "tickSize":"
                                    const char* tick_size_end = strstr(tick_size_ptr, "\"");
                                    //printf("[DEBUG] tick_size_end: %p\n", (void*)tick_size_end);
                                    if (tick_size_end) {
                                        std::string tick_size_str(tick_size_ptr, tick_size_end - tick_size_ptr);
                                        //printf("[DEBUG] tick_size_str: %s\n", tick_size_str.c_str());
                                        int price_prec = get_precision_from_step(tick_size_str);
                                        shm_bridge->price_precision.store(price_prec, std::memory_order_release);
                                        printf("[Executor] %s 价格精度: %d (tickSize: %s)\n", symbol.c_str(), price_prec, tick_size_str.c_str());
                                    }
                                }
                            }

                            // 查找 LOT_SIZE
                            const char* lot_size_filter_start = strstr(filters_str.c_str(), "\"filterType\":\"LOT_SIZE\"");
                            //printf("[DEBUG] lot_size_filter_start: %p\n", (void*)lot_size_filter_start);
                            if (lot_size_filter_start) {
                                const char* step_size_ptr = strstr(lot_size_filter_start, "\"stepSize\":\"");
                                //printf("[DEBUG] step_size_ptr: %p\n", (void*)step_size_ptr);
                                if (step_size_ptr) {
                                    step_size_ptr += 12; // 跳过 "stepSize":"
                                    const char* step_size_end = strstr(step_size_ptr, "\"");
                                    //printf("[DEBUG] step_size_end: %p\n", (void*)step_size_end);
                                    if (step_size_end) {
                                        std::string step_size_str(step_size_ptr, step_size_end - step_size_ptr);
                                        //printf("[DEBUG] step_size_str: %s\n", step_size_str.c_str());
                                        int qty_prec = get_precision_from_step(step_size_str);
                                        shm_bridge->quantity_precision.store(qty_prec, std::memory_order_release);
                                        printf("[Executor] %s 数量精度: %d (stepSize: %s)\n", symbol.c_str(), qty_prec, step_size_str.c_str());
                                    }
                                }
                            }
                        } else {
                            //printf("[DEBUG] filters_end is nullptr. Search string: ]] \n");
                        }
                    } else {
                        //printf("[DEBUG] filters_start is nullptr. Search string: \"filters\":[\n");
                    }
                } else {
                    //printf("[DEBUG] symbol_entry_start is nullptr. Search string: \"symbol\":\"%s\"\n", symbol.c_str());
                }
            } else {
                //printf("[DEBUG] symbols_start is nullptr. Search string: \"symbols\":[\n");
            }
        } else {
            std::cerr << "[Executor] 获取交易对 " << symbol << " 精度信息失败 (HTTP " << http_code << "): " << response_buffer << std::endl;
        }
    }

} // namespace Executor