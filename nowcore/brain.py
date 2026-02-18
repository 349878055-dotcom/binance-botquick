import time, signal, sys, os, csv, json, queue, threading
from collections import deque
from bridge import Bridge

class Genes:
    SYMBOL = "BNBUSDT"
    TOXIC_THRESHOLD = 0.8
    DECAY_FACTOR = 0.95
    UNIT_SIZE_USDT = 12.0
    MAX_POSITION_USDT = 100.0
    MIN_USDT_THRESHOLD = 10.0
    ABSOLUTE_SAFETY_LINE = 5.0 # 已删除此行的实际检查，但保留定义
    MAX_SPREAD_FOR_FAST_EXECUTION = 0.01
    VELOCITY_THRESHOLD = 1.5
    ACCELERATION_THRESHOLD = 0
    V_NET_THRESHOLD = 500.0
    PENETRATION_THRESHOLD = 0.6
    BOOK_SKEW_THRESHOLD = 3.0
    AGGRESSIVE_SLIPPAGE = 0.0005 # 初始建议值 0.05%
    LIQUIDATION_WEIGHT = 3.0
    LIQUIDATION_VOLUME_THRESHOLD = 100.0

class MomentumMonitor:
    def __init__(self):
        self.history = deque()
        self.window_ns = 3 * 1000 * 1000 * 1000

    def update(self, ts_ns: int, qty: float, side: int):
        self.history.append((ts_ns, qty, side))
        while self.history and ts_ns - self.history[0][0] > self.window_ns:
            self.history.popleft()

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

