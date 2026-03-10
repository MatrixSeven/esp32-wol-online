# ESP32 局域网设备扫描方案设计

## 问题分析

### 当前方案的问题

1. **UDP触发ARP不稳定**
   - 某些设备防火墙丢弃UDP包
   - 无线设备省电模式下不响应
   - ARP缓存超时导致信息丢失

2. **缺少重试机制**
   - 单次扫描，失败就失败
   - 没有对慢响应设备的容错

3. **名称解析效率低**
   - 每个设备都查询mDNS，即使大部分设备不支持
   - NetBIOS查询阻塞时间长

---

## 推荐方案：三阶段混合扫描

### 架构图

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         三阶段混合扫描                                   │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  阶段1: ICMP Ping 扫描 (0-30%)                                          │
│  ├─ 使用 ESP32Ping 库发送真正的 ICMP Echo Request                       │
│  ├─ 并行扫描：分批发送，等待响应                                         │
│  └─ 超时设置：500ms per IP                                              │
│                                                                         │
│  阶段2: ARP 表补充扫描 (30-60%)                                         │
│  ├─ 对 Ping 未响应的 IP 发送 ARP 请求                                   │
│  ├─ 使用 lwIP 的 etharp_request() 直接发送                              │
│  └─ 等待 2 秒后读取 ARP 表                                              │
│                                                                         │
│  阶段3: 设备识别 (60-100%)                                              │
│  ├─ 仅对已发现设备查询名称                                               │
│  ├─ 优先级：mDNS > NetBIOS > 反向DNS                                    │
│  └─ 可选：厂商识别（MAC OUI）                                           │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 详细设计

### 阶段1: ICMP Ping 扫描

**为什么用 ICMP Ping？**
- ICMP 是三层协议，所有 TCP/IP 设备必须响应
- 比 UDP 更可靠，防火墙通常允许
- ESP32Ping 库成熟稳定

**实现细节：**

```cpp
#include <ESP32Ping.h>

// 分批并行扫描
#define BATCH_SIZE 16        // 每批扫描16个IP
#define PING_TIMEOUT 500     // 单个IP超时500ms
#define PING_RETRY 2         // 失败重试2次

typedef struct {
    IPAddress ip;
    bool responded;
    uint8_t retry_count;
} ScanTarget;

ScanTarget scanTargets[254];
int respondedCount = 0;

void phase1_ICMPScan() {
    Serial.println("阶段1: ICMP Ping 扫描");

    // 初始化扫描目标
    for (int i = 0; i < 254; i++) {
        scanTargets[i].ip = IPAddress(localIP[0], localIP[1], localIP[2], i + 1);
        scanTargets[i].responded = false;
        scanTargets[i].retry_count = 0;
    }

    // 分批扫描
    for (int batch = 0; batch < 254; batch += BATCH_SIZE) {
        int batchEnd = min(batch + BATCH_SIZE, 254);

        // 发送本批次的所有 Ping
        for (int i = batch; i < batchEnd; i++) {
            if (scanTargets[i].ip == localIP) continue;

            // 异步 Ping（需要修改 ESP32Ping 库或使用回调）
            // 这里用同步方式示例
            if (Ping.ping(scanTargets[i].ip, 1)) {
                scanTargets[i].responded = true;
                respondedCount++;

                // 立即获取 MAC
                uint8_t mac[6];
                if (getMacFromARP(scanTargets[i].ip, mac)) {
                    addScannedDevice(scanTargets[i].ip, mac, "");
                }
            }
        }

        // 更新进度
        int progress = (batch * 30) / 254;
        broadcastScanProgress(progress, "ICMP 扫描中...");
    }
}
```

### 阶段2: ARP 补充扫描

**目的：** 捕获 ICMP 不响应但 ARP 能发现的设备

```cpp
void phase2_ARPScan() {
    Serial.println("阶段2: ARP 补充扫描");

    // 对未响应的 IP 发送 ARP 请求
    for (int i = 0; i < 254; i++) {
        if (scanTargets[i].responded) continue;
        if (scanTargets[i].ip == localIP) continue;

        // 使用 lwIP 直接发送 ARP 请求
        ip4_addr_t target_ip;
        target_ip.addr = scanTargets[i].ip;

        struct netif *netif = netif_default;
        if (netif != NULL) {
            etharp_request(netif, &target_ip);
        }

        delay(10);  // 避免过载
    }

    // 等待 ARP 响应
    broadcastScanProgress(45, "等待 ARP 响应...");
    delay(2000);

    // 再次读取 ARP 表
    broadcastScanProgress(50, "读取 ARP 表...");

    for (int i = 0; i < 254 && scannedDeviceCount < MAX_SCAN_RESULTS; i++) {
        if (scanTargets[i].responded) continue;

        uint8_t mac[6];
        if (getMacFromARP(scanTargets[i].ip, mac)) {
            addScannedDevice(scanTargets[i].ip, mac, "");
        }
    }
}
```

