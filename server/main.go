package main

import (
	"crypto/sha256"
	_ "embed"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/golang-jwt/jwt/v5"
	"github.com/gorilla/websocket"
)

//go:embed templates/index.html
var indexHTML []byte

var (
	jwtSecret   = []byte("W0L-S3cur3-K3y-2024!@Relay#Pr0duct10n")
	staticToken string
	users       = map[string]string{"admin": ""}

	// OAuth 配置
	oauthAppID     = "100003"
	oauthAppSecret = "7kPq9xZ2vR8mW4tL"
	oauthUserID    = "1991058972475527168" // 允许的用户ID

	espDevice    *DeviceConn
	devicesMu    sync.RWMutex
	webClients   = make(map[*websocket.Conn]bool)
	webClientsMu sync.RWMutex

	// 扫描状态缓存（非阻塞）
	scanCacheMu sync.RWMutex
	scanCache   = Message{"scanning": false, "progress": 0, "devices": []interface{}{}}

	upgrader = websocket.Upgrader{
		CheckOrigin:      func(r *http.Request) bool { return true },
		ReadBufferSize:   65536, // 64KB 读取缓冲区
		WriteBufferSize:  65536, // 64KB 写入缓冲区
	}
)

type DeviceConn struct {
	WS               *websocket.Conn
	User             string
	IP               string
	LastSeen         time.Time
	Done             chan struct{}
	PendingResponses []struct {
		ID   string
		Chan chan Message
	}
}

type Message map[string]interface{}

type Claims struct {
	Username string `json:"username"`
	jwt.RegisteredClaims
}

func main() {
	// 命令行参数
	port := flag.Int("port", 8080, "服务器监听端口")
	password := flag.String("password", "", "admin 用户密码")
	token := flag.String("token", "", "WebSocket 固定访问令牌")
	flag.Parse()

	// 设置固定 token
	if *token != "" {
		staticToken = *token
	} else {
		staticToken = os.Getenv("WOL_TOKEN")
	}
	if staticToken == "" {
		staticToken = "esp32-wol-fixed-token-x9k2m"
		log.Println("[配置] 使用默认固定 Token")
	}

	// 如果命令行未指定密码，尝试从环境变量获取
	if *password == "" {
		*password = os.Getenv("WOL_PASSWORD")
	}

	// 如果仍未设置密码，使用默认密码
	if *password == "" {
		*password = "%&@Wol@Secure2f24!"
		log.Println("[警告] 使用默认密码，建议通过 -password 参数或 WOL_PASSWORD 环境变量设置")
	}

	users["admin"] = *password

	// 打印配置信息
	if staticToken != "" {
		log.Printf("[配置] 固定 Token: %s", staticToken)
	}

	http.HandleFunc("/", indexHandler)
	http.HandleFunc("/login", loginHandler)
	http.HandleFunc("/auth/callback", authCallbackHandler)
	http.HandleFunc("/ws", wsHandler)
	http.HandleFunc("/api/devices", authMiddleware(devicesAPIHandler))
	http.HandleFunc("/scan/results", authMiddleware(scanResultsHandler))

	// 启动心跳检测
	go heartbeatChecker()

	addr := fmt.Sprintf(":%d", *port)
	log.Printf("WOL 中继服务器启动 %s", addr)
	log.Fatal(http.ListenAndServe(addr, nil))
}

func heartbeatChecker() {
	ticker := time.NewTicker(30 * time.Second)
	for range ticker.C {
		devicesMu.Lock()
		if espDevice != nil {
			// 超过60秒没有心跳，认为断开
			if time.Since(espDevice.LastSeen) > 60*time.Second {
				log.Println("[ESP32] 心跳超时，断开连接")
				espDevice.WS.Close()
				espDevice = nil
				go broadcastDeviceList()
			}
		}
		devicesMu.Unlock()
	}
}

func indexHandler(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Write(indexHTML)
}

func loginHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var creds struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if err := json.NewDecoder(r.Body).Decode(&creds); err != nil {
		http.Error(w, "Bad request", 400)
		return
	}

	if pass, ok := users[creds.Username]; !ok || pass != creds.Password {
		w.WriteHeader(401)
		json.NewEncoder(w).Encode(Message{"success": false, "message": "用户名或密码错误"})
		return
	}

	token := jwt.NewWithClaims(jwt.SigningMethodHS256, &Claims{
		Username: creds.Username,
		RegisteredClaims: jwt.RegisteredClaims{
			ExpiresAt: jwt.NewNumericDate(time.Now().Add(24 * time.Hour)),
		},
	})
	tokenString, err := token.SignedString(jwtSecret)
	if err != nil {
		w.WriteHeader(500)
		return
	}

	json.NewEncoder(w).Encode(Message{"success": true, "token": tokenString})
}

