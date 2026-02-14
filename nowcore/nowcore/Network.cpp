#include "Network.h" // 包含 Network 模块的声明
#include "Master_Logic_Bridge.h" // 引入 MasterBridge 共享内存，用于进程间通信，传递市场数据和账户信息
#include "Executor.h" // 包含 Executor::send_order_direct，用于直接发送交易指令
#include "Common.h"   // 包含 Common::get_now_ns 等通用函数，例如获取当前时间（纳秒）
#include "Strategy.h" // 引入 Strategy.h，包含策略相关的函数，例如检查交易触发条件和处理订单成交
#include <cstdlib> // 用于 strtod (字符串转 double) 和 strtoull (字符串转无符号长长整型)
#include <cstring> // 用于 strstr (查找子字符串)
#include <chrono> // 用于时间相关的操作，例如获取当前时间戳
#include <openssl/hmac.h> // 用于 HMAC-SHA256 签名算法，常用于加密认证
#include <openssl/sha.h> // 用于 SHA 散列算法
#include <curl/curl.h> // 用于 HTTP/HTTPS 请求，处理 REST API 通信

// 平台相关的网络头文件和库
#include <arpa/inet.h> // 用于 IP 地址转换
#include <netdb.h> // 用于主机名解析 (DNS)
#include <sys/socket.h> // 用于套接字编程
#include <unistd.h> // 用于 close 函数 (关闭文件描述符)
#include <sys/epoll.h> // Linux epoll，用于高效的 I/O 多路复用，监听多个套接字事件
#include <sched.h> // 用于 CPU affinity (Linux 特定)，绑定进程到特定 CPU 核心
#include <fcntl.h> // 用于文件控制，例如设置套接字为非阻塞模式
#include <netinet/tcp.h> // 用于 TCP_NODELAY 选项，禁用 Nagle 算法以减少延迟
#include <inttypes.h> // 用于 PRIu64 宏，方便打印 uint64_t 类型

// OpenSSL 相关头文件 (强制包含，无条件编译)
// 这些头文件提供了 SSL/TLS 加密通信所需的所有功能
#include <openssl/err.h> // OpenSSL 错误处理
#include <openssl/ssl.h> // SSL/TLS 核心功能
#include <openssl/rand.h> // 随机数生成，用于 WebSocket 握手密钥
#include <openssl/buffer.h> // BIO 缓冲区管理
#include <openssl/hmac.h> // 确保包含，再次强调 HMAC 功能
#include <openssl/evp.h>  // 确保包含，通用加密功能
#include <openssl/provider.h> // OpenSSL 3.0+ 的 provider 管理

// CURL 相关头文件 (强制包含，无条件编译)
// libcurl 已经包含在 <curl/curl.h> 中，这里不需要额外的特定头文件

#include <iostream> // 标准输入输出流
#include <string> // 字符串操作
#include <thread> // 用于 std::this_thread::sleep_for，在需要时用于重试逻辑（但目标是事件驱动）
#include <map> // 键值对容器，用于 HTTP 请求参数
#include <sstream> // 字符串流，用于构建字符串
#include <iomanip> // 格式化输出，例如设置浮点数精度
#include <algorithm> // for std::min

// 引入 nlohmann/json 命名空间 (如果需要 JSON 解析库)
// 这里注释掉，表示可能不需要完整的 JSON 库，而是手动解析关键字段

namespace Network { // 定义 Network 命名空间，将所有网络相关功能封装在此

// CURL Debug Callback 实现

static int debug_callback(CURL *handle, curl_infotype type, char *data, size_t size, void *userptr) {
    (void)handle;
    (void)userptr;

    // 获取当前微秒级时间戳
    auto now = std::chrono::system_clock::now();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    double ts = micros / 1000000.0; // 格式化为：秒.微秒

    switch (type) {
        case CURLINFO_HEADER_OUT:
            // 记录“发单”瞬间
            fprintf(stderr, "\n[TX_HEADER @ %.6f]\n%.*s", ts, (int)size, data);
            break;
        case CURLINFO_HEADER_IN:
            // 记录“回执”瞬间（Header 包含状态码和权重）
            fprintf(stderr, "\n[RX_HEADER @ %.6f]\n%.*s", ts, (int)size, data);
            break;
        case CURLINFO_DATA_IN:
            // 记录“数据负载”到达瞬间（JSON 结果）
            fprintf(stderr, "\n[RX_DATA @ %.6f]\n%.*s\n", ts, (int)size, data);
            break;
        
        // 以下是你想保留的其他信息，但我做了静默处理，或者你可以根据需要开启
        case CURLINFO_TEXT:
        case CURLINFO_DATA_OUT:
        case CURLINFO_SSL_DATA_OUT:
        case CURLINFO_SSL_DATA_IN:
            // 如果你平时不想看这些噪音，就保持 return 0；
            // 如果你想看，就取消下面这行的注释：
            // fprintf(stderr, "CURL_DEBUG (%d): %.*s", type, (int)size, data);
            return 0;

        default:
            return 0;
    }
    return 0;
}
// 全局的 WebSocket 客户端实例
// 全局的 WebSocket 客户端实例 (内部使用)
// 这里定义了公共流（市场行情）和私有流（用户订单、账户信息）的实例
// 它们是 InternalWebSocketClient 类型的全局对象，在程序启动时会进行初始化和连接
InternalWebSocketClient g_public_ws_client; // 公共 WebSocket 客户端，用于接收市场行情数据
InternalWebSocketClient g_user_ws_client;   // 用户 WebSocket 客户端，用于接收用户数据（如订单更新）

// 辅助函数，用于统一设置 g_running 为 false 并记录原因
// 任何地方调用它，g_running 变为 false，整个程序将安全停车，通常用于处理致命错误或程序退出
void set_g_running_false(const char* reason) {
    printf("[INFO] Network::set_g_running_false - Exiting due to: %s\n", reason);
    g_running = false;
}

// 外部声明其他全局变量的定义
// 这些全局变量可能在 Common.cpp 中定义，在这里仅进行声明或不重复声明，以避免重复定义错误
// std::atomic<bool> g_running{true}; // 定义已移至 Common.cpp，表示程序是否正在运行的原子布尔变量
std::string g_listen_key; // 存储币安用户数据流的 listenKey，用于连接用户私有 WebSocket 流
// std::atomic<uint64_t> last_network_heartbeat_ns{0}; // 定义已移至 Common.cpp，记录上次网络心跳时间（纳秒）
// std::atomic<bool> g_low_power_mode{false}; // 定义已移至 Common.cpp，表示是否处于低功耗模式的原子布尔变量


// CURL 句柄 (如果启用 CURL)
CURL *g_curl_handle = nullptr;

// 写入回调函数 (libcurl 使用)
// 当 libcurl 接收到 HTTP 响应数据时，会调用此函数将数据写入指定的缓冲区
size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string *)userp)->append((char *)contents, size * nmemb);
    return size * nmemb;
}

