# 高频插针伏击系统架构说明书 (Sentinel-V1)

## 1. 核心战术思想：逆向捡漏 (Contrarian Liquidity Capture)

本系统不追随趋势，而是利用大资金（土匪）暴力砸盘引发的多/空头爆仓连锁反应，在流动性真空的“针尖”位置进行预埋伏击。

*   **盈利点：** 捕捉价格偏离均值后的“物理回弹”。
*   **非对称优势：** 不拼网速，拼预判深度，利用限价单（Maker）提前到场。

## 2. 双脑隔离架构 (Dual-Brain Architecture)

系统分为战略大脑 (Python) 与 战术脊髓 (C++ )，通过共享内存通讯。

### A. 战略大脑 (Python) - 慢思考 (ms 级)

*   **系统角色定位身份：** 系统的“参谋部”与“观察哨”。
*   **核心任务：** 吞噬海量公有数据流，计算“伏击坐标”与“风险评分”，并将决策写入共享内存。
*   **运行特征：** 异步、宽容延迟（ms级）、低优先级（Nice值高）。它不负责开枪，只负责告诉 C++ 往哪打。
*   **产出：** 每秒更新内存中的“黑板”指令（Target Price & Status）。

### B. 战术脊髓 (C++ ) - 快反射 (μs 级)

*   **职责：** 状态对齐，物理执行。
*   **核心逻辑：** 永远以最新的“黑板”意志为终极目标，但执行过程中保持原子性。
*   **微秒级监控：** 当 Python 进入 1 秒的休眠时，C++ 核心并不会休眠。它会像一个“守门员”一样，持续监控 Python 通过共享内存传递过来的“纸条”信息。
    *   **风险等级阈值:** 如果纸条上的 `Risk_Level`（例如，由 Python 计算出的风险分数）超过 90（或其他预设的紧急阈值），C++ 会立刻进入微秒级的监控状态。
    *   **自主决策:** 在这种紧急状态下，C++ 不再等待 Python 的下一轮指令。它将自主地实时盯住成交流，一旦识别到“点火”（即插针爆发的核心信号，例如价格的瞬间剧烈变动、大单击穿流动性等），它将立即触发交易指令（“开火”），以确保最快的响应速度。

## 3. 数据吞噬与清洗 (Data Ingestion)

### 3.1 订阅的 5 个公有流

Python 进程需开启 WebSocket (推荐使用 `aiohttp` 或 `websockets`) 订阅以下数据：

*   **Diff Depth (增量深度)：** 只订阅 Level 5 或 Level 10，关注买一/卖一的厚度变化。
    *   **回溯窗口：** 实时（当前时刻）
    *   **理由：** 插针是瞬间击穿，你只需要知道**“此时此刻”**买卖单上还有多少钱。
    *   **Python 代码示例 (使用 `ccxt.pro` 监控深度):**
        ```python
        import ccxt.pro as ccxt
        import asyncio

        async def monitor_depth():
            # 初始化交易所 (以币安为例)
            exchange = ccxt.binance({'options': {'defaultType': 'future'}})
            symbol = 'BTC/USDT'

            print(f"正在监控 {symbol} 的城墙厚度...")

            while True:
                try:
                    # 1. 获取实时深度 (Order Book)
                    orderbook = await exchange.watch_order_book(symbol)
                    
                    # 2. 局部锁定：买一 (Bid) 和 卖一 (Ask)
                    best_bid_price = orderbook['bids'][0][0] # 买一价
                    best_bid_amount = orderbook['bids'][0][1] # 买一量
                    
                    best_ask_price = orderbook['asks'][0][0] # 卖一价
                    best_ask_amount = orderbook['asks'][0][1] # 卖一量

                    # 3. 计算“城墙”的物理价值 (USDT)
                    bid_wall_value = best_bid_price * best_bid_amount
                    ask_wall_value = best_ask_price * best_ask_amount

                    # 4. 打印情报
                    print(f"--- 实时情报 ---")
                    print(f"【买方防御】价格: {best_bid_price} | 厚度: ${bid_wall_value:.2f}")
                    print(f"【卖方防御】价格: {best_ask_price} | 厚度: ${ask_wall_value:.2f}")
                    
                    # 如果厚度低于你的阈值 (比如 1000 USDT)，发出预警
                    if bid_wall_value < 1000:
                        print("⚠️ 警告：买方城墙极薄！土匪一脚就能踢破！")

                except Exception as e:
                    print(f"连接中断: {e}")
                    break

            await exchange.close()

        # 启动探测器
        asyncio.run(monitor_depth())
        ```
