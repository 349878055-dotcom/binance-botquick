import mmap
import ctypes
import os
import time

class RingBufferOverflowError(Exception):
    """环形缓冲区溢出异常"""
    pass

class CommandBufferFullError(Exception):
    """指令缓冲区已满异常"""
    pass


# ==============================================================================
# 1. 物理层配置 (Physical Layer)
# ==============================================================================
# 必须与 C++ Master_Logic_Bridge.h 保持 1:1 的原子级一致
# ------------------------------------------------------------------------------
RING_BUFFER_SIZE = 8192
RING_BUFFER_MASK = RING_BUFFER_SIZE - 1

COMMAND_CAPACITY = 128
COMMAND_MASK = COMMAND_CAPACITY - 1

EVENT_CAPACITY = 1024
EVENT_MASK = EVENT_CAPACITY - 1

# ==============================================================================
# 2. 内存帧结构 (Data Frames) - 这里的 Padding 是关键！
# ==============================================================================

class MarketFrame(ctypes.Structure):
    """
    对应 C++ 的 MarketFrame (alignas(64))
    """
    _fields_ = [
        ("timestamp",       ctypes.c_uint64),
        ("price",           ctypes.c_double),
        # ("bid_p",           ctypes.c_double), # 【注释】不再使用盘口深度
        # ("ask_p",           ctypes.c_double), # 【注释】不再使用盘口深度
        ("quantity",        ctypes.c_double),
        # ("bid_q",           ctypes.c_double), # 【注释】不再使用盘口深度
        # ("ask_q",           ctypes.c_double), # 【注释】不再使用盘口深度
        ("type",            ctypes.c_int),
        ("side",            ctypes.c_int),
        ("padding",         ctypes.c_char * 32) # 填充以保持 64 字节对齐
    ]

class CommandFrame(ctypes.Structure):
    """
    对应 C++ 的 CommandFrame (alignas(64))
    """
    _fields_ = [
        ("request_id",           ctypes.c_uint64),
        ("trigger_ms",           ctypes.c_uint64),
        ("client_order_id",      ctypes.c_char * 32),
        ("parent_order_id",      ctypes.c_char * 32),
        ("symbol",               ctypes.c_char * 16),
        ("action",               ctypes.c_int),
        ("type",                 ctypes.c_int),
        ("side",                 ctypes.c_int),
        # ("tif",                  ctypes.c_int), # 【删除】不再使用 TIF
        # ("price",                ctypes.c_double), # 【删除】不再使用价格
        ("quantity",             ctypes.c_double),
        ("padding",              ctypes.c_char * 12) # 调整填充以匹配 C++ 大小
    ]

class OrderEventFrame(ctypes.Structure):
    """
    对应 C++ 的 OrderEventFrame (alignas(64))
    注意 C++ 内部的自然对齐 (Natural Alignment)
    """
    _fields_ = [
        ("timestamp",       ctypes.c_uint64),
        ("client_order_id", ctypes.c_char * 32),
        # ("exch_order_id",   ctypes.c_char * 32), # 【删除】交易所订单ID
        ("event_type",      ctypes.c_int),
        ("side",            ctypes.c_int),       # 【新增】订单方向 (1=Buy, -1=Sell)
        ("fill_price",      ctypes.c_double),
        ("fill_qty",        ctypes.c_double),
        ("remaining_qty",   ctypes.c_double),
        ("error_code",      ctypes.c_int),
        ("error_msg",       ctypes.c_char * 64),
        ("last_update_id",  ctypes.c_uint64),
        ("is_maker",        ctypes.c_bool),
        ("trigger_ms",      ctypes.c_uint64),
        ("parent_order_id", ctypes.c_char * 32),
        ("padding",         ctypes.c_char * 67)    # 调整填充以匹配 C++ 大小 (256 字节)
    ]

# ==============================================================================
# 3. 环形缓冲区结构 (Ring Buffers)
# ==============================================================================

class MarketRingBuffer(ctypes.Structure):
    _fields_ = [
        ("write_index", ctypes.c_uint64),
        ("padding",     ctypes.c_char * 56), # 补齐到 64 字节，防止伪共享
        ("frames",      MarketFrame * RING_BUFFER_SIZE)
    ]

class CommandRingBuffer(ctypes.Structure):
    _fields_ = [
        ("write_idx",   ctypes.c_uint64),
        ("read_idx",    ctypes.c_uint64),
        ("frames",      CommandFrame * COMMAND_CAPACITY),
        ("padding",     ctypes.c_char * 48) # 显式添加 48 字节填充
    ]

