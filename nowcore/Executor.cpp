// Executor.cpp
#include "Executor.h"
#include "Network.h" // 引用 Network 里的 perform_binance_request
#include "Master_Logic_Bridge.h" // 用于访问 GenericShmStruct
#include <iostream>
#include <map>
#include <string>
#include <cstring> // For strstr
#include <cmath> // For strtod
#include <thread> // 引入线程库

namespace Executor {

    // 静态变量存储 Key
    static std::string g_api_key;
    static std::string g_api_secret;

    void init(const std::string& key, const std::string& secret) {
        g_api_key = key;
        g_api_secret = secret;
        std::cout << "[Executor] API Key loaded." << std::endl;
    }

    // 辅助函数：简单的 JSON 解析函数
    static double get_json_double(const std::string& json_str, const std::string& key) {
        std::string search_key = """ + key + "":"";
        const char* key_ptr = strstr(json_str.c_str(), search_key.c_str());
        if (key_ptr) {
            return std::strtod(key_ptr + search_key.length(), nullptr);
        }
        return 0.0;
    }

    // 异步发单的内部实现
    void async_send_order_internal(const char* client_order_id, const std::string& symbol, const std::string& side, int order_type, int tif, double price, double quantity) {
        std::thread([=]() {
            std::map<std::string, std::string> params;
            params["symbol"] = symbol;
            params["side"] = side;
            params["quantity"] = std::to_string(quantity);

            // 订单类型映射
            std::string type_str;
            if (order_type == ORD_MARKET) {
                type_str = "MARKET";
            } else { // 默认为限价单
                type_str = "LIMIT";
                params["price"] = std::to_string(price);

                // TIF 映射
                if (tif == TIF_IOC) params["timeInForce"] = "IOC";
                else if (tif == TIF_FOK) params["timeInForce"] = "FOK";
                else params["timeInForce"] = "GTC"; // 默认为 GTC
            }
            params["type"] = type_str;

            // 设置 clientOrderId
            params["newClientOrderId"] = client_order_id; 

            std::string response;
            long code = Network::perform_binance_request(
                Network::g_curl_handle,
                "POST",
                "/fapi/v1/order",
                params,
                response,
                g_api_key,
                g_api_secret,
                true
            );

            if (code == 200) {
                // 【重要】这里不再直接修改 strategy_status，而是通过 write_order_event 推送事件
                // 解析响应，提取交易所订单ID (orderId), 客户端订单ID (clientOrderId), 订单状态 (status), 成交价 (avgPrice), 成交量 (executedQty)
                // 假设 response 是类似 {"symbol":"BTCUSDT","orderId":12345,"clientOrderId":"my_client_id","status":"FILLED","avgPrice":"10000.0","executedQty":"0.001",...}
                // 这里需要更健壮的 JSON 解析库，目前使用简易 strstr/strtod
                
                std::string exch_order_id_str; // 从 response 解析
                std::string order_status_str; // 从 response 解析
                double fill_px = 0.0; // 从 response 解析
                double fill_qty = 0.0; // 从 response 解析
                double remaining_qty = quantity; // 简化处理，实际需要从 response 解析
                int event_type = EVT_SUBMITTED; // 默认提交成功

                // 简易解析 exch_order_id
                const char* order_id_ptr = strstr(response.c_str(), "\"orderId\":");
                if (order_id_ptr) {
                    exch_order_id_str = std::to_string((long long)std::strtod(order_id_ptr + 11, nullptr));
                }

                // 简易解析 status
                const char* status_ptr = strstr(response.c_str(), "\"status\":\"");
                if (status_ptr) {
                    status_ptr += 9;
                    const char* status_end = strstr(status_ptr, "\"");
                    if (status_end) {
                        order_status_str = std::string(status_ptr, status_end - status_ptr);
                    }
                }

                // 简易解析 executedQty 和 avgPrice (如果已成交)
                if (order_status_str == "FILLED" || order_status_str == "PARTIALLY_FILLED") {
                    const char* executed_qty_ptr = strstr(response.c_str(), "\"executedQty\":\"");
                    if (executed_qty_ptr) {
                        fill_qty = std::strtod(executed_qty_ptr + 14, nullptr);
                    }
                    const char* avg_price_ptr = strstr(response.c_str(), "\"avgPrice\":\"");
                    if (avg_price_ptr) {
                        fill_px = std::strtod(avg_price_ptr + 12, nullptr);
                    }
                }

                if (order_status_str == "FILLED") event_type = EVT_FULL_FILL;
                else if (order_status_str == "PARTIALLY_FILLED") event_type = EVT_PARTIAL_FILL;
                else if (order_status_str == "CANCELED") event_type = EVT_CANCELED;
                else if (order_status_str == "REJECTED") event_type = EVT_REJECTED;
                // 否则默认为 EVT_SUBMITTED

                // 通知 Strategy 模块有订单事件发生
                Strategy::on_order_filled(client_order_id, exch_order_id_str.c_str(), fill_px, fill_qty, remaining_qty - fill_qty, event_type);
                std::cout << "[Executor] 下单成功响应。ClientOrderID: " << client_order_id << ", ExchOrderID: " << exch_order_id_str << ", Status: " << order_status_str << ", Resp: " << response << std::endl;

            } else {
                // 下单失败，通知 Strategy 模块订单被拒绝
                // 提取错误信息 (如果有)
                int error_code = -1; // 默认错误码
                std::string error_msg = "Order placement failed.";
                const char* msg_ptr = strstr(response.c_str(), "\"msg\":\"");
                if (msg_ptr) {
                    msg_ptr += 6;
                    const char* msg_end = strstr(msg_ptr, "\"");
                    if (msg_end) {
                        error_msg = std::string(msg_ptr, msg_end - msg_ptr);
                    }
                }
                const char* code_ptr = strstr(response.c_str(), "\"code\":");
                if (code_ptr) {
                    error_code = (int)std::strtod(code_ptr + 7, nullptr);
                }
                Strategy::on_order_filled(client_order_id, "", EVT_REJECTED, 0, 0, quantity, error_code, error_msg.c_str());
                std::cerr << "[Executor] 下单失败 (HTTP " << code << "): " << response << std::endl;
            }
        }).detach(); // 分离线程，让它独立运行
    }

    // --- 对外接口 ---

    void place_limit_order(const char* client_order_id, const char* symbol, const std::string& side, int order_type, int tif, double price, double quantity) {
        async_send_order_internal(client_order_id, symbol, side, order_type, tif, price, quantity);
    }

    void place_market_order(const char* client_order_id, const char* symbol, const std::string& side, double quantity) {
        async_send_order_internal(client_order_id, symbol, side, ORD_MARKET, TIF_GTC, 0.0, quantity);
    }

    void cancel_order(const char* client_order_id, const char* symbol) {
        std::thread([=]() {
            std::map<std::string, std::string> params;
            params["symbol"] = symbol;
            params["origClientOrderId"] = client_order_id;

            std::string response;
            long code = Network::perform_binance_request(
                Network::g_curl_handle,
                "DELETE",
                "/fapi/v1/order",
                params,
                response,
                g_api_key,
                g_api_secret,
                true
            );

            if (code == 200) {
                // 通知 Strategy 模块订单被取消
                Strategy::on_order_filled(client_order_id, "", EVT_CANCELED, 0, 0, 0, 0, "Order cancelled.");
                std::cout << "[Executor] 撤单成功! ClientOrderID: " << client_order_id << ", Resp: " << response << std::endl;
            } else {
                // 撤单失败，通知 Strategy 模块订单状态不变或拒绝
                int error_code = -1; 
                std::string error_msg = "Order cancellation failed.";
                const char* msg_ptr = strstr(response.c_str(), "\\"msg\\":\\"");
                if (msg_ptr) {
                    msg_ptr += 6;
                    const char* msg_end = strstr(msg_ptr, "\\"");
                    if (msg_end) {
                        error_msg = std::string(msg_ptr, msg_end - msg_ptr);
                    }
                }
                const char* code_ptr = strstr(response.c_str(), "\\"code\\":");
                if (code_ptr) {
                    error_code = (int)std::strtod(code_ptr + 7, nullptr);
                }
                Strategy::on_order_filled(client_order_id, "", EVT_REJECTED, 0, 0, 0, error_code, error_msg.c_str());
                std::cerr << "[Executor] 撤单失败 (HTTP " << code << "): " << response << std::endl;
            }
        }).detach();
    }

    void amend_order(const char* client_order_id, const char* symbol, double new_price, double new_quantity) {
        std::thread([=]() {
            std::map<std::string, std::string> params;
            params["symbol"] = symbol;
            params["origClientOrderId"] = client_order_id;
            params["side"] = "UNKNOWN"; // Binance API requires side, but amend usually doesn't change it
            params["quantity"] = std::to_string(new_quantity);
            params["price"] = std::to_string(new_price);

            std::string response;
            long code = Network::perform_binance_request(
                Network::g_curl_handle,
                "PUT", // 修改订单通常是 PUT 请求
                "/fapi/v1/order",
                params,
                response,
                g_api_key,
                g_api_secret,
                true
            );

            if (code == 200) {
                // 通知 Strategy 模块订单被修改
                // 这里可能需要更精细的事件，比如 EVT_AMENDED，但目前先用 EVT_SUBMITTED
                Strategy::on_order_filled(client_order_id, "", EVT_SUBMITTED, new_price, new_quantity, new_quantity, 0, "Order amended.");
                std::cout << "[Executor] 改单成功! ClientOrderID: " << client_order_id << ", New Price: " << new_price << ", New Qty: " << new_quantity << ", Resp: " << response << std::endl;
            } else {
                // 改单失败
                int error_code = -1; 
                std::string error_msg = "Order amendment failed.";
                const char* msg_ptr = strstr(response.c_str(), "\\"msg\\":\\"");
                if (msg_ptr) {
                    msg_ptr += 6;
                    const char* msg_end = strstr(msg_ptr, "\\"");
                    if (msg_end) {
                        error_msg = std::string(msg_ptr, msg_end - msg_ptr);
                    }
                }
                const char* code_ptr = strstr(response.c_str(), "\\"code\\":");
                if (code_ptr) {
                    error_code = (int)std::strtod(code_ptr + 7, nullptr);
                }
                Strategy::on_order_filled(client_order_id, "", EVT_REJECTED, 0, 0, 0, error_code, error_msg.c_str());
                std::cerr << "[Executor] 改单失败 (HTTP " << code << "): " << response << std::endl;
            }
        }).detach();
    }

    void cancel_all_orders(const std::string& symbol) {
        std::map<std::string, std::string> params;
        params["symbol"] = symbol;

        std::string response;
        long code = Network::perform_binance_request(
            Network::g_curl_handle,
            "DELETE",
            "/fapi/v1/allOpenOrders",
            params,
            response,
            g_api_key,
            g_api_secret,
            true
        );

        if (code == 200) {
            std::cout << "[Executor] 撤单成功 (Cancel All) for " << symbol << "." << std::endl;
        } else {
            std::cerr << "[Executor] 撤单失败 for " << symbol << ": " << response << std::endl;
        }
   111|    }
   112|
   113|    // 辅助函数：计算精度 (小数位数)
   114|    static int get_precision_from_step(const std::string& step_size_str) {
   115|        size_t dot_pos = step_size_str.find('.');
   116|        if (dot_pos == std::string::npos) {
   117|            return 0; // 没有小数点，精度为0
   118|        }
   119|        size_t first_nonzero = step_size_str.find_first_not_of('0', dot_pos + 1);
   120|        if (first_nonzero == std::string::npos) {
   121|            return 0; // 小数点后全是零
   122|        }
   123|        return first_nonzero - dot_pos;
   124|    }
   125|
   126|    void fetch_account_info(const std::string& symbol, GenericShmStruct* shm_bridge) {
   127|        if (!shm_bridge) {
   128|            std::cerr << "[Executor] Error: Shared memory bridge is null." << std::endl;
   129|            return;
   130|        }
   131|
   132|        std::string response_buffer;
   133|        std::map<std::string, std::string> params; // GET /api/v3/account 不需要额外的参数，但为了签名，可能需要 timestamp
   134|
   135|        long http_code = Network::perform_binance_request(
   136|            Network::g_curl_handle,
   137|            "GET",
   138|            "/fapi/v2/account",
   139|            params,
   140|            response_buffer,
   141|            g_api_key,
   142|            g_api_secret,
   143|            true // 需要签名
   144|        );
   145|
   146|        if (http_code == 200) {
   147|            // 解析 JSON 响应
   148|            const char* balance_cursor = response_buffer.c_str();
   149|            double usdt_free_balance = 0.0;
   150|            double bnb_free_balance = 0.0;
   151|            double btc_free_balance = 0.0;
   152|
   153|            // 查找 USDT
   154|            const char* usdt_asset_ptr = strstr(balance_cursor, "\"asset\":\"USDT\"");
   155|            if (usdt_asset_ptr) {
   156|                const char* free_balance_ptr = strstr(usdt_asset_ptr, "\"free\":\"");
   157|                if (free_balance_ptr) {
   158|                    usdt_free_balance = std::strtod(free_balance_ptr + 8, nullptr);
   159|                }
   160|            }
   161|            shm_bridge->account.usdt_balance.store(usdt_free_balance, std::memory_order_release);
   162|
   163|            // 查找 BNB
   164|            const char* bnb_asset_ptr = strstr(balance_cursor, "\"asset\":\"BNB\"");
   165|            if (bnb_asset_ptr) {
   166|                const char* free_balance_ptr = strstr(bnb_asset_ptr, "\"free\":\"");
   167|                if (free_balance_ptr) {
   168|                    bnb_free_balance = std::strtod(free_balance_ptr + 8, nullptr);
   169|                }
   170|            }
   171|            shm_bridge->account.bnb_balance.store(bnb_free_balance, std::memory_order_release);
   172|
   173|            // 查找 BTC
   174|            const char* btc_asset_ptr = strstr(balance_cursor, "\"asset\":\"BTC\"");
   175|            if (btc_asset_ptr) {
   176|                const char* free_balance_ptr = strstr(btc_asset_ptr, "\"free\":\"");
   177|                if (free_balance_ptr) {
   178|                    btc_free_balance = std::strtod(free_balance_ptr + 8, nullptr);
   179|                }
   180|            }
   181|            shm_bridge->account.btc_balance.store(btc_free_balance, std::memory_order_release);
   182|
   183|            // 查找合约持仓 (fapi/v2/account)
   184|            const char* pos_ptr = strstr(response_buffer.c_str(), ("\"symbol\":\"" + symbol + "\"").c_str());
   185|            if (pos_ptr) {
   186|                const char* amt_ptr = strstr(pos_ptr, ""positionAmt":"");
   187|                if (amt_ptr) {
   188|                    double pos_amt = std::strtod(amt_ptr + 15, nullptr);
   189|                    shm_bridge->account.position_amt.store(pos_amt, std::memory_order_release);
   190|                }
   191|                const char* entry_ptr = strstr(pos_ptr, ""entryPrice":"");
   192|                if (entry_ptr) {
   193|                    double entry_px = std::strtod(entry_ptr + 14, nullptr);
   194|                    shm_bridge->account.avg_price.store(entry_px, std::memory_order_release);
   195|                }
   196|            }
   197|
   198|
   199|            std::cout << "[Executor] 成功获取账户信息。USDT 可用余额: " << usdt_free_balance 
   200|                      << ", BNB 可用余额: " << bnb_free_balance 
   201|                      << ", BTC 可用余额: " << btc_free_balance 
   202|                      << ", 持仓量: " << shm_bridge->account.position_amt.load() << ", 均价: " << shm_bridge->account.avg_price.load() << std::endl;
   203|        } else {
   204|            std::cerr << "[Executor] 获取账户信息失败 (HTTP " << http_code << "): " << response_buffer << std::endl;
   205|        }
   206|    }
   207|
   208|    // 辅助函数：计算精度 (小数位数)
   209|    static int get_precision_from_step(const std::string& step_size_str) {
   210|        size_t dot_pos = step_size_str.find('.');
   211|        if (dot_pos == std::string::npos) {
   212|            return 0; // 没有小数点，精度为0
   213|        }
   214|        size_t first_nonzero = step_size_str.find_first_not_of('0', dot_pos + 1);
   215|        if (first_nonzero == std::string::npos) {
   216|            return 0; // 小数点后全是零
   217|        }
   218|        return first_nonzero - dot_pos;
   219|    }
   220|
   221|    void fetch_and_set_precision(const std::string& symbol, GenericShmStruct* shm_bridge) {
   222|        if (!shm_bridge) {
   223|            std::cerr << "[Executor] Error: Shared memory bridge is null." << std::endl;
   224|            return;
   225|        }
   226|
   227|        std::string response_buffer;
   228|        long http_code = Network::fetch_exchange_info(Network::g_curl_handle, symbol, response_buffer);
   229|
   230|        if (http_code == 200) {
   231|            printf("[DEBUG] fetch_and_set_precision - Full response_buffer: %s\n", response_buffer.c_str());
   232|
   233|            // 解析 JSON 响应
   234|            // 查找 "symbols" 数组
   235|            const char* symbols_start = strstr(response_buffer.c_str(), "\"symbols\":[");
   236|            printf("[DEBUG] symbols_start: %p\n", (void*)symbols_start);
   237|
   238|            if (symbols_start) {
   239|                const char* symbol_entry_start = strstr(symbols_start, ("\"symbol\":\"" + symbol + "\"").c_str());
   240|                printf("[DEBUG] symbol_entry_start: %p\n", (void*)symbol_entry_start);
   241|
   242|                if (symbol_entry_start) {
   243|                    // 找到对应的交易对
   244|                    const char* filters_start = strstr(symbol_entry_start, "\"filters\":[");
   245|                    printf("[DEBUG] filters_start: %p\n", (void*)filters_start);
   246|
   247|                    if (filters_start) {
   248|                        const char* filters_end = strstr(filters_start, "]}");
   249|                        printf("[DEBUG] filters_end: %p\n", (void*)filters_end);
   250|
   251|                        if (filters_end) {
   252|                            std::string filters_str(filters_start, filters_end - filters_start + 2);
   253|                            printf("[DEBUG] filters_str: %s\n", filters_str.c_str());
   254|
   255|                            // 查找 PRICE_FILTER
   256|                            const char* price_filter_start = strstr(filters_str.c_str(), "\"filterType\":\"PRICE_FILTER\"");
   257|                            printf("[DEBUG] price_filter_start: %p\n", (void*)price_filter_start);
   258|                            if (price_filter_start) {
   259|                                const char* tick_size_ptr = strstr(price_filter_start, "\"tickSize\":\"");
   260|                                printf("[DEBUG] tick_size_ptr: %p\n", (void*)tick_size_ptr);
   261|                                if (tick_size_ptr) {
   262|                                    tick_size_ptr += 12; // 跳过 "tickSize":"
   263|                                    const char* tick_size_end = strstr(tick_size_ptr, "\"");
   264|                                    printf("[DEBUG] tick_size_end: %p\n", (void*)tick_size_end);
   265|                                    if (tick_size_end) {
   266|                                        std::string tick_size_str(tick_size_ptr, tick_size_end - tick_size_ptr);
   267|                                        printf("[DEBUG] tick_size_str: %s\n", tick_size_str.c_str());
   268|                                        int price_prec = get_precision_from_step(tick_size_str);
   269|                                        shm_bridge->price_precision.store(price_prec, std::memory_order_release);
   270|                                        printf("[Executor] %s 价格精度: %d (tickSize: %s)\n", symbol.c_str(), price_prec, tick_size_str.c_str());
   271|                                    }
   272|                                }
   273|                            }
   274|
   275|                            // 查找 LOT_SIZE
   276|                            const char* lot_size_filter_start = strstr(filters_str.c_str(), "\"filterType\":\"LOT_SIZE\"");
   277|                            printf("[DEBUG] lot_size_filter_start: %p\n", (void*)lot_size_filter_start);
   278|                            if (lot_size_filter_start) {
   279|                                const char* step_size_ptr = strstr(lot_size_filter_start, "\"stepSize\":\"");
   280|                                printf("[DEBUG] step_size_ptr: %p\n", (void*)step_size_ptr);
   281|                                if (step_size_ptr) {
   282|                                    step_size_ptr += 12; // 跳过 "stepSize":"
   283|                                    const char* step_size_end = strstr(step_size_ptr, "\"");
   284|                                    printf("[DEBUG] step_size_end: %p\n", (void*)step_size_end);
   285|                                    if (step_size_end) {
   286|                                        std::string step_size_str(step_size_ptr, step_size_end - step_size_ptr);
   287|                                        printf("[DEBUG] step_size_str: %s\n", step_size_str.c_str());
   288|                                        int qty_prec = get_precision_from_step(step_size_str);
   289|                                        shm_bridge->quantity_precision.store(qty_prec, std::memory_order_release);
   290|                                        printf("[Executor] %s 数量精度: %d (stepSize: %s)\n", symbol.c_str(), qty_prec, step_size_str.c_str());
   291|                                    }
   292|                                }
   293|                            }
   294|                        } else {
   295|                            printf("[DEBUG] filters_end is nullptr. Search string: ]] \n");
   296|                        }
   297|                    } else {
   298|                        printf("[DEBUG] filters_start is nullptr. Search string: \"filters\":[\n");
   299|                    }
   300|                } else {
   301|                    printf("[DEBUG] symbol_entry_start is nullptr. Search string: \"symbol\":\"%s\"\n", symbol.c_str());
   302|                }
   303|            } else {
   304|                printf("[DEBUG] symbols_start is nullptr. Search string: \"symbols\":[\n");
   305|            }
   306|        } else {
   307|            std::cerr << "[Executor] 获取交易对 " << symbol << " 精度信息失败 (HTTP " << http_code << "): " << response_buffer << std::endl;
   308|        }
   309|    }
   310|
   311|} // namespace Executor