*   **AggTrade (归集交易)：** 流量最大，需特殊处理。
    *   **回溯窗口：** 过去 1 分钟
    *   **理由：** 你要看的是“密集程度”。单发子弹不重要，1 分钟内连发 10 颗子弹才叫点火。
*   **Liquidation (强平订单)：** 爆仓流，插针的核心燃料。
    *   **回溯窗口：** 过去 5 分钟
    *   **理由：** 爆仓有连锁反应。我们要看过去 5 分钟爆了多少，来判断插针是否已经到了强弩之末。
*   **Mark Price (标记价格)：** 用于计算费率和偏离度。
*   **Funding Rate / Open Interest (资金费率 / 期货持仓量)：**
    *   **回溯窗口（资金费率）：** 过去 24 小时
    *   **理由：** 费率是市场的“体温”，变动很慢。看一天的趋势，就能知道哪一边的赌徒最心虚。
    *   **回溯窗口（期货持仓量）：** 过去 1 小时
    *   **理由：** 持仓量是“火药量”。我们要看这 1 小时内火药是变多了还是变少了。

### 3.2 性能优化：针对 AggTrade 的“外科手术”

为了防止 CPU 被海量小单淹没，严禁对每一条 AggTrade 进行完整的 JSON 解析。

*   **字符串预过滤：** 在 `json.loads` 之前，先判断字符串长度或特定关键词。
    *   **逻辑：** 如果单笔金额 < $10,000 (需根据币种换算)，直接丢弃。
    *   **实现：** 使用正则或字符串切片快速剔除噪音。
*   **解析器替换：** 必须使用 `orjson` 代替标准 `json` 库（速度快 6-10 倍）。

### 3.3 共有流（Public Stream）：为什么要分开拿？

我们把“行情数据”比作 “路况信息”。

**Python 的任务：看地图（负责谋略）**
它要拿的数据：海量数据。包括 5 档深度（Depth）、成千上万条成交（AggTrade）、爆仓信息（Liquidation）。

为什么它拿？ 因为这些数据量太大，逻辑太复杂（要算 ATR、要评分）。交给 C++ 处理会把它累死，影响下单速度。所以 Python 自己拉一根网线，用 aiohttp 慢慢吞、慢慢算。

**结论：** 是的，99% 的分析用数据，都是 Python 自己去网上下载、解析、计算的。

**C++ 的任务：看仪表盘（负责保命）**
它要拿的数据：极少数据。它只关心一个数字——“最新成交价”。

为什么它也要拿？

还记得我们的 “棘轮止盈” 吗？（价格涨了就上移止损，跌了就跑）。

这个动作必须在 **微秒级** 完成。

如果 C++ 等着 Python 下载数据 -> 解析 -> 传给共享内存 -> C++ 再读，这中间可能延迟了 10-20 毫秒。

**后果：** 在插针行情里，10 毫秒价格可能已经跌了 50 美金了。你本来赚的，结果亏着出来了。

**结论：** C++ 必须自己连一根极细的网线，只听“最新价”，为了在这个数字跳动的一瞬间，直接扣动扳机。


## 4. 核心策略逻辑：插针预警模型

Python 内部维护一个无限循环，每秒进行一次**“战局评估”**。

### `brain.py` 的决策流程 (`每一秒`定时任务)

| 动作序号 | Python 的扫视逻辑            | 动作目的           |
| :------- | :--------------------------- | :----------------- |
| 1.       | 倒旧货                       | 保持数据新鲜（只看最近窗口）。 |
| 2.       | 算总账                       | 计算当前的能量值。 |
| 3.       | 下结论                       | 综合 5 个篮子，给出一个 0-100 的风险分。 |
| 4.       | 传纸条                       | 把这个分数和目标价写进“共享内存”。 |

### 4.1 阶段识别与评分 (Risk Scoring)

根据各项指标，计算当前市场的 `Risk_Score` (0-100)：