class AccountRingBuffer(ctypes.Structure):
    _fields_ = [
        ("write_idx",   ctypes.c_uint64),
        ("read_idx",    ctypes.c_uint64),
        ("frames",      OrderEventFrame * EVENT_CAPACITY),
        # 尾部的快照数据
        ("usdt_balance", ctypes.c_double),
        ("position_amt", ctypes.c_double),
        ("padding",      ctypes.c_char * 32) # 显式添加 32 字节填充 (修正)
    ]

# ==============================================================================
# 4. 总线接口 (Main Interface)
# ==============================================================================

class GenericShmStruct(ctypes.Structure):
    _fields_ = [
        ("market_ring",    MarketRingBuffer),
        ("command_ring",   CommandRingBuffer),
        ("account_feed",   AccountRingBuffer),
        ("cpp_alive",      ctypes.c_bool),
        ("py_alive",       ctypes.c_bool),
        ("price_precision",ctypes.c_int),
        ("quantity_precision", ctypes.c_int),
        ("ratchet_active_gap", ctypes.c_double),
        ("ratchet_callback",   ctypes.c_double),
        ("hard_stop_price",    ctypes.c_double),
        ("system_health",  ctypes.c_uint64),
        ("strategy_status",ctypes.c_int),
        ("padding",        ctypes.c_char * 10) # 调整填充以匹配 C++ 大小 (809216)
    ]

