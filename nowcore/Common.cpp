#include "Common.h"

// 在 Common.cpp 中正式定义这些全局变量
std::atomic<uint32_t> g_system_status = 0;
std::atomic<bool> g_running = true;