// URL 编码辅助函数：确保特殊字符被正确转义 (例如空格转 %20)
// 在构建 URL 参数时，需要对参数值进行 URL 编码，以防止特殊字符破坏 URL 结构
std::string url_encode(CURL *curl_handle, const std::string &value) {
    if (!curl_handle) return value;
    char *output = curl_easy_escape(curl_handle, value.c_str(), value.length());
    if (output) {
        std::string result = output;
        curl_free(output);
        return result;
    }
    return value;
}

// HMAC SHA256 实现
// 用于生成请求签名，确保 API 请求的安全性
std::string hmac_sha256(const std::string &key, const std::string &data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 32;

    // 调用 OpenSSL 的 HMAC 函数计算 HMAC-SHA256
    HMAC(EVP_sha256(),
         key.c_str(), key.length(),
         (unsigned char *)data.c_str(), data.length(),
         digest, &len);

    char hex_str[65];
    for (int i = 0; i < 32; i++) {
        sprintf(hex_str + i * 2, "%02x", digest[i]);
    }
    return std::string(hex_str);
}

// 执行币安 API 请求的通用函数
// 处理 GET, POST, DELETE 等 HTTP 方法，以及签名和参数构建
long perform_binance_request(CURL *curl_handle,
                             const std::string &method,
                             const std::string &path,
                             const std::map<std::string, std::string> &params,
                             std::string &response_buffer,
                             const std::string &api_key,
                             const std::string &api_secret,
                             bool signed_request) {

    if (!curl_handle) return 0;

    // 1. 准备参数副本，防止修改原始参数
    std::map<std::string, std::string> final_params = params;

    // 2. 注入 timestamp (这是签名必须的) 和 recvWindow
    // 对于需要签名的请求，必须包含时间戳和接收窗口
    if (signed_request) {
        // 获取当前时间戳（毫秒）
        long long timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        final_params["timestamp"] = std::to_string(timestamp);
        final_params["recvWindow"] = "10000";
    }

    // 3. 构建未编码的 Query String 用于签名
    // 签名是基于原始的、未 URL 编码的参数字符串
    std::string raw_query_string = "";
    for (auto it = final_params.begin(); it != final_params.end(); ++it) {
        if (!raw_query_string.empty()) {
            raw_query_string += "&";
        }
        // 不对 key 和 value 进行 URL Encoding，直接拼接
        raw_query_string += it->first + "=" + it->second;
    }

    // 4. 计算签名
    std::string signature_str = "";
    if (signed_request && !api_secret.empty()) {
        signature_str = hmac_sha256(api_secret, raw_query_string);
    }

    // 5. 构建最终的 URL 编码的 Query String (包含签名)
    // 实际发送的请求参数需要进行 URL 编码
    std::string query_string = "";
    for (auto it = final_params.begin(); it != final_params.end(); ++it) {
        if (!query_string.empty()) {
            query_string += "&";
        }
        // 对 key 和 value 进行 URL Encoding
        query_string += url_encode(curl_handle, it->first) + "=" + url_encode(curl_handle, it->second);
    }
   
    // 追加签名到编码后的 query_string
    if (!signature_str.empty()) {
        query_string += "&signature=" + signature_str;
    }

    // 构建完整的请求 URL
    std::string full_url = "https://fapi.binance.com" + path;

    // 5. 设置 HTTP 头
    struct curl_slist *headers = NULL;
    // 添加 API Key 头
    headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + api_key).c_str());
    // 明确指定 Content-Type 为 URL 编码表单
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

    // 打印调试信息
    printf("[DEBUG] perform_binance_request - Method: %s\n", method.c_str());
    printf("[DEBUG] perform_binance_request - Path: %s\n", path.c_str());
    printf("[DEBUG] perform_binance_request - Query String (or Body for POST): %s\n", query_string.c_str());
    printf("[DEBUG] perform_binance_request - Query String Length: %zu\n", query_string.length());
    printf("[DEBUG] perform_binance_request - X-MBX-APIKEY: %s\n", api_key.c_str());


    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);

    // 6. 根据 Method 配置 CURL
    if (method == "POST") {
        curl_easy_setopt(curl_handle, CURLOPT_POST, 1L);
        curl_easy_setopt(curl_handle, CURLOPT_URL, full_url.c_str());
        
        // POST 数据放入 Body
        curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, query_string.c_str());
        // 显式告知 Body 大小，防止截断
        curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, (long)query_string.length());
        printf("[DEBUG] perform_binance_request - Full URL (POST): %s\n", full_url.c_str());


    } else if (method == "DELETE") {
        curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, "DELETE");
        if (!query_string.empty()) {
            full_url += "?" + query_string;
        }
        curl_easy_setopt(curl_handle, CURLOPT_URL, full_url.c_str());
        printf("[DEBUG] perform_binance_request - Full URL (DELETE): %s\n", full_url.c_str());
        
    } else if (method == "PUT") { // 新增 PUT 方法处理
        curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, "PUT");
        if (!query_string.empty()) {
            full_url += "?" + query_string;
        }
        curl_easy_setopt(curl_handle, CURLOPT_URL, full_url.c_str());
        printf("[DEBUG] perform_binance_request - Full URL (PUT): %s\n", full_url.c_str());

    } else { // 默认为 GET
        curl_easy_setopt(curl_handle, CURLOPT_HTTPGET, 1L);
        if (!query_string.empty()) {
            full_url += "?" + query_string;
        }
        curl_easy_setopt(curl_handle, CURLOPT_URL, full_url.c_str());
        printf("[DEBUG] perform_binance_request - Full URL (GET): %s\n", full_url.c_str());
    }

    // 设置接收数据的回调函数和缓冲区
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &response_buffer);

    CURLcode res = curl_easy_perform(curl_handle);
    long http_code = 0;
    curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        response_buffer = "CURL Error: " + std::string(curl_easy_strerror(res));
        return -1;
    }

    return http_code;
}

// ListenKey 续期函数实现
void keep_alive_listen_key(CURL* curl_handle, const std::string& listen_key, 
                           const std::string& api_key, const std::string& api_secret) {
    if (listen_key.empty()) {
        printf("[WARNING] keep_alive_listen_key called with empty listenKey. Skipping refresh.\n");
        return;
    }
    
    std::map<std::string, std::string> params;
    params["listenKey"] = listen_key; // 续期时需要带上 listenKey
    std::string response_buffer;
    
    printf("[DEBUG] Attempting to refresh listenKey: %s\n", listen_key.c_str());
    long http_code = perform_binance_request(curl_handle, "PUT", "/fapi/v1/listenKey", 
                                             params, response_buffer, api_key, api_secret, false);

    if (http_code == 200) {
        printf("[INFO] ListenKey refreshed successfully. ListenKey: %s, Response: %s\n", listen_key.c_str(), response_buffer.c_str());
    } else {
        std::cerr << "[ERROR] Failed to refresh listenKey: HTTP " << http_code 
                  << ", ListenKey: " << listen_key << ", Response: " << response_buffer << std::endl;
    }
}


