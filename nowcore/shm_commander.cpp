#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>     // For sleep
#include <sys/mman.h>   // For shm_open, mmap, munmap
#include <fcntl.h>      // For O_RDWR, O_CREAT
#include <cstring>      // For strcpy, memset
#include <array>        // For std::array
#include <cmath>        // For std::pow, std::round
#include <iomanip>      // For std::fixed, std::setprecision

// 包含共享内存的结构定义
#include "Master_Logic_Bridge.h" 

// 定义共享内存名称
#define SHM_NAME "/nowcore_bridge"

// 辅助函数：打印共享内存状态
void print_shm_status(GenericShmStruct* shm_ptr) {
    std::cout << "C++ 状态: " << shm_ptr->cpp_status.load() << std::endl;
    std::cout << "指令版本: " << shm_ptr->cmd_version.load() << std::endl;
    std::cout << "可用 USDT 余额: " << shm_ptr->available_balance_usdt.load() << std::endl;
    std::cout << "可用 BNB 余额: " << shm_ptr->available_balance_bnb.load() << std::endl;
    std::cout << "可用 BTC 余额: " << shm_ptr->available_balance_btc.load() << std::endl;
    std::cout << "价格精度 (price_precision): " << shm_ptr->price_precision.load() << std::endl;
    std::cout << "数量精度 (quantity_precision): " << shm_ptr->quantity_precision.load() << std::endl;
    std::cout << "当前持仓量: " << shm_ptr->current_position_amount.load() << std::endl;
    std::cout << "持仓均价: " << shm_ptr->current_position_entry_price.load() << std::endl;
    std::cout << "未实现盈亏: " << shm_ptr->current_position_pnl.load() << std::endl;
    std::cout << "------------------------\n";
}