class Bridge:
    def __init__(self, shm_name="nowcore_bridge"):
        # 在 Linux 下，shm_open 创建的文件通常在 /dev/shm/
        self.shm_path = f"/dev/shm/{shm_name}"
        self.mem = None
        self.data = None
        self.connected = False
        
        # 本地记录读到了哪里 (追赶模式用)
        self.local_market_read_idx = 0
        self.local_account_read_idx = 0
        self.req_id_counter = 1

    def connect(self):
        """连接到 C++ 创建的共享内存"""
        if not os.path.exists(self.shm_path):
            return False
        try:
            # 打开文件，权限为 读+写
            f = open(self.shm_path, "r+b")
            # 内存映射
            self.mem = mmap.mmap(f.fileno(), ctypes.sizeof(GenericShmStruct))
            # 将裸字节转换为结构体对象
            self.data = GenericShmStruct.from_buffer(self.mem)
            self.connected = True
            
            # 告诉 C++ 我来了
            self.data.py_alive = True
            
            # 【重要】初始化时，直接跳到最新的数据，忽略历史陈旧数据
            # 除非你想回放之前的行情，否则从 current write index 开始读
            self.local_market_read_idx = self.data.market_ring.write_index
            self.local_account_read_idx = self.data.account_feed.write_idx
            
            return True
        except Exception as e:
            print(f"[Bridge] 连接失败: {e}")
            return False

    def fetch_market_stream(self):
        """
        生成器：流式获取新到达的市场数据
        Brain 调用这个函数，会不断吐出新的 frame，直到追上 C++ 的进度
        """
        if not self.connected: return

        # 1. 看一眼 C++ 写到哪了
        remote_write_idx = self.data.market_ring.write_index
        
        # 【新增逻辑】检测环形缓冲区是否溢出
        # 如果 write_index 超过 local_market_read_idx + RING_BUFFER_SIZE，说明发生了覆盖
        if remote_write_idx - self.local_market_read_idx > RING_BUFFER_SIZE:
            msg = f"MarketRingBuffer 发生溢出！Python 消费速度跟不上 C++ 生产速度。丢失了 {remote_write_idx - self.local_market_read_idx - RING_BUFFER_SIZE} 条数据。将从最新的数据开始追赶。"
            self.local_market_read_idx = remote_write_idx # 直接跳到最新的写入位置
            raise RingBufferOverflowError(msg)


        while self.local_market_read_idx < remote_write_idx:
            # 计算环形位置
            idx = self.local_market_read_idx & RING_BUFFER_MASK
            frame = self.data.market_ring.frames[idx]
            
            # 3. 把数据吐给 Brain
            yield frame
            
            # 4. 指针前进
            self.local_market_read_idx += 1

    def fetch_account_stream(self):
        """
        生成器：获取新的订单反馈"""
        if not self.connected: return
        
        remote_write_idx = self.data.account_feed.write_idx

        # 【新增逻辑】检测环形缓冲区是否溢出
        if remote_write_idx - self.local_account_read_idx > EVENT_CAPACITY:
            msg = f"AccountRingBuffer 发生溢出！Python 消费速度跟不上 C++ 生产速度。丢失了 {remote_write_idx - self.local_account_read_idx - EVENT_CAPACITY} 条订单事件数据。将从最新的数据开始追赶。"
            self.local_account_read_idx = remote_write_idx # 直接跳到最新的写入位置
            self.data.account_feed.read_idx = self.local_account_read_idx # 同时更新共享内存中的 read_idx
            raise RingBufferOverflowError(msg)


        while self.local_account_read_idx < remote_write_idx:
            idx = self.local_account_read_idx & EVENT_MASK
            frame = self.data.account_feed.frames[idx]
            yield frame
            
            # 告诉 C++ 我读过了 (更新 read_idx)
            self.local_account_read_idx += 1
            self.data.account_feed.read_idx = self.local_account_read_idx

    # def send_limit_order(self, symbol, side, price, quantity, market_timestamp_start, tif_type, parent_order_id=""): # 【删除】不再使用限价单
    #     """
    #     发送限价单
    #     tif_type: 1=GTC(挂单), 2=IOC(立即成交或取消), 3=FOK(全成或全撤)
    #     """
    #     if not self.connected: return
        
    #     # 1. 抢占一个槽位
    #     w_idx = self.data.command_ring.write_idx
    #     r_idx = self.data.command_ring.read_idx # 读取 C++ 的读取索引

    #     # 【新增逻辑】物理边界检查：确保环形缓冲区有空间写入新指令
    #     # 如果当前写入位置已经追上了读取位置，说明缓冲区已满，无法写入新指令
    #     if (w_idx - r_idx) >= COMMAND_CAPACITY:
    #         msg = f"CommandRingBuffer 已满！无法发送新指令。当前写入索引: {w_idx}, 读取索引: {r_idx}, 容量: {COMMAND_CAPACITY}"
    #         raise CommandBufferFullError(msg)

   
    #     idx = w_idx & COMMAND_MASK
    #     frame = self.data.command_ring.frames[idx]
        
    #     # 2. 填数据 (先填数据!)
    #     frame.request_id = self.req_id_counter
    #     frame.trigger_ms = market_timestamp_start # 修改为 trigger_ms
    #     self.req_id_counter += 1
        
    #     # 记录并返回这个 Client Order ID，给 Brain 用
    #     cid_str = f"TX_{time.time_ns()}_{self.req_id_counter}"
    #     frame.client_order_id = cid_str.encode('utf-8')
    #     frame.parent_order_id = parent_order_id.encode('utf-8')
    #     
    #     frame.symbol = symbol.encode('utf-8')
    #     frame.action = 1 # ACT_NEW
    #     frame.type = 1   # ORD_LIMIT
    #     frame.side = 1 if side == "BUY" else -1
    #     
    #     # --- [重点修改] 使用传入的 TIF 参数 ---
    #     # frame.tif = tif_type  # 【删除】不再使用 TIF
    #     
    #     # frame.price = float(price) # 【删除】不再使用价格
    #     frame.quantity = float(quantity)
    #     
    #     # 3. 推送索引 (最后改索引!)
    #     self.data.command_ring.write_idx = w_idx + 1
    #     
    #     return cid_str # 返回 ID 方便追踪

    def send_market_order(self, symbol, side, quantity, market_timestamp_start, parent_order_id=""):
        """
        发送市价单
        """
        if not self.connected: return

        w_idx = self.data.command_ring.write_idx
        r_idx = self.data.command_ring.read_idx

        if (w_idx - r_idx) >= COMMAND_CAPACITY:
            msg = f"CommandRingBuffer 已满！无法发送新指令。当前写入索引: {w_idx}, 读取索引: {r_idx}, 容量: {COMMAND_CAPACITY}"
            raise CommandBufferFullError(msg)


        idx = w_idx & COMMAND_MASK
        frame = self.data.command_ring.frames[idx]

        frame.request_id = self.req_id_counter
        frame.trigger_ms = market_timestamp_start
        self.req_id_counter += 1
        
        cid_str = f"TX_{time.time_ns()}_{self.req_id_counter}"
        frame.client_order_id = cid_str.encode('utf-8')
        frame.parent_order_id = parent_order_id.encode('utf-8')
        
        frame.symbol = symbol.encode('utf-8')
        frame.action = 1 # ACT_NEW
        frame.type = 2   # ORD_MARKET
        frame.side = 1 if side == "BUY" else -1
        
        # 市价单不需要价格和TIF
        # frame.tif = 0 # 或其他表示不适用 【删除】不再使用 TIF
        # frame.price = 0.0  【删除】不再使用价格
        frame.quantity = float(quantity)
        
        self.data.command_ring.write_idx = w_idx + 1
        
        return cid_str

    def close(self):
        if self.connected:
            self.data.py_alive = False
        if self.mem:
            self.mem.close()