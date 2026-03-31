# ESP32 WOL 远程唤醒器

基于 ESP32-S3 的 Wake-on-LAN 远程唤醒系统，支持通过公网服务器远程控制局域网设备唤醒。

## 功能特性

- **远程唤醒** - 通过 WebSocket 连接公网服务器，实现外网唤醒局域网设备
- **设备管理** - 添加、删除、唤醒已保存的设备
- **局域网扫描** - 多轮 ARP 扫描，自动发现局域网设备，mDNS 识别主机名
- **广播唤醒** - 一键向整个局域网发送广播唤醒包
- **状态监控** - 实时显示运行时间、唤醒次数、温度、WiFi 信号、内存使用等
- **BLE 防休眠** - 定时通过蓝牙发送按键，防止 PC 自动休眠
- **双界面** - ESP32 本地 Web 界面 + Go 服务器远程界面
- **深色/浅色主题** - 支持主题切换，自动保存偏好
- **OAuth 登录** - 支持微信 OAuth 登录
- **灵活配置** - 支持命令行参数、环境变量、.env 文件三种配置方式

## 系统架构

```
┌─────────────┐     WebSocket      ┌─────────────┐
│   ESP32-S3  │◄──────────────────►│  Go Server  │
│  (局域网)   │                    │  (公网服务器) │
└─────────────┘                    └─────────────┘
       │                                  │
       │ WOL Magic Packet                 │ Web UI
       ▼                                  ▼
┌─────────────┐                    ┌─────────────┐
│  局域网设备  │                    │  用户浏览器  │
└─────────────┘                    └─────────────┘
```

## 硬件要求

- ESP32-S3-DevKitC-1 开发板
- 可选：按钮（连接 BOOT 引脚，用于物理唤醒）

## 项目结构

```
esp32-wol/
├── src/
│   ├── main.cpp          # 主程序
│   ├── config.h          # 配置文件
│   ├── ble_keyboard.h    # BLE 蓝牙键盘
│   └── web_server.h      # Web 界面 HTML
├── server/
│   ├── main.go           # Go WebSocket 服务器
│   ├── templates/
│   │   └── index.html    # 远程 Web 界面
│   ├── deploy.sh         # 部署脚本
│   ├── .env.example      # 环境变量示例
│   ├── go.mod
│   └── go.sum
├── platformio.ini        # PlatformIO 配置
└── README.md
```

## 快速开始

### 1. 配置 ESP32

编辑 `src/config.h`，设置 WiFi 和认证信息：

```cpp
const char* WIFI_SSID = "your-wifi-ssid";
const char* WIFI_PASSWORD = "your-wifi-password";
const char* AUTH_USERNAME = "admin";
const char* AUTH_PASSWORD = "your-password";
```

同时设置 WebSocket Token（需与服务器端一致）：

```cpp
String WS_TOKEN = "your-secret-token";
```

### 2. 编译上传 ESP32

```bash
# 编译
pio run

# 上传（连接 ESP32 后）
pio run -t upload

# 查看串口输出
pio device monitor
```

### 3. 配置 Go 服务器

复制环境变量示例文件：

```bash
cd server
cp .env.example .env
```

编辑 `.env` 文件，填入你的配置：

```bash
# 服务器监听端口
WOL_PORT=8199

# admin 用户密码（必改）
WOL_PASSWORD=your_secure_password

# WebSocket 固定访问令牌，ESP32 连接使用（必改）
WOL_TOKEN=your_secret_token

# OAuth 配置（可选）
WOL_OAUTH_APP_ID=
WOL_OAUTH_APP_SECRET=
WOL_OAUTH_USER_ID=
```

### 4. 部署 Go 服务器

```bash
cd server

# 基本用法（只传 IP，使用 .env 中的配置）
./deploy.sh your-server-ip

# 完整参数
./deploy.sh <服务器IP> [用户名] [端口] [密码]

# 示例
./deploy.sh 1.2.3.4
./deploy.sh 1.2.3.4 root 8199 your_password
```

### 5. 配置 ESP32 连接远程服务器

访问 ESP32 的 Web 界面（通过串口查看 IP），在设置中：
- 启用远程连接
- 填写 WebSocket 服务器地址：`ws://your-server:8199/ws`
- 填写 Token（与服务器端一致）

## 服务器配置

Go 服务器支持三种配置方式（优先级从高到低）：

| 优先级 | 方式 | 示例 |
|--------|------|------|
| 1 | 命令行参数 | `-port 8199 -password xxx` |
| 2 | 环境变量 | `WOL_PORT=8199` |
| 3 | .env 文件 | `WOL_PORT=8199` |
| 4 | 默认值 | `8080` |

### 环境变量

| 变量名 | 说明 | 必填 |
|--------|------|------|
| `WOL_PORT` | 监听端口（默认 `8080`） | 否 |
| `WOL_PASSWORD` | admin 密码 | **是** |
| `WOL_TOKEN` | WebSocket 令牌 | **是** |
| `WOL_OAUTH_APP_ID` | OAuth 应用 ID | 否 |
| `WOL_OAUTH_APP_SECRET` | OAuth 应用密钥 | 否 |
| `WOL_OAUTH_USER_ID` | OAuth 允许的用户 ID | 否 |

### 命令行参数

```bash
./wol-server -port 8199 \
  -password "your_password" \
  -token "your_token" \
  -app-id "100003" \
  -app-secret "your_secret" \
  -user-id "your_user_id"
```

## API 接口

### ESP32 HTTP API

| 接口 | 方法 | 说明 |
|------|------|------|
| `/` | GET | Web 界面 |
| `/wake?index=N` | GET | 唤醒指定设备 |
| `/wake?all=1` | GET | 唤醒所有已保存设备 |
| `/wake/broadcast` | GET | 广播唤醒整个局域网 |
| `/add?name=&mac=` | GET | 添加设备 |
| `/delete?index=N` | GET | 删除设备 |
| `/list` | GET | 获取设备列表 |
| `/scan` | GET | 启动局域网扫描 |
| `/scan/results` | GET | 获取扫描结果 |
| `/status` | GET | 获取系统状态 |
| `/settings` | GET | 获取远程连接设置 |
| `/settings/save` | GET | 保存远程连接设置 |

### WebSocket 命令

| 命令 | 说明 |
|------|------|
| `get_devices` | 获取设备列表 |
| `wake` | 唤醒设备 |
| `wake_broadcast` | 广播唤醒 |
| `add_device` | 添加设备 |
| `delete_device` | 删除设备 |
| `scan` | 启动扫描 |
| `get_scan_status` | 获取扫描状态 |
| `get_status` | 获取状态 |

## 技术栈

- **ESP32 固件**
  - Arduino Framework
  - HijelHID_BLEKeyboard (NimBLE)
  - ArduinoWebsockets
  - ArduinoJson 7.x
  - ESP32Ping
  - ESPmDNS

- **Go 服务器**
  - gorilla/websocket
  - golang-jwt/jwt
  - godotenv

## 许可证

MIT License