// --- 辅助函数实现 (InternalWebSocketClient methods adapted) ---
// 内部 WebSocket 客户端相关辅助函数，主要用于建立和维护 WebSocket 连接

// 初始化 SSL 上下文 (通用)
// 配置 OpenSSL 环境，用于 SSL/TLS 加密通信
void init_ssl_ctx(SSL_CTX*& ctx) {
    OSSL_PROVIDER_load(NULL, "legacy");
    OSSL_PROVIDER_load(NULL, "default");
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        std::cerr << "[Network] SSL_CTX_new failed" << std::endl;
        ERR_print_errors_fp(stderr);
        fflush(stderr);
        exit(1);
    }
}

// 连接 TCP 服务器
// 建立底层的 TCP 连接，为后续的 SSL/TLS 和 WebSocket 握手做准备
int connect_tcp(const std::string &host, const std::string &port) {
    struct addrinfo hints = {}, *res;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    // 解析主机名和端口号，获取可用的地址信息
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) {
        return -1;
    }

    // 创建套接字
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return -1;
    }

    // 设置 TCP_NODELAY 选项
    // 禁用 Nagle 算法，即使数据包很小也会立即发送，减少延迟
    int enable = 1;
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (void*)&enable, sizeof(enable)) < 0) {
        printf("[LOG] TCP_NODELAY set failed. errno: %d, msg: %s\n", errno, strerror(errno));
    }

    // 设置 SO_KEEPALIVE 选项
    // 启用 TCP Keep-Alive 机制，定期发送探测包以检测连接是否存活
    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void*)&enable, sizeof(enable)) < 0) {
        printf("[LOG] SO_KEEPALIVE set failed. errno: %d, msg: %s\n", errno, strerror(errno));
    }

    // 连接到服务器
    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        close(sock);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    return sock;
}

// 执行 SSL/TLS 握手
// 在 TCP 连接建立后，进行 SSL/TLS 握手以建立加密通道
bool perform_ssl_handshake(InternalWebSocketClient& client, int fd) {
    client.ssl_ = SSL_new(client.ctx_);
    SSL_set_fd(client.ssl_, fd);
    if (SSL_connect(client.ssl_) != 1) {
        ERR_print_errors_fp(stderr);
        fflush(stderr);
        return false;
    }
    return true;
}

// 执行 WebSocket 握手
// 在 SSL/TLS 加密通道建立后，进行 WebSocket 协议握手
bool perform_ws_handshake(InternalWebSocketClient& client, const std::string &host, const std::string &path) {
    char request[1024];
    unsigned char key_bytes[16];
    if (RAND_bytes(key_bytes, sizeof(key_bytes)) != 1) {
        return false;
    }

    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *bio = BIO_new(BIO_s_mem());
    bio = BIO_push(b64, bio);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, key_bytes, sizeof(key_bytes));
    BIO_flush(bio);

    BUF_MEM *bufferPtr;
    BIO_get_mem_ptr(bio, &bufferPtr);
    std::string sec_websocket_key(bufferPtr->data, bufferPtr->length);
    BIO_free_all(bio);

    // 构建 WebSocket 握手请求的 HTTP 头
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\n"
             "Host: %s\n"
             "Upgrade: websocket\n"
             "Connection: Upgrade\n"
             "Sec-WebSocket-Key: %s\n"
             "Sec-WebSocket-Version: 13\n"
             "\n",
             path.c_str(), host.c_str(), sec_websocket_key.c_str());

    // 发送 WebSocket 握手请求
    int write_len = SSL_write(client.ssl_, request, strlen(request));
    if (write_len <= 0) {
        return false;
    }

    char response[1024];
    int len = SSL_read(client.ssl_, response, sizeof(response) - 1);
    if (len <= 0) {
        return false;
    }
    response[len] = 0; // 确保响应字符串以 null 结尾

    // 检查服务器响应是否包含 "101 Switching Protocols"，这是 WebSocket 握手成功的标志
    if (strstr(response, "101 Switching Protocols") == nullptr) {
        std::cerr << "[ERROR] WebSocket handshake failed. Server response: " << response << std::endl;
        fflush(stderr);
        return false;
    }
    return true;
}

// 发送 WebSocket 帧
// 将数据封装成 WebSocket 帧并发送
void send_frame(InternalWebSocketClient& client, const char* payload, size_t len, uint8_t opcode) {
    unsigned char header[14];
    size_t header_len = 0;
    header[0] = 0x80 | opcode;
                               // Opcode 0x1 表示文本帧，0x2 表示二进制帧，0x9 表示 Ping 帧，0xA 表示 Pong 帧

    // 根据 payload 长度设置 WebSocket 帧的长度字段
    if (len <= 125) {
        header[1] = 0x80 | (uint8_t)len;
        header_len = 2;
    }
    else if (len <= 0xFFFF) {
        header[1] = 0x80 | 126;
        header[2] = (len >> 8) & 0xFF;
        header[3] = len & 0xFF;
        header_len = 4;
    }
    else {
        header[1] = 0x80 | 127;
        // 写入 8 字节的 payload 长度
        header[2] = (len >> 56) & 0xFF; 
        header[3] = (len >> 48) & 0xFF;
        header[4] = (len >> 40) & 0xFF; 
        header[5] = (len >> 32) & 0xFF; 
        header[6] = (len >> 24) & 0xFF; 
        header[7] = (len >> 16) & 0xFF; 
        header[8] = (len >> 8) & 0xFF;  
        header[9] = len & 0xFF;        
        header_len = 10;
    }

    uint8_t mask[4];
    if (RAND_bytes(mask, sizeof(mask)) != 1) {
        fprintf(stderr, "[ERROR] Failed to generate random WebSocket mask key.\n");
    }    memcpy(header + header_len, mask, 4);
    header_len += 4;

    SSL_write(client.ssl_, header, header_len);

    std::vector<char> masked(len);
    for (size_t i = 0; i < len; ++i) {
        masked[i] = payload[i] ^ mask[i % 4];
    }

    SSL_write(client.ssl_, masked.data(), len);

}

// JSON 值提取辅助函数
// --------------------------------------------------------------------------------
// 查找 JSON 字符串字段的值。例如，对于 "key":"value"，返回指向 value 的指针和长度
const char* find_json_string_value(const char* json, const char* key, size_t* out_len) {
    std::string search_key = std::string("\"") + key + std::string("\":\"");
    const char* key_start = strstr(json, search_key.c_str());
    if (!key_start) return nullptr;

    const char* value_start = key_start + search_key.length();
    const char* value_end = value_start;
    while (*value_end && *value_end != '"' && *value_end != ',' && *value_end != '}' && *value_end != ']') {
        value_end++;
    }
    if (*value_end != '"') { // If the value is not enclosed in quotes, it might be a number or boolean, not a string we are looking for.
        return nullptr; // This handles cases like "key":true, "key":123 where we expect a string.
    }
    if (!value_end) return nullptr;

    *out_len = value_end - value_start;
    return value_start;
}

