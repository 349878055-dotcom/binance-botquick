import time, signal, sys, os, csv, json, queue, threading

from Common import get_now_ns # 新增导入 Common.get_now_ns
from collections import deque

# 模拟 get_now_ns 函数，以便在 NeuroCore 外部使用

# 在回测环境中，这个函数会被 BacktestBridge 中的 get_now_ns 覆盖
def get_now_ns():
    return int(time.time() * 1_000_000_000)
from bridge import Bridge
from Common import get_now_ns # 新增导入 Common.get_now_ns


class Genes:

    SYMBOL = "BTCUSDT"

    DECAY_FACTOR = 0.95 # 成交量不平衡的衰减因子

    UNIT_SIZE_USDT = 150.0

    MIN_USDT_THRESHOLD = 350.0

    ORDER_EXPIRATION_SECONDS = 60
    
    # --- [0 -> 1 -> ∞] 绝对力量触发参数 ---
    DUST_FILTER = 0.01        # [散户屏蔽] 过滤 <0.01 BTC 物理尘埃
    MIN_DENSITY_COUNT = 15    # [套利商屏蔽] 50ms内物理碰撞 >= 15 次
    PURITY_THRESHOLD = 0.80   # [做市商屏蔽] 动量纯度 >= 80% (单边屠杀)
    
    # --- 核心坍缩阈值 (基于 14小时 98万笔数据的物理提纯) ---
    ENERGY_THRESHOLD = 30.0   # 绝对净量：25ms 内单向扫盘 > 30 BTC
    ACCEL_THRESHOLD = 20.0    # 物理加速：动能差分 > 20 BTC

class MarketFrameType:
    TRADE = 1
    DEPTH = 2
    LIQUIDATION = 3

class OrderEventType:
    SUBMITTED = 1
    PARTIAL_FILL = 2
    FULL_FILL = 3
    CANCELED = 4
    REJECTED = 5
    AMENDED = 6


class MomentumMonitor:

    def __init__(self):

        self.history = deque()

        self.window_ns = 25 * 1000 * 1000 # 25ms 窗口


    def update(self, ts_ns: int, qty: float, side: int):
        if self.history and ts_ns < self.history[-1][0]:
            print(f"[WARNING] MomentumMonitor 检测到时间戳倒退！当前: {ts_ns}, 上一个: {self.history[-1][0]}。重置历史数据。")
            sys.stdout.flush() # 强制刷新输出
            self.history.clear() # 重置历史数据
        self.history.append((ts_ns, qty, side))
        # 不再在此处自动清理历史，让各个计算方法自行管理窗口


    def get_recent_volume_200ms(self) -> float:

        if not self.history: return 0.0

        now = self.history[-1][0]

        return sum(s[1] for s in self.history if now - s[0] <= 200 * 1000 * 1000)


    def get_velocity_metrics(self) -> tuple[float, float]:

        if not self.history: return 0.0, 0.0

        now = self.history[-1][0]

        vol_3s = sum(s[1] for s in self.history)

        avg_200ms_ref = vol_3s / 15.0

        vol_200ms = sum(s[1] for s in self.history if now - s[0] <= 200 * 1000 * 1000)

        vol_now_100 = sum(s[1] for s in self.history if now - s[0] <= 100 * 1000 * 1000)

        vol_last_100 = sum(s[1] for s in self.history if 100 * 1000 * 1000 < now - s[0] <= 200 * 1000 * 1000)

        velocity = vol_200ms / (avg_200ms_ref + 1e-9) if avg_200ms_ref > 0 else 0.0

        acceleration = vol_now_100 - vol_last_100

        return velocity, acceleration


    def get_inventory_metrics(self) -> tuple[float, float]:

        if not self.history: return 0.0, 0.0

        v_net = sum(s[1] * s[2] for s in self.history)

        v_total_abs = sum(s[1] for s in self.history)

        return v_net, v_total_abs

    def get_collapse_metrics(self) -> tuple[float, float, float, int]:
        """
        计算瞬时脉冲指标 (25ms/50ms 窗口)
        返回: 净动量, 加速动量, 纯度, 密度 (碰撞次数)
        """
        if not self.history: return 0.0, 0.0, 0.0, 0
        
        current_time = get_now_ns()
        
        # 定义 25ms 和 50ms 窗口
        window_25ms_ns = 25 * 1000 * 1000
        window_50ms_ns = 50 * 1000 * 1000
        
        v_net_25ms = 0.0
        v_total_abs_25ms = 0.0
        trade_count_50ms = 0 # 密度使用 50ms 窗口
        
        v_net_now_12_5ms = 0.0
        v_net_last_12_5ms = 0.0

        for ts, qty, side in reversed(self.history): # 从最近的开始遍历
            if current_time - ts <= window_50ms_ns:
                trade_count_50ms += 1
            
            if current_time - ts <= window_25ms_ns:
                v_net_25ms += qty * side
                v_total_abs_25ms += qty
                
                # 计算加速动量 (12.5ms 窗口)
                if current_time - ts <= window_25ms_ns // 2: # 最近 12.5ms
                    v_net_now_12_5ms += qty * side
                else: # 之前 12.5ms
                    v_net_last_12_5ms += qty * side
            else:
                break # 历史数据已超出 50ms 窗口，无需继续遍历
                
        acceleration = v_net_now_12_5ms - v_net_last_12_5ms
        purity = abs(v_net_25ms) / v_total_abs_25ms if v_total_abs_25ms > 0 else 0.0
        
        return v_net_25ms, acceleration, purity, trade_count_50ms

    def get_1min_net_flow(self) -> float:
        """
        计算 1 分钟内的净流量 (趋势洪流指标)
        """
        if not self.history: return 0.0
        
        current_time = get_now_ns()
        window_1min_ns = 60 * 1000 * 1000 * 1000 # 1分钟窗口
        
        v_net_1min = 0.0
        
        # 确保只计算 1 分钟内的历史数据
        for ts, qty, side in reversed(self.history):
            if current_time - ts <= window_1min_ns:
                v_net_1min += qty * side
            else:
                break # 历史数据已超出 1分钟 窗口，无需继续遍历
                
        return v_net_1min


