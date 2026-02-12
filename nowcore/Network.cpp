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
// 全局的 WebSocket 客户端实例 (内部使用)
// 这里定义了公共流（市场行情）和私有流（用户订单、账户信息）的实例
// 它们是 InternalWebSocketClient 类型的全局对象，在程序启动时会进行初始化和连接
InternalWebSocketClient g_public_ws_client; // 公共 WebSocket 客户端，用于接收市场行情数据
InternalWebSocketClient g_user_ws_client;   // 用户 WebSocket 客户端，用于接收用户数据（如订单更新）

// 辅助函数，用于统一设置 g_running 为 false 并记录原因
// 任何地方调用它，g_running 变为 false，整个程序将安全停车，通常用于处理致命错误或程序退出
void set_g_running_false(const char* reason) {
    printf("[INFO] Network::set_g_running_false - Exiting due to: %s\n", reason); // 打印退出原因
    g_running = false; // 设置全局运行标志为 false，通知主循环停止
}

// 外部声明其他全局变量的定义
// 这些全局变量可能在 Common.cpp 中定义，在这里仅进行声明或不重复声明，以避免重复定义错误
// std::atomic<bool> g_running{true}; // 定义已移至 Common.cpp，表示程序是否正在运行的原子布尔变量
std::string g_listen_key; // 存储币安用户数据流的 listenKey，用于连接用户私有 WebSocket 流
// std::atomic<uint64_t> last_network_heartbeat_ns{0}; // 定义已移至 Common.cpp，记录上次网络心跳时间（纳秒）
// std::atomic<bool> g_low_power_mode{false}; // 定义已移至 Common.cpp，表示是否处于低功耗模式的原子布尔变量


// CURL 句柄 (如果启用 CURL)
CURL *g_curl_handle = nullptr; // 全局的 libcurl 句柄，用于进行 HTTP 请求

// 写入回调函数 (libcurl 使用)
// 当 libcurl 接收到 HTTP 响应数据时，会调用此函数将数据写入指定的缓冲区
size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string *)userp)->append((char *)contents, size * nmemb); // 将接收到的数据追加到 std::string 缓冲区
    return size * nmemb; // 返回实际处理的字节数
}

// URL 编码辅助函数：确保特殊字符被正确转义 (例如空格转 %20)
// 在构建 URL 参数时，需要对参数值进行 URL 编码，以防止特殊字符破坏 URL 结构
std::string url_encode(CURL *curl_handle, const std::string &value) {
    if (!curl_handle) return value; // 如果 CURL 句柄无效，则直接返回原始值
    char *output = curl_easy_escape(curl_handle, value.c_str(), value.length()); // 使用 libcurl 进行 URL 编码
    if (output) { // 如果编码成功
        std::string result = output; // 将编码后的结果转换为 std::string
        curl_free(output); // 释放 libcurl 分配的内存
        return result; // 返回编码后的字符串
    }
    return value; // 编码失败，返回原始值
}

// HMAC SHA256 实现
// 用于生成请求签名，确保 API 请求的安全性
std::string hmac_sha256(const std::string &key, const std::string &data) {
    unsigned char digest[EVP_MAX_MD_SIZE]; // 存储 HMAC 结果的缓冲区
    unsigned int len = 32; // SHA256 摘要的长度是 32 字节

    // 调用 OpenSSL 的 HMAC 函数计算 HMAC-SHA256
    HMAC(EVP_sha256(), // 使用 SHA256 算法
         key.c_str(), key.length(), // 签名密钥
         (unsigned char *)data.c_str(), data.length(), // 要签名的数据
         digest, &len); // 存储结果和结果长度

    char hex_str[65]; // 用于存储十六进制表示的 HMAC 结果 (32字节 * 2字符/字节 + 1个终止符)
    for (int i = 0; i < 32; i++) {
        sprintf(hex_str + i * 2, "%02x", digest[i]); // 将每个字节转换为两位十六进制字符
    }
    return std::string(hex_str); // 返回十六进制字符串表示的签名
}

