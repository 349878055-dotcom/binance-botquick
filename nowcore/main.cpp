#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <sys/mman.h>   // 内存锁定
#include <sys/resource.h> // 进程优先级
#include <fcntl.h>      // shm_open
#include <unistd.h>     // ftruncate
#include <cstring>      // strerror
#include "Network.h"
#include "Master_Logic_Bridge.h"
#include "Executor.h"  // 执行器
#include <chrono>
// --- [新加入的时钟巡检函数] ---
void check_clock_sync() {
    std::cout << "[System] 正在检查物理时钟对齐状态..." << std::endl;
    // 检查 chrony 的同步状态。如果 Leap status 是 Normal，说明时钟已同步
    int sync_status = std::system("chronyc tracking | grep -q 'Leap status     : Normal'");
    
    if (sync_status != 0) {
        std::cerr << "[FATAL] 时钟未同步！HFT 生存体拒绝在不确定的时间维度下工作。" << std::endl;
        std::cerr << "[Advice] 请运行: sudo apt install chrony && sudo systemctl start chrony" << std::endl;
        exit(1); // 强制终止，防止脏数据下单
    }
    std::cout << "[System] 物理时钟已锁定 (Chrony: Normal)." << std::endl;
}


GenericShmStruct* g_master_bridge = nullptr; // 定义 g_master_bridge

// --- [原 SanityCheck.h 的内容] ---
// 编译时检查：确保结构体大小符合预期 (64字节对齐)
static_assert(sizeof(GenericShmStruct) % 64 == 0, "FATAL: ShmStruct not 64-byte aligned!");

// --- [原 SystemUtils.cpp 的内容] ---
void optimize_system() {
    // 1. 锁定内存 (防止被交换到硬盘)
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0) {
        std::cout << "[System] 内存锁定成功 (mlockall)." << std::endl;
    } else {
        std::cerr << "[WARNING] 内存锁定失败: " << strerror(errno) << std::endl;
    }

    // 2. 提高进程优先级 (需要 sudo)
    // if (setpriority(PRIO_PROCESS, 0, -20) == 0) {
    //     std::cout << "[System] 进程优先级已设为最高 (-20)." << std::endl;
    // } else {
    //     std::cerr << "[WARNING] 无法设置高优先级 (可能需要 sudo)." << std::endl;
    }


// 全局运行标志
extern std::atomic<bool> g_running;

void signal_handler(int signum) {
    std::cout << "\n[System] 捕获信号 " << signum << "，准备退出..." << std::endl;
    g_running = false;
    Network::set_g_running_false("Signal received");
}

int main() {
    // 1. 注册退出信号
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 2. 系统优化 (合并进来了)
    optimize_system();

    // 3. 初始化共享内存
    const char* shm_name = "/nowcore_bridge";
    int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        std::cerr << "[FATAL] 无法创建共享内存" << std::endl;
        return 1;
    }
    ftruncate(shm_fd, sizeof(GenericShmStruct));
    void* ptr = mmap(0, sizeof(GenericShmStruct), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (ptr == MAP_FAILED) {
        std::cerr << "[FATAL] mmap 失败" << std::endl;
        return 1;
    }

    // 4. 绑定全局指针
    g_master_bridge = static_cast<GenericShmStruct*>(ptr);

    // 【新增逻辑】强制初始化共享内存的关键字段
    // 无论是首次创建还是崩溃重启，都确保索引和数据处于已知安全状态
    // 尤其重要的是，清零环形缓冲区的读写索引，以防止残留的旧值导致逻辑错误
    g_master_bridge->market_ring.write_index.store(0, std::memory_order_release);
    g_master_bridge->command_ring.write_idx.store(0, std::memory_order_release);
    g_master_bridge->command_ring.read_idx.store(0, std::memory_order_release);
    g_master_bridge->account_feed.write_idx.store(0, std::memory_order_release);
    g_master_bridge->account_feed.read_idx.store(0, std::memory_order_release);

    // 清零账户快照数据
    g_master_bridge->account_feed.usdt_balance.store(0.0, std::memory_order_release);
    g_master_bridge->account_feed.bnb_balance.store(0.0, std::memory_order_release);
    g_master_bridge->account_feed.position_amt.store(0.0, std::memory_order_release);
    g_master_bridge->account_feed.avg_price.store(0.0, std::memory_order_release);

    // 初始化其他系统状态
    g_master_bridge->cpp_alive.store(true, std::memory_order_release); // 1 = Alive
    g_master_bridge->py_alive.store(false, std::memory_order_release); // Python 启动后会设置为 true
    g_master_bridge->system_health.store(Common::get_now_ns(), std::memory_order_release);
    g_master_bridge->strategy_status.store(0, std::memory_order_release); // 初始策略状态

    // 精度信息会在 Network::init() 中调用 Executor::fetch_and_set_precision 重新获取和设置，所以这里无需额外清零。

    std::cout << "[System] 共享内存已就绪并初始化: " << shm_name << std::endl;

    // 5. 初始化各模块
    std::string api_key = "YXM98FXxAmyzx0OFJfzn7QLxsMRQsEj5TNLo7q4IozaKj51cyANGvQMoKxcHn8zJ"; // 从云端文件获取
    std::string api_secret = "utlwVk5r8fsqEeMZKjyI24AzVDTjBnV9bzxmvOZmT9vyEKeHefWi5hWQEi0p8qWs"; // 从云端文件获取
    
    Executor::init(api_key, api_secret);
    Network::init();

    // 8. 获取账户信息并初始化共享内存
    std::string trading_symbol = "BNBUSDT"; // 示例交易对，可根据实际需求调整
    Executor::fetch_account_info(trading_symbol, g_master_bridge);

    // 9. 获取并设置交易对精度信息
    Executor::fetch_and_set_precision(trading_symbol, g_master_bridge);
    
    // BlackBox logger(1000, 1000); // 日志系统已移除

    // 6. 启动主循环 (Network)
    std::cout << "[System] 系统启动完成，进入事件循环..." << std::endl;
    Network::run_event_loop();

    // 7. 退出清理
    munmap(ptr, sizeof(GenericShmStruct));
    close(shm_fd);
    shm_unlink(shm_name);
    std::cout << "[System] 安全退出." << std::endl;
    return 0;
}