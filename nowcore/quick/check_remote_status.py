import os

import subprocess

from datetime import datetime



# 从 deploy_nowcore.py 获取远程配置

LOCAL_WORK_DIR = r"/home/jintao/文档/myfile/quick"

KEY_PATH = os.path.join(LOCAL_WORK_DIR, "toyo.pem")

REMOTE_USER = "ubuntu"

REMOTE_IP = "35.77.211.180"

REMOTE_BASE_DIR = "/home/ubuntu"

REMOTE_HFT_RUN_DIR = f"{REMOTE_BASE_DIR}/nowcore_run"

REMOTE_EXECUTABLE_NAME = "nowcore_executable"

REMOTE_LOG_FILE = f"{REMOTE_HFT_RUN_DIR}/{REMOTE_EXECUTABLE_NAME}_app.log"



def run_cmd(cmd, check_returncode=True, timeout_seconds=60):

    print(f"\n[{datetime.now().strftime('%H:%M:%S')}] [EXEC] {cmd}")

    try:

        process = subprocess.Popen(cmd, shell=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding='utf-8', errors='ignore')

        stdout, stderr = process.communicate(timeout=timeout_seconds)



        if stdout:

            print(f"[STDOUT] {stdout.strip()}")

        if stderr:

            print(f"[STDERR] {stderr.strip()}")

        

        result = subprocess.CompletedProcess(cmd, process.returncode, stdout, stderr)



        if check_returncode and result.returncode != 0:

            print(f"[ERROR] 命令执行失败，返回码: {result.returncode}")

            return None

        return result



    except subprocess.TimeoutExpired:

        process.kill()

        stdout, stderr = process.communicate()

        print(f"[{datetime.now().strftime('%H:%M:%S')}] [WARNING] 命令超时...")

        return None

    except Exception as e:

        print(f"[ERROR] 未知错误: {e}")

        return None



def check_remote_status():

    print(f"\n[{datetime.now().strftime('%H:%M:%S')}] --- 开始检查远程服务器状态 ---\n")



    # 1. 检查 nowcore_executable 进程

    print(f"[{datetime.now().strftime('%H:%M:%S')}] --- 检查 nowcore_executable 进程 ---\n")



    check_pid_cmd = f'ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i "{KEY_PATH}" {REMOTE_USER}@{REMOTE_IP} "pgrep -f {REMOTE_EXECUTABLE_NAME}"'

    result = run_cmd(check_pid_cmd, check_returncode=False, timeout_seconds=20)



    if result and result.stdout.strip():

        pid = result.stdout.strip()
        print(f"[{datetime.now().strftime('%H:%M:%S')}] [SUCCESS] nowcore_executable 正在运行，PID: {pid}")

        # 检查 CPU 使用率
        check_cpu_cmd = f'ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i "{KEY_PATH}" {REMOTE_USER}@{REMOTE_IP} "ps -p {pid} -o %cpu --no-headers"'
        cpu_result = run_cmd(check_cpu_cmd, check_returncode=False, timeout_seconds=10)
        if cpu_result and cpu_result.stdout.strip():
            try:
                cpu_usage = float(cpu_result.stdout.strip())
                print(f"[{datetime.now().strftime('%H:%M:%S')}] [INFO] nowcore_executable CPU 使用率: {cpu_usage:.2f}%")
                if cpu_usage > 90.0: # 设定一个高CPU使用率阈值
                    print(f"[{datetime.now().strftime('%H:%M:%S')}] [WARNING] nowcore_executable CPU 使用率过高！")
            except ValueError:
                print(f"[{datetime.now().strftime('%H:%M:%S')}] [WARNING] 无法解析 CPU 使用率数据: {cpu_result.stdout.strip()}")
        else:
            print(f"[{datetime.now().strftime('%H:%M:%S')}] [WARNING] 无法获取 nowcore_executable 的 CPU 使用率。")

    else:

        print(f"[{datetime.now().strftime('%H:%M:%S')}] [WARNING] 未检测到 nowcore_executable 进程。")

    

    # 2. 检查 nowcore_executable 日志文件 (保持不变)

    print(f"\n[{datetime.now().strftime('%H:%M:%S')}] --- 检查 nowcore_executable 日志文件 ---\n")

    read_log_cmd = f'ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i "{KEY_PATH}" {REMOTE_USER}@{REMOTE_IP} "tail -n 50 {REMOTE_LOG_FILE}"'

    result = run_cmd(read_log_cmd, check_returncode=False, timeout_seconds=20)

    if result and result.stdout.strip():

        print(f"[{datetime.now().strftime('%H:%M:%S')}] [INFO] nowcore_executable 日志内容:")

        print(f"[LOG_CONTENT]\n{result.stdout.strip()}")

        # 搜索日志中的错误

        if "FATAL" in result.stdout or "ERROR" in result.stdout:

            print(f"[{datetime.now().strftime('%H:%M:%S')}] [WARNING] 日志中包含 FATAL 或 ERROR 关键字。")

    else:

        print(f"[{datetime.now().strftime('%H:%M:%S')}] [INFO] nowcore_executable 日志文件为空或不存在。")



    # 3. 列出 nowcore_run 目录内容

    print(f"\n[{datetime.now().strftime('%H:%M:%S')}] --- 列出远程 nowcore_run 目录内容 ---\n")

    list_run_dir_cmd = f'ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i "{KEY_PATH}" {REMOTE_USER}@{REMOTE_IP} "ls -F {REMOTE_HFT_RUN_DIR}"'

    result = run_cmd(list_run_dir_cmd, check_returncode=False, timeout_seconds=20)

    if result and result.stdout.strip():

        print(f"[RUN_DIR_CONTENT]\n{result.stdout.strip()}")

    else:

        print(f"[{datetime.now().strftime('%H:%M:%S')}] [WARNING] 无法列出 nowcore_run 目录或目录为空/不存在。")



    print(f"\n[{datetime.now().strftime('%H:%M:%S')}] --- 远程服务器状态检查完成 ---\n")



if __name__ == "__main__":

    check_remote_status()