// 查找 JSON 数字字段的值。例如，对于 "key":123，返回指向 123 的指针和长度
const char* find_json_numeric_value(const char* json, const char* key, size_t* out_len) {
    std::string search_key = std::string("\"") + key + std::string("\":");
    const char* key_start = strstr(json, search_key.c_str());
    if (!key_start) return nullptr;

    const char* value_start = key_start + search_key.length();
    // 数字的结束可以是逗号, 括号, 或者空格等
    const char* value_end = value_start;
    // 确保 value_end 不会越界，并且是有效的数字字符
    while (*value_end && ((*value_end >= '0' && *value_end <= '9') || *value_end == '.' || *value_end == '-')) {
        value_end++;
    }
   
    if (value_end == value_start) return nullptr; // 没有找到数字

    *out_len = value_end - value_start;
    return value_start;
}



// --- 核心 Network 模块函数实现 ---
// Network 模块的核心功能，包括初始化、事件循环和数据解析

void init() {
    curl_global_init(CURL_GLOBAL_ALL);
    printf("[DEBUG] Network::init() - Starting initialization.\n");
    init_ssl_ctx(g_public_ws_client.ctx_);
    init_ssl_ctx(g_user_ws_client.ctx_);
    printf("[DEBUG] Network::init() - SSL contexts initialized.\n");

    // 设置 is_public_stream 标志位，用于区分公共流和私有流
    g_public_ws_client.is_public_stream = true;
    g_user_ws_client.is_public_stream = false;

    g_curl_handle = curl_easy_init();
    if (!g_curl_handle) {
        std::cerr << "[FATAL] Could not initialize CURL" << std::endl;
        exit(1);
    }
    curl_easy_setopt(g_curl_handle, CURLOPT_TCP_NODELAY, 1L);
    //【在这里加入这 3 行保命代码！】
    // ==========================================
    
    // 1. 开启 TCP Keep-Alive (心脏起搏器)
    // 告诉操作系统：这条 HTTP 连接，没人说话时也要帮我维护着，别断了。
    curl_easy_setopt(g_curl_handle, CURLOPT_TCP_KEEPALIVE, 1L);

    // 2. 空闲多久开始探测 (60秒)
    // 如果 60 秒都没发单，就开始发探测包。
    curl_easy_setopt(g_curl_handle, CURLOPT_TCP_KEEPIDLE, 60L);

    // 3. 探测频率 (30秒)
    // 每隔 30 秒发一次，确保存活。
    curl_easy_setopt(g_curl_handle, CURLOPT_TCP_KEEPINTVL, 30L);

    // ==========================================
    curl_easy_setopt(g_curl_handle, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(g_curl_handle, CURLOPT_TIMEOUT_MS, 10000L);
    
    // 启用 CURL 详细调试信息
    curl_easy_setopt(g_curl_handle, CURLOPT_VERBOSE, 0L);
    curl_easy_setopt(g_curl_handle, CURLOPT_DEBUGFUNCTION, debug_callback);
    // curl_easy_setopt(g_curl_handle, CURLOPT_DEBUGDATA, nullptr);
    
    // 显式设置 SSL 验证
    curl_easy_setopt(g_curl_handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(g_curl_handle, CURLOPT_SSL_VERIFYHOST, 2L);
    
    printf("[DEBUG] Network::init() - CURL initialized.\n");

    // 获取 listenKey for user data stream
    // listenKey 是币安用于用户数据流的唯一标识，需要通过 HTTP 请求获取
    std::map<std::string, std::string> params;
    std::string response;
    const char *env_key = std::getenv("BINANCE_API_KEY");
    const char *env_secret = std::getenv("BINANCE_API_SECRET"); // 获取 API Secret
    std::string api_key;
    std::string api_secret; // 定义 api_secret

    if (env_key) {
        api_key = env_key;
    }
    else {
        std::cerr << "[FATAL] BINANCE_API_KEY environment variable not set. Exiting." << std::endl;
        exit(1);
    }
    if (env_secret) { // 检查 API Secret
        api_secret = env_secret;
    } else {
        std::cerr << "[FATAL] BINANCE_API_SECRET environment variable not set. Exiting." << std::endl;
        exit(1);
    }

    // 强制设置为单向持仓模式 (One-Way)
    printf("[DEBUG] Network::init() - Setting One-Way Position Mode.\n");
    long mode_code = perform_binance_request(g_curl_handle, "POST", "/fapi/v1/positionSide/dual", 
                                             {{"dualSidePosition", "false"}}, response, api_key, api_secret, true);
    if (mode_code != 200) {
        std::cerr << "[WARNING] Failed to set One-Way Position Mode: HTTP " << mode_code << ", Response: " << response << std::endl;
    }
    else {
        printf("[DEBUG] Network::init() - One-Way Position Mode set successfully.\n");
    }

    // 强制设置 20倍 杠杆 (避免默认 1倍 或 125倍 的意外)
    printf("[DEBUG] Network::init() - Setting 20x leverage for BNBUSDT.\n");
    long leverage_code = perform_binance_request(g_curl_handle, "POST", "/fapi/v1/leverage", 
                                                 {{"symbol", "BNBUSDT"}, {"leverage", "20"}}, response, api_key, api_secret, true);
    if (leverage_code != 200) {
        std::cerr << "[WARNING] Failed to set 20x leverage: HTTP " << leverage_code << ", Response: " << response << std::endl;
    }
    else {
        printf("[DEBUG] Network::init() - 20x leverage set successfully for BNBUSDT.\n");
    }
   
    printf("[DEBUG] Network::init() - Attempting to get ListenKey.\n");
    // 发送 POST 请求获取 listenKey
    http_code = perform_binance_request(g_curl_handle, "POST", "/fapi/v1/listenKey",
                                            params, response, api_key, api_secret, false); // listenKey 接口不需要签名
    if (http_code == 200) {
        // 从响应中解析 listenKey
        const char *key_ptr = strstr(response.c_str(), "\"listenKey\":\"");
        if (key_ptr) {
            const char *end_ptr = strstr(key_ptr + 13, "\"");
            if (end_ptr) {
                g_listen_key = std::string(key_ptr + 13, end_ptr - (key_ptr + 13));
                printf("[DEBUG] Network::init() - ListenKey obtained: %s\n", g_listen_key.c_str());
            }
            else {
                std::cerr << "[ERROR] end_ptr is NULL! Failed to find the closing quote for listenKey." << std::endl;
                printf("[LOG] Event Type: 3, Error: end_ptr is NULL!\n");
            }
        }
        else {
            std::cerr << "[WARNING] ListenKey not found in response during init: " << response << std::endl;
            printf("[LOG] Event Type: 3, Error: ListenKey not found!\n");
        }
    }
    else {
            std::cerr << "[WARNING] Failed to get ListenKey during init: HTTP " << http_code << ", Response: " << response << std::endl;
            printf("[LOG] Event Type: 3, Error: Failed to get ListenKey! HTTP: %ld\n", http_code);
    }

    // 连接公共行情 WebSocket
    // 用于获取市场行情数据，例如聚合交易、盘口数据和强平订单
    std::string public_host = "fstream.binance.com", public_path = "/ws/bnbusdt@aggTrade?timeUnit=MICROSECOND", public_port = "443";
    printf("[DEBUG] Network::init() - Connecting public WebSocket to %s:%s%s\n", public_host.c_str(), public_port.c_str(), public_path.c_str());
    g_public_ws_client.fd_ = connect_tcp(public_host, public_port);
    if (g_public_ws_client.fd_ < 0) {
        std::cerr << "[FATAL] Public WebSocket TCP connection failed." << std::endl;
        printf("[LOG] Event Type: 3, Error: Public WebSocket TCP connection failed!\n");
        exit(1);
    }
    printf("[DEBUG] Network::init() - Public WebSocket TCP connected. FD: %d\n", g_public_ws_client.fd_);

    if (!perform_ssl_handshake(g_public_ws_client, g_public_ws_client.fd_)) {
        std::cerr << "[FATAL] Public WebSocket SSL handshake failed." << std::endl;
        ERR_print_errors_fp(stderr);
        printf("[LOG] Event Type: 3, Error: Public WebSocket SSL handshake failed!\n");
        exit(1);
    }
    printf("[DEBUG] Network::init() - Public WebSocket SSL handshake successful.\n");

    if (!perform_ws_handshake(g_public_ws_client, public_host, public_path)) {
        std::cerr << "[FATAL] Public WebSocket handshake failed." << std::endl;
        printf("[LOG] Event Type: 3, Error: Public WebSocket handshake failed!\n");
        exit(1);
    }
    printf("[DEBUG] Network::init() - Public WebSocket handshake successful.\n");

    // 订阅 U本位永续合约 (bnbusdt) 的三个核心流
    // 包括 aggTrade (聚合交易), bookTicker (最优买卖盘), forceOrder (强平订单)
    const char* subscribe_aggtrade_payload = "{\"method\":\"SUBSCRIBE\",\"params\":[\"bnbusdt@aggTrade\",\"bnbusdt@bookTicker\",\"bnbusdt@forceOrder\"],\"id\":1}";
    send_frame(g_public_ws_client, subscribe_aggtrade_payload, strlen(subscribe_aggtrade_payload), 0x1);
    printf("[DEBUG] Network::init() - Public WebSocket aggTrade subscribe frame sent.\n");

    // 连接用户数据流 WebSocket (如果 listenKey 成功获取)
    // 用于获取用户相关的订单、账户更新等信息
    if (!g_listen_key.empty()) {
        std::string user_host = "fstream.binance.com", user_path = "/ws/" + g_listen_key, user_port = "443";
        printf("[DEBUG] Network::init() - Connecting user WebSocket to %s:%s%s\n", user_host.c_str(), user_port.c_str(), user_path.c_str());
        g_user_ws_client.fd_ = connect_tcp(user_host, user_port);
        if (g_user_ws_client.fd_ < 0) {
            std::cerr << "[WARNING] User WebSocket TCP connection failed, continuing without it." << std::endl;
            printf("[LOG] Event Type: 3, Error: User WebSocket TCP connection failed!\n");
            // 不退出，但用户数据流不可用，程序会继续运行
        }
        else {
            printf("[DEBUG] Network::init() - User WebSocket TCP connected. FD: %d\n", g_user_ws_client.fd_);
            if (!perform_ssl_handshake(g_user_ws_client, g_user_ws_client.fd_)) {
                std::cerr << "[WARNING] User WebSocket SSL handshake failed, continuing without it." << std::endl;
                ERR_print_errors_fp(stderr);
                printf("[LOG] Event Type: 3, Error: User WebSocket SSL handshake failed!\n");
            }
            else {
                printf("[DEBUG] Network::init() - User WebSocket SSL handshake successful.\n");
                if (!perform_ws_handshake(g_user_ws_client, user_host, user_path)) {
                    std::cerr << "[WARNING] User WebSocket handshake failed, continuing without it." << std::endl;
                    printf("[LOG] Event Type: 3, Error: User WebSocket handshake failed!\n");
                }
                else {
                    printf("[DEBUG] Network::init() - User WebSocket handshake successful.\n");
                }
            }
        }
    }
    
    // 设置非阻塞模式
    // 将套接字设置为非阻塞模式，以便在没有数据可读时不会阻塞程序
    fcntl(g_public_ws_client.fd_, F_SETFL, fcntl(g_public_ws_client.fd_, F_GETFL, 0) | O_NONBLOCK);
    if (g_user_ws_client.fd_ != -1) fcntl(g_user_ws_client.fd_, F_SETFL, fcntl(g_user_ws_client.fd_, F_GETFL, 0) | O_NONBLOCK);
    printf("[DEBUG] Network::init() - Sockets set to non-blocking mode.\n");

    printf("[DEBUG] Network::init() - Initialization complete.\n");
}

/// -----------------------------------------------------------
// 终极解析器：同时处理 成交(aggTrade)、盘口(bookTicker)、爆仓(forceOrder)
// -----------------------------------------------------------
// 终极解析器：修复了“方向丢失”的 Bug 和潜在的内存安全问题
static ParsedMarketData parse_market_data_json_no_alloc(const char* msg, size_t len) {
    MarketFrame frame = {}; // 初始化一个 MarketFrame 结构体，用于存储解析后的市场数据
    frame.timestamp = Common::get_now_ns(); // 1. 保底时间：先记录当前系统时间，作为数据的基准时间
    frame.local_timestamp = Common::get_now_ns(); // 记录本地接收时间
    bool valid_data = false; // 标记是否成功解析到有效数据
    size_t val_len = 0;

    // 优先尝试识别 bookTicker (不含 "e" 字段)
    const char* u_start = find_json_numeric_value(msg, "u", &val_len); // 查找 "u": 字段，bookTicker 消息的特征
    if (u_start) {
        frame.type = 2; // 设置帧类型为 2 (代表 bookTicker)
        // 解析 b/B (买一价/买一量), a/A (卖一价/卖一量)
        const char* b_ptr = find_json_string_value(msg, "b", &val_len); // 查找买一价
        if (b_ptr) frame.bid_p = strtod(std::string(b_ptr, val_len).c_str(), nullptr); else { /* 错误处理或日志 */ }
        const char* B_ptr = find_json_string_value(msg, "B", &val_len); // 查找买一量
        if (B_ptr) frame.bid_q = strtod(std::string(B_ptr, val_len).c_str(), nullptr); else { /* 错误处理或日志 */ }

        const char* a_ptr = find_json_string_value(msg, "a", &val_len); // 查找卖一价
        if (a_ptr) frame.ask_p = strtod(std::string(a_ptr, val_len).c_str(), nullptr); else { /* 错误处理或日志 */ }
        const char* A_ptr_actual = find_json_string_value(msg, "A", &val_len); // 查找卖一量
        if (A_ptr_actual) frame.ask_q = strtod(std::string(A_ptr_actual, val_len).c_str(), nullptr); else { /* 错误处理或日志 */ }

        valid_data = true; // 标记为有效数据
    }
    else {
        // 如果没有 "u" 字段，则继续查找事件类型 'e' 字段
        const char* e_start_key = strstr(msg, "\"e\":\""); // 查找 "e":" 字符串
        if (e_start_key) { // 如果找到了 'e' 字段
            const char* e_start = e_start_key + 5; // 跳过 "e":"，指向实际的事件类型字符串开始位置
            
            // === 1. aggTrade (成交) ===
            // 处理聚合交易消息，记录价格、数量和方向
            if (strncmp(e_start, "aggTrade", 8) == 0) { // 如果事件类型是 "aggTrade"
                frame.type = 1; // 设置帧类型为 1 (代表 aggTrade)
                
                // 解析 P (价) 和 Q (量)
                const char* p_raw = find_json_string_value(msg, "p", &val_len); // 查找 "p":" 字符串，获取价格
                if (p_raw) frame.price = strtod(std::string(p_raw, val_len).c_str(), nullptr); else { /* 错误处理或日志 */ }
                const char* q_raw = find_json_string_value(msg, "q", &val_len); // 查找 "q":" 字符串，获取数量
                if (q_raw) frame.quantity = strtod(std::string(q_raw, val_len).c_str(), nullptr); else { /* 错误处理或日志 */ }

                // 【关键修复】解析方向 ("m": true/false)
                // 币安逻辑：m=true 代表 Maker 是买方 -> 意味着是主动卖出 -> Side = -1
                // m=false 代表 Maker 是卖方 -> 意味着是主动买入 -> Side = 1
                std::string m_search_key = "\"m\":";
                const char* m_ptr = strstr(msg, m_search_key.c_str()); // 查找 "m": 字段，表示是否为卖方撮合 (maker is buyer)
                if (m_ptr) {
                    const char* bool_val_start = m_ptr + m_search_key.length();
                    // 检查 m 后面是 true 还是 false
                    if (strncmp(bool_val_start, "true", 4) == 0) {
                        frame.side = -1; // 设置方向为 -1 (主动卖出/Short/Sell)
                    }
                    else if (strncmp(bool_val_start, "false", 5) == 0) {
                        frame.side = 1;  // 设置方向为 1 (主动买入/Long/Buy)
                    }
                }
                else { /* 错误处理或日志 */ }

                // 覆盖时间：使用消息中自带的时间戳，更精确
                const char* T_raw = find_json_numeric_value(msg, "T", &val_len); // 查找 "T": 字段，获取交易时间戳 (毫秒)
                if (T_raw) frame.timestamp = strtoull(std::string(T_raw, val_len).c_str(), nullptr, 10) * 1000000ULL; else { /* 错误处理或日志 */ }

                valid_data = true; // 标记为有效数据
            }
            // === 2. forceOrder (爆仓) ===
            // 处理强平订单消息，记录价格、数量和方向
            else if (strncmp(e_start, "forceOrder", 10) == 0) { // 如果事件类型是 "forceOrder"
                frame.type = 3; // 设置帧类型为 3 (代表 forceOrder)
                const char* o_obj = strstr(msg, "\"o\":"); // 查找 "o": 字段，指向订单对象
                if (o_obj) { // 如果找到了订单对象
                    const char* p_raw = find_json_string_value(o_obj, "p", &val_len); // 查找 "p":" 字符串，获取价格
                    if (p_raw) frame.price = strtod(std::string(p_raw, val_len).c_str(), nullptr); else { /* 错误处理或日志 */ }
                    const char* q_raw = find_json_string_value(o_obj, "q", &val_len); // 查找 "q":" 字符串，获取数量
                    if (q_raw) frame.quantity = strtod(std::string(q_raw, val_len).c_str(), nullptr); else { /* 错误处理或日志 */ }

                    // 解析方向: "S":"SELL" -> 多头爆仓 (卖出平仓) -> side = -1
                    const char* S_raw = find_json_string_value(o_obj, "S", &val_len); // 查找 "S":" 字符串，获取交易方向 (BUY/SELL)
                    if (S_raw) {
                        if (strncmp(S_raw, "BUY", 3) == 0) frame.side = 1;
                        else if (strncmp(S_raw, "SELL", 4) == 0) frame.side = -1;
                    }
                    else { /* 错误处理或日志 */ }
                    // ... 时间解析略 ... (这里省略了时间解析，可以根据需要添加)
                    valid_data = true; // 标记为有效数据
                }
                else { /* 错误处理或日志 */ }
            }
        }
    }

    // === 写入仓库 ===
    // 将解析后的市场数据写入共享内存环形缓冲区 (g_master_bridge->market_ring)
    if (valid_data && g_master_bridge) { // 如果数据有效且共享内存桥接存在
        // 原子地获取当前写入索引并递增 (release memory order，确保数据在索引更新前可见)
        uint64_t idx = g_master_bridge->market_ring.write_index.fetch_add(1, std::memory_order_release);
        // 计算在环形缓冲区中的实际位置
        int pos = idx & RING_BUFFER_MASK;
        g_master_bridge->market_ring.frames[pos] = frame; // 将解析后的帧写入环形缓冲区
    }

    ParsedMarketData parsed_data = {};
    if (valid_data) {
        parsed_data.price = frame.price;
        parsed_data.quantity = frame.quantity;
        parsed_data.bid_price = frame.bid_p;
        parsed_data.bid_quantity = frame.bid_q;
        parsed_data.ask_price = frame.ask_p;
        parsed_data.ask_quantity = frame.ask_q;
        parsed_data.t_exch = frame.timestamp;
        parsed_data.t_local = frame.local_timestamp; // 增加本地时间戳的赋值

        if (frame.type == 1) parsed_data.message_type = MessageType::AGG_TRADE;
        else if (frame.type == 2) parsed_data.message_type = MessageType::BOOK_TICKER;
        else if (frame.type == 3) parsed_data.message_type = MessageType::LIQUIDATION_ORDER;
        else parsed_data.message_type = MessageType::UNKNOWN;
    }
    return parsed_data;
}
// 最终清洗版 run_event_loop (直接覆盖)
// ============================================================
// 核心事件循环函数，负责监听 WebSocket 连接上的数据，发送心跳包，并处理接收到的消息。
// 使用 epoll 进行高效的 I/O 多路复用。
void run_event_loop() {
    // 从环境变量获取 API Key 和 Secret
    const char *env_key = std::getenv("BINANCE_API_KEY");
    const char *env_secret = std::getenv("BINANCE_API_SECRET");
    std::string api_key;
    std::string api_secret;

    if (env_key) api_key = env_key;
    else {
        std::cerr << "[FATAL] BINANCE_API_KEY environment variable not set. Exiting." << std::endl;
        set_g_running_false("BINANCE_API_KEY not set");
        return;
    }
    if (env_secret) api_secret = env_secret;
    else {
        std::cerr << "[FATAL] BINANCE_API_SECRET environment variable not set. Exiting." << std::endl;
        set_g_running_false("BINANCE_API_SECRET not set");
        return;
    }

    // 创建 epoll 实例
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        set_g_running_false("epoll_create1 failed");
        return;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = g_public_ws_client.fd_;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_public_ws_client.fd_, &ev);

    if (g_user_ws_client.fd_ != -1) {
        ev.data.fd = g_user_ws_client.fd_;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_user_ws_client.fd_, &ev);
    }

    struct epoll_event events[2];
    uint64_t last_ping_ns = 0;
    uint64_t last_listen_key_refresh_ns = 0; // 新增：记录上次 listenKey 续期时间

    while (g_running) {
        if (g_master_bridge) {
            g_master_bridge->system_health.store(Common::get_now_ns(), std::memory_order_release);
        }
        uint64_t current_ns = Common::get_now_ns();

        // 1. 发送心跳 PING (每10秒)
        if (current_ns - last_ping_ns > 10 * 1000 * 1000 * 1000ULL) {
            std::string ping_payload = std::to_string(current_ns);
            send_frame(g_public_ws_client, ping_payload.c_str(), ping_payload.length(), 0x9);
            last_ping_ns = current_ns;
        }

        // 2. ListenKey 续期 (每 30 分钟)
        // 30 分钟 = 30 * 60 秒 = 1800 秒 = 1800 * 10^9 纳秒
        if (!g_listen_key.empty() && current_ns - last_listen_key_refresh_ns > 30 * 60 * 1000 * 1000 * 1000ULL) {
            keep_alive_listen_key(g_curl_handle, g_listen_key, api_key, api_secret);
            last_listen_key_refresh_ns = current_ns;
        }

        // ============================================================
        // [新增] 3. 指令消费 (Command Consumer) - 这里的耳朵终于治好了
        // ============================================================
        if (g_master_bridge) {
            // 1. 获取当前读写指针
            // load(acquire) 确保我们看到的是最新的写入
            uint64_t w_idx = g_master_bridge->command_ring.write_idx.load(std::memory_order_acquire);
            uint64_t r_idx = g_master_bridge->command_ring.read_idx.load(std::memory_order_relaxed);

            // 2. 追赶处理 (如果 读 < 写，说明有新指令)
            while (r_idx < w_idx) {
                // 计算环形缓冲区的位置
                int idx = r_idx & COMMAND_RING_BUFFER_MASK;
                CommandFrame& frame = g_master_bridge->command_ring.frames[idx];

                // 3. 解析指令并调用 Executor
                std::string symbol(frame.symbol);
                std::string side_str = (frame.side == 1) ? "BUY" : "SELL";

                if (frame.action == ACT_NEW) {
                    if (frame.type == ORD_LIMIT) {
                        // 限价单
                        printf("[Command] 收到限价单 (TIF=%d): %s %s %.4f @ %.2f\n", 
                               frame.tif, symbol.c_str(), side_str.c_str(), frame.quantity, frame.price);
                               
                        // 修改这行，把 frame.tif 传进去
                        Executor::place_limit_order(symbol, side_str, frame.price, frame.quantity, frame.tif);
                    } 
                    else if (frame.type == ORD_MARKET) {
                        // 市价单
                        printf("[Command] 收到市价单: %s %s %.4f\n", 
                               symbol.c_str(), side_str.c_str(), frame.quantity);
                        Executor::place_market_order(symbol, side_str, frame.quantity);
                    }
                }
                else if (frame.action == ACT_CANCEL_ALL) {
                    // 撤销所有
                    printf("[Command] 收到全撤指令: %s\n", symbol.c_str());
                    Executor::cancel_all_orders(symbol);
                }
                // ... 如果有 ACT_CANCEL (单撤) 或 ACT_AMEND (改单) 可以在这里扩展 ...

                // 4. 处理完毕，推进读指针
                r_idx++;
                g_master_bridge->command_ring.read_idx.store(r_idx, std::memory_order_release);
            }
        }
        // ============================================================

        // 等待 epoll 事件，最长等待 100 毫秒
        int nfds = epoll_wait(epoll_fd, events, 2, 100);
        if (nfds == -1 && errno != EINTR) continue;

        for (int i = 0; i < nfds; ++i) {
            InternalWebSocketClient* current_client = nullptr;
            if (events[i].data.fd == g_public_ws_client.fd_) current_client = &g_public_ws_client;
            else if (events[i].data.fd == g_user_ws_client.fd_) current_client = &g_user_ws_client;

            if (current_client) {
                while (true) {
                    size_t read_start_pos = current_client->offset_;
                    size_t max_read_len = InternalWebSocketClient::kBufferSize - read_start_pos;

                    int len = SSL_read(current_client->ssl_, current_client->buffer_ + read_start_pos, max_read_len);

                    if (len > 0) {
                        size_t total_len = read_start_pos + len;
                        unsigned char *ptr = (unsigned char *)current_client->buffer_;
                        size_t remaining = total_len;

                        while (remaining >= 2) {
                            unsigned char opcode = ptr[0] & 0x0F;
                            unsigned char len_byte = ptr[1] & 0x7F;
                            size_t header_len = 2;
                            size_t payload_len = len_byte;
                            size_t frame_len = 0;

                            if (len_byte == 126) {
                                if (remaining < 4) break; // Ensure enough bytes for 2-byte length
                                payload_len = (ptr[2] << 8) | ptr[3];
                                header_len = 4;
                            }
                            else if (len_byte == 127) {
                                if (remaining < 10) break; // Ensure enough bytes for 8-byte length
                                
                                payload_len = (
                                    ((uint64_t)ptr[2] << 56) |
                                    ((uint64_t)ptr[3] << 48) |
                                    ((uint64_t)ptr[4] << 40) |
                                    ((uint64_t)ptr[5] << 32) |
                                    ((uint64_t)ptr[6] << 24) |
                                    ((uint64_t)ptr[7] << 16) |
                                    ((uint64_t)ptr[8] << 8)  |
                                    ((uint64_t)ptr[9])        
                                );
                                header_len = 10;
                            }
                            frame_len = header_len + payload_len;

                            if (remaining < frame_len) break;

                            // === 核心逻辑分流开始 ===
                            if (opcode == 0x9) {
                                send_frame(*current_client, (const char *)ptr + header_len, payload_len, 0xA);
                            }
                            else if (opcode == 0x8) {
                                set_g_running_false("WebSocket Closed");
                                break;
                            }
                            else if (opcode == 0x1 || opcode == 0x2) {
                                
                                // 路径 A: 公共行情 (Market Data)
                                if (current_client->is_public_stream) {
                                    // 解析市场数据 JSON 消息
                                    ParsedMarketData data = parse_market_data_json_no_alloc((const char *)ptr + header_len, payload_len);
                                    // Strategy::check_trigger(data.price); // C++ 端不再做策略决策，移除此行
                                }
                                // 路径 B: 用户私有流 (User Stream)
                                else {
                                    const char* msg_start = (const char*)ptr + header_len;
                                    if (strstr(msg_start, "\"e\":\"ORDER_TRADE_UPDATE\"")) {
                                        const char* o_obj = strstr(msg_start, "\"o\":");
                                        if (o_obj) {

                                            // 1. 提取所有关键数据
                                            size_t val_len = 0;

                                            const char* client_order_id_raw = find_json_string_value(o_obj, "c", &val_len);
                                            char client_order_id[64] = {0};
                                            if (client_order_id_raw) {
                                                size_t copy_len = std::min((size_t)63, val_len);
                                                strncpy(client_order_id, client_order_id_raw, copy_len);
                                                client_order_id[copy_len] = '\0'; // 确保 null 终止
                                            }

                                            const char* exch_order_id_raw = find_json_string_value(o_obj, "i", &val_len); // Changed to string value
                                            char exch_order_id[64] = {0};
                                            if (exch_order_id_raw) {
                                                size_t copy_len = std::min((size_t)63, val_len);
                                                strncpy(exch_order_id, exch_order_id_raw, copy_len);
                                                exch_order_id[copy_len] = '\0'; // 确保 null 终止
                                            }
                                            
                                            const char* status_ptr = strstr(o_obj, "\"X\":\"");
                                            int order_status_event = EVT_NONE;
                                            if (status_ptr) {
                                                // Compare using length of actual status string to avoid partial matches
                                                if (strncmp(status_ptr + 5, "NEW", 3) == 0) order_status_event = EVT_SUBMITTED;
                                                else if (strncmp(status_ptr + 5, "PARTIALLY_FILLED", 16) == 0) order_status_event = EVT_PARTIAL_FILL;
                                                else if (strncmp(status_ptr + 5, "FILLED", 6) == 0) order_status_event = EVT_FULL_FILL;
                                                else if (strncmp(status_ptr + 5, "CANCELED", 8) == 0) order_status_event = EVT_CANCELED;
                                                else if (strncmp(status_ptr + 5, "REJECTED", 8) == 0) order_status_event = EVT_REJECTED;
                                                // Add other relevant statuses if necessary, e.g., EXPIRED, PENDING_CANCEL, etc.
                                            }

                                            const char* L_raw = find_json_string_value(o_obj, "L", &val_len);
                                            double fill_px = L_raw ? strtod(std::string(L_raw, val_len).c_str(), nullptr) : 0.0;
                                            const char* l_raw = find_json_string_value(o_obj, "l", &val_len);
                                            double fill_qty = l_raw ? strtod(std::string(l_raw, val_len).c_str(), nullptr) : 0.0;
                                            const char* q_raw = find_json_string_value(o_obj, "q", &val_len);
                                            double original_qty = q_raw ? strtod(std::string(q_raw, val_len).c_str(), nullptr) : 0.0;
                                            const char* z_raw = find_json_numeric_value(o_obj, "z", &val_len);
                                            double cum_fill_qty = z_raw ? strtod(std::string(z_raw, val_len).c_str(), nullptr) : 0.0;

                                            double remaining_qty = original_qty - cum_fill_qty;
                                            if (remaining_qty < 0) remaining_qty = 0;

                                            const char* T_raw = find_json_numeric_value(o_obj, "T", &val_len); // 获取事件时间戳 (毫秒)
                                            uint64_t event_timestamp = T_raw ? strtoull(std::string(T_raw, val_len).c_str(), nullptr, 10) : 0ULL;

                                            const char* u_raw = find_json_numeric_value(o_obj, "u", &val_len); // 获取 update_id
                                            uint64_t update_id = u_raw ? strtoull(std::string(u_raw, val_len).c_str(), nullptr, 10) : 0ULL;

                                            const char* m_raw = find_json_string_value(o_obj, "m", &val_len); // 获取 is_maker 字段
                                            bool is_maker = (m_raw && strncmp(m_raw, "true", 4) == 0); // 将 "true" 字符串转换为 bool 值

                                            Strategy::on_order_update_private_stream(client_order_id, exch_order_id, fill_px, fill_qty, remaining_qty, order_status_event, update_id, is_maker);

                                        }
                                    }
                                }
                            }
                            else {
                            }

                            ptr += frame_len;
                            remaining -= frame_len;
                        }

                        if (remaining > 0) {
                            memmove(current_client->buffer_, ptr, remaining);
                        }
                        current_client->offset_ = remaining;

                    }
                    else if (len <= 0) {
                        int err = SSL_get_error(current_client->ssl_, len);
                        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                            ERR_print_errors_fp(stderr);
                            set_g_running_false("SSL Error in read loop");
                            break;
                        }
                        // current_client->offset_ 已经在前面的逻辑中更新过，这里无需再次修改
                        break;
                    }
                }
            }
        }
    }

    close(epoll_fd);
}