| 阶段       | 特征描述                       | 动作 (写入共享内存)                                    |
| :--------- | :----------------------------- | :------------------------------------------------------- |
| 0. 平静期  | 深度厚，无大单，无爆仓。       | `StrategyStatus = 0` (休息), `TargetPrice = 0`           |
| 1. 前兆期  | 深度突然变薄 (<300k USDT)，持仓量高企。 | `StrategyStatus = 1` (预警), 计算初步伏击点 (如均价-1%) |
| 2. 触发期  | 出现连续大卖单，价格开始急跌。 | `StrategyStatus = 1` (准备), 更新伏击点 (如均价-3%)     |
| 3. 爆发期  | 爆仓流指数级激增，价格瞬间击穿支撑。 | `StrategyStatus = 2` (开火), 写入最终伏击坐标           |
| 4. 连续崩盘 | 爆仓流无衰减迹象，深度彻底消失。 | `StrategyStatus = 99` (ABORT/紧急撤单)                   |

### 4.2 伏击坐标计算公式 (简化版)

*   **逻辑：** `Target_Price = EMA(Price, 1min) - (ATR * K)`
    *   利用 ATR (平均真实波幅) 乘以一个激进系数 $K$ (如 3.5)，寻找统计学上的极端偏离点。
*   **修正：** 如果检测到爆仓量极大，将 $K$ 值动态调大（接得更低）。

## 5. 订单生命周期管理 (Order State Machine)

### 关键规则：

*   **指令覆盖性：** C++ 永远只看黑板上最新的指令，旧指令若未被执行则直接作废。
*   **状态原子性：** 当前笔订单未结清（未收到交易所明确的成交/撤单回报）前，C++ 拒绝执行黑板上的下一条新指令。

### 主权切换：

*   **未成交前：** Python 拥有最高指挥权，随时修改/撤销挂单。
*   **成交瞬间：** C++ 立即切断 Python 指令输入，进入独立接管模式（脊髓反射），瞬间发出止盈单与硬止损单。

### 策略场景与双脑联动

Python 的观察结果与 C++ 的动作在不同策略场景下的联动：

| 场景       | Python 的观察结果              | C++ 的动作           |
| :--------- | :----------------------------- | :------------------- |
| 黄金插针   | 爆仓一波流 + 深度瞬间回补      | 成交 -> 瞬间获利平仓 |
| 连续下跌   | 爆仓连绵不断 + 卖压持续增强    | 取消挂单 / 触发硬止损 |

### 5.4 C++ 内部状态机 (The Ratchet Logic)

这段代码每接收到一个最新成交价 (`current_price`) 就要运行一次。耗时约为 20 纳秒（极快）。

#### 5.4.1 变量初始化 (在成交瞬间)

```cpp
// 当 receive_execution_report == FILLED
double entry_price = 628.00;      // 你的买入价
double max_price = 628.00;        // 历史最高价（初始为买入价）
double dynamic_stop = 628.00 * (1 - 0.008); // 初始硬止损：622.97
bool ratchet_active = false;      // 棘轮激活状态：未激活
```

#### 5.4.2 核心循环逻辑 (在 `on_tick` 或 `on_trade` 中执行)

```cpp
void on_price_update(double current_price) {
    
    // 1. 更新历史最高价 (只升不降)
    if (current_price > max_price) {
        max_price = current_price;
    }

    // 2. 检查是否需要“激活”棘轮模式
    // 如果当前浮盈超过 0.5%，且之前没激活过
    if (!ratchet_active && current_price > entry_price * (1 + 0.005)) {
        ratchet_active = true;
        // 激活瞬间，把止损线直接提上来，提至“成本价 + 0.1%” (保护利润)
        // 或者简单的提至 entry_price (保本)
        dynamic_stop = entry_price * 1.001; 
    }

    // 3. 棘轮逻辑：动态提升止损线 (仅在激活后)
    if (ratchet_active) {
        // 新的止损线 = 最高价 - 回撤幅度 (例如 0.2%)
        double potential_stop = max_price * (1 - 0.002);
        
        // 核心规则：止损线只能上移，不能下移！
        if (potential_stop > dynamic_stop) {
            dynamic_stop = potential_stop;
        }
    }

    // 4. 最终裁决：触线即斩
    if (current_price <= dynamic_stop) {
        execute_market_sell(); // 【开火】市价全抛
        reset_strategy();      // 重置状态，等待 Python 下一个指令
    }
}
```

## 6. 关键模块：决策防抖与合规 (Decision Debouncing)

这是防止被交易所判定为“虚假交易 (Spoofing)”并封号的核心模块。

### 6.1 价格偏离过滤 (Price Delta Filter)

