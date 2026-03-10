/*
 * ESP32-S3 Wake-on-LAN 唤醒器 v3.0
 * 支持多设备管理、局域网扫描、HTTP API
 * 外网访问: WebSocket 客户端连接公网服务器
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>
#include <ESP32Ping.h>
#include <lwip/etharp.h>
#include <lwip/netif.h>
#include "config.h"
#include "web_server.h"

using namespace websockets;

WebServer server(WEB_SERVER_PORT);
WiFiUDP udp;
Preferences preferences;
WebsocketsClient *wsClient = nullptr;

volatile bool buttonPressed = false;
unsigned long lastButtonTime = 0;
bool ledState = false;
unsigned long lastLedToggle = 0;
unsigned long lastWsReconnect = 0;
unsigned long lastWsHeartbeat = 0;
bool wsConnecting = false;
unsigned int wakeCount = 0;

// ==================== 函数声明 ====================
void loadDevices();
void saveDevices();
void loadSettings();
void saveSettings();
void connectWiFi();
void sendWOL(uint8_t *mac);
void flashLED(int times);
void updateLED();
void printDeviceList();
bool checkAuth();
bool getMacFromARP(IPAddress ip, uint8_t *mac);
void startNetworkScan();
String generateDeviceList();
void IRAM_ATTR buttonISR();
void initWebSocket();
void connectWebSocket();
void handleWebSocket();
void onWsMessage(WebsocketsMessage message);
void sendWsResponse(String requestId, bool success, const char* message, JsonDocument* data = nullptr);
String handleWsCommand(JsonDocument& doc);
void updateWsStatus();

// ==================== Setup ====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=================================");
  Serial.println("  ESP32-S3 WOL 唤醒器 v3.0");
  Serial.println("=================================\n");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  loadSettings();
  loadDevices();
  if (deviceCount == 0) initDefaultDevices();

  connectWiFi();

  // WiFi 连接成功后再初始化 WebSocket
  if (WS_ENABLED && WiFi.status() == WL_CONNECTED) {
    delay(1000);  // 等待网络栈完全就绪
    initWebSocket();
  }

  if (!MDNS.begin("esp32-wol")) {
    Serial.println("mDNS 启动失败");
  } else {
    Serial.println("mDNS: esp32-wol.local");
  }

  // Web 路由
  server.on("/", []() {
    String html = INDEX_HTML;
    html.replace("%IP_ADDR%", WiFi.localIP().toString());
    html.replace("%DEVICE_LIST%", generateDeviceList());
    server.send(200, "text/html", html);
  });

  server.on("/wake", []() {
    if (!checkAuth()) return;
    String all = server.arg("all");
    String idxStr = server.arg("index");

    if (all == "1") {
      // 唤醒所有已保存设备
      for (int i = 0; i < deviceCount; i++) { sendWOL(devices[i].mac); delay(50); }
      server.send(200, "application/json", "{\"success\":true,\"device\":\"所有设备\"}");
    } else if (idxStr != "") {
      int idx = idxStr.toInt();
      if (idx >= 0 && idx < deviceCount) {
        sendWOL(devices[idx].mac);
        server.send(200, "application/json", "{\"success\":true,\"device\":\"" + devices[idx].name + "\"}");
      } else {
        server.send(200, "application/json", "{\"success\":false,\"message\":\"无效索引\"}");
      }
    } else {
      server.send(200, "application/json", "{\"success\":false,\"message\":\"缺少参数\"}");
    }
  });

  // 广播唤醒 - 向整个局域网广播发送 WOL
  server.on("/wake/broadcast", []() {
    if (!checkAuth()) return;

    Serial.println("\n========================================");
    Serial.println("广播唤醒: 向整个局域网发送 WOL 包");
    Serial.println("========================================");

    IPAddress broadcastIP(255, 255, 255, 255);
    IPAddress subnetBroadcast = WiFi.localIP();
    subnetBroadcast[3] = 255;

    int sentCount = 0;

    // 1. 首先唤醒所有已保存的设备
    Serial.println("步骤1: 唤醒已保存设备列表...");
    for (int i = 0; i < deviceCount; i++) {
      sendWOL(devices[i].mac);
      sentCount++;
      delay(50);
      Serial.printf("  [%d] %s -> %s\n", i + 1, devices[i].name.c_str(), macToString(devices[i].mac).c_str());
    }

    // 2. 向广播地址发送通用 WOL 包 (多次)
    Serial.println("\n步骤2: 向广播地址发送通用唤醒包...");

    // 构造通用广播 WOL 包 - 使用子网广播 MAC
    uint8_t packet[102];
    for (int i = 0; i < 6; i++) packet[i] = 0xFF;
    for (int i = 1; i <= 16; i++)
      for (int j = 0; j < 6; j++)
        packet[i * 6 + j] = 0xFF;

    // 发送到多个广播地址
    for (int retry = 0; retry < 5; retry++) {
      // 发送到 255.255.255.255
      udp.beginPacket(broadcastIP, WOL_PORT);
      udp.write(packet, 102);
      udp.endPacket();

      // 发送到子网广播
      udp.beginPacket(subnetBroadcast, WOL_PORT);
      udp.write(packet, 102);
      udp.endPacket();

      delay(100);
      sentCount += 2;
    }

    // 3. 如果有扫描结果，也唤醒扫描到的设备
    if (scannedDeviceCount > 0) {
      Serial.println("\n步骤3: 唤醒扫描发现的设备...");
      for (int i = 0; i < scannedDeviceCount; i++) {
        // 跳过无效 MAC
        if (scannedDevices[i].mac[0] == 0 && scannedDevices[i].mac[1] == 0) continue;

        sendWOL(scannedDevices[i].mac);
        sentCount++;
        delay(30);
        Serial.printf("  [%d] %s\n", i + 1, scannedDevices[i].ip.toString().c_str());
      }
    }

    Serial.printf("\n广播唤醒完成! 共发送 %d 个 WOL 包\n", sentCount);
    Serial.println("========================================\n");

    String json = "{\"success\":true,\"message\":\"广播唤醒已发送\",\"count\":" + String(sentCount) + "}";
    server.send(200, "application/json", json);
  });

  server.on("/add", []() {
    if (!checkAuth()) return;
    String name = server.arg("name");
    String mac = server.arg("mac");

    if (name == "" || mac == "") {
      server.send(200, "application/json", "{\"success\":false,\"message\":\"参数不完整\"}");
      return;
    }
    if (deviceCount >= MAX_DEVICES) {
      server.send(200, "application/json", "{\"success\":false,\"message\":\"设备数量已达上限\"}");
      return;
    }

    devices[deviceCount].name = name;
    parseMac(mac, devices[deviceCount].mac);
    devices[deviceCount].enabled = true;
    deviceCount++;
    saveDevices();
    Serial.printf("添加设备: %s [%s]\n", name.c_str(), mac.c_str());
    server.send(200, "application/json", "{\"success\":true}");
  });

  server.on("/delete", []() {
    if (!checkAuth()) return;
    int idx = server.arg("index").toInt();

    if (idx < 0 || idx >= deviceCount) {
      server.send(200, "application/json", "{\"success\":false}");
      return;
    }

    Serial.printf("删除设备: %s\n", devices[idx].name.c_str());
    for (int i = idx; i < deviceCount - 1; i++) {
      devices[i] = devices[i + 1];
    }
    deviceCount--;
    saveDevices();
    server.send(200, "application/json", "{\"success\":true}");
  });

  server.on("/list", []() {
    String json = "{\"devices\":[";
    for (int i = 0; i < deviceCount; i++) {
      if (i > 0) json += ",";
      json += "{\"name\":\"" + devices[i].name + "\",\"mac\":\"" + macToString(devices[i].mac) + "\"}";
    }
    json += "]}";
    server.send(200, "application/json", json);
  });

  server.on("/scan", []() {
    if (isScanning) {
      server.send(200, "application/json", "{\"success\":false,\"message\":\"扫描进行中\"}");
      return;
    }
    server.send(200, "application/json", "{\"success\":true,\"message\":\"开始扫描\"}");
    delay(100);
    startNetworkScan();
  });

  server.on("/scan/results", []() {
    String json = "{\"scanning\":" + String(isScanning ? "true" : "false") +
                  ",\"progress\":" + String(scanProgress) + ",\"devices\":[";
    for (int i = 0; i < scannedDeviceCount; i++) {
      if (i > 0) json += ",";
      json += "{\"ip\":\"" + scannedDevices[i].ip.toString() +
              "\",\"mac\":\"" + macToString(scannedDevices[i].mac) + "\"" +
              ",\"name\":\"" + scannedDevices[i].hostname + "\"}";
    }
    json += "]}";
    server.send(200, "application/json", json);
  });

  server.on("/settings", []() {
    String json = "{\"ws_enabled\":" + String(WS_ENABLED ? "true" : "false") +
                  ",\"ws_server\":\"" + WS_SERVER + "\"" +
                  ",\"ws_user\":\"" + WS_USER + "\"" +
                  ",\"ws_connected\":" + String((wsClient && wsClient->available()) ? "true" : "false") +
                  ",\"remote_url\":\"" + REMOTE_URL + "\"}";
    server.send(200, "application/json", json);
  });

  server.on("/status", []() {
    // 读取芯片内部温度 (ESP32-S3)
    float tempC = temperatureRead();
    String json = "{\"uptime\":" + String(millis() / 1000) +
                  ",\"wake_count\":" + String(wakeCount) +
                  ",\"wifi_rssi\":" + String(WiFi.RSSI()) +
                  ",\"free_heap\":" + String(ESP.getFreeHeap()) +
                  ",\"temperature\":" + String(tempC, 1) +
                  ",\"ip\":\"" + WiFi.localIP().toString() + "\"" +
                  ",\"mac\":\"" + WiFi.macAddress() + "\"" +
                  ",\"ssid\":\"" + String(WIFI_SSID) + "\"" +
                  ",\"chip_model\":\"" + String(ESP.getChipModel()) + "\"" +
                  ",\"flash_size\":" + String(ESP.getFlashChipSize()) +
                  ",\"device_count\":" + String(deviceCount) +
                  ",\"sdk_version\":\"" + String(ESP.getSdkVersion()) + "\"}";
    server.send(200, "application/json", json);
  });

  server.on("/settings/save", HTTP_GET, []() {
    if (!checkAuth()) return;
    bool wsChanged = false;
    if (server.hasArg("ws_enabled")) {
      bool newEnabled = server.arg("ws_enabled") == "true";
      if (newEnabled != WS_ENABLED) wsChanged = true;
      WS_ENABLED = newEnabled;
    }
    if (server.hasArg("ws_server")) {
      if (WS_SERVER != server.arg("ws_server")) wsChanged = true;
      WS_SERVER = server.arg("ws_server");
    }
    if (server.hasArg("ws_user")) WS_USER = server.arg("ws_user");
    if (server.hasArg("ws_pass")) WS_PASS = server.arg("ws_pass");
    if (server.hasArg("ws_token")) WS_TOKEN = server.arg("ws_token");
    if (server.hasArg("remote_url")) REMOTE_URL = server.arg("remote_url");
    saveSettings();

    // WebSocket 配置变更时重新连接
    if (wsChanged) {
      if (wsClient && wsClient->available()) wsClient->close();
      if (WS_ENABLED && WiFi.status() == WL_CONNECTED) {
        delay(500);
        initWebSocket();
      }
    }

    server.send(200, "application/json", "{\"success\":true}");
  });

  // 静态资源处理
  server.on("/favicon.ico", []() { server.send(204); });
  server.on("/robots.txt", []() { server.send(204); });

  server.onNotFound([]() { server.send(404, "text/plain", "Not Found"); });

  server.begin();
  Serial.printf("Web 服务器: http://%s\n", WiFi.localIP().toString().c_str());

  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);
  Serial.println("系统就绪! 按下 BOOT 按钮唤醒第一个设备\n");
  printDeviceList();
}

// ==================== Loop ====================
void loop() {
  server.handleClient();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi 断开，重连中...");
    connectWiFi();
  }

  // WebSocket 处理
  if (WS_ENABLED) {
    handleWebSocket();
  }

  if (buttonPressed) {
    buttonPressed = false;
    if (deviceCount > 0) {
      Serial.printf("按钮唤醒: %s\n", devices[0].name.c_str());
      sendWOL(devices[0].mac);
      flashLED(3);
    }
  }

  updateLED();
  delay(10);
}

// ==================== 中断 ====================
void IRAM_ATTR buttonISR() {
  if (millis() - lastButtonTime > DEBOUNCE_TIME) {
    buttonPressed = true;
    lastButtonTime = millis();
  }
}

// ==================== 认证 ====================
bool checkAuth() {
  if (!server.authenticate(AUTH_USERNAME, AUTH_PASSWORD)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

// ==================== WiFi ====================
void connectWiFi() {
  Serial.printf("连接 WiFi: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi 连接成功! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi 连接失败!");
  }
}

// ==================== WOL ====================
void sendWOL(uint8_t *mac) {
  uint8_t packet[102];
  for (int i = 0; i < 6; i++) packet[i] = 0xFF;
  for (int i = 1; i <= 16; i++)
    for (int j = 0; j < 6; j++)
      packet[i * 6 + j] = mac[j];

  IPAddress broadcastIP = WiFi.localIP();
  broadcastIP[3] = 255;

  udp.beginPacket(broadcastIP, WOL_PORT);
  udp.write(packet, 102);
  int result = udp.endPacket();

  if (result) wakeCount++;
  Serial.printf("WOL 发送%s: %s\n", result ? "成功" : "失败", macToString(mac).c_str());
}

// ==================== LED ====================
void flashLED(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH); delay(100);
    digitalWrite(LED_PIN, LOW); delay(100);
  }
}

void updateLED() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!ledState) { ledState = true; digitalWrite(LED_PIN, HIGH); }
  } else {
    if (millis() - lastLedToggle > 500) {
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState ? HIGH : LOW);
      lastLedToggle = millis();
    }
  }
}

// ==================== 存储 ====================
void loadSettings() {
  preferences.begin("settings", true);
  REMOTE_URL = preferences.getString("remote_url", "");
  WS_ENABLED = preferences.getBool("ws_enabled", false);
  WS_SERVER = preferences.getString("ws_server", "");
  WS_USER = preferences.getString("ws_user", "");
  WS_PASS = preferences.getString("ws_pass", "");
  WS_TOKEN = preferences.getString("ws_token", WS_TOKEN);
  preferences.end();
}

void saveSettings() {
  preferences.begin("settings", false);
  preferences.putString("remote_url", REMOTE_URL);
  preferences.putBool("ws_enabled", WS_ENABLED);
  preferences.putString("ws_server", WS_SERVER);
  preferences.putString("ws_user", WS_USER);
  preferences.putString("ws_pass", WS_PASS);
  preferences.putString("ws_token", WS_TOKEN);
  preferences.end();
  Serial.println("设置已保存");
}

void saveDevices() {
  preferences.begin("wol", false);
  preferences.putUInt("count", deviceCount);
  for (int i = 0; i < deviceCount; i++) {
    preferences.putString(("n" + String(i)).c_str(), devices[i].name);
    preferences.putString(("m" + String(i)).c_str(), macToString(devices[i].mac));
  }
  preferences.end();
  Serial.println("设备已保存");
}

void loadDevices() {
  preferences.begin("wol", true);
  deviceCount = preferences.getUInt("count", 0);
  for (int i = 0; i < deviceCount && i < MAX_DEVICES; i++) {
    devices[i].name = preferences.getString(("n" + String(i)).c_str(), "");
    String macStr = preferences.getString(("m" + String(i)).c_str(), "00:00:00:00:00:00");
    parseMac(macStr, devices[i].mac);
    devices[i].enabled = true;
  }
  preferences.end();
  Serial.printf("已加载 %d 个设备\n", deviceCount);
}

void printDeviceList() {
  Serial.println("--- 设备列表 ---");
  for (int i = 0; i < deviceCount; i++) {
    Serial.printf("%d. %s [%s]\n", i + 1, devices[i].name.c_str(), macToString(devices[i].mac).c_str());
  }
  Serial.println("----------------");
}

// ==================== ARP 扫描 ====================
bool getMacFromARP(IPAddress ip, uint8_t *mac) {
  struct netif *netif = netif_list;
  ip4_addr_t target_ip;
  target_ip.addr = ip;

  while (netif != NULL) {
    struct eth_addr *eth_ret = NULL;
    const ip4_addr_t *ip_ret = NULL;
    s8_t result = etharp_find_addr(netif, &target_ip, &eth_ret, &ip_ret);
    if (result >= 0 && eth_ret != NULL) {
      memcpy(mac, eth_ret->addr, 6);
      return true;
    }
    netif = netif->next;
  }
  return false;
}

// 查询 NetBIOS 名称
String queryNetBIOSName(IPAddress ip) {
  WiFiUDP nbUDP;

  // NetBIOS 名称查询包
  uint8_t query[] = {
    0x00, 0x00,  // Transaction ID
    0x00, 0x01,  // Flags: Standard query
    0x00, 0x01,  // Questions: 1
    0x00, 0x00,  // Answer RRs: 0
    0x00, 0x00,  // Authority RRs: 0
    0x00, 0x00,  // Additional RRs: 0
    // Query: *<00> (wildcard)
    0x20,  // Length: 32 bytes (encoded name)
    0x43, 0x4B, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
    0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
    0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
    0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
    0x00,  // Name terminator
    0x00, 0x21,  // Type: NBSTAT (Node Status)
    0x00, 0x01   // Class: IN
  };

  if (!nbUDP.beginPacket(ip, 137)) return "";
  nbUDP.write(query, sizeof(query));
  if (!nbUDP.endPacket()) return "";

  // 等待响应
  delay(100);

  int len = nbUDP.parsePacket();
  if (len <= 0) return "";

  uint8_t response[576];
  int rlen = nbUDP.read(response, sizeof(response));
  nbUDP.stop();

  if (rlen < 57) return "";

  // 解析响应中的主机名
  // 响应格式: Header(12) + Name encoding + Type(2) + Class(2) + TTL(4) + RDLEN(2) + NUM_NAMES(1) + NAMES...
  int numNames = response[56];
  if (numNames > 0 && rlen >= 57 + 15) {
    char name[16] = {0};
    memcpy(name, &response[57], 15);
    // 去除空格
    for (int i = 14; i >= 0; i--) {
      if (name[i] == ' ') name[i] = '\0';
      else break;
    }
    return String(name);
  }

  return "";
}

// ==================== 扫描实时推送 ====================
void broadcastScanProgress(int progress, const char* status) {
  scanProgress = progress;
  Serial.printf("[%d%%] %s\n", progress, status);
}

void broadcastScanDevice(int index) {
  if (index < 0 || index >= scannedDeviceCount) return;
  Serial.printf(">> 发现: %s [%s] %s\n",
    scannedDevices[index].ip.toString().c_str(),
    macToString(scannedDevices[index].mac).c_str(),
    scannedDevices[index].hostname.length() > 0 ? ("(" + scannedDevices[index].hostname + ")").c_str() : ""
  );
}

// 添加设备到扫描结果（去重）
bool addScannedDevice(IPAddress ip, uint8_t *mac, const char* hostname) {
  if (scannedDeviceCount >= MAX_SCAN_RESULTS) return false;

  // 检查 MAC 是否已存在
  for (int i = 0; i < scannedDeviceCount; i++) {
    if (memcmp(scannedDevices[i].mac, mac, 6) == 0) {
      return false;  // 已存在
    }
  }

  scannedDevices[scannedDeviceCount].ip = ip;
  memcpy(scannedDevices[scannedDeviceCount].mac, mac, 6);
  scannedDevices[scannedDeviceCount].hostname = hostname;
  scannedDeviceCount++;

  broadcastScanDevice(scannedDeviceCount - 1);
  return true;
}

// 非阻塞处理：在扫描期间处理网络请求
void scanYield() {
  server.handleClient();
  if (WS_ENABLED && wsClient && wsClient->available()) {
    wsClient->poll();
  }
  delay(1);
}

// 推送扫描状态到 Go 服务器（通过 WebSocket）
void pushScanStatus() {
  if (!WS_ENABLED || !wsClient || !wsClient->available()) return;

  JsonDocument doc;
  doc["cmd"] = "scan_status";
  doc["data"]["scanning"] = isScanning;
  doc["data"]["progress"] = scanProgress;

  JsonArray devices = doc["data"]["devices"].to<JsonArray>();
  for (int i = 0; i < scannedDeviceCount; i++) {
    JsonObject d = devices.add<JsonObject>();
    d["ip"] = scannedDevices[i].ip.toString();
    d["mac"] = macToString(scannedDevices[i].mac);
    d["name"] = scannedDevices[i].hostname;
  }

  String out;
  serializeJson(doc, out);
  wsClient->send(out);
}

// ==================== 多轮 ARP 扫描（整合去重）====================
void startNetworkScan() {
  scannedDeviceCount = 0;
  isScanning = true;
  scanProgress = 0;

  IPAddress localIP = WiFi.localIP();

  Serial.println("\n========================================");
  Serial.println("多轮 ARP 扫描（3轮整合）");
  Serial.printf("子网: %d.%d.%d.0/24\n", localIP[0], localIP[1], localIP[2]);
  Serial.println("========================================\n");

  // 全局临时存储（避免栈溢出）
  static struct {
    uint8_t mac[6];
    int count;
    bool hasMac;
  } tempScanData[256];

  // 初始化
  for (int i = 0; i < 256; i++) {
    memset(tempScanData[i].mac, 0, 6);
    tempScanData[i].count = 0;
    tempScanData[i].hasMac = false;
  }

  // ========================================
  // 3 轮 ARP 扫描 (0-60%)
  // ========================================
  for (int round = 0; round < 3; round++) {
    Serial.printf("\n[轮次 %d/3] ARP 扫描...\n", round + 1);

    // 发送 ARP 请求
    for (int i = 1; i < 255; i++) {
      IPAddress targetIP(localIP[0], localIP[1], localIP[2], i);
      if (targetIP == localIP) continue;

      ip4_addr_t target_ip;
      target_ip.addr = targetIP;
      etharp_request(netif_default, &target_ip);

      if (i % 20 == 0) scanYield();
    }

    // 更新进度
    scanProgress = round * 15 + 5;
    char status[32];
    snprintf(status, sizeof(status), "ARP 轮次 %d/3", round + 1);
    broadcastScanProgress(scanProgress, status);
    pushScanStatus();

    // 等待响应
    for (int w = 0; w < 10; w++) {  // 1秒
      delay(100);
      scanYield();
    }

    // 读取 ARP 表并累计
    int roundFound = 0;
    for (int i = 1; i < 255; i++) {
      if (i == localIP[3]) continue;

      IPAddress targetIP(localIP[0], localIP[1], localIP[2], i);
      uint8_t mac[6];
      if (getMacFromARP(targetIP, mac)) {
        tempScanData[i].count++;

        // 保存 MAC（首次发现）
        if (!tempScanData[i].hasMac) {
          memcpy(tempScanData[i].mac, mac, 6);
          tempScanData[i].hasMac = true;
          roundFound++;
        }
      }

      if (i % 50 == 0) scanYield();
    }

    scanProgress = (round + 1) * 20;
    snprintf(status, sizeof(status), "轮次 %d 完成", round + 1);
    broadcastScanProgress(scanProgress, status);
    pushScanStatus();

    Serial.printf("轮次 %d: 发现 %d 个响应\n", round + 1, roundFound);
  }

  // ========================================
  // 整合结果：出现 >= 2 次的设备 (60-70%)
  // ========================================
  Serial.println("\n[整合] 筛选稳定设备...");
  scanProgress = 60;
  broadcastScanProgress(60, "整合结果...");
  pushScanStatus();

  int stableCount = 0;
  for (int i = 1; i < 255 && scannedDeviceCount < MAX_SCAN_RESULTS; i++) {
    // 跳过本机 IP
    if (i == localIP[3]) continue;

    // 出现 2 次以上才算稳定
    if (tempScanData[i].count >= 2 && tempScanData[i].hasMac) {
      scannedDevices[scannedDeviceCount].ip = IPAddress(localIP[0], localIP[1], localIP[2], i);
      memcpy(scannedDevices[scannedDeviceCount].mac, tempScanData[i].mac, 6);
      scannedDevices[scannedDeviceCount].hostname = "";
      scannedDeviceCount++;
      stableCount++;

      Serial.printf("  [%d] %d.%d.%d.%d -> %s (出现 %d 次)\n",
        stableCount,
        localIP[0], localIP[1], localIP[2], i,
        macToString(tempScanData[i].mac).c_str(),
        tempScanData[i].count);
    }
  }

  Serial.printf("稳定设备: %d 个\n", stableCount);

  // ========================================
  // 设备名称识别 (70-100%)
  // ========================================
  if (scannedDeviceCount > 0) {
    Serial.println("\n[识别] 查询设备名称...");
    scanProgress = 70;
    broadcastScanProgress(70, "mDNS 查询...");
    pushScanStatus();

    // 批量 mDNS
    int mdnsCount = MDNS.queryService("_workstation._tcp", "local");
    int mdnsMatched = 0;

    for (int i = 0; i < scannedDeviceCount; i++) {
      for (int j = 0; j < mdnsCount; j++) {
        if (MDNS.IP(j) == scannedDevices[i].ip) {
          scannedDevices[i].hostname = MDNS.hostname(j);
          mdnsMatched++;
          break;
        }
      }
      if (i % 10 == 0) scanYield();
    }

    // NetBIOS（仅未命名的）
    scanProgress = 85;
    broadcastScanProgress(85, "NetBIOS...");

    int netbiosMatched = 0;
    for (int i = 0; i < scannedDeviceCount; i++) {
      if (scannedDevices[i].hostname.length() == 0) {
        String hostname = queryNetBIOSName(scannedDevices[i].ip);
        if (hostname.length() > 0) {
          scannedDevices[i].hostname = hostname;
          netbiosMatched++;
        }
        delay(30);
      }
      if (i % 5 == 0) scanYield();
    }

    Serial.printf("mDNS: %d, NetBIOS: %d\n", mdnsMatched, netbiosMatched);
  }

  // ========================================
  // 完成
  // ========================================
  isScanning = false;
  scanProgress = 100;
  broadcastScanProgress(100, "扫描完成");
  pushScanStatus();

  int namedCount = 0;
  for (int i = 0; i < scannedDeviceCount; i++) {
    if (scannedDevices[i].hostname.length() > 0) namedCount++;
  }

  Serial.println("\n========================================");
  Serial.printf("扫描完成! 共 %d 设备, %d 个已命名\n", scannedDeviceCount, namedCount);
  Serial.println("========================================\n");
}

// ==================== 生成设备列表 HTML ====================
String generateDeviceList() {
  if (deviceCount == 0) {
    return "<li class='empty-state'>暂无设备，点击下方添加</li>";
  }

  String list = "";
  for (int i = 0; i < deviceCount; i++) {
    list += "<li class='device-item'>";
    list += "<div class='device-info'>";
    list += "<div class='device-name'>" + devices[i].name + "</div>";
    list += "<div class='device-mac'>" + macToString(devices[i].mac) + "</div>";
    list += "</div>";
    list += "<div class='device-actions'>";
    list += "<button class='btn btn-wake' onclick='wakeDevice(" + String(i) + ")'>唤醒</button>";
    list += "<button class='btn btn-delete' onclick='deleteDevice(" + String(i) + ")'>删除</button>";
    list += "</div>";
    list += "</li>";
  }
  return list;
}

// ==================== WebSocket 客户端 ====================
void setupWsClient() {
  // 销毁旧实例，创建全新客户端（避免 SSL 状态残留）
  if (wsClient) {
    wsClient->close();
    delete wsClient;
  }
  wsClient = new WebsocketsClient();
  wsClient->setInsecure();

  wsClient->onMessage(onWsMessage);
  wsClient->onEvent([](WebsocketsEvent event, String data) {
    if (event == WebsocketsEvent::ConnectionOpened) {
      Serial.println("WebSocket 已连接");
      wsConnecting = false;
      JsonDocument auth;
      auth["type"] = "auth";
      auth["device_id"] = "esp32-wol";
      auth["user"] = WS_USER;
      auth["pass"] = WS_PASS;
      auth["token"] = WS_TOKEN;
      String authStr;
      serializeJson(auth, authStr);
      wsClient->send(authStr);
    } else if (event == WebsocketsEvent::ConnectionClosed) {
      Serial.println("WebSocket 已断开");
      wsConnecting = false;
    }
  });
}

void initWebSocket() {
  if (WS_SERVER.isEmpty()) {
    Serial.println("WebSocket 服务器地址未配置");
    return;
  }

  setupWsClient();
  connectWebSocket();
}

void connectWebSocket() {
  if (WS_SERVER.isEmpty() || wsConnecting) return;

  // 确保 WiFi 已连接
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi 未连接，跳过 WebSocket 连接");
    return;
  }

  wsConnecting = true;
  Serial.printf("连接 WebSocket: %s\n", WS_SERVER.c_str());
  Serial.printf("可用堆内存: %d bytes\n", ESP.getFreeHeap());

  // 每次重连前重建客户端实例
  setupWsClient();

  bool connected = false;
  for (int i = 0; i < 3; i++) {
    connected = wsClient->connect(WS_SERVER);
    if (connected) break;
    Serial.printf("连接尝试 %d 失败，重试...\n", i + 1);
    delay(1000);
  }

  if (connected) {
    Serial.println("WebSocket 连接成功");
  } else {
    Serial.println("WebSocket 连接失败");
    wsConnecting = false;
  }
  lastWsReconnect = millis();
}

void handleWebSocket() {
  static unsigned long lastPoll = 0;

  // WiFi 未连接时跳过
  if (WiFi.status() != WL_CONNECTED) return;

  // 轮询 WebSocket 消息
  if (wsClient && millis() - lastPoll > 50) {
    wsClient->poll();
    lastPoll = millis();
  }

  // 断线重连
  if ((!wsClient || !wsClient->available()) && !wsConnecting && millis() - lastWsReconnect > 10000) {
    Serial.println("WebSocket 重连中...");
    connectWebSocket();
  }

  // 心跳保活
  if (wsClient && wsClient->available() && millis() - lastWsHeartbeat > 30000) {
    JsonDocument ping;
    ping["type"] = "ping";
    String pingStr;
    serializeJson(ping, pingStr);
    wsClient->send(pingStr);
    lastWsHeartbeat = millis();
  }
}

void onWsMessage(WebsocketsMessage message) {
  Serial.printf("WS 收到: %s\n", message.data().c_str());

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, message.data());
  if (err) {
    Serial.println("JSON 解析失败");
    return;
  }

  String response = handleWsCommand(doc);
  if (response.length() > 0) {
    wsClient->send(response);
  }
}

String handleWsCommand(JsonDocument& doc) {
  String cmd = doc["cmd"] | "";
  String requestId = doc["request_id"] | "";

  JsonDocument resp;
  resp["request_id"] = requestId;

  if (cmd == "ping") {
    resp["cmd"] = "pong";
    resp["success"] = true;
  }
  else if (cmd == "get_devices") {
    resp["cmd"] = "devices";
    resp["success"] = true;
    JsonArray arr = resp["data"].to<JsonArray>();
    for (int i = 0; i < deviceCount; i++) {
      JsonObject d = arr.add<JsonObject>();
      d["index"] = i;
      d["name"] = devices[i].name;
      d["mac"] = macToString(devices[i].mac);
      d["enabled"] = devices[i].enabled;
    }
  }
  else if (cmd == "wake") {
    int index = doc["index"] | -1;
    if (index >= 0 && index < deviceCount) {
      sendWOL(devices[index].mac);
      resp["cmd"] = "wake_result";
      resp["success"] = true;
      resp["data"]["device"] = devices[index].name;
      resp["data"]["mac"] = macToString(devices[index].mac);
    } else if (doc["all"] == true) {
      for (int i = 0; i < deviceCount; i++) {
        sendWOL(devices[i].mac);
        delay(50);
      }
      resp["cmd"] = "wake_result";
      resp["success"] = true;
      resp["data"]["device"] = "all";
    } else {
      resp["cmd"] = "wake_result";
      resp["success"] = false;
      resp["message"] = "无效索引";
    }
  }
  else if (cmd == "wake_broadcast") {
    // 广播唤醒整个局域网
    Serial.println("WS 广播唤醒");

    IPAddress broadcastIP(255, 255, 255, 255);
    IPAddress subnetBroadcast = WiFi.localIP();
    subnetBroadcast[3] = 255;

    int sentCount = 0;

    // 唤醒已保存设备
    for (int i = 0; i < deviceCount; i++) {
      sendWOL(devices[i].mac);
      sentCount++;
      delay(50);
    }

    // 发送广播包
    uint8_t packet[102];
    for (int i = 0; i < 6; i++) packet[i] = 0xFF;
    for (int i = 1; i <= 16; i++)
      for (int j = 0; j < 6; j++)
        packet[i * 6 + j] = 0xFF;

    for (int retry = 0; retry < 5; retry++) {
      udp.beginPacket(broadcastIP, WOL_PORT);
      udp.write(packet, 102);
      udp.endPacket();
      udp.beginPacket(subnetBroadcast, WOL_PORT);
      udp.write(packet, 102);
      udp.endPacket();
      delay(100);
      sentCount += 2;
    }

    // 唤醒扫描到的设备
    for (int i = 0; i < scannedDeviceCount; i++) {
      if (scannedDevices[i].mac[0] == 0 && scannedDevices[i].mac[1] == 0) continue;
      sendWOL(scannedDevices[i].mac);
      sentCount++;
      delay(30);
    }

    resp["cmd"] = "wake_result";
    resp["success"] = true;
    resp["data"]["device"] = "broadcast";
    resp["data"]["count"] = sentCount;
    Serial.printf("广播唤醒完成, 发送 %d 个包\n", sentCount);
  }
  else if (cmd == "add_device") {
    String name = doc["name"] | "";
    String mac = doc["mac"] | "";
    if (name.isEmpty() || mac.isEmpty()) {
      resp["cmd"] = "add_result";
      resp["success"] = false;
      resp["message"] = "参数不完整";
    } else if (deviceCount >= MAX_DEVICES) {
      resp["cmd"] = "add_result";
      resp["success"] = false;
      resp["message"] = "设备数量已达上限";
    } else {
      devices[deviceCount].name = name;
      parseMac(mac, devices[deviceCount].mac);
      devices[deviceCount].enabled = true;
      deviceCount++;
      saveDevices();
      resp["cmd"] = "add_result";
      resp["success"] = true;
      resp["data"]["index"] = deviceCount - 1;
      resp["data"]["name"] = name;
      resp["data"]["mac"] = mac;
      Serial.printf("WS 添加设备: %s [%s]\n", name.c_str(), mac.c_str());
    }
  }
  else if (cmd == "delete_device") {
    int index = doc["index"] | -1;
    if (index >= 0 && index < deviceCount) {
      Serial.printf("WS 删除设备: %s\n", devices[index].name.c_str());
      for (int i = index; i < deviceCount - 1; i++) {
        devices[i] = devices[i + 1];
      }
      deviceCount--;
      saveDevices();
      resp["cmd"] = "delete_result";
      resp["success"] = true;
    } else {
      resp["cmd"] = "delete_result";
      resp["success"] = false;
      resp["message"] = "无效索引";
    }
  }
  else if (cmd == "scan") {
    if (isScanning) {
      resp["cmd"] = "scan_result";
      resp["success"] = false;
      resp["message"] = "扫描进行中";
    } else {
      resp["cmd"] = "scan_started";
      resp["success"] = true;
      String respStr;
      serializeJson(resp, respStr);
      wsClient->send(respStr);

      // 异步扫描
      startNetworkScan();

      // 返回扫描结果
      resp.clear();
      resp["request_id"] = requestId;
      resp["cmd"] = "scan_result";
      resp["success"] = true;
      resp["data"]["progress"] = 100;
      JsonArray arr = resp["data"]["devices"].to<JsonArray>();
      for (int i = 0; i < scannedDeviceCount; i++) {
        JsonObject d = arr.add<JsonObject>();
        d["ip"] = scannedDevices[i].ip.toString();
        d["mac"] = macToString(scannedDevices[i].mac);
      }
    }
  }
  else if (cmd == "get_scan_status") {
    // 返回当前扫描状态
    resp["cmd"] = "scan_status";
    resp["success"] = true;
    resp["data"]["scanning"] = isScanning;
    resp["data"]["progress"] = scanProgress;
    JsonArray arr = resp["data"]["devices"].to<JsonArray>();
    for (int i = 0; i < scannedDeviceCount; i++) {
      JsonObject d = arr.add<JsonObject>();
      d["ip"] = scannedDevices[i].ip.toString();
      d["mac"] = macToString(scannedDevices[i].mac);
      d["name"] = scannedDevices[i].hostname;
    }
  }
  else if (cmd == "get_status") {
    // 读取芯片内部温度 (ESP32-S3)
    float tempC = temperatureRead();
    resp["cmd"] = "status";
    resp["success"] = true;
    resp["data"]["ip"] = WiFi.localIP().toString();
    resp["data"]["mac"] = WiFi.macAddress();
    resp["data"]["ssid"] = String(WIFI_SSID);
    resp["data"]["wifi_rssi"] = WiFi.RSSI();
    resp["data"]["device_count"] = deviceCount;
    resp["data"]["wake_count"] = wakeCount;
    resp["data"]["uptime"] = millis() / 1000;
    resp["data"]["free_heap"] = ESP.getFreeHeap();
    resp["data"]["temperature"] = tempC;
    resp["data"]["chip_model"] = ESP.getChipModel();
    resp["data"]["flash_size"] = ESP.getFlashChipSize();
    resp["data"]["sdk_version"] = ESP.getSdkVersion();
  }
  else {
    resp["cmd"] = "error";
    resp["success"] = false;
    resp["message"] = "未知命令: " + cmd;
  }

  String respStr;
  serializeJson(resp, respStr);
  return respStr;
}

void updateWsStatus() {
  // 用于前端状态更新，可通过 WebSocket 广播
}