class NeuroCore:
    def __init__(self):
        self.bridge = Bridge()
        self.running = True
        self.last_trade_price = 0.0
        self.book_bid_q, self.book_ask_q = 1.0, 1.0
        self.book_bid_p, self.book_ask_p = 0.0, 0.0
        self.last_market_frame_timestamp = 0
        self.order_context = {}
        self.active_order_id = ""
        self.active_entry_order_id = ""
        self.last_order_trigger_ms = 0
        self.mom_monitor = MomentumMonitor()
        self.log_queue = queue.Queue()
        self.trade_log_file = "trade_physics.csv"
        self.flow_imbalance = 0.0
        self.liq_imbalance = 0.0
        signal.signal(signal.SIGINT, self.shutdown)

    def connect(self):
        print("[Neuro] 连接脊髓...", end="")
        while not self.bridge.connect(): time.sleep(1); print(".", end="", flush=True)
        print(" [OK]")

    def shutdown(self, signum: int, frame):
        print("\n[Neuro] 停机...")
        self.running = False
        self.log_queue.put(None)

    def run(self):
        self.connect()
        print("[Neuro] 视觉系统已升级：全息订单流监控启动。")
        logging_thread = threading.Thread(target=self._logging_worker, daemon=True)
        logging_thread.start()
        while self.running:
            frames_processed = 0
            for frame in self.bridge.fetch_market_stream():
                self.process_frame(frame)
                frames_processed += 1
            self.process_account_events()
            if frames_processed > 0: self.logic_loop()
            else: time.sleep(0.005)

    def process_frame(self, frame):
        self.last_market_frame_timestamp = frame.timestamp
        if frame.type == 2:
            self.book_bid_p, self.book_bid_q = frame.bid_p, frame.bid_q
            self.book_ask_p, self.book_ask_q = frame.ask_p, frame.ask_q
        elif frame.type == 1:
            self.last_trade_price = frame.price
            signed_vol = frame.quantity * frame.side
            self.flow_imbalance = (self.flow_imbalance * Genes.DECAY_FACTOR) + signed_vol
            self.mom_monitor.update(frame.timestamp * 1_000_000, frame.quantity, frame.side)
        elif frame.type == 3:
            self.liq_imbalance = (self.liq_imbalance * Genes.DECAY_FACTOR) + (frame.quantity * frame.side)

    def process_account_events(self):
        for frame in self.bridge.fetch_account_stream():
            cid = frame.client_order_id.decode('utf-8')
            snapshot = self.order_context.get(cid, None)
            if frame.event_type == 3:
                print(f" [Event] 🎉 订单 {cid} 成交! 均价: {frame.fill_price}")
                if snapshot and snapshot["type"] == "Entry": self.active_entry_order_id = cid
                self.log_queue.put({"frame": frame, "snapshot": snapshot})
                if cid in self.order_context: del self.order_context[cid]
                if cid == self.active_order_id: self.active_order_id = ""
            elif frame.event_type in [4, 5]:
                print(f" [Event] 订单 {cid} 被 {('撤销' if frame.event_type == 4 else '拒绝')}。")
                self.log_queue.put({"frame": frame, "snapshot": snapshot})
                if cid in self.order_context: del self.order_context[cid]
                if cid == self.active_order_id: self.active_order_id = ""
            elif frame.event_type in [1, 2, 6, 7]:
                self.log_queue.put({"frame": frame, "snapshot": snapshot})

    def _calculate_total_momentum(self) -> float:
        return self.flow_imbalance + (self.liq_imbalance * Genes.LIQUIDATION_WEIGHT)

    def _calculate_toxicity(self, total_momentum: float) -> float:
        toxicity = 0.0
        if total_momentum > 0 and self.book_ask_q > 0.0: toxicity = total_momentum / self.book_ask_q
        elif total_momentum < 0 and self.book_bid_q > 0.0: toxicity = total_momentum / self.book_bid_q
        return toxicity

    def _check_penetration(self, side: str, vol_200ms: float) -> float:
        opp_depth = self.book_ask_q if side == "BUY" else self.book_bid_q
        if opp_depth <= 0: return 999.0
        return vol_200ms / opp_depth

    def _fire_atomic_order(self, side: str, qty: float, order_type_str: str, reason: str, aggressive_slippage_override: float = None) -> str:
        self.last_order_trigger_ms = self.last_market_frame_timestamp
        client_order_id, parent_id = "", ""
        decision_params = {
            "flow_imbalance": self.flow_imbalance, "liq_imbalance": self.liq_imbalance,
            "total_momentum": self._calculate_total_momentum(),
            "toxicity": self._calculate_toxicity(self._calculate_total_momentum()),
            "book_bid_p": self.book_bid_p, "book_ask_p": self.book_ask_p,
            "last_trade_price": self.last_trade_price, "order_type_str": order_type_str,
            "reason": reason, "quantity": qty
        }
        if order_type_str == "MARKET":
            if "CLOSE" in reason: parent_id = self.active_entry_order_id
            client_order_id = self.bridge.send_market_order(
                Genes.SYMBOL, side, qty, self.last_market_frame_timestamp, parent_order_id=parent_id
            )
        else: # FOK
            price = self.book_ask_p if side == "BUY" else self.book_bid_p
            
            slippage_to_use = aggressive_slippage_override if aggressive_slippage_override is not None else Genes.AGGRESSIVE_SLIPPAGE
            price *= (1 + slippage_to_use) if side == "BUY" else (1 - slippage_to_use)
            
            client_order_id = self.bridge.send_limit_order(
                Genes.SYMBOL, side, price, qty, self.last_market_frame_timestamp, parent_order_id="", tif_type=3
            )

        if client_order_id:
            self.active_order_id = client_order_id
            self.order_context[client_order_id] = {
                "trigger_ms": self.last_market_frame_timestamp, "decision_params": decision_params,
                "type": "Entry" if "OPEN" in reason else "Exit", "side": side, "parent_order_id": parent_id
            }
            print(f"[Order] {reason}: {side} {qty:.4f} {Genes.SYMBOL} @ {price if order_type_str != 'MARKET' else 'MARKET'} (CID: {client_order_id})")
            self.flow_imbalance, self.liq_imbalance = 0.0, 0.0
        return client_order_id

    def _evaluate_filters(self, side: str, current_pos: float, usdt_bal: float, velocity: float, acceleration: float, v_net: float, vol_200ms: float, is_fast_execution: bool, spread: float):        
        # --- 第一步：物理动能门槛 (P0) ---
        liquidation_boost = abs(self.liq_imbalance) > Genes.LIQUIDATION_VOLUME_THRESHOLD
        if not (velocity > Genes.VELOCITY_THRESHOLD and (acceleration > Genes.ACCELERATION_THRESHOLD or liquidation_boost)): return

        # --- 第二步：物理阻力与穿透率审计 ---
        penetration = self._check_penetration(side, vol_200ms)
        if penetration < Genes.PENETRATION_THRESHOLD: return

        if side == "BUY":
            if self.book_bid_q > 0 and (self.book_ask_q / self.book_bid_q > Genes.BOOK_SKEW_THRESHOLD): return
        else: # side == "SELL"
            if self.book_ask_q > 0 and (self.book_bid_q / self.book_ask_q > Genes.BOOK_SKEW_THRESHOLD): return

        # --- 第三步：原子化执行 (根据持仓状态分流) ---
        
        # 计算有效滑点 (Dynamic Slippage Tuning)
        effective_slippage = Genes.AGGRESSIVE_SLIPPAGE

        current_price_ref = 0.0
        if self.last_trade_price > 0:
            current_price_ref = self.last_trade_price
        elif self.book_ask_p > 0 and self.book_bid_p > 0:
            current_price_ref = (self.book_ask_p + self.book_bid_p) / 2

        if current_price_ref > 0 and spread > 0:
            spread_pct_from_price = spread / current_price_ref
            effective_slippage = max(effective_slippage, 2 * spread_pct_from_price)

        if velocity > 2.5:
            effective_slippage = max(effective_slippage, 0.001) # 至少 0.1%

        # 1. 对冲逻辑 (Hedge/Close)
        if (side == "BUY" and current_pos < 0) or (side == "SELL" and current_pos > 0):
            reason = "CLOSE_SHORT" if side == "BUY" else "CLOSE_LONG"
            print(f"[TRIGGER] ⚡ 信号对冲! V:{velocity:.2f} A:{acceleration:.2f} Pos:{current_pos}")
            # 市价单不直接使用滑点计算价格，但保持函数签名一致
            self._fire_atomic_order(side, abs(current_pos), "MARKET", reason, aggressive_slippage_override=effective_slippage) 
            return

        # 2. 开仓逻辑 (Open)
        if current_pos == 0:
            if abs(v_net) < Genes.V_NET_THRESHOLD: return
            # if usdt_bal < Genes.ABSOLUTE_SAFETY_LINE: return # 已删除此行，因为是冗余检查
            if usdt_bal < Genes.MIN_USDT_THRESHOLD: return
            if not is_fast_execution: return

            reason = "OPEN_LONG" if side == "BUY" else "OPEN_SHORT"
            print(f"[TRIGGER] ⚡ 信号开仓! V:{velocity:.2f} A:{acceleration:.2f} Pen:{penetration:.2f}")
            self._fire_atomic_order(side, Genes.UNIT_SIZE_USDT / self.last_trade_price, "FOK", reason, aggressive_slippage_override=effective_slippage)

    def logic_loop(self):
        if self.active_order_id != "": return
        if self.last_market_frame_timestamp <= self.last_order_trigger_ms: return

        current_pos = self.bridge.data.account_feed.position_amt
        usdt_bal = self.bridge.data.account_feed.usdt_balance
        total_momentum = self._calculate_total_momentum()
        toxicity = self._calculate_toxicity(total_momentum)

        spread = self.book_ask_p - self.book_bid_p if self.book_ask_p > 0 and self.book_bid_p > 0 else 0.0
        is_fast_execution = (spread < Genes.MAX_SPREAD_FOR_FAST_EXECUTION) and (self.book_ask_p > 0) and (self.book_bid_p > 0)

        velocity, acceleration = self.mom_monitor.get_velocity_metrics()
        v_net, _ = self.mom_monitor.get_inventory_metrics()
        vol_200ms = self.mom_monitor.get_recent_volume_200ms()

        if toxicity > Genes.TOXIC_THRESHOLD:
            self._evaluate_filters("BUY", current_pos, usdt_bal, velocity, acceleration, v_net, vol_200ms, is_fast_execution, spread)
        elif toxicity < -Genes.TOXIC_THRESHOLD:
            self._evaluate_filters("SELL", current_pos, usdt_bal, velocity, acceleration, v_net, vol_200ms, is_fast_execution, spread)

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
                    parent_id = frame.parent_order_id.decode('utf-8') if frame.parent_order_id else ""
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
                    error_msg = frame.error_msg.decode('utf-8') if frame.error_msg else ""
                    order_price = snapshot["decision_params"].get("book_ask_p", 0.0) if snapshot and snapshot["side"] == "BUY" else snapshot["decision_params"].get("book_bid_p", 0.0) if snapshot and snapshot["side"] == "SELL" else 0.0
                    order_quantity = snapshot["decision_params"].get("quantity", 0.0) if snapshot else 0.0

                    writer.writerow([
                        order_id, parent_id, order_type, order_side, trigger_ms, transact_ms,
                        decision_params_str, order_price, order_quantity,
                        fill_price, fill_qty, remaining_qty, event_type, error_code, error_msg
                    ])
                except queue.Empty: continue
                except Exception as e: print(f"[ERROR] 日志写入失败: {e}")

if __name__ == "__main__":
    core = NeuroCore()
    core.run()