func authCallbackHandler(w http.ResponseWriter, r *http.Request) {
	userID := r.URL.Query().Get("userId")
	oauthToken := r.URL.Query().Get("token")
	sign := r.URL.Query().Get("sign")
	signTime := r.URL.Query().Get("time")
	nextUrl := r.URL.Query().Get("nextUrl")

	log.Printf("[OAuth] 收到回调: userId=%s, token=%s, sign=%s, time=%s, nextUrl=%s",
		userID, oauthToken, sign, signTime, nextUrl)

	// 检查必要参数
	if userID == "" || oauthToken == "" || sign == "" || signTime == "" {
		log.Printf("[OAuth] 缺少必要参数")
		errorMsg := url.QueryEscape("缺少必要参数")
		http.Redirect(w, r, fmt.Sprintf("/?auth_error=%s", errorMsg), http.StatusTemporaryRedirect)
		return
	}

	// 验证用户ID
	if userID != oauthUserID {
		log.Printf("[OAuth] 用户ID不匹配: got=%s, expected=%s", userID, oauthUserID)
		errorMsg := url.QueryEscape("无权限访问")
		http.Redirect(w, r, fmt.Sprintf("/?auth_error=%s", errorMsg), http.StatusTemporaryRedirect)
		return
	}

	// 验证签名
	if !verifyOAuthSignature(userID, oauthToken, sign, signTime) {
		log.Printf("[OAuth] 签名验证失败")
		errorMsg := url.QueryEscape("签名验证失败或请求已过期")
		http.Redirect(w, r, fmt.Sprintf("/?auth_error=%s", errorMsg), http.StatusTemporaryRedirect)
		return
	}

	// 生成 JWT token
	token := jwt.NewWithClaims(jwt.SigningMethodHS256, &Claims{
		Username: userID,
		RegisteredClaims: jwt.RegisteredClaims{
			ExpiresAt: jwt.NewNumericDate(time.Now().Add(24 * time.Hour)),
		},
	})
	tokenString, err := token.SignedString(jwtSecret)
	if err != nil {
		errorMsg := url.QueryEscape("生成令牌失败")
		http.Redirect(w, r, fmt.Sprintf("/?auth_error=%s", errorMsg), http.StatusTemporaryRedirect)
		return
	}

	// 设置 token cookie 并重定向到前端
	http.SetCookie(w, &http.Cookie{
		Name:     "token",
		Value:    tokenString,
		Path:     "/",
		HttpOnly: false, // 允许 JavaScript 读取
		Secure:   false, // 开发环境设为 false，生产环境应为 true
		MaxAge:   86400,
	})

	if nextUrl == "" {
		nextUrl = "/"
	}
	// 通过 URL 参数传递 token，确保前端能获取
	redirectURL := nextUrl + "?auth_token=" + tokenString
	log.Printf("[OAuth] 用户登录成功: userId=%s", userID)
	http.Redirect(w, r, redirectURL, http.StatusTemporaryRedirect)
}

// verifyOAuthSignature 验证 OAuth 回调签名
// 签名算法: sha256(token + userId + appId + secret + time).upper()
func verifyOAuthSignature(userID, token, sign, signTime string) bool {
	// 解析时间戳
	timestamp, err := strconv.ParseInt(signTime, 10, 64)
	if err != nil {
		log.Printf("[OAuth] 时间戳解析失败: %v", err)
		return false
	}

	// 验证时间 - 必须在 30 秒内
	now := time.Now().Unix()
	diff := now - timestamp
	if diff < -30 || diff > 30 {
		log.Printf("[OAuth] 时间差超限: diff=%d秒", diff)
		return false
	}

	// 计算期望签名: sha256(token + userId + appId + secret + time)
	data := token + userID + oauthAppID + oauthAppSecret + signTime
	hash := sha256.Sum256([]byte(data))
	expectedSign := strings.ToUpper(hex.EncodeToString(hash[:]))

	// 验证签名
	if sign != expectedSign {
		log.Printf("[OAuth] 签名不匹配: expected=%s, got=%s", expectedSign, sign)
		return false
	}

	return true
}