class NeuroCore:

    def __init__(self):
        self.bridge = Bridge()
        self.running = True
        self.last_trade_price = 0.0
        self.last_market_frame_timestamp = 0
        self.order_context = {}
        self.active_order_id = "" # 这是正在执行的标记
        self.active_entry_order_id = ""
        self.last_order_trigger_ms = 0
        self.mom_monitor = MomentumMonitor()
        self.log_queue = queue.Queue(maxsize=1000)
        self.trade_log_file = "trade_physics.csv"
        self.flow_imbalance = 0.0
        self.liq_imbalance = 0.0
        self.logic_side = 0 # 新增：逻辑锁，0=空仓, 1=多, -1=空
        self.trigger_log = [] # 新增：用于存储触发事件的列表 (timestamp, momentum, side, price)

        signal.signal(signal.SIGINT, self.shutdown)

    def connect(self):

        print("[Neuro] 连接脊髓...", end="")
        while not self.bridge.connect(): time.sleep(1); print(".", end="", flush=True)

        print(" [OK]")


    def shutdown(self, signum: int, frame):

        print("\n[Neuro] 停机...")
        self.running = False

        self.log_queue.put(None)
        self._dump_trigger_history() # 在停机时保存触发历史


    def run(self):

        self.connect()

        print("[Neuro] 视觉系统已升级：全息订单流监控启动。")

        logging_thread = threading.Thread(target=self._logging_worker, daemon=True)

        logging_thread.start()

        while self.running:

            frames_processed = 0

            try:
                for frame in self.bridge.fetch_market_stream():
                    self.process_frame(frame)
                    frames_processed += 1
            except Exception as e:
                print(f"[ERROR] 获取市场流失败: {e}")
                time.sleep(0.1) # 短暂休眠，避免错误循环

            try:
                self.process_account_events()
            except Exception as e:
                print(f"[ERROR] 处理账户事件失败: {e}")
                time.sleep(0.1) # 短暂休眠，避免错误循环


            if frames_processed > 0: self.logic_loop()

            else: time.sleep(0.005)

    def process_frame(self, frame):

        self.last_market_frame_timestamp = frame.timestamp

        # if frame.type == MarketFrameType.DEPTH: # 【注释】不再处理深度数据

        #     self.book_bid_p, self.book_bid_q = frame.bid_p, frame.bid_q

        #     self.book_ask_p, self.book_ask_q = frame.ask_p, frame.ask_q

        if frame.type == MarketFrameType.TRADE:

            self.last_trade_price = frame.price

            signed_vol = frame.quantity * frame.side

            self.flow_imbalance = (self.flow_imbalance * Genes.DECAY_FACTOR) + signed_vol

            self.mom_monitor.update(frame.timestamp, frame.quantity, frame.side)

        # elif frame.type == MarketFrameType.LIQUIDATION: # 【注释】不再处理爆仓数据

        #     self.liq_imbalance = (self.liq_imbalance * Genes.DECAY_FACTOR) + (frame.quantity * frame.side)


    def process_account_events(self):

        for frame in self.bridge.fetch_account_stream():

            try:
                cid = frame.client_order_id.decode('utf-8')
            except UnicodeDecodeError:
                cid = f"DecodeError_{int(time.time() * 1000)}_{frame.client_order_id.hex()}" # 生成一个唯一的ID
                print(f"[WARNING] 无法解码 client_order_id: {frame.client_order_id}")

            snapshot = self.order_context.get(cid, None)

            if frame.event_type == OrderEventType.FULL_FILL:

                print(f" [Event] 🎉 订单 {cid} 成交! 均价: {frame.fill_price}")

                if snapshot and snapshot["type"] == "Entry": self.active_entry_order_id = cid

                self.log_queue.put({"frame": frame, "snapshot": snapshot})

                if cid in self.order_context: del self.order_context[cid]

                if cid == self.active_order_id: self.active_order_id = ""

            elif frame.event_type == OrderEventType.CANCELED or frame.event_type == OrderEventType.REJECTED:

                print(f" [Event] 订单 {cid} 被 {('撤销' if frame.event_type == OrderEventType.CANCELED else '拒绝')}。")

                self.log_queue.put({"frame": frame, "snapshot": snapshot})

                if cid in self.order_context: del self.order_context[cid]

                if cid == self.active_order_id: self.active_order_id = ""

            elif frame.event_type in [OrderEventType.SUBMITTED, OrderEventType.PARTIAL_FILL, OrderEventType.AMENDED]: # 没有 7, 可能是 EVT_AMENDED 的旧值或者未来预留

                self.log_queue.put({"frame": frame, "snapshot": snapshot})




    # def _calculate_toxicity(self, total_momentum: float) -> float: # 【删除】不再使用 toxicity

    #     toxicity = 0.0

    #     if total_momentum > 0 and self.book_ask_q > 0.0: toxicity = total_momentum / self.book_ask_q

    #     elif total_momentum < 0 and self.book_bid_q > 0.0: toxicity = total_momentum / self.book_bid_q

    #     return toxicity


    # def _check_penetration(self, side: str, vol_200ms: float) -> float: # 【删除】不再使用 penetration

    #     opp_depth = self.book_ask_q if side == "BUY" else self.book_bid_q

    #     if opp_depth <= 0: return 999.0

    #     return vol_200ms / opp_depth


    def _fire_atomic_order(self, side: str, qty: float, order_type_str: str, reason: str, tif_type: int = 3, aggressive_slippage_override: float = None) -> str:

        self.last_order_trigger_ms = get_now_ns() # 使用 Common.get_now_ns() 获取纳秒时间戳

        client_order_id, parent_id = "", ""

        decision_params = {

            "flow_imbalance": self.flow_imbalance, "liq_imbalance": self.liq_imbalance,

            "total_momentum": self._calculate_total_momentum(),

            # "toxicity": self._calculate_toxicity(self._calculate_total_momentum()), # 【注释】不再使用 toxicity

            # "book_bid_p": self.book_bid_p, "book_ask_p": self.book_ask_p, # 【注释】不再使用盘口深度

            "last_trade_price": self.last_trade_price, "order_type_str": order_type_str,

            "reason": reason, "quantity": qty

        }

        # 强制只处理市价单
        if order_type_str == "MARKET": # 总是 MARKET

            if "CLOSE" in reason: parent_id = self.active_entry_order_id

            try:
                client_order_id = self.bridge.send_market_order(
                    Genes.SYMBOL, side, qty, self.last_market_frame_timestamp, parent_order_id=parent_id
                )
            except CommandBufferFullError as e:
                print(f"[ERROR] 发送市价单失败: {e}")
                return ""

        # 【删除】不再处理限价单 FOK 逻辑
        # else: # FOK
        #     price = self.book_ask_p if side == "BUY" else self.book_bid_p
        #     slippage_to_use = aggressive_slippage_override if aggressive_slippage_override is not None else Genes.AGGRESSIVE_SLIPPAGE
        #     price *= (1 + slippage_to_use) if side == "BUY" else (1 - slippage_to_use)
        #     if price <= 0:
        #         print(f"[ERROR] 计算出的订单价格无效: {price}")
        #         return ""
        #     try:
        #         client_order_id = self.bridge.send_limit_order(
        #             Genes.SYMBOL, side, price, qty, self.last_market_frame_timestamp, tif_type, parent_order_id=""
        #         )
        #     except CommandBufferFullError as e:
        #         print(f"[ERROR] 发送限价单失败: {e}")
        #         return ""

        if client_order_id:

            self.active_order_id = client_order_id # 必须设置 active_order_id 以启用保护期
            # 不需要等待成交回执，所以不阻塞 active_order_id

            self.order_context[client_order_id] = {

                "trigger_ms": self.last_market_frame_timestamp, "decision_params": decision_params,

                "type": "Entry" if "OPEN" in reason else "Exit", "side": side, "parent_order_id": parent_id,
                "creation_timestamp": time.time() # 以秒为单位

            }

            print(f"[Order] {reason}: {side} {qty:.4f} {Genes.SYMBOL} @ {'MARKET'} (CID: {client_order_id})")

            self.flow_imbalance, self.liq_imbalance = 0.0, 0.0

        return client_order_id


    def logic_loop(self):
        # 1. 物理保护期与逻辑锁判定 (保持不变)
        physical_pos = self.bridge.data.account_feed.position_amt
        usdt_bal = self.bridge.data.account_feed.usdt_balance

        if self.active_order_id == "":
            self.logic_side = 1 if physical_pos > 1e-6 else (-1 if physical_pos < -1e-6 else 0)

        if self.active_order_id != "":
            return 

        # 2. 获取双频能量指标
        v_now, accel, purity_25, density_50 = self.mom_monitor.get_collapse_metrics()
        v_net_1min = self.mom_monitor.get_1min_net_flow()

        # 3. 判定触发
        # 条件 1: 瞬时坍缩爆发 (同时满足净量、加速、纯度、密度、方向一致性)
        trigger_A = (abs(v_now) > Genes.ENERGY_THRESHOLD and
                     abs(accel) > Genes.ACCEL_THRESHOLD and
                     v_now * accel > 0 and # 净动量和加速方向一致
                     purity_25 >= Genes.PURITY_THRESHOLD and
                     density_50 >= Genes.MIN_DENSITY_COUNT)
        
        # 条件 2: 趋势洪流接管 (例如一分钟累积净量超过 80 BTC)
        trigger_B = (abs(v_net_1min) > 80.0)

        if trigger_A or trigger_B:
            # 确定方向：谁触发了听谁的，都触发了方向肯定一致
            target_side_int = 0
            if trigger_A:
                target_side_int = 1 if v_now > 0 else -1
            elif trigger_B:
                target_side_int = 1 if v_net_1min > 0 else -1
            
            if target_side_int != 0 and target_side_int != self.logic_side:
                # 执行 150U 翻转连发协议
                self.execute_flip(target_side_int)

    def execute_flip(self, target_side_int: int):
        """
        执行 150U 翻转连发协议：先平仓，不等回执，立刻开仓 150U
        """
        physical_pos = self.bridge.data.account_feed.position_amt
        usdt_bal = self.bridge.data.account_feed.usdt_balance

        if usdt_bal < Genes.MIN_USDT_THRESHOLD:
            print("[CRITICAL] 余额不足 350U，系统停机保护")
            return

        # 锁定意图状态
        old_logic_side = self.logic_side
        self.logic_side = target_side_int

        target_side_str = "BUY" if target_side_int == 1 else "SELL"
        if self.last_trade_price <= 0:
            print("[WARNING] Last trade price is zero or negative, cannot place order.")
            self.trigger_log.append({"timestamp": get_now_ns(), "momentum": 0.0, "action": "NO_TRADE_PRICE_ZERO"})
            return

        target_qty_coin = Genes.UNIT_SIZE_USDT / self.last_trade_price

        if old_logic_side != 0: # 如果当前有持仓，先平仓
            print(f"[FLIP] 动量反转！立刻平仓 {physical_pos} 并开仓 {target_side_str}")
            self.bridge.send_market_order(Genes.SYMBOL, "SELL" if old_logic_side == 1 else "BUY",
                                          abs(physical_pos), self.last_market_frame_timestamp)
            self.trigger_log.append((get_now_ns(), 0.0, -old_logic_side, self.last_trade_price)) # 记录平仓动作

        # 立即开仓新仓
        self._fire_atomic_order(target_side_str, target_qty_coin, "MARKET", "OPEN_POWER")
        self.trigger_log.append((get_now_ns(), 0.0, target_side_int, self.last_trade_price)) # 记录开仓动作



    def _logging_worker(self):

        file_exists = os.path.exists(self.trade_log_file)

        with open(self.trade_log_file, 'a', newline='') as f:

            writer = csv.writer(f)

            if not file_exists:

                writer.writerow([

                    "OrderID", "ParentID", "Type", "Side", "Trigger_MS", "Transact_MS",

                    "Decision_Params", "Order_Price", "Order_Quantity", "Fill_Price", "Fill_Quantity",

                    "Remaining_Quantity", "Event_Type", "Error_Code", "Error_Msg"

                ])

            while self.running:

                try:

                    log_data = self.log_queue.get(timeout=1)

                    if log_data is None: break

                    frame = log_data["frame"]

                    snapshot = log_data["snapshot"]


                    order_id = frame.client_order_id.decode('utf-8')

                    parent_id = ""
                    if frame.parent_order_id:
                        try:
                            parent_id = frame.parent_order_id.decode('utf-8')
                        except UnicodeDecodeError:
                            print(f"[WARNING] 无法解码 parent_order_id: {frame.parent_order_id}")

                    event_type = frame.event_type

                    trigger_ms = snapshot["trigger_ms"] if snapshot else 0

                    transact_ms = frame.timestamp

                    decision_params_str = json.dumps(snapshot["decision_params"]) if snapshot and snapshot["decision_params"] else "{}"

                    order_type = snapshot["type"] if snapshot else "UNKNOWN"

                    order_side = snapshot["side"] if snapshot else "UNKNOWN"

                    fill_price = frame.fill_price

                    fill_qty = frame.fill_qty

                    remaining_qty = frame.remaining_qty

                    error_code = frame.error_code

                    error_msg = ""
                    if frame.error_msg:
                        try:
                            error_msg = frame.error_msg.decode('utf-8')
                        except UnicodeDecodeError:
                            print(f"[WARNING] 无法解码 error_msg: {frame.error_msg}")


                    order_price = snapshot["decision_params"].get("book_ask_p", 0.0) if snapshot and snapshot["side"] == "BUY" else snapshot["decision_params"].get("book_bid_p", 0.0) if snapshot and snapshot["side"] == "SELL" else 0.0

                    order_quantity = snapshot["decision_params"].get("quantity", 0.0) if snapshot else 0.0


                    writer.writerow([

                        order_id, parent_id, order_type, order_side, trigger_ms, transact_ms,

                        decision_params_str, order_price, order_quantity,

                        fill_price, fill_qty, remaining_qty, event_type, error_code, error_msg

                    ])

                except queue.Empty:
                    continue
                except (csv.Error, IOError) as e:
                    print(f"[ERROR] 日志写入 CSV 文件失败: {e}")
                except Exception as e:
                    print(f"[ERROR] 日志处理中发生未知错误: {e}")


    def _dump_trigger_history(self):
        if not self.trigger_log:
            return
        timestamp_str = time.strftime("%Y%m%d-%H%M%S", time.localtime(time.time()))
        filename = f"trigger_history_{timestamp_str}.csv"
        try:
            with open(filename, 'w', newline='') as f:
                writer = csv.writer(f)
                writer.writerow(["Timestamp_ns", "Momentum", "Side", "Price"])
                for entry in self.trigger_log:
                    writer.writerow(entry)
            print(f"[INFO] Trigger history dumped to {filename}")
            self.trigger_log.clear() # 清空已保存的记录
        except IOError as e:
            print(f"[ERROR] 无法写入触发历史到文件 {filename}: {e}")


if __name__ == "__main__":

    core = NeuroCore()

    core.run()
