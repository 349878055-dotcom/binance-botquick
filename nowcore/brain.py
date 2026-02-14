import time
from bridge import Bridge

class Brain:
    def __init__(self):
        self.bridge = Bridge()
        self.connected = False

    def start(self):
        print("[Brain] 等待 C++ 核心启动 (寻找共享内存)...")
        while not self.bridge.connect():
            time.sleep(1)
        
        self.connected = True
        print("[Brain] 已连接 C++ 核心。")
        self.run_strategy_loop()

    def run_strategy_loop(self):
        print("[Brain] 策略循环开始...")
        
        last_price = 0.0
        
        while self.connected:
            state = self.bridge.read_full_state()
            if not state: break
            
            curr_price = state['price']
            
            # 等待 C++ 获取到有效的价格和精度
            if curr_price == 0 or state['q_prec'] == 0:
                print(f"\r[Brain] 等待数据... 价格:{curr_price} 精度:{state['p_prec']}/{state['q_prec']}", end="")
                time.sleep(0.5)
                continue

            # 简单的打印心跳
            if abs(curr_price - last_price) > 0.0:
                print(f"\n[Brain] 市场价更新: {curr_price} | 持仓: {state['bnb_pos']} | USDT: {state['usdt']}")
                last_price = curr_price

            # --- 示例逻辑 (请替换为你的 14倍 模型) ---
            # 如果空仓 (cpp_status == 0) 且 价格跌破 600 (示例)
            if state['cpp_status'] == 0 and curr_price < 600.0 and curr_price > 10.0:
                print("[Brain] 发现机会！准备从 C++ 发起进攻...")
                
                # 计算参数
                entry_price = curr_price * 1.001 # 稍微吃一点滑点
                quantity = 0.01 # 固定数量，或者用 state['usdt'] / entry_price * 0.95 计算
                
                # 棘轮配置：
                # 1. 硬止损：买入价跌 1%
                hard_stop = entry_price * 0.99 
                # 2. 激活门槛：涨 0.5% 后激活移动止损
                active_gap = entry_price * 1.005 
                # 3. 回撤容忍：最高点回撤 0.2% 止盈
                callback = 0.002 

                self.bridge.send_order(
                    "BNBUSDT", 
                    entry_price, 
                    quantity, 
                    hard_stop, 
                    active_gap, 
                    callback
                )
                
                # 发送完休息一会，防止重复发单 (C++ 会把 cpp_status 变为 1 或 2)
                time.sleep(2)

            time.sleep(0.001) # 1ms 思考间隔

    def stop(self):
        self.bridge.close()

if __name__ == "__main__":
    brain = Brain()
    try:
        brain.start()
    except KeyboardInterrupt:
        print("\n[Brain] 用户终止。")
        brain.bridge.cancel_all() # 退出前尝试撤单
        brain.stop()