// 执行币安 API 请求的通用函数
// 处理 GET, POST, DELETE 等 HTTP 方法，以及签名和参数构建
long perform_binance_request(CURL *curl_handle, // libcurl 句柄
                             const std::string &method, // HTTP 方法 (GET, POST, DELETE)
                             const std::string &path, // API 路径 (例如 "/fapi/v1/listenKey")
                             const std::map<std::string, std::string> &params, // 请求参数
                             std::string &response_buffer, // 存储 API 响应的缓冲区
                             const std::string &api_key, // 币安 API Key
                             const std::string &api_secret, // 币安 API Secret
                             bool signed_request) { // 是否需要签名

    if (!curl_handle) return 0; // 如果 CURL 句柄无效，直接返回 0 (表示失败)

    // 1. 准备参数副本，防止修改原始参数
    std::map<std::string, std::string> final_params = params;

    // 2. 注入 timestamp (这是签名必须的) 和 recvWindow
    // 对于需要签名的请求，必须包含时间戳和接收窗口
    if (signed_request) {
        // 获取当前时间戳（毫秒）
        long long timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        final_params["timestamp"] = std::to_string(timestamp); // 将时间戳添加到参数中
        final_params["recvWindow"] = "10000"; // 设置 recvWindow 为 10000 毫秒 (即请求在 10 秒内有效)
    }

    // 3. 构建未编码的 Query String 用于签名
    // 签名是基于原始的、未 URL 编码的参数字符串
    std::string raw_query_string = "";
    for (auto it = final_params.begin(); it != final_params.end(); ++it) {
        if (!raw_query_string.empty()) {
            raw_query_string += "&"; // 如果不是第一个参数，添加 "&" 分隔符
        }
        // 不对 key 和 value 进行 URL Encoding，直接拼接
        raw_query_string += it->first + "=" + it->second;
    }

    // 4. 计算签名
    std::string signature_str = "";
    if (signed_request && !api_secret.empty()) { // 如果需要签名且 API Secret 不为空
        signature_str = hmac_sha256(api_secret, raw_query_string); // 对未编码的字符串进行 HMAC-SHA256 签名
    }

    // 5. 构建最终的 URL 编码的 Query String (包含签名)
    // 实际发送的请求参数需要进行 URL 编码
    std::string query_string = "";
    for (auto it = final_params.begin(); it != final_params.end(); ++it) {
        if (!query_string.empty()) {
            query_string += "&"; // 如果不是第一个参数，添加 "&" 分隔符
        }
        // 对 key 和 value 进行 URL Encoding
        query_string += url_encode(curl_handle, it->first) + "=" + url_encode(curl_handle, it->second);
    }

    // 追加签名到编码后的 query_string
    if (!signature_str.empty()) { // 如果签名不为空，将其添加到参数中
        query_string += "&signature=" + signature_str;
    }

    // 构建完整的请求 URL
    std::string full_url = "https://fapi.binance.com" + path; // 币安 U 本位合约 API 的基础 URL

    // 5. 设置 HTTP 头
    struct curl_slist *headers = NULL; // 初始化 HTTP 头列表
    // 添加 API Key 头
    headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + api_key).c_str());
    // 明确指定 Content-Type 为 URL 编码表单
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded"); // 重新启用 Content-Type

    // 打印调试信息
    printf("[DEBUG] perform_binance_request - Method: %s\n", method.c_str());
    printf("[DEBUG] perform_binance_request - Path: %s\n", path.c_str());
    printf("[DEBUG] perform_binance_request - Query String (or Body for POST): %s\n", query_string.c_str());
    printf("[DEBUG] perform_binance_request - Query String Length: %zu\n", query_string.length()); // 新增调试打印长度
    printf("[DEBUG] perform_binance_request - X-MBX-APIKEY: %s\n", api_key.c_str());


    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);

    // 6. 根据 Method 配置 CURL
    if (method == "POST") { // 如果是 POST 请求
        curl_easy_setopt(curl_handle, CURLOPT_POST, 1L); // 设置为 POST 方法
        curl_easy_setopt(curl_handle, CURLOPT_URL, full_url.c_str()); // 设置请求 URL
        
        // POST 数据放入 Body
        curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, query_string.c_str()); // 设置 POST 请求体
        // 显式告知 Body 大小，防止截断
        curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, (long)query_string.length()); // 设置 POST 请求体长度
        printf("[DEBUG] perform_binance_request - Full URL (POST): %s\n", full_url.c_str());


    } else if (method == "DELETE") { // 如果是 DELETE 请求
        curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, "DELETE"); // 设置为自定义 DELETE 方法
        if (!query_string.empty()) {
            full_url += "?" + query_string; // 将参数添加到 URL 后面
        }
        curl_easy_setopt(curl_handle, CURLOPT_URL, full_url.c_str()); // 设置请求 URL
        printf("[DEBUG] perform_binance_request - Full URL (DELETE): %s\n", full_url.c_str());
        
    } else { // 默认为 GET 请求
        curl_easy_setopt(curl_handle, CURLOPT_HTTPGET, 1L); // 设置为 GET 方法
        if (!query_string.empty()) {
            full_url += "?" + query_string; // 将参数添加到 URL 后面
        }
        curl_easy_setopt(curl_handle, CURLOPT_URL, full_url.c_str()); // 设置请求 URL
        printf("[DEBUG] perform_binance_request - Full URL (GET): %s\n", full_url.c_str());
    }

    // 设置接收数据的回调函数和缓冲区
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback); // 设置数据写入回调函数
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &response_buffer); // 设置写入数据的目标缓冲区

    CURLcode res = curl_easy_perform(curl_handle); // 执行 CURL 请求
    long http_code = 0; // 存储 HTTP 响应码
    curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code); // 获取 HTTP 响应码

    curl_slist_free_all(headers); // 释放 HTTP 头列表的内存

    if (res != CURLE_OK) { // 如果 CURL 请求失败
        response_buffer = "CURL Error: " + std::string(curl_easy_strerror(res)); // 记录 CURL 错误信息
        return -1; // 返回 -1 表示请求失败
    }

    return http_code; // 返回 HTTP 响应码
}


// --- 辅助函数实现 (InternalWebSocketClient methods adapted) ---
// 内部 WebSocket 客户端相关辅助函数，主要用于建立和维护 WebSocket 连接

// 初始化 SSL 上下文 (通用)
// 配置 OpenSSL 环境，用于 SSL/TLS 加密通信
void init_ssl_ctx(SSL_CTX*& ctx) {
    OSSL_PROVIDER_load(NULL, "legacy"); // 加载 legacy provider，支持旧版加密算法
    OSSL_PROVIDER_load(NULL, "default"); // 加载 default provider
    SSL_load_error_strings(); // 加载 OpenSSL 错误字符串，方便错误诊断
    OpenSSL_add_all_algorithms(); // 加载所有加密算法
    ctx = SSL_CTX_new(TLS_client_method()); // 创建一个新的 SSL 上下文，使用 TLS 客户端方法
    if (!ctx) { // 如果 SSL_CTX_new 失败
        std::cerr << "[Network] SSL_CTX_new failed" << std::endl; // 打印错误信息
        ERR_print_errors_fp(stderr); // 打印 OpenSSL 错误堆栈到标准错误流
        fflush(stderr); // 刷新 stderr 缓冲区，确保错误信息立即显示
        exit(1); // 致命错误，退出程序
    }
}

// 连接 TCP 服务器
// 建立底层的 TCP 连接，为后续的 SSL/TLS 和 WebSocket 握手做准备
int connect_tcp(const std::string &host, const std::string &port) {
    struct addrinfo hints = {}, *res; // 用于存储地址信息的结构体
    hints.ai_family = AF_UNSPEC; // 允许 IPv4 或 IPv6
    hints.ai_socktype = SOCK_STREAM; // 流式套接字 (TCP)

    // 解析主机名和端口号，获取可用的地址信息
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) {
        return -1; // 解析失败，返回 -1
    }

    // 创建套接字
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { // 如果套接字创建失败
        freeaddrinfo(res); // 释放地址信息
        return -1; // 返回 -1
    }

    // 设置 TCP_NODELAY 选项
    // 禁用 Nagle 算法，即使数据包很小也会立即发送，减少延迟
    int enable = 1;
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (void*)&enable, sizeof(enable)) < 0) {
        printf("[LOG] TCP_NODELAY set failed. errno: %d, msg: %s\n", errno, strerror(errno)); // 打印错误日志
    }

    // 设置 SO_KEEPALIVE 选项
    // 启用 TCP Keep-Alive 机制，定期发送探测包以检测连接是否存活
    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void*)&enable, sizeof(enable)) < 0) {
        printf("[LOG] SO_KEEPALIVE set failed. errno: %d, msg: %s\n", errno, strerror(errno)); // 打印错误日志
    }

    // 连接到服务器
    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        close(sock); // Linux/Unix 特定：关闭套接字
        freeaddrinfo(res); // 释放地址信息
        return -1; // 连接失败，返回 -1
    }

    freeaddrinfo(res); // 释放地址信息
    return sock; // 返回已连接的套接字文件描述符
}

