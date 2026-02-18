#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <map>
#include <vector> // Add for std::vector
#include <cstddef> // For size_t
#include <pthread.h> // For pthread_setaffinity_np (Linux specific)

// 前置声明 OpenSSL 和 CURL 结构体
typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
#include <curl/curl.h> // Directly include curl/curl.h

namespace Network {

// 用于 Network 内部解析 WebSocket 原始数据后的临时结构体
enum class MessageType {
    UNKNOWN = 0,
    AGG_TRADE = 1,
    LIQUIDATION_ORDER = 2,
    BOOK_TICKER = 3
};

// 用于 Network 内部解析 WebSocket 原始数据后的临时结构体
struct ParsedMarketData {
    MessageType message_type;   // 消息类型
    double price;
    double quantity;
    double liquidation_volume; // 爆仓量
    double bid_price;          // 买一价
    double bid_quantity;       // 买一量
    double ask_price;          // 卖一价
    double ask_quantity;       // 卖一量
    uint64_t t_exch;           // 交易所时间戳
};

void init(); // 初始化网络模块，建立连接但不开始循环
void run_event_loop(); // 全能独狼的核心循环，包含所有业务逻辑

// 供内部使用的 WebSocket 客户端结构 (无需暴露类定义)
struct InternalWebSocketClient {
    SSL_CTX *ctx_ = nullptr;
    SSL *ssl_ = nullptr;
    int fd_ = -1;
    static constexpr size_t kBufferSize = 4 * 1024 * 1024; // 4MB Fixed Buffer
    alignas(64) char buffer_[kBufferSize]; // Fixed memory alignment
    bool is_public_stream = false; // 新增标志位
    size_t offset_ = 0;
};

// 全局的 WebSocket 客户端实例 (内部使用)
extern InternalWebSocketClient g_public_ws_client;
extern InternalWebSocketClient g_user_ws_client;

// 辅助函数，用于统一设置 g_running 为 false 并记录原因
void set_g_running_false(const char* reason);

// 获取交易对精度信息
long fetch_exchange_info(CURL *curl_handle, const std::string& symbol, std::string &response_buffer);

// 外部声明其他全局变量
extern std::string g_listen_key;

// 【恢复】声明全局的CURL句柄和perform_binance_request函数，供Executor使用
extern CURL* g_curl_handle;
extern long perform_binance_request(CURL* curl_handle, 
                                    const std::string& method, 
                                    const std::string& path, 
                                    const std::map<std::string, std::string>& params, 
                                    std::string& response_buffer, 
                                    const std::string& api_key, 
                                    const std::string& api_secret, 
                                    bool signed_request);

// 新增 ListenKey 续期函数
bool keep_alive_listen_key(CURL* curl_handle, const std::string& listen_key, 
                           const std::string& api_key, const std::string& api_secret); // 添加 api_key 和 api_secret 参数

// 内部辅助函数 (不需要暴露在 .h 中，但为了方便暂时放在这里)
static int connect_tcp(const std::string &host, const std::string &port);
static bool perform_ssl_handshake(InternalWebSocketClient& client, int fd);
static bool perform_ws_handshake(InternalWebSocketClient& client, const std::string &host, const std::string &path);
void send_frame(InternalWebSocketClient& client, const char* payload, size_t len, uint8_t opcode);

} // namespace Network