*   **问题：** Python 算出的价格可能是 628.11 -> 628.12 -> 628.11。
*   **规则：**
    ```python
    # 伪代码
    if abs(new_price - current_memory_price) / current_memory_price < 0.0005:
        return # 变化幅度小于万分之五，不更新内存，不折腾 C++
    ```

### 6.2 时间平滑锁 (Time Smoothing)

*   **规则：** 同一笔挂单在内存中的存活时间至少为 1-2 秒。
*   **例外：** 如果触发 ABORT (紧急撤单) 信号，忽略时间锁，立即执行。

### 6.3 权重熔断 (Rate Limit Guard)

*   **监控：** 内存中维护一个计数器，记录过去 1 分钟的“指令更新次数”。
*   **动作：** 如果更新超过 50 次/分，强制暂停策略 10 秒，防止 API 权重耗尽。

## 7. 异常处理与风险闸门 (The Circuit Breaker)

*   **连续下跌防护 (Waterfall Protection)：**
    *   **检测：** 如果 Liquidation 流在 1 秒内推送了超过 50 条爆仓信息，且价格下跌无支撑。
    *   **判断：** 这不仅是插针，这是崩盘。
    *   **动作：** 立即将共享内存中的 `StrategyStatus` 设为 99 (ABORT)。C++ 读到 99 会瞬间撤销所有挂单。
*   **死人开关 (Dead Man's Switch)：** 若 C++ 超过 2 秒未检测到 Python 进程的心跳更新，C++ 必须自动撤销全场挂单，原地进入安全模式。
*   **滑点保护：** 所有止盈单必须在成交回报回传的 10ms 内发出，利用交易所私有水管（User Data Stream）获取最快反馈。

## 8. 输出协议：写入共享内存 (Memory Writer)

Python 计算完毕后，需将结果打包写入 `/dev/shm/sentinel_bridge`。

### 8.1 数据打包 (struct.pack)

必须严格遵循 C++ 定义的字节对齐：

```python
import struct

# 格式必须与 C++ 结构体完全一致 (alignas 64)
# Q: uint64 version
# d: double target_price
# d: double amount
# i: int strategy_status
# 32x: padding
# ... (读取区)
fmt = "Qddi32xid" 

def write_to_memory(mm, version, price, amount, status):
    # 写入前先读取 C++ 的状态，防止在 C++ 接管时捣乱
    # ... (读取逻辑)
    
    # 写入新指令
    packed_data = struct.pack(fmt, version, price, amount, status, ...)
    mm.seek(0)
    mm.write(packed_data)
```

### 8.2 策略参数定义 (Config in Shared Memory)

这些参数由 Python 在下达买入指令时一并写入共享内存，C++ 读取后锁定，直到本单结束。

| 参数名 (C++ Variable) | 推荐值 | 含义                                                                                     |
| :-------------------- | :----- | :--------------------------------------------------------------------------------------- |
| `STOP_LOSS_HARD`      | -0.8%  | 硬止损线。进场后，如果直接跌去 0.8%，无条件市价全抛。这是保命底线。                     |
| `ACTIVATE_THRESHOLD`  | +0.5%  | 激活门槛。只有浮盈超过 0.5% 后，才启动“移动止盈”逻辑。在此之前，只守硬止损。           |
| `CALLBACK_RATIO`      | 0.2%   | 回撤容忍度。启动后，允许价格从最高点回落 0.2%。超过这个幅度，视为趋势结束，止盈离场。 |

## 9. 极致性能要求 (Tech Stack Constraints)

*   **语言：** 极致 C++ (C++17/20)。
*   **内存管理：** 零动态分配 (Zero Dynamic Allocation)。所有对象预分配在栈或物理连续的内存池中。
*   **并行优化：**
    *   C++ 主线程绑定物理核心 (CPU Affinity)。
    *   使用 `std::atomic` 结合 `memory_order_relaxed` 进行无锁读取。
    *   结构体对齐 `alignas(64)`，防止缓存行伪共享 (False Sharing)。

## 10. 总结：Python 端开发清单

*   **连接层：** 5 路 WebSocket 异步接收。
*   **清洗层：** `orjson` + 字符串预过滤。
*   **计算层：** ATR 指标计算 + 风险评分状态机。
*   **过滤层：** 价格防抖 + 频率限制。
*   **通讯层：** `mmap` 读写 + `struct` 打包。
*   **风控层：** 爆仓流熔断逻辑。