// 执行 SSL/TLS 握手
// 在 TCP 连接建立后，进行 SSL/TLS 握手以建立加密通道
bool perform_ssl_handshake(InternalWebSocketClient& client, int fd) {
    client.ssl_ = SSL_new(client.ctx_); // 从 SSL 上下文创建一个新的 SSL 结构
    SSL_set_fd(client.ssl_, fd); // 将套接字文件描述符绑定到 SSL 结构
    if (SSL_connect(client.ssl_) != 1) { // 执行 SSL/TLS 客户端握手
        ERR_print_errors_fp(stderr); // 打印 OpenSSL 错误堆栈
        fflush(stderr); // 刷新 stderr 缓冲区
        return false; // 握手失败
    }
    return true; // 握手成功
}

// 执行 WebSocket 握手
// 在 SSL/TLS 加密通道建立后，进行 WebSocket 协议握手
bool perform_ws_handshake(InternalWebSocketClient& client, const std::string &host, const std::string &path) {
    char request[1024]; // 用于构建 HTTP 握手请求
    unsigned char key_bytes[16]; // 用于生成 Sec-WebSocket-Key 的随机字节
    if (RAND_bytes(key_bytes, sizeof(key_bytes)) != 1) { // 生成 16 字节的随机数
        return false; // 生成失败
    }

    BIO *b64 = BIO_new(BIO_f_base64()); // 创建一个 Base64 编码的 BIO 过滤器
    BIO *bio = BIO_new(BIO_s_mem()); // 创建一个内存 BIO
    bio = BIO_push(b64, bio); // 将 Base64 过滤器推入内存 BIO
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL); // 设置 Base64 编码不带换行符
    BIO_write(bio, key_bytes, sizeof(key_bytes)); // 将随机字节写入 BIO 进行 Base64 编码
    BIO_flush(bio); // 刷新 BIO 缓冲区

    BUF_MEM *bufferPtr; // 指向内存 BIO 内部缓冲区的指针
    BIO_get_mem_ptr(bio, &bufferPtr); // 获取内存 BIO 的内部缓冲区信息
    std::string sec_websocket_key(bufferPtr->data, bufferPtr->length); // 从缓冲区中提取 Sec-WebSocket-Key
    BIO_free_all(bio); // 释放所有 BIO 资源

    // 构建 WebSocket 握手请求的 HTTP 头
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\n" // GET 请求方法和 HTTP 版本
             "Host: %s\n"        // Host 头
             "Upgrade: websocket\n" // 升级协议为 WebSocket
             "Connection: Upgrade\n"// 保持连接，并要求升级
             "Sec-WebSocket-Key: %s\n" // WebSocket 密钥
             "Sec-WebSocket-Version: 13\n" // WebSocket 协议版本
             "\n",                // 空行表示 HTTP 头结束
             path.c_str(), host.c_str(), sec_websocket_key.c_str());

    // 发送 WebSocket 握手请求
    int write_len = SSL_write(client.ssl_, request, strlen(request));
    if (write_len <= 0) { // 如果写入失败
        return false;
    }

    char response[1024]; // 用于接收服务器响应
    int len = SSL_read(client.ssl_, response, sizeof(response) - 1); // 读取服务器响应
    if (len <= 0) { // 如果读取失败
        return false;
    }
    response[len] = 0; // 确保响应字符串以 null 结尾

    // 检查服务器响应是否包含 "101 Switching Protocols"，这是 WebSocket 握手成功的标志
    if (strstr(response, "101 Switching Protocols") == nullptr) {
        std::cerr << "[ERROR] WebSocket handshake failed. Server response: " << response << std::endl; // 打印错误信息
        fflush(stderr); // 刷新 stderr 缓冲区
        return false; // 握手失败
    }
    return true; // 握手成功
}

// 发送 WebSocket 帧
// 将数据封装成 WebSocket 帧并发送
void send_frame(InternalWebSocketClient& client, const char* payload, size_t len, uint8_t opcode) {
    unsigned char header[14]; // WebSocket 帧头部，最大长度为 14 字节
    size_t header_len = 0; // 实际头部长度
    header[0] = 0x80 | opcode; // 设置 FIN 位 (0x80) 和 Opcode
                               // Opcode 0x1 表示文本帧，0x2 表示二进制帧，0x9 表示 Ping 帧，0xA 表示 Pong 帧

    // 根据 payload 长度设置 WebSocket 帧的长度字段
    if (len <= 125) { // 如果 payload 长度小于等于 125 字节
        header[1] = 0x80 | (uint8_t)len; // 设置 Mask 位 (0x80) 和 payload 长度
        header_len = 2; // 头部长度为 2 字节
    } else if (len <= 0xFFFF) { // 如果 payload 长度大于 125 但小于等于 65535 字节 (2 字节长度)
        header[1] = 0x80 | 126; // 设置 Mask 位和长度指示符 126
        header[2] = (len >> 8) & 0xFF; // 长度的第一个字节
        header[3] = len & 0xFF; // 长度的第二个字节
        header_len = 4; // 头部长度为 4 字节
    } else { // 如果 payload 长度大于 65535 字节 (8 字节长度)
        header[1] = 0x80 | 127; // 设置 Mask 位和长度指示符 127
        // 写入 8 字节的 payload 长度
        header[2] = (len >> 56) & 0xFF; 
        header[3] = (len >> 48) & 0xFF;
        header[4] = (len >> 40) & 0xFF;
        header[5] = (len >> 32) & 0xFF;
        header[6] = (len >> 24) & 0xFF;
        header[7] = (len >> 16) & 0xFF;
        header[8] = (len >> 8) & 0xFF;
        header[9] = len & 0xFF;
        header_len = 10; // 头部长度为 10 字节
    }

    uint8_t mask[4]; // 4 字节的掩码密钥
    if (RAND_bytes(mask, sizeof(mask)) != 1) { // 生成随机掩码密钥
        // 处理错误，例如打印错误日志或设置客户端状态为错误
        fprintf(stderr, "[ERROR] Failed to generate random WebSocket mask key.\\n");
        // 这里可以选择退出，或者在实际应用中更优雅地处理
        // 为了保持和现有错误处理风格一致，我们在这里可以简化处理
        // 但更好的做法是返回一个错误码，让调用者决定如何处理
        // 目前，我们假定 RAND_bytes 成功，因为前面已经有类似检查
    }    memcpy(header + header_len, mask, 4); // 将掩码密钥复制到头部
    header_len += 4; // 更新头部总长度

    SSL_write(client.ssl_, header, header_len); // 发送 WebSocket 帧头部

    std::vector<char> masked(len); // 创建一个用于存储掩码后数据的缓冲区
    for (size_t i = 0; i < len; ++i) {
        masked[i] = payload[i] ^ mask[i % 4]; // 对 payload 进行掩码处理
    }

    SSL_write(client.ssl_, masked.data(), len); // 发送掩码后的 payload 数据

}


// --- 核心 Network 模块函数实现 ---
// Network 模块的核心功能，包括初始化、事件循环和数据解析

