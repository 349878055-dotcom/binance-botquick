import time
import signal
import sys
from collections import deque
from bridge import Bridge

class Genes:
    SYMBOL = "BNBUSDT"
    PRESSURE_THRESHOLD = 50.0  
    TOXIC_RATIO_THRESHOLD = 0.7 
    ORDER_SIZE = 0.1
    # 止盈止损
    TAKE_PROFIT = 0.0015 
    STOP_LOSS = 0.0010   

# 状态其实只需要两个：要么没仓位，要么有仓位
# 挂单那种中间状态，因为用了 IOC，瞬间就没了，所以不需要专门的状态
STATE_EMPTY   = 0 
STATE_HOLDING = 1 

class NeuroCore:
    def __init__(self):
        self.bridge = Bridge()
        self.running = True
        self.memory = deque()
        self.current_pressure = 0.0
        self.current_toxicity = 0.0
        self.last_price = 0.0
        
        self.state = STATE_EMPTY
        self.entry_price = 0.0
        self.entry_side = 0 
        self.active_order_id = "" # 记录刚才发出去的那一单

        signal.signal(signal.SIGINT, self.shutdown)

    def connect(self):
        print("[Neuro] 连接 C++ 脊髓...", end="")
        while not self.bridge.connect():
            time.sleep(1)
            print(".", end="", flush=True)
        print(" [OK]")

    def shutdown(self, signum, frame):
        print("\n[Neuro] 正在停机...")
        self.running = False

    def run(self):
        self.connect()
        print("[Neuro] 极简推土机启动。")

        while self.running:
            # 1. 读数据
            for frame in self.bridge.fetch_market_stream():
                if frame.type == 1: self.process_market(frame)
            
            # 2. 读回执
            self.process_account_events()

            # 3. 极简逻辑
            self.run_simple_logic()

            # 休息
            time.sleep(0.0001)

    def process_market(self, frame):
        """更新市场指标 (同之前逻辑)"""
        now = frame.timestamp
        qty = frame.quantity
        self.last_price = frame.price
        
        # 由于状态机简化，这里不再关注毒性，只关注压力
        signed_qty = qty if frame.side == 1 else -qty
        self.memory.append({'ts': now, 'sq': signed_qty, 'q': qty, 'large': qty>=Genes.LARGE_ORDER_THRESHOLD})
        
        # 滑动窗口
        while len(self.memory) > 0 and (now - self.memory[0]['ts'] > Genes.WINDOW_5S_NS):
            self.memory.popleft()
            
        # 计算指标
        net_vol = sum(x['sq'] for x in self.memory)
        self.current_pressure = net_vol
        
        # 毒性计算可以简化或移除，因为策略不再依赖它
        self.current_toxicity = 0 # 简化为0，不再使用

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
            # 对于 IOC 订单，没有成交的回执不需要特别处理，因为它们会自动失效
            # 如果 C++ 端解析了 EXPIRED 事件，可以在这里加入处理，但当前简化逻辑不依赖

    def run_simple_logic(self):
        # ----------------------------------------------------
        # 状态 1: 空仓找机会
        # ----------------------------------------------------
        if self.state == STATE_EMPTY:
            signal_side = 0
            # 简单的信号判断
            # 毒性阈值在这里被移除了，只依赖压力阈值
            if self.current_pressure > Genes.PRESSURE_THRESHOLD: signal_side = 1
            elif self.current_pressure < -Genes.PRESSURE_THRESHOLD: signal_side = -1
            
            if signal_side != 0:
                print(f"[Fire] 发射 IOC 订单! 方向: {signal_side}")
                side_str = "BUY" if signal_side == 1 else "SELL"
                
                # 价格给激进点 (千1)，保证 IOC 能吃到
                price = self.last_price * (1.001 if signal_side == 1 else 0.999)
                
                # 发送 IOC (tif_type=2)
                self.active_order_id = self.bridge.send_limit_order(
                    Genes.SYMBOL, side_str, price, Genes.ORDER_SIZE, tif_type=2
                )
                
                # 发完就完了，不需要切换到 PENDING 状态
                # 因为它是 IOC，下一毫秒要么成交变 HOLDING，要么直接消失
                # 我们这里清空记忆，防止连续发单
                self.memory.clear()
                self.current_pressure = 0
                # 强制冷却 1 秒，防止日志刷屏
                time.sleep(1.0) 

        # ----------------------------------------------------
        # 状态 2: 持仓算盈亏
        # ----------------------------------------------------
        elif self.state == STATE_HOLDING:
            # 算 PnL
            pnl = (self.last_price - self.entry_price) / self.entry_price
            if self.entry_side == -1: pnl = -pnl
            
            close = False
            if pnl > Genes.TAKE_PROFIT: close = True
            elif pnl < -Genes.STOP_LOSS: close = True
            
            if close:
                print(f"[Exit] 平仓! PnL: {pnl*100:.2f}%")
                # 平仓也用 IOC，价格极度激进 (千5)，保证甩掉
                c_side = "SELL" if self.entry_side == 1 else "BUY"
                c_price = self.last_price * (0.995 if c_side == "SELL" else 1.005)
                
                self.bridge.send_limit_order(Genes.SYMBOL, c_side, c_price, Genes.ORDER_SIZE, tif_type=2)
                
                # 假设平仓必成，直接切回空仓
                self.state = STATE_EMPTY
                self.active_order_id = "" # 清空活跃订单ID
                time.sleep(1.0) # 休息一下

if __name__ == "__main__":
    brain = NeuroCore()
    brain.run()
