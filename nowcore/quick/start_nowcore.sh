#!/bin/bash
# 1. 强制对齐库路径，防止提权后的路径丧失
export LD_LIBRARY_PATH=/usr/local/lib:/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH

# 2. 动态探测并锁定 OpenSSL 配置（解决 OpenSSL 3.0 环境崩溃）
export OPENSSL_MODULES=$(find /usr/lib -name ossl-modules -type d | head -n 1)
CONF_PATH=$(openssl version -d | cut -d'"' -f2)
export OPENSSL_CONF="${CONF_PATH}/openssl.cnf"

# 3. 提升系统物理极限 (这些命令可能需要 root 权限，请在交互式 shell 中手动运行或配置 sudoers)
ulimit -c unlimited  # 允许产生 Core Dump（崩溃时留证据）
# ulimit -l unlimited # 这行通常需要 sudo，暂时移除，建议用户手动执行或配置 sudoers
# setcap 'cap_net_raw,cap_net_admin,cap_sys_nice+eip' /home/ubuntu/nowcore_run/nowcore_executable # 这行也需要 sudo，暂时移除

# 进入程序运行目录
cd /home/ubuntu/nowcore_run/

# 4. 绑核起爆：强制将程序压入 CPU 0 号核心
echo "[Shell] Launching NowCore on Core 0..."
taskset -c 0 ./nowcore_executable