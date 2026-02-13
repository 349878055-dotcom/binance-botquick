#include <iostream>
#include <fstream>
#include <cstdint>
#include <chrono>
#include <iomanip>

// 从 Common.h 复制 BlackBoxFrame 结构
// 确保这个结构与您程序中实际使用的 BlackBoxFrame 严格一致
struct BlackBoxFrame {
    uint64_t t_local;      // 本地接收时间 (偏移 0)
    uint64_t t_exch;       // 交易所时间 (偏移 8)
    double price;          // 价格 (偏移 16)
    double quantity;       // 成交量 (偏移 24)
    char event_type;       // 事件类型 (偏移 32)
    char padding[31];      // 补齐到 64 字节
};

int main() {
    std::string filename = "/home/ubuntu/nowcore_run/flow.bin";
    std::ifstream file(filename, std::ios::binary);

    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << filename << std::endl;
        return 1;
    }

    BlackBoxFrame frame;
    // size_t frame_size = sizeof(BlackBoxFrame); // 旧的 sizeof 可能会导致错误
    size_t frame_size = 64; // 显式指定为 64 字节
    std::cout << "Reading flow.bin (Frame size: " << frame_size << " bytes):\n";
    std::cout << "------------------------------------------------------\n";

    while (file.read(reinterpret_cast<char*>(&frame), frame_size)) {
        // 将本地接收纳秒时间戳转换为可读的日期时间格式
        std::chrono::system_clock::time_point tp_local(std::chrono::nanoseconds(frame.t_local));
        std::time_t time_local = std::chrono::system_clock::to_time_t(tp_local);
        char mbstr_local[100];
        if (std::strftime(mbstr_local, sizeof(mbstr_local), "%Y-%m-%d %H:%M:%S", std::localtime(&time_local))) {
            std::cout << "Local Timestamp (UTC):    " << mbstr_local << "." << std::setfill('0') << std::setw(9) << (frame.t_local % 1000000000ULL) << " ns\\n";
        } else {
            std::cout << "Local Timestamp (ns):     " << frame.t_local << "\\n";
        }

        // 将交易所纳秒时间戳转换为可读的日期时间格式 (如果 t_exch 也是纳秒)
        // 注意：如果 t_exch 是毫秒或微秒，这里需要调整转换逻辑
        std::chrono::system_clock::time_point tp_exch(std::chrono::nanoseconds(frame.t_exch));
        std::time_t time_exch = std::chrono::system_clock::to_time_t(tp_exch);
        char mbstr_exch[100];
        if (std::strftime(mbstr_exch, sizeof(mbstr_exch), "%Y-%m-%d %H:%M:%S", std::localtime(&time_exch))) {
            std::cout << "Exchange Timestamp (UTC): " << mbstr_exch << "." << std::setfill('0') << std::setw(9) << (frame.t_exch % 1000000000ULL) << " ns\\n";
        } else {
            std::cout << "Exchange Timestamp (ns):  " << frame.t_exch << "\\n";
        }
        
        std::cout << "  Price:              " << std::fixed << std::setprecision(8) << frame.price << "\\n";
        std::cout << "  Quantity:           " << std::fixed << std::setprecision(8) << frame.quantity << "\\n";
        std::cout << "  Event Type:         '" << frame.event_type << "'\\n";
        std::cout << "------------------------------------------------------\\n";
    }

    if (file.eof()) {
        std::cout << "End of file reached.\n";
    } else if (file.fail()) {
        std::cerr << "Error reading file, possibly truncated frame.\n";
    }

    file.close();
    return 0;
}