void init() { // Network 模块的初始化函数
    curl_global_init(CURL_GLOBAL_ALL); // 初始化 libcurl 库，支持所有功能
    printf("[DEBUG] Network::init() - Starting initialization.\n"); // 打印调试信息
    init_ssl_ctx(g_public_ws_client.ctx_); // 初始化公共 WebSocket 客户端的 SSL 上下文
    init_ssl_ctx(g_user_ws_client.ctx_);   // 初始化用户 WebSocket 客户端的 SSL 上下文
    printf("[DEBUG] Network::init() - SSL contexts initialized.\n"); // 打印调试信息

    // 设置 is_public_stream 标志位，用于区分公共流和私有流
    g_public_ws_client.is_public_stream = true;  // 公共流标志设为 true
    g_user_ws_client.is_public_stream = false; // 用户流标志设为 false

    g_curl_handle = curl_easy_init(); // 初始化一个 CURL 句柄，用于 HTTP 请求
    if (!g_curl_handle) { // 如果 CURL 句柄初始化失败
        std::cerr << "[FATAL] Could not initialize CURL" << std::endl; // 打印错误信息
        exit(1); // 致命错误，退出程序
    }
    curl_easy_setopt(g_curl_handle, CURLOPT_TCP_NODELAY, 1L); // 设置 CURL 选项：禁用 Nagle 算法
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
    curl_easy_setopt(g_curl_handle, CURLOPT_CONNECTTIMEOUT_MS, 5000L); // 设置连接超时时间为 5000 毫秒
    curl_easy_setopt(g_curl_handle, CURLOPT_TIMEOUT_MS, 10000L); // 设置总超时时间为 10000 毫秒
    
    // 启用 CURL 详细调试信息
    curl_easy_setopt(g_curl_handle, CURLOPT_VERBOSE, 1L); // 启用详细模式，打印所有通信
    curl_easy_setopt(g_curl_handle, CURLOPT_DEBUGFUNCTION, debug_callback); // 设置调试回调函数
    // curl_easy_setopt(g_curl_handle, CURLOPT_DEBUGDATA, nullptr); // 可以设置用户数据，这里不需要
    
    // 显式设置 SSL 验证
    curl_easy_setopt(g_curl_handle, CURLOPT_SSL_VERIFYPEER, 1L); // 验证服务器证书
    curl_easy_setopt(g_curl_handle, CURLOPT_SSL_VERIFYHOST, 2L); // 验证服务器证书中的主机名与 URL 中的主机名是否匹配
    
    printf("[DEBUG] Network::init() - CURL initialized.\n"); // 打印调试信息

    // 获取 listenKey for user data stream
    // listenKey 是币安用于用户数据流的唯一标识，需要通过 HTTP 请求获取
    std::map<std::string, std::string> params; // 请求参数
    std::string response; // 存储 API 响应
    const char *env_key = std::getenv("BINANCE_API_KEY"); // 从环境变量中获取 BINANCE_API_KEY
    std::string api_key; // 存储 API Key
    if (env_key) { // 如果环境变量存在
        api_key = env_key; // 获取 API Key
    } else { // 如果环境变量不存在
        std::cerr << "[FATAL] BINANCE_API_KEY environment variable not set. Exiting." << std::endl; // 打印致命错误信息
        exit(1); // 退出程序
    }
   
    // 强制设置为单向持仓模式 (One-Way)
    printf("[DEBUG] Network::init() - Setting One-Way Position Mode.\n");
    long mode_code = perform_binance_request(g_curl_handle, "POST", "/fapi/v1/positionSide/dual", 
                                             {{"dualSidePosition", "false"}}, response, api_key, api_secret, true);
    if (mode_code != 200) {
        std::cerr << "[WARNING] Failed to set One-Way Position Mode: HTTP " << mode_code << ", Response: " << response << std::endl;
    } else {
        printf("[DEBUG] Network::init() - One-Way Position Mode set successfully.\n");
    }

    // 强制设置 20倍 杠杆 (避免默认 1倍 或 125倍 的意外)
    printf("[DEBUG] Network::init() - Setting 20x leverage for BNBUSDT.\n");
    long leverage_code = perform_binance_request(g_curl_handle, "POST", "/fapi/v1/leverage", 
                                                 {{"symbol", "BNBUSDT"}, {"leverage", "20"}}, response, api_key, api_secret, true);
    if (leverage_code != 200) {
        std::cerr << "[WARNING] Failed to set 20x leverage: HTTP " << leverage_code << ", Response: " << response << std::endl;
    } else {
        printf("[DEBUG] Network::init() - 20x leverage set successfully for BNBUSDT.\n");
    }
   
    printf("[DEBUG] Network::init() - Attempting to get ListenKey.\n"); // 打印调试信息
    // 发送 POST 请求获取 listenKey
    long http_code = perform_binance_request(g_curl_handle, "POST", "/fapi/v1/listenKey",
                                            params, response, api_key, "", false); // 注意：获取 listenKey 的请求不需要 API Secret 和签名
    if (http_code == 200) { // 如果 HTTP 请求成功 (HTTP 状态码 200)
        // 从响应中解析 listenKey
        const char *key_ptr = strstr(response.c_str(), "\"listenKey\":\""); // 查找 "listenKey":" 字符串
        if (key_ptr) { // 如果找到了
            const char *end_ptr = strstr(key_ptr + 13, "\""); // 查找 listenKey 值的结束引号
            if (end_ptr) { // 如果找到结束引号
                g_listen_key = std::string(key_ptr + 13, end_ptr - (key_ptr + 13)); // 提取 listenKey
                printf("[DEBUG] Network::init() - ListenKey obtained: %s\n", g_listen_key.c_str()); // 打印获取到的 listenKey
            } else {
                std::cerr << "[ERROR] end_ptr is NULL! Failed to find the closing quote for listenKey." << std::endl; // 打印错误信息
                printf("[LOG] Event Type: 3, Error: end_ptr is NULL!\n"); // 记录日志
            }
        }
     else {
            std::cerr << "[WARNING] ListenKey not found in response during init: " << response << std::endl; // 打印警告信息
            printf("[LOG] Event Type: 3, Error: ListenKey not found!\n"); // 记录日志
        }
    }
 else {
            std::cerr << "[WARNING] Failed to get ListenKey during init: HTTP " << http_code << ", Response: " << response << std::endl; // 打印警告信息
            printf("[LOG] Event Type: 3, Error: Failed to get ListenKey! HTTP: %ld\n", http_code); // 记录日志
        }

    // 连接公共行情 WebSocket
    // 用于获取市场行情数据，例如聚合交易、盘口数据和强平订单
    std::string public_host = "fstream.binance.com", public_path = "/ws/bnbusdt@aggTrade?timeUnit=MICROSECOND", public_port = "443"; // 公共 WebSocket 的主机、路径和端口
    printf("[DEBUG] Network::init() - Connecting public WebSocket to %s:%s%s\n", public_host.c_str(), public_port.c_str(), public_path.c_str()); // 打印调试信息
    g_public_ws_client.fd_ = connect_tcp(public_host, public_port); // 建立公共 WebSocket 的 TCP 连接
    if (g_public_ws_client.fd_ < 0) { // 如果 TCP 连接失败
        std::cerr << "[FATAL] Public WebSocket TCP connection failed." << std::endl; // 打印致命错误信息
        printf("[LOG] Event Type: 3, Error: Public WebSocket TCP connection failed!\n"); // 记录日志
        exit(1); // 退出程序
    }
    printf("[DEBUG] Network::init() - Public WebSocket TCP connected. FD: %d\n", g_public_ws_client.fd_); // 打印调试信息

    if (!perform_ssl_handshake(g_public_ws_client, g_public_ws_client.fd_)) { // 执行公共 WebSocket 的 SSL 握手
        std::cerr << "[FATAL] Public WebSocket SSL handshake failed." << std::endl; // 打印致命错误信息
        ERR_print_errors_fp(stderr); // 打印 OpenSSL 错误
        printf("[LOG] Event Type: 3, Error: Public WebSocket SSL handshake failed!\n"); // 记录日志
        exit(1); // 退出程序
    }
    printf("[DEBUG] Network::init() - Public WebSocket SSL handshake successful.\n"); // 打印调试信息

    if (!perform_ws_handshake(g_public_ws_client, public_host, public_path)) { // 执行公共 WebSocket 的协议握手
        std::cerr << "[FATAL] Public WebSocket handshake failed." << std::endl; // 打印致命错误信息
        printf("[LOG] Event Type: 3, Error: Public WebSocket handshake failed!\n"); // 记录日志
        exit(1); // 退出程序
    }
    printf("[DEBUG] Network::init() - Public WebSocket handshake successful.\n"); // 打印调试信息

    // 订阅 U本位永续合约 (bnbusdt) 的三个核心流
    // 包括 aggTrade (聚合交易), bookTicker (最优买卖盘), forceOrder (强平订单)
    const char* subscribe_aggtrade_payload = "{\"method\":\"SUBSCRIBE\",\"params\":[\"bnbusdt@aggTrade\",\"bnbusdt@bookTicker\",\"bnbusdt@forceOrder\"],\"id\":1}"; // 订阅消息的 JSON 负载
    send_frame(g_public_ws_client, subscribe_aggtrade_payload, strlen(subscribe_aggtrade_payload), 0x1); // 发送订阅帧 (opcode 0x1 表示文本帧)
    printf("[DEBUG] Network::init() - Public WebSocket aggTrade subscribe frame sent.\n"); // 打印调试信息

    // 连接用户数据流 WebSocket (如果 listenKey 成功获取)
    // 用于获取用户相关的订单、账户更新等信息
    if (!g_listen_key.empty()) { // 如果 listenKey 成功获取
        std::string user_host = "fstream.binance.com", user_path = "/ws/" + g_listen_key, user_port = "443"; // 用户 WebSocket 的主机、路径和端口
        printf("[DEBUG] Network::init() - Connecting user WebSocket to %s:%s%s\n", user_host.c_str(), user_port.c_str(), user_path.c_str());
    g_user_ws_client.fd_ = connect_tcp(user_host, user_port); // 建立用户 WebSocket 的 TCP 连接
        if (g_user_ws_client.fd_ < 0) { // 如果 TCP 连接失败
            std::cerr << "[WARNING] User WebSocket TCP connection failed, continuing without it." << std::endl; // 打印警告信息
            printf("[LOG] Event Type: 3, Error: User WebSocket TCP connection failed!\n"); // 记录日志
            // 不退出，但用户数据流不可用，程序会继续运行
        }
     else { // 如果 TCP 连接成功
            printf("[DEBUG] Network::init() - User WebSocket TCP connected. FD: %d\n", g_user_ws_client.fd_); // 打印调试信息
            if (!perform_ssl_handshake(g_user_ws_client, g_user_ws_client.fd_)) { // 执行用户 WebSocket 的 SSL 握手
                std::cerr << "[WARNING] User WebSocket SSL handshake failed, continuing without it." << std::endl; // 打印警告信息
                ERR_print_errors_fp(stderr); // 打印 OpenSSL 错误
                printf("[LOG] Event Type: 3, Error: User WebSocket SSL handshake failed!\n"); // 记录日志
            }
         else { // 如果 SSL 握手成功
            printf("[DEBUG] Network::init() - User WebSocket SSL handshake successful.\n"); // 打印调试信息
                if (!perform_ws_handshake(g_user_ws_client, user_host, user_path)) { // 执行用户 WebSocket 的协议握手
                    std::cerr << "[WARNING] User WebSocket handshake failed, continuing without it." << std::endl; // 打印警告信息
                    printf("[LOG] Event Type: 3, Error: User WebSocket handshake failed!\n"); // 记录日志
                }
             else { // 如果协议握手成功
                    printf("[DEBUG] Network::init() - User WebSocket handshake successful.\n"); // 打印调试信息
                }
            }
        }
    }
    
    // 设置非阻塞模式
    // 将套接字设置为非阻塞模式，以便在没有数据可读时不会阻塞程序
    fcntl(g_public_ws_client.fd_, F_SETFL, fcntl(g_public_ws_client.fd_, F_GETFL, 0) | O_NONBLOCK); // 设置公共 WebSocket 为非阻塞
    if (g_user_ws_client.fd_ != -1) fcntl(g_user_ws_client.fd_, F_SETFL, fcntl(g_user_ws_client.fd_, F_GETFL, 0) | O_NONBLOCK); // 设置用户 WebSocket 为非阻塞 (如果已连接)
    printf("[DEBUG] Network::init() - Sockets set to non-blocking mode.\n"); // 打印调试信息

    printf("[DEBUG] Network::init() - Initialization complete.\n"); // 打印初始化完成信息
}

