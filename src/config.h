#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ==================== WiFi 配置 ====================
const char* WIFI_SSID = "sucang";
const char* WIFI_PASSWORD = "Sc12345678";

// ==================== 安全认证配置 ====================
const char* AUTH_USERNAME = "admin";
const char* AUTH_PASSWORD = "wol2024";

// ==================== 远程连接配置 (通过 frp 反向代理) ====================
String REMOTE_URL = "";  // 外网访问地址，如 http://your-server.com/wol

// ==================== WebSocket 远程连接配置 ====================
bool WS_ENABLED = false;
String WS_SERVER = "";     // WebSocket服务器地址，如 ws://server:8080/ws
String WS_USER = "";
String WS_PASS = "";
String WS_TOKEN = "esp32-wol-fixed-token-x9k2m";  // WebSocket 认证令牌，需与服务端一致

// ==================== 设备配置结构 ====================
struct Device {
  String name;
  uint8_t mac[6];
  bool enabled;
};

struct ScannedDevice {
  IPAddress ip;
  uint8_t mac[6];
  String hostname;
};

// ==================== 设备列表 ====================
#define MAX_DEVICES 10
#define MAX_SCAN_RESULTS 64
Device devices[MAX_DEVICES];
ScannedDevice scannedDevices[MAX_SCAN_RESULTS];
int deviceCount = 0;
int scannedDeviceCount = 0;
bool isScanning = false;
int scanProgress = 0;

// ==================== GPIO 配置 ====================
const int BUTTON_PIN = 0;
const int LED_PIN = 2;
const int WOL_PORT = 9;
const int WEB_SERVER_PORT = 80;
const unsigned long DEBOUNCE_TIME = 200;

// ==================== 辅助函数 ====================
String macToString(uint8_t *mac) {
  char buf[18];
  sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

bool parseMac(String macStr, uint8_t *mac) {
  macStr.replace("-", ":");
  macStr.toUpperCase();
  int idx = 0;
  for (int i = 0; i < 6 && idx < macStr.length(); i++) {
    String byteStr = macStr.substring(idx, idx + 2);
    mac[i] = (uint8_t)strtol(byteStr.c_str(), NULL, 16);
    idx += 3;
  }
  return true;
}

void initDefaultDevices() {
  deviceCount = 0;
}

#endif
