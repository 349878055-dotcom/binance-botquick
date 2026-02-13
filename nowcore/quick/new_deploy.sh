#!/bin/bash

# =========================================
# 核心配置区：请根据你的实际情况修改以下变量
# =========================================

# 远程服务器配置
REMOTE_USER="ubuntu"
REMOTE_IP="13.115.197.173"
KEY_PATH="/home/jintao/文档/myfile/quick/toyo.pem"  # 密钥路径 (转换为 Linux 风格)

# 本地项目配置
LOCAL_PROJECT_DIR="/home/jintao/文档/myfile/quick"  # 你的 quick 文件夹
PROJECT_NAME="nowcore"
EXECUTABLE_NAME="nowcore_executable"
SHM_COMMANDER_EXECUTABLE="shm_commander_executable"

# 远程目录配置
REMOTE_SOURCE_DIR="/home/ubuntu/nowcore_source"
REMOTE_RUN_DIR="/home/ubuntu/nowcore_run"

# =========================================
# 部署脚本
# =========================================

# 1. 云端彻底清理 (杀进程 + 删文件 + 刷缓存)
echo "--- 1. 云端彻底清理 (杀进程 + 删文件 + 刷缓存) ---"
REMOTE_STOP_COMMAND="sudo pkill -9 -f '${EXECUTABLE_NAME}' || true; sudo pkill -f '${SHM_COMMANDER_EXECUTABLE}' || true; rm -rf '${REMOTE_SOURCE_DIR}' '${REMOTE_RUN_DIR}'; sudo sync; echo 3 | sudo tee /proc/sys/vm/drop_caches"
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i "$KEY_PATH" "$REMOTE_USER@$REMOTE_IP" "$REMOTE_STOP_COMMAND"

# 2. 准备源码环境
echo "--- 2. 准备源码环境 ---"
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i "$KEY_PATH" "$REMOTE_USER@$REMOTE_IP" "mkdir -p '${REMOTE_SOURCE_DIR}' '${REMOTE_RUN_DIR}'"

# 3. 打包并传输 (请确保你在 quick 目录下运行)
echo "--- 3. 打包并传输 (请确保你在 quick 目录下运行) ---"
cd "$LOCAL_PROJECT_DIR"
tar -czvf nowcore.tar.gz "$PROJECT_NAME"
scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i "$KEY_PATH" nowcore.tar.gz "$REMOTE_USER@$REMOTE_IP:$REMOTE_SOURCE_DIR/"

# 4. 云端解压与编译
echo "--- 4. 云端解压与编译 ---"
REMOTE_COMPILE_COMMAND=" \
    tar -xzvf '${REMOTE_SOURCE_DIR}'/nowcore.tar.gz -C '${REMOTE_SOURCE_DIR}'; \
    cd '${REMOTE_SOURCE_DIR}'/'${PROJECT_NAME}'; \
    sudo apt-get update && sudo apt-get install -y libcurl4-openssl-dev libssl-dev dos2unix cmake; \
    cmake . && make -j\$(nproc); \
    cp nowcore '${REMOTE_RUN_DIR}'/'${EXECUTABLE_NAME}'; \
    g++ -o '${REMOTE_RUN_DIR}'/'${SHM_COMMANDER_EXECUTABLE}' '${REMOTE_SOURCE_DIR}'/'${PROJECT_NAME}'/shm_commander.cpp \
"
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i "$KEY_PATH" "$REMOTE_USER@$REMOTE_IP" "$REMOTE_COMPILE_COMMAND"

# 5. 赋权并暴力启动 (最高优先级)
echo "--- 5. 赋权并暴力启动 (最高优先级) ---"
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i "$KEY_PATH" "$REMOTE_USER@$REMOTE_IP" "sudo setcap cap_net_raw,cap_net_admin,cap_sys_nice+eip ${REMOTE_RUN_DIR}/${EXECUTABLE_NAME}"
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i "$KEY_PATH" "$REMOTE_USER@$REMOTE_IP" "sudo chrt -f 99 ${REMOTE_RUN_DIR}/${EXECUTABLE_NAME} > /home/ubuntu/${EXECUTABLE_NAME}_app.log 2>&1 &"
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i "$KEY_PATH" "$REMOTE_USER@$REMOTE_IP" "nohup ${REMOTE_RUN_DIR}/${SHM_COMMANDER_EXECUTABLE} > /dev/null 2>&1 &"

# 6. 检查状态
echo "--- 6. 检查状态 ---"
sleep 5 # 等待进程启动
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i "$KEY_PATH" "$REMOTE_USER@$REMOTE_IP" "pgrep -lf '${EXECUTABLE_NAME}' || pgrep -lf '${SHM_COMMANDER_EXECUTABLE}'"

echo "--- 部署完成 ---"