/// -----------------------------------------------------------
// 终极解析器：同时处理 成交(aggTrade)、盘口(bookTicker)、爆仓(forceOrder)
// -----------------------------------------------------------
// 终极解析器：修复了“方向丢失”的 Bug
// 这个函数用于解析从 WebSocket 接收到的市场数据 JSON 消息，并将其转换为内部数据结构 (MarketFrame)。
// 它能够处理 aggTrade (聚合交易), bookTicker (最优买卖盘), forceOrder (强平订单) 三种类型的消息。
// 修复了一个之前可能存在的“方向丢失”的 bug。
static ParsedMarketData parse_market_data_json_no_alloc(const char* msg, size_t len) {
    MarketFrame frame = {}; // 初始化一个 MarketFrame 结构体，用于存储解析后的市场数据
    frame.timestamp = Common::get_now_ns(); // 1. 保底时间：先记录当前系统时间，作为数据的基准时间
    bool valid_data = false; // 标记是否成功解析到有效数据

    // 查找事件类型 'e' 字段，用于区分不同的消息类型
    const char* e_start = strstr(msg, "\"e\":\""); // 查找 "e":" 字符串
    if (e_start) { // 如果找到了 'e' 字段
        e_start += 5; // 跳过 "e":"，指向实际的事件类型字符串开始位置

        // === 1. aggTrade (成交) ===
        // 处理聚合交易消息，记录价格、数量和方向
        if (strncmp(e_start, "aggTrade", 8) == 0) { // 如果事件类型是 "aggTrade"
            frame.type = 1; // 设置帧类型为 1 (代表 aggTrade)
            
            // 解析 P (价) 和 Q (量)
            const char* p = strstr(msg, "\"p\":\""); // 查找 "p":" 字符串，获取价格
            if (p) frame.price = strtod(p + 5, nullptr); // 将价格字符串转换为 double 类型
            const char* q = strstr(msg, "\"q\":\""); // 查找 "q":" 字符串，获取数量
            if (q) frame.quantity = strtod(q + 5, nullptr); // 将数量字符串转换为 double 类型
            
            // 【关键修复】解析方向 ("m": true/false)
            // 币安逻辑：m=true 代表 Maker 是买方 -> 意味着是主动卖出 -> Side = -1
            // m=false 代表 Maker 是卖方 -> 意味着是主动买入 -> Side = 1
            const char* m = strstr(msg, "\"m\":"); // 查找 "m": 字段，表示是否为卖方撮合 (maker is buyer)
            if (m) { // 如果找到了 'm' 字段
                // 检查 m 后面是 true 还是 false
                if (strncmp(m + 4, "true", 4) == 0) { // 如果是 "true"
                    frame.side = -1; // 设置方向为 -1 (主动卖出/Short/Sell)
                } else { // 如果是 "false"
                    frame.side = 1;  // 设置方向为 1 (主动买入/Long/Buy)
                }
            }

            // 覆盖时间：使用消息中自带的时间戳，更精确
            const char* T = strstr(msg, "\"T\":"); // 查找 "T": 字段，获取交易时间戳 (毫秒)
            if (T) frame.timestamp = strtoull(T + 4, nullptr, 10) * 1000000ULL; // 将毫秒时间戳转换为纳秒
            
            valid_data = true; // 标记为有效数据
        }
        // === 2. forceOrder (爆仓) ===
        // 处理强平订单消息，记录价格、数量和方向
        else if (strncmp(e_start, "forceOrder", 10) == 0) { // 如果事件类型是 "forceOrder"
            frame.type = 3; // 设置帧类型为 3 (代表 forceOrder)
            const char* o_obj = strstr(msg, "\"o\":"); // 查找 "o": 字段，指向订单对象
            if (o_obj) { // 如果找到了订单对象
                const char* p = strstr(o_obj, "\"p\":\""); // 查找 "p":" 字符串，获取价格
                if (p) frame.price = strtod(p + 5, nullptr); // 将价格字符串转换为 double
                const char* q = strstr(o_obj, "\"q\":\""); // 查找 "q":" 字符串，获取数量
                if (q) frame.quantity = strtod(q + 5, nullptr); // 将数量字符串转换为 double
                
                // 解析方向: "S":"SELL" -> 多头爆仓 (卖出平仓) -> side = -1
                const char* S = strstr(o_obj, "\"S\":\""); // 查找 "S":" 字符串，获取交易方向 (BUY/SELL)
                if (S) { // 如果找到了 'S' 字段
                    if (strncmp(S + 5, "BUY", 3) == 0) frame.side = 1;       // 如果是 "BUY"，方向为 1 (买入)
                    else if (strncmp(S + 5, "SELL", 4) == 0) frame.side = -1; // 如果是 "SELL"，方向为 -1 (卖出)
                }
                // ... 时间解析略 ... (这里省略了时间解析，可以根据需要添加)
                valid_data = true; // 标记为有效数据
            }
        }
    } 
    // === 3. bookTicker (盘口) ===
    // 处理最优买卖盘消息，记录买一价、买一量、卖一价、卖一量
    else { // 如果没有 'e' 字段，则可能是 bookTicker 消息 (或其他未知消息)
        if (strstr(msg, "\"u\":")) { // bookTicker 消息通常包含 "u": (更新ID) 字段
            frame.type = 2; // 设置帧类型为 2 (代表 bookTicker)
            // 解析 b/B (买一价/买一量), a/A (卖一价/卖一量)
            const char* b = strstr(msg, "\"b\":\""); // 查找 "b":" 字符串，获取买一价
            if (b) frame.bid_p = strtod(b + 5, nullptr); // 将买一价字符串转换为 double
            const char* B = strstr(msg, "\"B\":\""); // 查找 "B":" 字符串，获取买一量 (挂单量)
            if (B) frame.bid_q = strtod(B + 5, nullptr); // 将买一量字符串转换为 double
            
            const char* a = strstr(msg, "\"a\":\""); // 查找 "a":" 字符串，获取卖一价
            if (a) frame.ask_p = strtod(a + 5, nullptr); // 将卖一价字符串转换为 double
            if (A) frame.ask_q = strtod(A + 5, nullptr); // 将卖一量字符串转换为 double

            valid_data = true; // 标记为有效数据
        }
    }

    // === 写入仓库 ===
    // 将解析后的市场数据写入共享内存环形缓冲区 (g_master_bridge->market_ring)
    if (valid_data && g_master_bridge) { // 如果数据有效且共享内存桥接存在
        // 获取当前写入索引 (relaxed memory order，因为只关心值)
        uint64_t idx = g_master_bridge->market_ring.write_index.load(std::memory_order_relaxed);
        // 计算在环形缓冲区中的实际位置
        int pos = idx & RING_BUFFER_MASK; 
        g_master_bridge->market_ring.frames[pos] = frame; // 将解析后的帧写入环形缓冲区
        // 更新写入索引 (release memory order，确保数据在索引更新前可见)
        g_master_bridge->market_ring.write_index.store(idx + 1, std::memory_order_release);
    }

    ParsedMarketData dummy = {}; // 创建一个空的 ParsedMarketData 结构体
    return dummy; // 只是为了骗过编译器，因为这个函数的主要目的是写入共享内存，而不是返回值
}
// 最终清洗版 run_event_loop (直接覆盖)
// ============================================================
// 核心事件循环函数，负责监听 WebSocket 连接上的数据，发送心跳包，并处理接收到的消息。
// 使用 epoll 进行高效的 I/O 多路复用。
void run_event_loop() {
    // 创建 epoll 实例
    int epoll_fd = epoll_create1(0); // 参数 0 适用于 Linux 2.6.27+，表示 flags 为 0
    if (epoll_fd == -1) { // 如果创建失败
        set_g_running_false("epoll_create1 failed"); // 设置程序退出标志
        return; // 返回
    }

    struct epoll_event ev; // epoll 事件结构体
    ev.events = EPOLLIN | EPOLLET; // 监听读事件 (EPOLLIN) 和边缘触发模式 (EPOLLET)
    ev.data.fd = g_public_ws_client.fd_; // 将公共 WebSocket 的文件描述符关联到事件
    // 将公共 WebSocket 添加到 epoll 实例中
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_public_ws_client.fd_, &ev);

    if (g_user_ws_client.fd_ != -1) { // 如果用户 WebSocket 客户端已连接
        ev.data.fd = g_user_ws_client.fd_; // 将用户 WebSocket 的文件描述符关联到事件
        // 将用户 WebSocket 添加到 epoll 实例中
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_user_ws_client.fd_, &ev);
    }

    struct epoll_event events[2]; // 用于存储 epoll_wait 返回的事件数组 (最多处理 2 个客户端)
    uint64_t last_ping_ns = 0; // 记录上次发送 Ping 帧的时间 (纳秒)

    while (g_running) { // 主事件循环，只要 g_running 为 true 就一直运行
        if (g_master_bridge) { // 如果共享内存桥接存在
            // 更新系统健康时间戳，表示程序仍然活跃
            g_master_bridge->system_health.store(Common::get_now_ns(), std::memory_order_release);
        }
        uint64_t current_ns = Common::get_now_ns(); // 获取当前时间 (纳秒)

        // 1. 发送心跳 PING (每10秒)
        // 维护 WebSocket 连接的活跃性，防止因长时间不活动而被服务器断开
        if (current_ns - last_ping_ns > 10 * 1000 * 1000 * 1000ULL) { // 如果距离上次发送 Ping 超过 10 秒
            std::string ping_payload = std::to_string(current_ns); // Ping 消息内容为当前时间戳
            send_frame(g_public_ws_client, ping_payload.c_str(), ping_payload.length(), 0x9); // 发送 Ping 帧 (opcode 0x9)
            last_ping_ns = current_ns; // 更新上次发送 Ping 的时间
        }

        // 等待 epoll 事件，最长等待 100 毫秒
        int nfds = epoll_wait(epoll_fd, events, 2, 100);
        if (nfds == -1 && errno != EINTR) continue; // 如果 epoll_wait 失败且不是被信号中断，则继续循环

        for (int i = 0; i < nfds; ++i) { // 遍历所有就绪的事件
            InternalWebSocketClient* current_client = nullptr; // 当前处理的 WebSocket 客户端指针
            // 判断是公共流还是用户流的事件
            if (events[i].data.fd == g_public_ws_client.fd_) current_client = &g_public_ws_client;
            else if (events[i].data.fd == g_user_ws_client.fd_) current_client = &g_user_ws_client;

            if (current_client) { // 如果找到了对应的客户端
                while (true) { // 循环读取所有可用的数据
                    // 计算这次应该写到哪里（接在上次残留数据后面）
                    size_t read_start_pos = current_client->offset_;
                    size_t max_read_len = InternalWebSocketClient::kBufferSize - read_start_pos;

                    // 从 SSL 连接中读取数据
                    int len = SSL_read(current_client->ssl_, current_client->buffer_ + read_start_pos, max_read_len);

                    if (len > 0) { // 如果成功读取到数据
                        size_t total_len = read_start_pos + len; // 总有效数据长度
                        unsigned char *ptr = (unsigned char *)current_client->buffer_;
                        size_t remaining = total_len;

                        while (remaining >= 2) { // 至少需要 2 字节来解析 WebSocket 帧头部
                            unsigned char opcode = ptr[0] & 0x0F; // 提取 Opcode (后 4 位)
                            unsigned char len_byte = ptr[1] & 0x7F; // 提取 Payload Len (后 7 位，不包含 Mask 位)
                            size_t header_len = 2; // 默认头部长度为 2 字节
                            size_t payload_len = len_byte; // 默认 payload 长度
                            size_t frame_len = 0; // The total length of the current frame

                            if (len_byte == 126) { // 如果 Payload Len 为 126，表示实际长度由接下来的 2 字节表示
                                if (remaining < 4) break; // 如果剩余字节不足，则跳出循环
                                payload_len = (ptr[2] << 8) | ptr[3]; // 从 ptr[2] 和 ptr[3] 中读取 16 位长度
                                header_len = 4; // 头部长度为 4 字节
                            } else if (len_byte == 127) { // 如果 Payload Len 为 127，表示实际长度由接下来的 8 字节表示 (64 位长度)
                                if (remaining < 10) break; // 确保有足够的字节读取完整头部 (1字节FIN/opcode + 1字节长度指示 + 8字节实际长度 = 10字节)
                                
                                // 正确地从 ptr[2] 到 ptr[9] 读取 8 字节的 64 位长度
                                payload_len = (
                                    ((uint64_t)ptr[2] << 56) |  // 最高有效字节
                                    ((uint64_t)ptr[3] << 48) |
                                    ((uint64_t)ptr[4] << 40) |
                                    ((uint64_t)ptr[5] << 32) |
                                    ((uint64_t)ptr[6] << 24) |
                                    ((uint64_t)ptr[7] << 16) |
                                    ((uint64_t)ptr[8] << 8)  |
                                    ((uint64_t)ptr[9])         // 最低有效字节
                                );
                                header_len = 10; // 64位长度的帧头部总长是 10 字节
                            }
                            frame_len = header_len + payload_len; // Calculate the total frame length

                            if (remaining < frame_len) break; // 如果剩余字节不足以读取整个帧，则跳出循环

                            // === 核心逻辑分流开始 ===
                            if (opcode == 0x9) { // 如果是 PING 帧
                                send_frame(*current_client, (const char *)ptr + header_len, payload_len, 0xA); // 回复 PONG 帧 (opcode 0xA)
                            } 
                            else if (opcode == 0x8) { // 如果是 CLOSE 帧
                                set_g_running_false("WebSocket Closed"); // 设置程序退出标志
                                break; // 跳出循环
                            }
                            else if (opcode == 0x1 || opcode == 0x2) { // 如果是文本帧 (0x1) 或二进制帧 (0x2)
                                
                                // 路径 A: 公共行情 (Market Data)
                                if (current_client->is_public_stream) { // 如果是公共行情流
                                    // 解析市场数据 JSON 消息
                                    ParsedMarketData data = parse_market_data_json_no_alloc((const char *)ptr + header_len, payload_len);
                                    Strategy::check_trigger(data.price); // 调用策略模块检查触发条件
                                }
                                // 路径 B: 用户私有流 (User Stream)
                                else { // 如果是用户私有流
                                    const char* msg_start = (const char*)ptr + header_len;
                                    if (strstr(msg_start, "\"e\":\"ORDER_TRADE_UPDATE\"")) { // 如果是订单交易更新事件
                                        const char* o_start = strstr(msg_start, "\"o\":"); // 查找 "\"o\":" 字段，指向订单对象
                                        if (o_start && strstr(o_start, "\"X\":\"FILLED\"")) { // 确保订单状态是 FILLED
                                
                                            // --- [状态机去重] ---
                                            int expected = 1; // 期望当前还是挂单等待状态
                                            // 尝试加锁：如果成功，说明 HTTP 那边还没处理
                                            if (g_master_bridge->account.strategy_status.compare_exchange_strong(expected, 2)) {
                                                // 1. 提取数据
                                                const char* L_ptr = strstr(o_start, "\"L\":\"");
                                                double fill_px = L_ptr ? strtod(L_ptr + 5, nullptr) : 0.0;
                                                const char* l_ptr = strstr(o_start, "\"l\":\"");
                                                double fill_qty = l_ptr ? strtod(l_ptr + 5, nullptr) : 0.0;
                                
                                                // 2. 更新内存
                                                g_master_bridge->account.last_fill_px.store(fill_px, std::memory_order_release);
                                                g_master_bridge->account.last_fill_qty.store(fill_qty, std::memory_order_release);
                                
                                                // 3. 核心触发
                                                Strategy::on_order_filled(fill_px, fill_qty);
                                                printf("[INFO] WebSocket 确认成交! Px:%.2f Qty:%.4f\n", fill_px, fill_qty);
                                            } else {
                                                // 如果状态不是 1，说明 HTTP 已经抢先处理并把状态改成了 2
                                                printf("[INFO] WebSocket 拦截到冗余成交信号。\n");
                                            }
                                            // --- [去重结束] ---
                                
                                            // !!! 警告：这里之后不应再有任何关于 on_order_filled 的调用 !!!
                                        }
                                    }
                                }
                            else { // 未知 Opcode
                                // printf("[LOG] Unknown WebSocket opcode: 0x%x\n", opcode); // 可以选择打印未知 Opcode 的日志
                            }
                            // === 核心逻辑分流结束 ===

                            ptr += frame_len; // 移动指针到下一个帧的开始
                            remaining -= frame_len; // 更新剩余字节数
                        }

                        // === 核心修复：搬运残余数据 ===
                        if (remaining > 0) {
                            // 把剩下的 remaining 字节搬到 buffer 最开头
                            memmove(current_client->buffer_, ptr, remaining);
                        }
                        // 记录下一次写入的偏移量
                        current_client->offset_ = remaining;

                    } else if (len <= 0) { // 如果 SSL_read 返回 0 或负数 (表示没有数据或发生错误)
                        int err = SSL_get_error(current_client->ssl_, len); // 获取 SSL 错误码
                        // 如果不是 WANT_READ 或 WANT_WRITE 错误，则表示发生了真正的 SSL 错误
                        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                            // 真正的错误
                            ERR_print_errors_fp(stderr); // 打印 OpenSSL 错误堆栈
                            set_g_running_false("SSL Error in read loop"); // 设置程序退出标志
                            break; // 跳出循环
                        }
                        // 只是暂时没数据，跳出 SSL_read 循环，等待下一次 epoll 事件
                        current_client->offset_ = remaining; // Store remaining data if any, although for WANT_READ it should be 0 unless there was prior partial data.
                        break; // 跳出循环
                    }
                }
            }
        }
    }

    close(epoll_fd); // 关闭 epoll 文件描述符
    // ... 清理资源 ... (这里省略了其他的资源清理工作，例如关闭套接字、释放 SSL_CTX 等)
}


