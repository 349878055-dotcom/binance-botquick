#!/bin/bash
cd nowcore
nohup ./sentinel_v1 > sentinel_v1.log 2>&1 &
sleep 20 # 增加等待时间，确保 sentinel_v1 完全启动并写入市场价格到共享内存
./shm_commander