func authMiddleware(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		tokenStr := r.Header.Get("Authorization")
		if len(tokenStr) > 7 && tokenStr[:7] == "Bearer " {
			tokenStr = tokenStr[7:]
		}
		if tokenStr == "" {
			tokenStr = r.URL.Query().Get("token")
		}

		token, err := jwt.ParseWithClaims(tokenStr, &Claims{}, func(t *jwt.Token) (interface{}, error) {
			return jwtSecret, nil
		})
		if err != nil || !token.Valid {
			w.WriteHeader(401)
			json.NewEncoder(w).Encode(Message{"success": false, "message": "未授权"})
			return
		}

		next(w, r)
	}
}

func devicesAPIHandler(w http.ResponseWriter, r *http.Request) {
	devicesMu.RLock()
	defer devicesMu.RUnlock()

	if espDevice != nil {
		json.NewEncoder(w).Encode(Message{
			"success":   true,
			"connected": true,
			"user":      espDevice.User,
			"ip":        espDevice.IP,
		})
	} else {
		json.NewEncoder(w).Encode(Message{"success": true, "connected": false})
	}
}

func scanResultsHandler(w http.ResponseWriter, r *http.Request) {
	// 直接返回缓存的扫描状态（非阻塞）
	scanCacheMu.RLock()
	cache := scanCache
	scanCacheMu.RUnlock()

	// 复制缓存数据
	result := make(Message)
	for k, v := range cache {
		result[k] = v
	}

	// 检查ESP32是否在线
	devicesMu.RLock()
	espOnline := espDevice != nil && espDevice.WS != nil
	devicesMu.RUnlock()

	if !espOnline {
		result["error"] = "ESP32 设备离线"
	}

	json.NewEncoder(w).Encode(result)
}

// 更新扫描缓存
func updateScanCache(msg Message) {
	scanCacheMu.Lock()
	defer scanCacheMu.Unlock()

	data, ok := msg["data"].(map[string]interface{})
	if !ok {
		return
	}

	// 处理分批设备列表
	if cmd, _ := msg["cmd"].(string); cmd == "scan_devices_batch" {
		// 获取当前设备列表
		currentDevices, _ := scanCache["devices"].([]interface{})

		// 添加新批次设备
		if newDevices, ok := data["devices"].([]interface{}); ok {
			scanCache["devices"] = append(currentDevices, newDevices...)
		}

		// 更新总数
		if total, ok := data["total"].(float64); ok {
			scanCache["total"] = int(total)
		}
		return
	}

	// 处理扫描状态更新
	if scanning, ok := data["scanning"]; ok {
		scanCache["scanning"] = scanning
	}
	if progress, ok := data["progress"]; ok {
		scanCache["progress"] = progress
	}
	if total, ok := data["total"].(float64); ok {
		scanCache["total"] = int(total)
	}

	// 如果是新扫描开始，清空设备列表
	if scanning, ok := data["scanning"].(bool); ok && scanning {
		if progress, ok := data["progress"].(float64); ok && progress < 5 {
			scanCache["devices"] = []interface{}{}
		}
	}

	// 处理完整设备列表（兼容旧格式）
	if devices, ok := data["devices"].([]interface{}); ok && len(devices) > 0 {
		scanCache["devices"] = devices
	}
}