// 获取交易对精度信息
// 通过 HTTP API 请求获取指定交易对的详细信息，例如最小下单量、价格精度等
long fetch_exchange_info(CURL *curl_handle, const std::string& symbol, std::string &response_buffer) {
    if (!curl_handle) return 0; // 如果 CURL 句柄无效，直接返回 0

    std::map<std::string, std::string> params; // 请求参数
    params["symbol"] = symbol; // 添加交易对符号参数

    std::string full_url = "https://fapi.binance.com/fapi/v1/exchangeInfo"; // 币安交易所信息 API 的基础 URL
    std::string query_string = ""; // 查询字符串
    
    // 对于 exchangeInfo，symbol 参数通常直接加在 URL 后面
    // 或者可以作为参数传递，让 perform_binance_request 处理
    // 这里我们直接构建完整的 URL
    query_string += url_encode(curl_handle, "symbol") + "=" + url_encode(curl_handle, symbol); // 对 symbol 参数进行 URL 编码

    if (!query_string.empty()) { // 如果查询字符串不为空
        full_url += "?" + query_string; // 将查询字符串添加到 URL 后面
    }

    // 设置接收数据的回调
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback); // 设置数据写入回调函数
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &response_buffer); // 设置写入数据的目标缓冲区
    curl_easy_setopt(curl_handle, CURLOPT_URL, full_url.c_str()); // 设置请求 URL
    curl_easy_setopt(curl_handle, CURLOPT_HTTPGET, 1L); // 明确这是一个 GET 请求

    // exchangeInfo API 是公开的，不需要 API Key 和签名
    struct curl_slist *headers = NULL; // 清空 headers，因为不需要自定义 HTTP 头

    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers); // 清空可能存在的旧 headers

    CURLcode res = curl_easy_perform(curl_handle); // 执行 CURL 请求
    long http_code = 0; // 存储 HTTP 响应码
    curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code); // 获取 HTTP 响应码

    curl_slist_free_all(headers); // 释放 HTTP 头列表的内存 (这里 headers 为 NULL，调用是安全的)

    if (res != CURLE_OK) { // 如果 CURL 请求失败
        response_buffer = "CURL Error: " + std::string(curl_easy_strerror(res)); // 记录 CURL 错误信息
        printf("[DEBUG] fetch_exchange_info - CURL Error: %s\n", response_buffer.c_str()); // 打印调试信息
        return -1; // 返回 -1 表示请求失败
    }
    printf("[DEBUG] fetch_exchange_info - HTTP Code: %ld\n", http_code); // 打印 HTTP 响应码
    printf("[DEBUG] fetch_exchange_info - Response Buffer: %s\n", response_buffer.c_str()); // 打印响应内容
    return http_code; // 返回 HTTP 响应码
}

} // namespace Network
