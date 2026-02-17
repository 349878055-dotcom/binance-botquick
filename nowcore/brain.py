import time
import signal
import sys
from collections import deque
from bridge import Bridge

# ==========================================
# 策略基因 (Strategy Genes)
# ==========================================
class Genes:
    SYMBOL = "BNBUSDT"
    
    # 毒性阈值: 资金流 / 盘口厚度
    # 例如: 1.5 意味着买入量是卖一盘口量的 1.5 倍 -> 瞬间击穿
    TOXIC_THRESHOLD = 0.8 
    
    # 压力衰减 (半衰期逻辑，单位: 次)
    DECAY_FACTOR = 0.95
    
    # 单笔下单金额 (USDT)
    UNIT_SIZE_USDT = 20.0 
    
    # 最大持仓 (绝对风控)
    MAX_POSITION_USDT = 100.0

# 状态其实只需要两个：要么没仓位，要么有仓位
# 挂单那种中间状态，因为用了 IOC，瞬间就没了，所以不需要专门的状态
STATE_EMPTY   = 0 
STATE_HOLDING = 1 

class NeuroCore:
    def __init__(self):
        self.bridge = Bridge()
        self.running = True
        
        # --- 市场微观状态 ---
        self.last_trade_price = 0.0
        self.book_bid_q = 1.0 # 避免除零，初始给个底
        self.book_ask_q = 1.0
        self.book_bid_p = 0.0
        self.book_ask_p = 0.0
        
        # --- 核心动能指标 ---
        # 净买入流 accumulator (Buy Volume - Sell Volume)
        self.flow_imbalance = 0.0 
        
        signal.signal(signal.SIGINT, self.shutdown)

    def connect(self):
        print("[Neuro] 正在连接脊髓 (Shared Memory)...", end="")
        while not self.bridge.connect():
            time.sleep(1)
            print(".", end="", flush=True)
        print(" [OK]")

    def shutdown(self, signum, frame):
        print("\n[Neuro] 正在停机...")
        self.running = False

    def run(self):
        self.connect()
        print("[Neuro] 视觉系统已升级：全息订单流监控启动。")

        while self.running:
            # 1. 极速吸取数据 (Drain the pipe)
            # 我们必须处理完所有积压数据，才能做决策，否则决策会滞后
            frames_processed = 0
            
            # 使用生成器流式读取
            for frame in self.bridge.fetch_market_stream():
                self.process_frame(frame)
                frames_processed += 1
            
            # [新增] 必须加这一行！否则永远读不到成交回报！
            self.process_account_events() 
            
            # 2. 如果刚才处理了新数据，就根据最新状态做一次决策
            if frames_processed > 0:
                self.logic_loop()
            else:
                # 没数据时短暂休眠，避免单核 CPU 100%
                time.sleep(0.005) # 5ms

    def process_frame(self, frame):
        """
        全息数据处理：同时融合 成交(Type1) 和 盘口(Type2)
        """
        # === Type 2: 盘口更新 (Shield) ===
        # C++ Network.cpp 会推送 type=2 的 BookTicker
        if frame.type == 2:
            self.book_bid_p = frame.bid_p
            self.book_bid_q = frame.bid_q
            self.book_ask_p = frame.ask_p
            self.book_ask_q = frame.ask_q
            # 这里的 price 可能是 0，不要用 type 2 更新 last_trade_price
            
        # === Type 1: 成交更新 (Spear) ===
        elif frame.type == 1:
            self.last_trade_price = frame.price
            
            # 核心：计算资金流冲击
            # frame.side: 1=Buy(主动买), -1=Sell(主动卖)
            # volume = price * quantity
            trade_vol = frame.quantity 
            signed_vol = trade_vol * frame.side
            
            # 动能累加器 (带衰减)
            # 每一个新成交都会对之前的动能产生冲击，同时也继承之前的动能
            self.flow_imbalance = (self.flow_imbalance * Genes.DECAY_FATOR) + signed_vol

        # === Type 3: 强平/爆仓 (Liquidations) ===
        elif frame.type == 3:
            # 爆仓单通常是反转信号（对手盘力竭）
            print(f"[LIQ] 观测到爆仓: {frame.quantity} @ {frame.price}")
            # 可以给 flow_imbalance 加一个反向的巨大权重，或者单独逻辑
            pass

    def process_account_events(self):
        """只看成交，不管撤单"""
        for frame in self.bridge.fetch_account_stream():
            cid = frame.client_order_id.decode('utf-8')
            
            # 如果是我刚才发的那个 IOC 单成交了
            if cid == self.active_order_id and frame.event_type == 3: # FILLED
                print(f" [Event] 🎉 铲到了! 均价: {frame.fill_price}")
                self.state = STATE_HOLDING
                self.entry_price = frame.fill_price
                self.active_order_id = "" # 清空，防止重复处理

    def logic_loop(self):
        """
        决策层：每处理一批数据后执行一次
        """
        if self.last_trade_price == 0: return

        # 1. 获取真实库存 (从 C++ 共享内存原子读取)
        # 这是一个 reliable snapshot，不需要 Python 自己记账
        current_pos = self.bridge.data.account_feed.position_amt
        
        # 2. 计算 毒性 (Toxicity / Pressure Ratio)
        # 这是一个无量纲指标，表示当前资金流能否击穿盘口
        toxicity = 0.0
        
        if self.flow_imbalance > 0:
            # 买方动能 vs 卖方阻力 (Ask Qty)
            if self.book_ask_q > 0.0: # 避免除零
                toxicity = self.flow_imbalance / self.book_ask_q
        elif self.flow_imbalance < 0:
            # 卖方动能 vs 买方支撑 (Bid Qty)
            # 结果为负数
            if self.book_bid_q > 0.0: # 避免除零
                toxicity = self.flow_imbalance / self.book_bid_q

        # 3. 打印观测流 (Debug Log)
        # 只在剧烈波动时打印，避免刷屏
        if abs(toxicity) > 0.5:
            print(f"[Flow] Imbalance: {self.flow_imbalance:.2f} | Depth: {self.book_ask_q if toxicity>0 else self.book_bid_q:.2f} | Toxic: {toxicity:.2f}")

        # 4. 执行逻辑
        # ---------------------------------------------------------
        # Case A: 做多逻辑 (Toxic Buy Flow)
        # ---------------------------------------------------------
        if self.state == STATE_EMPTY and toxicity > Genes.TOXIC_THRESHOLD:
            # 检查是否超仓
            if current_pos * self.last_trade_price < Genes.MAX_POSITION_USDT: # 确保不会超仓
                print(f"[Signal] 毒性买流爆发 (Toxic={toxicity:.2f}) -> 开多")
                # 开仓数量 = 订单金额 / 价格
                order_qty = Genes.UNIT_SIZE_USDT / self.last_trade_price
                self.active_order_id = self.bridge.send_limit_order(
                    Genes.SYMBOL, "BUY", self.book_ask_p, order_qty, tif_type=3 # 用盘口卖一价吃单
                )
                # 重置动能，防止一个脉冲重复触发
                self.flow_imbalance = 0 

        # ---------------------------------------------------------
        # Case B: 做空逻辑 (Toxic Sell Flow)
        # ---------------------------------------------------------
        elif self.state == STATE_EMPTY and toxicity < -Genes.TOXIC_THRESHOLD:
            if current_pos * self.last_trade_price > -Genes.MAX_POSITION_USDT: # 确保不会超仓
                print(f"[Signal] 毒性卖流爆发 (Toxic={toxicity:.2f}) -> 开空")
                order_qty = Genes.UNIT_SIZE_USDT / self.last_trade_price
                self.active_order_id = self.bridge.send_limit_order(
                    Genes.SYMBOL, "SELL", self.book_bid_p, order_qty, tif_type=3 # 用盘口买一价吃单
                )
                self.flow_imbalance = 0

        # ---------------------------------------------------------
        # Case C: 止盈/平仓逻辑 (Inventory Management)
        # ---------------------------------------------------------
        # 简单示例：如果有持仓，且毒性反转，就平仓
        elif self.state == STATE_HOLDING:
            # 计算 PnL (与入场方向一致)
            pnl_pct = 0.0
            if self.entry_side == 1: # 多头
                pnl_pct = (self.last_trade_price - self.entry_price) / self.entry_price
            else: # 空头
                pnl_pct = (self.entry_price - self.last_trade_price) / self.entry_price
            
            should_close = False
            close_reason = ""

            # 止盈
            if pnl_pct > Genes.TAKE_PROFIT:
                should_close = True
                close_reason = f"止盈 (+{pnl_pct*100:.2f}%)"
            # 止损
            elif pnl_pct < -Genes.STOP_LOSS:
                should_close = True
                close_reason = f"止损 ({pnl_pct*100:.2f}%)"
            
            # 信号反转平仓
            # 多头遭遇空头毒性流 -> 平多
            if self.entry_side == 1 and toxicity < -0.3:
                should_close = True
                close_reason = "信号反转(多转空)"
            
            # 空头遭遇多头毒性流 -> 平空
            elif self.entry_side == -1 and toxicity > 0.3:
                should_close = True
                close_reason = "信号反转(空转多)"

            if should_close:
                print(f"[Exit] 平仓! Reason: {close_reason}")
                close_side = "SELL" if self.entry_side == 1 else "BUY"
                
                # 平仓时用当前盘口最优价，确保成交
                close_price = self.book_bid_p if close_side == "SELL" else self.book_ask_p

                self.active_order_id = self.bridge.send_limit_order(
                    Genes.SYMBOL, close_side, close_price, abs(current_pos), tif_type=3 # IOC
                )
                # 平仓后，等待回执将状态切回 EMPTY，这里不清空持仓，让 C++ 回执更新
                # 这里需要将 active_order_id 设为新的平仓单ID，等待其回执。
                # 在 process_account_events 收到 FILLED 后，会清空 active_order_id 并切换到 EMPTY。

if __name__ == "__main__":
    core = NeuroCore()
    core.run()
