import mmap
import ctypes
import os

# ==========================================
# 1. 物理层对齐 (The Map)
# 必须严格对应 C++ Master_Logic_Bridge.h
# ==========================================
class GenericShmStruct(ctypes.Structure):
    _fields_ = [
        # --- 区域 A: 眼睛 (C++ 写入 -> Python 读取) ---
        ("market_price",       ctypes.c_double),
        ("energy_accumulator", ctypes.c_double),
        ("instant_depth_bid",  ctypes.c_double),
        ("instant_depth_ask",  ctypes.c_double),
        ("system_health",      ctypes.c_double),
        
        # 【重要】Padding 1 (64 bytes)
        ("padding1",           ctypes.c_char * 64),

        # --- 区域 B: 大脑指令 (Python 写入 -> C++ 读取) ---
        ("cmd_version",        ctypes.c_uint64),
        
        # 0=睡觉, 1=预警, 2=开火, 99=逃命
        ("strategy_status",    ctypes.c_int),     
        
        # Symbol 数组 (16 bytes)
        ("symbol",             ctypes.c_char * 16),

        ("target_price",       ctypes.c_double),
        ("target_amount",      ctypes.c_double),
        
        # 棘轮参数
        ("hard_stop_price",    ctypes.c_double),
        ("ratchet_active_gap", ctypes.c_double),
        ("ratchet_callback",   ctypes.c_double),
        
        # 【重要】Padding 2 (32 bytes)
        ("padding2",           ctypes.c_char * 32),

        # --- 区域 C: 执行反馈 (C++ 写入 -> Python 读取) ---
        ("cpp_status",                   ctypes.c_int),
        ("last_fill_price",              ctypes.c_double),
        ("available_balance_usdt",       ctypes.c_double),
        ("available_balance_bnb",        ctypes.c_double),
        ("available_balance_btc",        ctypes.c_double),
        ("price_precision",              ctypes.c_int),
        ("quantity_precision",           ctypes.c_int),
        ("current_position_amount",      ctypes.c_double),
        ("current_position_entry_price", ctypes.c_double),
        ("current_position_pnl",         ctypes.c_double),
    ]

class Bridge:
    def __init__(self, shm_path="/dev/shm/nowcore_bridge"):
        self.shm_path = shm_path
        self.mem = None
        self.data = None
        self.connected = False

    def connect(self):
        """尝试连接 C++ 创建的共享内存"""
        if not os.path.exists(self.shm_path):
            return False
        try:
            # 打开文件
            f = open(self.shm_path, "r+b")
            # 内存映射
            self.mem = mmap.mmap(f.fileno(), ctypes.sizeof(GenericShmStruct))
            # 强转指针
            self.data = ctypes.cast(self.mem, ctypes.POINTER(GenericShmStruct)).contents
            self.connected = True
            return True
        except Exception as e:
            print(f"[Bridge] 连接异常: {e}")
            return False

    def read_full_state(self):
        """读取所有状态"""
        if not self.connected: return None
        return {
            "price": self.data.market_price,
            "cpp_status": self.data.cpp_status,
            "usdt": self.data.available_balance_usdt,
            "bnb_pos": self.data.current_position_amount,
            "p_prec": self.data.price_precision,
            "q_prec": self.data.quantity_precision
        }

    def send_order(self, symbol_str, price, amount, hard_stop, active_gap, callback):
        """发送下单指令"""
        if not self.connected: return
        
        # 1. 设置参数
        self.data.strategy_status = 2 # 开火
        
        # 处理 Symbol 字符串转字节数组
        encoded_symbol = symbol_str.encode('utf-8')
        # 确保不超过16字节，并填充到 ctypes 数组
        ctypes_symbol = (ctypes.c_char * 16)(*encoded_symbol)
        self.data.symbol = ctypes_symbol
        
        self.data.target_price = price
        self.data.target_amount = amount
        
        # 棘轮参数
        self.data.hard_stop_price = hard_stop
        self.data.ratchet_active_gap = active_gap
        self.data.ratchet_callback = callback
        
        # 2. 版本号自增 (触发 C++ 执行)
        self.data.cmd_version += 1
        print(f"[Bridge] 指令发送完毕 v{self.data.cmd_version}: {symbol_str} Buy {amount} @ {price}")

    def cancel_all(self):
        """一键逃命"""
        if not self.connected: return
        self.data.strategy_status = 99
        self.data.cmd_version += 1
        print("[Bridge] 发送紧急撤单/清仓指令!")

    def close(self):
        if self.mem: self.mem.close()