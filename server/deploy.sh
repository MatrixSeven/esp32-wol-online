#!/bin/bash
# WOL Server 部署脚本

REMOTE_HOST="root@101.34.56.108"
REMOTE_DIR="/root/app"
BINARY_NAME="wol-server"
PORT=8199
PASSWORD="txJlstWCbaaabSK@"

echo "=== WOL Server 部署脚本 ==="

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
ssh ${REMOTE_HOST} "cd ${REMOTE_DIR} && chmod +x ${BINARY_NAME} && tmux new-session -d -s wol-server './${BINARY_NAME} -port ${PORT} -password ${PASSWORD}'"

echo ""
echo "=== 部署完成 ==="
echo "服务地址: http://101.34.56.108:${PORT}"
echo "查看日志: ssh ${REMOTE_HOST} 'tmux attach -t wol-server'"