int main() {
    // 1. 打开共享内存
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd == -1) {
        std::cerr << "错误: 无法打开共享内存. 请确保sentinel_v1已经运行并创建了共享内存." << std::endl;
        return 1;
    }

    // 2. 映射共享内存到进程地址空间
    GenericShmStruct* shm_ptr = static_cast<GenericShmStruct*>(mmap(0, sizeof(GenericShmStruct), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
    if (shm_ptr == MAP_FAILED) {
        std::cerr << "错误: 映射共享共享内存失败." << std::endl;
        close(shm_fd);
        return 1;
    }

    std::cout << "成功连接到共享内存: " << SHM_NAME << std::endl;
    std::cout << "--- 共享内存初始状态 ---\n";
    print_shm_status(shm_ptr);

    // 等待精度信息被 sentinel_v1 初始化
    std::cout << "\n--- 等待 sentinel_v1 初始化精度信息 (最多 10 秒) ---\n";
    int wait_time = 0;
    while ((shm_ptr->price_precision.load() == 0 || shm_ptr->quantity_precision.load() == 0) && wait_time < 10) {
        std::cout << "等待中... 价格精度: " << shm_ptr->price_precision.load() 
                  << ", 数量精度: " << shm_ptr->quantity_precision.load() << std::endl;
        sleep(1);
        wait_time++;
    }

    if (shm_ptr->price_precision.load() == 0 || shm_ptr->quantity_precision.load() == 0) {
        std::cerr << "[错误] sentinel_v1 未能成功初始化精度信息. 停止测试." << std::endl;
        munmap(shm_ptr, sizeof(GenericShmStruct));
        close(shm_fd);
        return 1;
    }
    std::cout << "精度信息已就绪！ 价格精度: " << shm_ptr->price_precision.load() 
              << ", 数量精度: " << shm_ptr->quantity_precision.load() << std::endl;
    print_shm_status(shm_ptr);

    // --- 模拟 Python 大脑发送指令 ---    // 场景 1: 发送一个买入限价单 (小额) - 基于市场价格和 15 USDT 预算
    std::cout << "\n--- 场景 1: 发送买入限价单指令 (基于市场价格和 15 USDT 预算) ---\n";
    double current_market_price = shm_ptr->market_price.load(std::memory_order_relaxed);
    if (current_market_price == 0.0) {
        std::cerr << "[ERROR] 共享内存中的市场价格为 0，无法计算买入价格和数量。请等待行情数据更新。\n";
        return 1; // 退出 shm_commander
    }

    double budget_usdt = 15.0; // 您的预算 15 USDT
    double buy_price = current_market_price * 0.99; // 设定一个低于市价 1% 的价格，更容易挂上但不容易立即成交
    double raw_buy_quantity = budget_usdt / buy_price;
    double buy_quantity; // 声明 buy_quantity

    // 获取并应用数量精度 (从共享内存读取)
    int quantity_precision = shm_ptr->quantity_precision.load(std::memory_order_relaxed);
    if (quantity_precision <= 0) { // 确保精度已初始化
        std::cerr << "[ERROR] 共享内存中的数量精度为 0 或负数，无法计算买入数量。请等待精度信息更新。\n";
        double default_step_size = 0.001; // 默认步长
        buy_quantity = std::round(raw_buy_quantity / default_step_size) * default_step_size;
        std::cerr << "[WARNING] 由于精度无效，使用默认步长进行数量计算。\n";
        if (buy_quantity <= 0) { // 防止无效数量
            std::cerr << "[ERROR] 使用默认步长计算出的买入数量为零或负数。退出。\n";
            return 1;
        }
    } else {
        double step_size = std::pow(10, -quantity_precision);
        buy_quantity = std::round(raw_buy_quantity / step_size) * step_size;
        if (buy_quantity <= 0) { // 防止无效数量
            std::cerr << "[ERROR] 计算出的买入数量为零或负数。退出。\n";
            return 1;
        }
    }

    std::cout << "基于市场价格 (" << std::fixed << std::setprecision(2) << current_market_price << ") 和 "
              << budget_usdt << " USDT 预算，计算买入价格: " << std::fixed << std::setprecision(2) << buy_price
              << ", 数量: " << std::fixed << std::setprecision(4) << buy_quantity << std::endl;

    shm_ptr->strategy_status.store(2); // 2=开火(挂单)
    std::strncpy(shm_ptr->symbol.data(), "BNBUSDT", shm_ptr->symbol.size());
    shm_ptr->symbol.data()[shm_ptr->symbol.size() - 1] = '\0';
    shm_ptr->target_price.store(buy_price);
    shm_ptr->target_amount.store(buy_quantity);
    shm_ptr->hard_stop_price.store(buy_price * (1 - 0.008)); // 硬止损价格 (例如，低于买入价 0.8%)
    shm_ptr->ratchet_active_gap.store(buy_price * (1 + 0.005)); // 止盈触发价格 (例如，高于买入价 0.5%)
    shm_ptr->ratchet_callback.store(0.002); // 棘轮回撤比例 0.2%
    shm_ptr->cmd_version.fetch_add(1);
    std::cout << "已发送买入限价单指令. 新版本号: " << shm_ptr->cmd_version.load() << std::endl;
    sleep(5); // 延长等待时间以确保订单处理
    std::cout << "--- 场景 1 结束状态 ---\n";
    print_shm_status(shm_ptr);

    // 场景 2: 等待买入成交 (cpp_status 变为 2 - 持仓中)
    std::cout << "\n--- 场景 2: 等待买入成交 (cpp_status == 2) ---\n";
    wait_time = 0;
    while (shm_ptr->cpp_status.load() != 2 && wait_time < 30) { // 最多等待 30 秒
        std::cout << "等待买入成交中... 当前 C++ 状态: " << shm_ptr->cpp_status.load() << std::endl;
        sleep(1);
        wait_time++;
    }
    if (shm_ptr->cpp_status.load() == 2) {
        std::cout << "买入订单已成交！当前持仓量: " << shm_ptr->current_position_amount.load() << std::endl;
    } else {
        std::cerr << "[警告] 买入订单未能在规定时间内成交或状态不正确. 继续测试，但请检查." << std::endl;
    }
    std::cout << "--- 场景 2 结束状态 ---\n";
    print_shm_status(shm_ptr);
    sleep(2); // 稍微等待

    // 场景 3: 发送一个卖出限价单 (略高于买入价)
    std::cout << "\n--- 场景 3: 发送卖出限价单指令 (0.001 BNB @ 250.5) ---\n";
    shm_ptr->strategy_status.store(2); // 再次开火
    shm_ptr->target_price.store(250.5); // 卖出价格略高于买入
    shm_ptr->target_amount.store(0.001); // 卖出数量
    shm_ptr->cmd_version.fetch_add(1); 
    std::cout << "已发送卖出限价单指令. 新版本号: " << shm_ptr->cmd_version.load() << std::endl;
    sleep(5); // 延长等待时间
    std::cout << "--- 场景 3 结束状态 ---\n";
    print_shm_status(shm_ptr);

    // 场景 4: 等待卖出成交 (cpp_status 变为 0 - 空闲)
    std::cout << "\n--- 场景 4: 等待卖出成交 (cpp_status == 0) ---\n";
    wait_time = 0;
    while (shm_ptr->cpp_status.load() != 0 && wait_time < 30) { // 最多等待 30 秒
        std::cout << "等待卖出成交中... 当前 C++ 状态: " << shm_ptr->cpp_status.load() << std::endl;
        sleep(1);
        wait_time++;
    }
    if (shm_ptr->cpp_status.load() == 0) {
        std::cout << "卖出订单已成交！当前持仓量: " << shm_ptr->current_position_amount.load() << std::endl;
    } else {
        std::cerr << "[警告] 卖出订单未能在规定时间内成交或状态不正确. 请检查." << std::endl;
    }
    std::cout << "--- 场景 4 结束状态 ---\n";
    print_shm_status(shm_ptr);
    sleep(2); // 稍微等待

    // 场景 5: 发送紧急撤单指令 (status=99) - 如果还有挂单，确保撤销
    std::cout << "\n--- 场景 5: 发送紧急撤单指令 (status=99) ---\n";
    shm_ptr->strategy_status.store(99); 
    shm_ptr->cmd_version.fetch_add(1);
    std::cout << "已发送紧急撤单指令. 新版本号: " << shm_ptr->cmd_version.load() << std::endl;
    sleep(3);
    std::cout << "--- 场景 5 结束状态 ---\n";
    print_shm_status(shm_ptr);

    // 场景 6: 将状态设置为空闲 (Status=0)
    std::cout << "\n--- 场景 6: 设置状态为空闲 (status=0) ---\n";
    shm_ptr->strategy_status.store(0); 
    shm_ptr->cmd_version.fetch_add(1);
    std::cout << "已发送空闲指令. 新版本号: " << shm_ptr->cmd_version.load() << std::endl;
    sleep(3);
    std::cout << "--- 场景 6 结束状态 ---\n";
    print_shm_status(shm_ptr);

    // 释放共享内存映射
    munmap(shm_ptr, sizeof(GenericShmStruct));
    close(shm_fd);

    return 0;
}