### 阶段3: 设备识别

**优化策略：** 只对已发现设备查询，使用快速方法优先

```cpp
void phase3_IdentifyDevices() {
    Serial.printf("阶段3: 识别 %d 个设备\n", scannedDeviceCount);

    // 1. 批量 mDNS 查询（一次性）
    broadcastScanProgress(60, "mDNS 查询...");
    int mdnsCount = MDNS.queryService("_workstation._tcp", "local");

    // 建立 IP -> hostname 映射
    std::map<String, String> mdnsHosts;
    for (int i = 0; i < mdnsCount; i++) {
        mdnsHosts[MDNS.IP(i).toString()] = MDNS.hostname(i);
    }

    // 2. 为每个设备分配名称
    for (int i = 0; i < scannedDeviceCount; i++) {
        String ipStr = scannedDevices[i].ip.toString();

        // 优先使用 mDNS 结果
        if (mdnsHosts.count(ipStr)) {
            scannedDevices[i].hostname = mdnsHosts[ipStr];
            continue;
        }

        // 回退到 NetBIOS（仅对 Windows 设备有效）
        scannedDevices[i].hostname = queryNetBIOSName(scannedDevices[i].ip);

        // 更新进度
        int progress = 60 + (i * 35) / scannedDeviceCount;
        broadcastScanProgress(progress, "识别设备...");
    }

    // 3. 可选：MAC 厂商识别
    broadcastScanProgress(95, "识别厂商...");
    identifyManufacturers();
}
```

---

## 可选增强：被动监听模式

**原理：** 在扫描期间监听网络流量，被动学习设备

```cpp
// 在扫描开始时启动
void startPassiveListener() {
    // 使用 raw socket 或 promiscuous mode
    // 监听 ARP 请求/响应、DHCP 包等
    // 学习网络中的设备
}

// 在扫描期间持续调用
void processPassivePackets() {
    // 解析捕获的包
    // 提取源 MAC 和 IP
    // 补充到扫描结果
}
```

**注意：** ESP32 的 promiscuous mode 会影响 WiFi 连接稳定性，需谨慎使用。

---

## 性能对比

| 方案 | 稳定性 | 速度 | 发现率 | 复杂度 |
|------|--------|------|--------|--------|
| UDP 触发 ARP (当前) | 中 | 快 | 70% | 低 |
| ICMP Ping + ARP (推荐) | 高 | 中 | 95% | 中 |
| 多轮采样 | 中 | 慢 | 80% | 中 |
| 被动监听 | 高 | 慢 | 60% | 高 |

---

## 实现计划

### Step 1: 添加 ESP32Ping 库

```ini
; platformio.ini
lib_deps =
    bblanchon/ArduinoJson @ ^7.0.0
    gilmaimon/ArduinoWebsockets @ ^0.5.4
    marian-craciunescu/ESP32Ping @ ^1.7
```

### Step 2: 修改扫描函数

- 重写 `startNetworkScan()` 为三阶段
- 添加分批并行扫描逻辑
- 优化名称查询

### Step 3: 添加重试机制

```cpp
#define MAX_RETRY 3
#define RETRY_DELAY_MS 500

bool pingWithRetry(IPAddress ip, int maxRetry) {
    for (int i = 0; i < maxRetry; i++) {
        if (Ping.ping(ip, 1)) return true;
        delay(RETRY_DELAY_MS);
    }
    return false;
}
```

### Step 4: 添加 ARP 直接请求

```cpp
#include "lwip/etharp.h"
#include "lwip/netif.h"

void sendDirectARPRequest(IPAddress targetIP) {
    ip4_addr_t ip;
    ip.addr = targetIP;
    etharp_request(netif_default, &ip);
}
```

---

## 预期效果

| 指标 | 当前方案 | 新方案 |
|------|----------|--------|
| 发现率 | ~70% | ~95% |
| 扫描时间 | ~15秒 | ~20秒 |
| 稳定性 | 中 | 高 |
| CPU 占用 | 低 | 中 |

---

## 风险与缓解

| 风险 | 缓解措施 |
|------|----------|
| ESP32Ping 库兼容性问题 | 测试多个版本，准备回退方案 |
| 某些网络禁止 ICMP | 保留 ARP 扫描作为补充 |
| 扫描时间增加 | 提供快速模式选项 |
| 内存不足 | 优化数据结构，减少缓冲区 |