// 获取交易对精度信息
// 通过 HTTP API 请求获取指定交易对的详细信息，例如最小下单量、价格精度等
long fetch_exchange_info(CURL *curl_handle, const std::string& symbol, std::string &response_buffer) {
    if (!curl_handle) return 0;

    std::map<std::string, std::string> params;
    params["symbol"] = symbol;

    std::string full_url = "https://fapi.binance.com/fapi/v1/exchangeInfo";
    std::string query_string = "";
    
    // 对于 exchangeInfo，symbol 参数通常直接加在 URL 后面
    // 或者可以作为参数传递，让 perform_binance_request 处理
    // 这里我们直接构建完整的 URL
    query_string += url_encode(curl_handle, "symbol") + "=" + url_encode(curl_handle, symbol);

    if (!query_string.empty()) {
        full_url += "?" + query_string;
    }

    // 设置接收数据的回调
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &response_buffer);
    curl_easy_setopt(curl_handle, CURLOPT_URL, full_url.c_str());
    curl_easy_setopt(curl_handle, CURLOPT_HTTPGET, 1L);

    // exchangeInfo API 是公开的，不需要 API Key 和签名
    struct curl_slist *headers = NULL;

    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl_handle);
    long http_code = 0;
    curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        response_buffer = "CURL Error: " + std::string(curl_easy_strerror(res));
        printf("[DEBUG] fetch_exchange_info - CURL Error: %s\n", response_buffer.c_str());
        return -1;
    }
    printf("[DEBUG] fetch_exchange_info - HTTP Code: %ld\n", http_code);
    printf("[DEBUG] fetch_exchange_info - Response Buffer: %s\n", response_buffer.c_str());
    return http_code;
}

} // namespace Network
