#!/bin/bash
# WOL Server 部署脚本
# 用法: ./deploy.sh <服务器IP> [用户名] [端口] [密码]
# 示例: ./deploy.sh 1.2.3.4
#        ./deploy.sh 1.2.3.4 root 8199 mypassword

if [ -z "$1" ]; then
  echo "用法: ./deploy.sh <服务器IP> [用户名] [端口] [密码]"
  echo "示例: ./deploy.sh 1.2.3.4"
  echo "       ./deploy.sh 1.2.3.4 root 8199 mypassword"
  exit 1
fi

SERVER_IP="$1"
REMOTE_USER="${2:-root}"
REMOTE_DIR="/root/app"
BINARY_NAME="wol-server"
PORT="${3:-8199}"
PASSWORD="$4"

REMOTE_HOST="${REMOTE_USER}@${SERVER_IP}"

echo "=== WOL Server 部署脚本 ==="
echo "目标: ${REMOTE_HOST}"

# 1. 编译
echo "[1/4] 编译 Linux AMD64 版本..."
CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -ldflags="-s -w" -o ./${BINARY_NAME}
echo "编译完成"

# 2. 停止远程服务并等待
echo "[2/4] 停止远程服务..."
ssh ${REMOTE_HOST} "tmux kill-session -t wol-server 2>/dev/null; pkill -f '${BINARY_NAME}' 2>/dev/null; sleep 1; echo '已停止'" || true

# 3. 确保目录存在并上传
echo "[3/4] 上传到远程服务器..."
ssh ${REMOTE_HOST} "mkdir -p ${REMOTE_DIR}"
scp ./${BINARY_NAME} ${REMOTE_HOST}:${REMOTE_DIR}/${BINARY_NAME}
echo "上传完成"

# 4. 启动服务
echo "[4/4] 启动远程服务..."
if [ -n "$PASSWORD" ]; then
  ssh ${REMOTE_HOST} "cd ${REMOTE_DIR} && chmod +x ${BINARY_NAME} && tmux new-session -d -s wol-server './${BINARY_NAME} -port ${PORT} -password ${PASSWORD}'"
else
  ssh ${REMOTE_HOST} "cd ${REMOTE_DIR} && chmod +x ${BINARY_NAME} && tmux new-session -d -s wol-server './${BINARY_NAME} -port ${PORT}'"
fi

echo ""
echo "=== 部署完成 ==="
echo "服务地址: http://${SERVER_IP}:${PORT}"
echo "查看日志: ssh ${REMOTE_HOST} 'tmux attach -t wol-server'"