func wsHandler(w http.ResponseWriter, r *http.Request) {
	ws, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Println("WebSocket upgrade error:", err)
		return
	}
	defer ws.Close()

	ip := r.RemoteAddr
	var isESP32 bool

	for {
		_, msgBytes, err := ws.ReadMessage()
		if err != nil {
			break
		}

		var msg Message
		if err := json.Unmarshal(msgBytes, &msg); err != nil {
			continue
		}

		msgType, _ := msg["type"].(string)
		cmd, _ := msg["cmd"].(string)

		switch {
		// ESP32 认证
		case msgType == "auth":
			tokenStr, _ := msg["token"].(string)

			// 验证 token（固定 token 或 JWT）
			valid := false
			if staticToken != "" && tokenStr == staticToken {
				valid = true
			} else {
				token, err := jwt.ParseWithClaims(tokenStr, &Claims{}, func(t *jwt.Token) (interface{}, error) {
					return jwtSecret, nil
				})
				valid = err == nil && token.Valid
			}

			if !valid {
				ws.WriteJSON(Message{"type": "error", "message": "ESP32 认证失败"})
				log.Printf("[ESP32] 认证失败: ip=%s", ip)
				ws.Close()
				return
			}

			isESP32 = true
			user, _ := msg["user"].(string)

			devicesMu.Lock()
			// 关闭旧连接
			if espDevice != nil && espDevice.Done != nil {
				close(espDevice.Done)
			}
			espDevice = &DeviceConn{
				WS:       ws,
				User:     user,
				IP:       ip,
				LastSeen: time.Now(),
				Done:     make(chan struct{}),
			}
			devicesMu.Unlock()

			log.Printf("[ESP32] 已连接: user=%s ip=%s", user, ip)
			broadcastDeviceList()

		// Web 客户端
		case msgType == "web_client":
			tokenStr, _ := msg["token"].(string)

			// 检查固定 token 或 JWT token
			valid := false
			if staticToken != "" && tokenStr == staticToken {
				valid = true
			} else {
				token, err := jwt.ParseWithClaims(tokenStr, &Claims{}, func(t *jwt.Token) (interface{}, error) {
					return jwtSecret, nil
				})
				valid = err == nil && token.Valid
			}

			if !valid {
				ws.WriteJSON(Message{"type": "error", "message": "认证失败"})
				ws.Close()
				return
			}

			webClientsMu.Lock()
			webClients[ws] = true
			webClientsMu.Unlock()

			log.Println("[Web] 客户端已连接")
			sendDeviceList(ws)

		// ESP32 心跳/响应
		case isESP32:
			skipBroadcast := false
			devicesMu.Lock()
			if espDevice != nil {
				espDevice.LastSeen = time.Now()

				// 检查是否是等待中的 HTTP 请求响应
				reqID, _ := msg["request_id"].(string)
				if reqID != "" {
					for i, pr := range espDevice.PendingResponses {
						if pr.ID == reqID {
							pr.Chan <- msg
							espDevice.PendingResponses = append(espDevice.PendingResponses[:i], espDevice.PendingResponses[i+1:]...)
							skipBroadcast = true
							break
						}
					}
				}
			}
			devicesMu.Unlock()

			// 处理扫描状态更新（缓存 + 广播）
			if cmd == "scan_status" || cmd == "scan_result" || cmd == "scan_devices_batch" {
				updateScanCache(msg)
			}

			if skipBroadcast {
				continue
			}

			// 回复 pong 或转发响应
			if cmd == "ping" || msgType == "ping" {
				ws.WriteJSON(Message{"request_id": msg["request_id"], "cmd": "pong", "success": true})
			} else {
				// 广播给所有 Web 客户端
				webClientsMu.RLock()
				for client := range webClients {
					client.WriteJSON(msg)
				}
				webClientsMu.RUnlock()
				log.Printf("[ESP32->Web] %s", cmd)
			}

		// Web -> ESP32
		case !isESP32:
			devicesMu.RLock()
			device := espDevice
			devicesMu.RUnlock()

			if device == nil || device.WS == nil {
				ws.WriteJSON(Message{
					"request_id": msg["request_id"],
					"cmd":        cmd + "_result",
					"success":    false,
					"message":    "ESP32 设备离线",
				})
				continue
			}

			device.WS.WriteJSON(msg)
			log.Printf("[Web->ESP32] %s", cmd)
		}
	}

	// 清理
	if isESP32 {
		devicesMu.Lock()
		if espDevice != nil && espDevice.WS == ws {
			espDevice = nil
		}
		devicesMu.Unlock()
		log.Println("[ESP32] 已断开")
		broadcastDeviceList()
	} else {
		webClientsMu.Lock()
		delete(webClients, ws)
		webClientsMu.Unlock()
		log.Println("[Web] 客户端已断开")
	}
}

func sendDeviceList(ws *websocket.Conn) {
	devicesMu.RLock()
	defer devicesMu.RUnlock()

	var list []map[string]interface{}
	if espDevice != nil {
		list = append(list, map[string]interface{}{
			"id":        "esp32",
			"user":      espDevice.User,
			"ip":        espDevice.IP,
			"connected": true,
		})
	}

	ws.WriteJSON(Message{"type": "device_list", "devices": list})
}

func broadcastDeviceList() {
	devicesMu.RLock()
	var list []map[string]interface{}
	if espDevice != nil {
		list = append(list, map[string]interface{}{
			"id":        "esp32",
			"user":      espDevice.User,
			"ip":        espDevice.IP,
			"connected": true,
		})
	}
	devicesMu.RUnlock()

	msg := Message{"type": "device_list", "devices": list}
	webClientsMu.RLock()
	defer webClientsMu.RUnlock()
	for client := range webClients {
		client.WriteJSON(msg)
	}
}
