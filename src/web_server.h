#ifndef WEB_SERVER_H
#define WEB_SERVER_H

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>ESP32 WOL</title>
<style>
:root{--bg:linear-gradient(135deg,#1a1a2e,#16213e);--card:rgba(255,255,255,0.08);--card-border:rgba(255,255,255,0.12);--text:#e8e8e8;--text-muted:#9ca3af;--accent:#4facfe;--success:#00e676;--warning:#ffb300;--danger:#ff5252}
.light{--bg:linear-gradient(135deg,#f0f4f8,#e2e8f0);--card:rgba(255,255,255,0.9);--card-border:rgba(0,0,0,0.1);--text:#1f2937;--text-muted:#6b7280;--accent:#2563eb;--success:#059669;--warning:#d97706;--danger:#dc2626}
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:var(--bg);min-height:100vh;padding:20px;transition:background .3s}
.container{max-width:600px;margin:0 auto}
h1{color:var(--text);text-align:center;margin-bottom:8px;font-weight:600}
.subtitle{color:var(--text-muted);text-align:center;margin-bottom:20px;font-size:0.9rem}
.theme-toggle{position:fixed;top:15px;right:15px;background:var(--card);border:1px solid var(--card-border);border-radius:8px;padding:8px 12px;cursor:pointer;color:var(--text);font-size:1.1rem}
.card{background:var(--card);border-radius:16px;padding:20px;margin-bottom:16px;border:1px solid var(--card-border);backdrop-filter:blur(10px)}
.card-title{color:var(--text-muted);font-size:0.85rem;text-transform:uppercase;margin-bottom:15px;letter-spacing:0.5px}
.device-list{list-style:none}
.device-item{display:flex;align-items:center;justify-content:space-between;padding:14px;background:var(--card);border-radius:10px;margin-bottom:10px;border:1px solid var(--card-border)}
.device-info{flex:1}
.device-name{color:var(--text);font-size:1.05rem;margin-bottom:4px;font-weight:500}
.device-mac{color:var(--text-muted);font-family:monospace;font-size:0.85rem}
.device-actions{display:flex;gap:8px}
.btn{padding:10px 18px;border:none;border-radius:8px;cursor:pointer;font-size:0.9rem;transition:all .2s}
.btn-wake{background:var(--success);color:#fff;font-weight:500}
.btn-delete{background:rgba(255,82,82,0.15);color:var(--danger)}
.btn-add{background:rgba(79,172,254,0.15);color:var(--accent);width:100%;margin-top:10px;font-weight:500}
.btn-scan{background:rgba(255,179,0,0.15);color:var(--warning);width:100%;margin-top:8px;font-weight:500}
.btn-settings{background:rgba(107,114,128,0.15);color:var(--text-muted);width:100%;margin-top:8px}
.form-group{margin-bottom:15px}
.form-group label{display:block;color:var(--text-muted);font-size:0.85rem;margin-bottom:6px}
.form-group input{width:100%;padding:12px;border:1px solid var(--card-border);border-radius:8px;background:var(--card);color:var(--text);font-size:1rem}
.form-group input:focus{outline:none;border-color:var(--accent)}
.modal{display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.6);justify-content:center;align-items:center;z-index:1000}
.modal.show{display:flex}
.modal-content{background:var(--bg);border-radius:16px;padding:25px;max-width:400px;width:90%;border:1px solid var(--card-border);max-height:80vh;overflow-y:auto}
.modal-large{max-width:550px}
.modal-title{color:var(--text);margin-bottom:20px;font-weight:500}
.modal-actions{display:flex;gap:10px;margin-top:20px}
.modal-actions .btn{flex:1}
.btn-cancel{background:rgba(107,114,128,0.2);color:var(--text-muted)}
.btn-save{background:var(--success);color:#fff;font-weight:500}
.message{position:fixed;top:20px;left:50%;transform:translateX(-50%);padding:12px 24px;border-radius:8px;z-index:1001;display:none;font-weight:500}
.message.success{background:var(--success);color:#fff;display:block}
.message.error{background:var(--danger);color:#fff;display:block}
.wake-all-btn{background:linear-gradient(135deg,#4facfe,#00f2fe);color:#fff;width:100%;padding:14px;font-weight:500;margin-bottom:15px}
.empty-state{text-align:center;color:var(--text-muted);padding:30px}
.status-bar{display:flex;justify-content:space-between;align-items:center;padding:12px 16px;background:rgba(0,230,118,0.12);border-radius:10px;margin-bottom:16px}
.status-dot{width:10px;height:10px;background:var(--success);border-radius:50%;margin-right:10px;box-shadow:0 0 8px var(--success)}
.status-text{color:var(--success);display:flex;align-items:center;font-weight:500}
.ip-text{color:var(--text-muted);font-size:0.9rem}
.progress-bar{height:6px;background:rgba(255,255,255,0.1);border-radius:3px;margin:12px 0;overflow:hidden}
.progress-fill{height:100%;background:linear-gradient(90deg,#4facfe,#00f2fe);transition:width .3s;border-radius:3px}
.progress-text{color:var(--accent);font-size:0.9rem;margin-top:8px;text-align:center}
.scan-results{list-style:none;max-height:350px;overflow-y:auto}
.scan-item{display:flex;justify-content:space-between;align-items:center;padding:12px;background:var(--card);border-radius:8px;margin-bottom:8px;border:1px solid var(--card-border)}
.scan-item:hover{border-color:var(--accent)}
.scan-item-info{flex:1}
.scan-item-ip{color:var(--text);margin-bottom:4px;font-weight:500}
.scan-item-mac{color:var(--text-muted);font-family:monospace;font-size:0.85rem}
.btn-add-scan{padding:6px 14px;font-size:0.85rem;background:var(--success);color:#fff;border-radius:6px}
.switch{position:relative;display:inline-block;width:50px;height:26px}
.switch input{opacity:0;width:0;height:0}
.slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:rgba(107,114,128,0.3);border-radius:26px}
.slider:before{position:absolute;content:"";height:20px;width:20px;left:3px;bottom:3px;background:#fff;border-radius:50%}
input:checked+.slider{background:var(--success)}
input:checked+.slider:before{transform:translateX(24px)}
.ws-status{display:flex;align-items:center;gap:10px;font-size:0.85rem;margin-top:12px;padding:10px 14px;background:rgba(0,230,118,0.08);border-radius:8px;border:1px solid rgba(0,230,118,0.2);color:var(--success)}
.ws-status.disconnected{background:rgba(255,179,0,0.08);border-color:rgba(255,179,0,0.2);color:var(--warning)}
.ws-status.disabled{background:var(--card);border-color:var(--card-border);color:var(--text-muted)}
.ws-dot{width:8px;height:8px;border-radius:50%}
.ws-dot.connected{background:var(--success);box-shadow:0 0 6px var(--success)}
.ws-dot.connecting{background:var(--warning);animation:blink 1s infinite}
.ws-dot.disabled{background:var(--text-muted)}
@keyframes blink{50%{opacity:.4}}
.stats-grid{display:grid;grid-template-columns:repeat(2,1fr);gap:10px;margin-top:10px}
.stat-item{background:var(--card);border-radius:10px;padding:12px;border:1px solid var(--card-border)}
.stat-label{color:var(--text-muted);font-size:0.8rem;margin-bottom:4px}
.stat-value{color:var(--text);font-size:1rem;font-weight:500}
.stat-value.mono{font-family:monospace;font-size:0.95rem}
.stat-value .unit{color:var(--text-muted);font-size:0.75rem;font-weight:400;margin-left:2px}
</style>
</head>
<body>
<button class="theme-toggle" onclick="toggleTheme()" title="切换主题">🌓</button>
<div class="container">
<h1>WOL 唤醒器</h1>
<p class="subtitle">ESP32-S3 v3.0</p>
<div class="status-bar">
<span class="status-text"><span class="status-dot"></span>在线</span>
<span class="ip-text">%IP_ADDR%</span>
</div>
<div class="card">
<div class="card-title">设备列表</div>
<button class="btn wake-all-btn" onclick="wakeAll()">唤醒所有设备</button>
<ul class="device-list">%DEVICE_LIST%</ul>
<button class="btn btn-add" onclick="show('addModal')">+ 添加设备</button>
<button class="btn btn-scan" onclick="startScan()">扫描局域网</button>
<button class="btn btn-settings" onclick="loadSettings()">远程连接设置</button>
<div class="ws-status">
<span class="ws-dot disabled" id="wsDot"></span>
<span id="wsStatusText">远程: 检测中...</span>
</div>
</div>
<div class="card">
<div class="card-title">设备状态 <span id="chipModel" style="float:right;color:var(--text-muted);font-size:0.8rem;text-transform:none"></span></div>
<div class="stats-grid">
<div class="stat-item"><div class="stat-label">运行时间</div><div class="stat-value mono" id="statUptime">--</div></div>
<div class="stat-item"><div class="stat-label">唤醒次数</div><div class="stat-value" id="statWakeCount">--</div></div>
<div class="stat-item"><div class="stat-label">WiFi 信号</div><div class="stat-value" id="statRssi">--</div></div>
<div class="stat-item"><div class="stat-label">芯片温度</div><div class="stat-value" id="statTemp">--</div></div>
<div class="stat-item"><div class="stat-label">内存剩余</div><div class="stat-value mono" id="statHeap">--</div></div>
<div class="stat-item"><div class="stat-label">WiFi 网络</div><div class="stat-value" id="statSsid">--</div></div>
</div>
</div>
</div>
<div class="message" id="msg"></div>
<div class="modal" id="addModal">
<div class="modal-content">
<h3 class="modal-title">添加新设备</h3>
<div class="form-group"><label>设备名称</label><input type="text" id="deviceName" placeholder="我的电脑"></div>
<div class="form-group"><label>MAC地址</label><input type="text" id="deviceMac" placeholder="AA:BB:CC:DD:EE:FF"></div>
<div class="modal-actions">
<button class="btn btn-cancel" onclick="hide('addModal')">取消</button>
<button class="btn btn-save" onclick="addDevice()">保存</button>
</div>
</div>
</div>
<div class="modal" id="scanModal">
<div class="modal-content modal-large">
<h3 class="modal-title">局域网扫描</h3>
<div id="scanProgress">
<div class="progress-bar"><div class="progress-fill" id="progressFill" style="width:0%"></div></div>
<div class="progress-text" id="progressText">准备扫描...</div>
</div>
<ul class="scan-results" id="scanResults"></ul>
<div class="modal-actions"><button class="btn btn-cancel" onclick="hide('scanModal')">关闭</button></div>
</div>
</div>
<div class="modal" id="settingsModal">
<div class="modal-content">
<h3 class="modal-title">远程连接设置</h3>
<div class="form-group"><label>启用远程连接</label><label class="switch"><input type="checkbox" id="wsEnabled"><span class="slider"></span></label></div>
<div class="form-group"><label>WebSocket地址</label><input type="text" id="wsServer" placeholder="ws://server/ws"></div>
<div class="form-group"><label>用户名</label><input type="text" id="wsUser"></div>
<div class="form-group"><label>密码</label><input type="password" id="wsPass"></div>
<div class="modal-actions">
<button class="btn btn-cancel" onclick="hide('settingsModal')">取消</button>
<button class="btn btn-save" onclick="saveSettings()">保存</button>
</div>
</div>
</div>
<script>
let scanInt=null;
let uptimeBase=0;
let uptimeStart=0;
let uptimeTimer=null;
// 主题切换
function toggleTheme(){document.body.classList.toggle('light');localStorage.setItem('theme',document.body.classList.contains('light')?'light':'dark')}
if(localStorage.getItem('theme')==='light')document.body.classList.add('light');
function msg(t,s){const m=document.getElementById('msg');m.textContent=t;m.className='message '+s;setTimeout(()=>m.className='message',2500)}
function updateWsStatus(){fetch('/settings').then(r=>r.json()).then(d=>{const dot=document.getElementById('wsDot'),txt=document.getElementById('wsStatusText'),box=txt.parentElement;if(!d.ws_enabled){dot.className='ws-dot disabled';box.className='ws-status disabled';txt.textContent='远程: 未启用'}else if(d.ws_connected){dot.className='ws-dot connected';box.className='ws-status';txt.textContent='远程: 已连接'}else{dot.className='ws-dot connecting';box.className='ws-status disconnected';txt.textContent='远程: 连接中...'}}).catch(()=>{const txt=document.getElementById('wsStatusText');txt.parentElement.className='ws-status disabled';txt.textContent='远程: 状态未知'})}
setInterval(updateWsStatus,5000);updateWsStatus();
function show(id){document.getElementById(id).classList.add('show')}
function hide(id){document.getElementById(id).classList.remove('show')}
function wakeDevice(i){fetch('/wake?index='+i).then(r=>r.json()).then(d=>msg(d.success?'已唤醒 '+d.device:'失败: '+d.message,d.success?'success':'error')).catch(()=>msg('请求失败','error'))}
function wakeAll(){fetch('/wake?all=1').then(r=>r.json()).then(d=>msg(d.success?'已唤醒所有设备':'失败','success')).catch(()=>msg('请求失败','error'))}
function deleteDevice(i){if(!confirm('确定删除?'))return;fetch('/delete?index='+i).then(r=>r.json()).then(d=>d.success&&location.reload())}
function addDevice(){const n=document.getElementById('deviceName').value.trim(),m=document.getElementById('deviceMac').value.trim();if(!n||!m)return msg('请填写完整','error');fetch('/add?name='+encodeURIComponent(n)+'&mac='+encodeURIComponent(m)).then(r=>r.json()).then(d=>d.success?location.reload():msg('添加失败','error'))}
function startScan(){show('scanModal');document.getElementById('scanProgress').style.display='block';document.getElementById('progressFill').style.width='0%';document.getElementById('progressText').textContent='正在启动扫描...';document.getElementById('scanResults').innerHTML='';fetch('/scan').then(r=>r.json()).then(d=>{if(d.success)scanInt=setInterval(checkScan,500);else msg(d.message||'启动失败','error')})}
function checkScan(){fetch('/scan/results').then(r=>r.json()).then(d=>{document.getElementById('progressFill').style.width=d.progress+'%';document.getElementById('progressText').textContent='扫描进度 '+d.progress+'% · 已发现 '+d.devices.length+' 个设备';if(!d.scanning){clearInterval(scanInt);showResults(d.devices)}}).catch(()=>{clearInterval(scanInt);msg('扫描出错','error')})}
function showResults(d){document.getElementById('scanProgress').style.display='none';const l=document.getElementById('scanResults');if(!d.length){l.innerHTML='<li class="empty-state">未发现设备，请确保设备在线</li>';return}l.innerHTML=d.map(e=>'<li class="scan-item"><div class="scan-item-info"><div class="scan-item-ip">'+(e.name||e.ip)+'</div><div class="scan-item-mac">'+e.mac+(e.name?' <span style="color:var(--text-muted)">'+e.ip+'</span>':'')+'</div></div><button class="btn btn-add-scan" onclick="addScanned(\''+e.mac+'\',\''+(e.name||e.ip)+'\')">添加</button></li>').join('')}
function addScanned(m,n){hide('scanModal');document.getElementById('deviceName').value=n;document.getElementById('deviceMac').value=m;show('addModal')}
function loadSettings(){fetch('/settings').then(r=>r.json()).then(d=>{document.getElementById('wsEnabled').checked=d.ws_enabled;document.getElementById('wsServer').value=d.ws_server||'';document.getElementById('wsUser').value=d.ws_user||'';show('settingsModal')})}
function saveSettings(){const d={ws_server:document.getElementById('wsServer').value,ws_user:document.getElementById('wsUser').value,ws_pass:document.getElementById('wsPass').value,ws_enabled:document.getElementById('wsEnabled').checked};fetch('/settings/save?'+Object.keys(d).map(k=>k+'='+encodeURIComponent(d[k])).join('&')).then(r=>r.json()).then(r=>{msg('设置已保存','success');hide('settingsModal')})}
function formatUptime(s){const d=Math.floor(s/86400),h=Math.floor((s%86400)/3600),m=Math.floor((s%3600)/60),sec=s%60;return d+':'+String(h).padStart(2,'0')+':'+String(m).padStart(2,'0')+':'+String(sec).padStart(2,'0')}
function updateUptimeDisplay(){if(uptimeBase>0){const elapsed=Math.floor((Date.now()-uptimeStart)/1000);const total=uptimeBase+elapsed;document.getElementById('statUptime').innerHTML=formatUptime(total)+'<span class="unit">(天:时:分:秒)</span>'}}
function fetchStatus(){fetch('/status').then(r=>r.json()).then(d=>{uptimeBase=d.uptime||0;uptimeStart=Date.now();if(!uptimeTimer){uptimeTimer=setInterval(updateUptimeDisplay,1000)}updateUptimeDisplay();document.getElementById('statWakeCount').textContent=(d.wake_count||0)+' 次';const rssi=d.wifi_rssi||0,q=rssi>=-50?'极强':rssi>=-65?'良好':rssi>=-75?'一般':'弱';document.getElementById('statRssi').innerHTML=rssi+' dBm<span class="unit">('+q+')</span>';const temp=d.temperature||0,tq=temp<40?'正常':temp<55?'偏高':'过热';document.getElementById('statTemp').innerHTML=temp.toFixed(1)+'°C<span class="unit">('+tq+')</span>';document.getElementById('statHeap').textContent=((d.free_heap||0)/1024).toFixed(1)+' KB';document.getElementById('statSsid').textContent=d.ssid||'--';if(d.chip_model)document.getElementById('chipModel').textContent=d.chip_model}).catch(()=>{})}
fetchStatus();setInterval(fetchStatus,30000);
</script>
</body>
</html>
)rawliteral";

#endif
