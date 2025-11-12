#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <NimBLEDevice.h>

/************** Wi-Fi AP **************/
static const char* AP_SSID = "ESP32_Config";
static const char* AP_PASS = ""; // 空串=开放AP，如需加密，填8+位

/************** NVS 键 **************/
Preferences prefs;
static const char* NS = "cfg";
static const char* KEY_BLE_NAME = "bleName";
static const char* KEY_SVC_UUID  = "svcUUID";
static const char* KEY_CHR_UUID  = "chrUUID";

/************** 缺省配置 **************/
String g_targetName   = "nRF5340DK"; // 目标外设广播名
String g_serviceUUID  = "0000ffff-0000-1000-8000-00805f9b34fb";
String g_charUUID     = "0000ff01-0000-1000-8000-00805f9b34fb"; // 不确定可先保留，后用 /discover 查

/************** Web Server **************/
WebServer server(80);

/************** BLE 全局 **************/
NimBLEClient* g_client = nullptr;
NimBLERemoteCharacteristic* g_remoteChr = nullptr;
bool g_isConnecting = false;
unsigned long g_lastScanAttemptMs = 0;
const unsigned long RECONNECT_INTERVAL_MS = 5000;

/************** 页面 **************/
const char* HTML_PAGE = R"HTML(
<!doctype html><html><head>
<meta charset="utf-8"/><meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>ESP32 BLE Config</title>
<style>
body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;margin:24px;max-width:720px}
h1{font-size:20px}label{display:block;margin:12px 0 6px}
input{width:100%;padding:10px;font-size:16px;box-sizing:border-box}
button{padding:10px 14px;font-size:16px;margin:6px 6px 6px 0;cursor:pointer}
.card{padding:16px;border:1px solid #ddd;border-radius:12px;margin-bottom:16px}
.row{display:flex;gap:8px;flex-wrap:wrap}.ok{color:#0a0}.bad{color:#a00}
code{background:#f5f5f5;padding:2px 6px;border-radius:6px}
</style></head><body>
<h1>ESP32 Web Config → BLE Control</h1>
<div class="card">
  <form method="POST" action="/save">
    <label>Target BLE Name (nRF5340DK advertising name)</label>
    <input name="bleName" value="%BLE_NAME%" required/>
    <label>Service UUID</label>
    <input name="svcUUID" value="%SVC_UUID%" required/>
    <label>Characteristic UUID (writable)</label>
    <input name="chrUUID" value="%CHR_UUID%" required/>
    <button type="submit">Save & Try Connect</button>
  </form>
</div>
<div class="card">
  <h3>Status</h3>
  <div id="status">Loading...</div>
  <div class="row">
    <button onclick="send('/led?state=on')">LED ON</button>
    <button onclick="send('/led?state=off')">LED OFF</button>
    <button onclick="send('/led?state=toggle')">TOGGLE</button>
    <button onclick="refresh()">Refresh</button>
    <button onclick="discover()">Discover</button>
  </div>
  <small>ESP32 writes: '1' / '0' / 'T'.</small>
</div>
<script>
function refresh(){
  fetch('/status').then(r=>r.json()).then(j=>{
    const ok=j.connected?'ok':'bad';
    document.getElementById('status').innerHTML=
      `<div>BLE Connected: <b class="${ok}">${j.connected}</b></div>
       <div>Target: <code>${j.bleName}</code></div>
       <div>Service: <code>${j.svcUUID}</code></div>
       <div>Char: <code>${j.chrUUID}</code></div>
       <div>Message: ${j.msg||''}</div>`;
  }).catch(_=>{document.getElementById('status').innerText='Failed to fetch status';});
}
function send(url){
  fetch(url,{method:'POST'}).then(r=>r.json()).then(j=>{
    alert(j.msg||JSON.stringify(j)); refresh();
  }).catch(_=>alert('Request failed'));
}
function discover(){
  fetch('/discover').then(r=>r.json()).then(j=>{
    alert((j.list&&j.list.length?j.list.join('\n'):'No services/chars found.'));
  }).catch(_=>alert('Discover failed'));
}
refresh();
</script>
</body></html>
)HTML";

/************** 小工具 **************/
String renderIndex() {
  String page = HTML_PAGE;
  page.replace("%BLE_NAME%", g_targetName);
  page.replace("%SVC_UUID%", g_serviceUUID);
  page.replace("%CHR_UUID%", g_charUUID);
  return page;
}

/************** 扫描 + 连接（注意 1.4.3 的返回类型） **************/
bool connectToTarget() {
  if (g_isConnecting) return false;
  g_isConnecting = true;

  if (g_client && g_client->isConnected()) { g_isConnecting = false; return true; }
  if (!g_client) {
    g_client = NimBLEDevice::createClient();
    g_client->setConnectTimeout(10);
  }

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->stop();
  scan->clearResults();
  scan->setActiveScan(true);
  scan->setInterval(45);
  scan->setWindow(30);
  scan->setDuplicateFilter(true);

  Serial.printf("[BLE] Scanning for: name='%s' or service=%s\n",
                g_targetName.c_str(), g_serviceUUID.c_str());

  // v1.4.3: start() 返回 NimBLEScanResults（不是 bool）
  NimBLEScanResults results = scan->start(7 /*seconds*/, false /*is_continue*/);
  Serial.printf("[DBG] 扫描完成, 发现 %d 个设备.\n", results.getCount());

  NimBLEUUID svcUUID(g_serviceUUID.c_str());
  bool found = false;
  NimBLEAddress candidateAddr;

  for (int i = 0; i < results.getCount(); ++i) {
    // v1.4.3: getDevice(i) 返回对象（不是指针），所以用 '.'
    NimBLEAdvertisedDevice d = results.getDevice(i);
    std::string nm = d.getName();

    Serial.printf("[DBG] Dev: %s RSSI=%d Name='%s'\n",
                  d.getAddress().toString().c_str(), d.getRSSI(), nm.c_str());

    bool nameHit = (!nm.empty() && String(nm.c_str()).equalsIgnoreCase(g_targetName));
    bool svcHit  = d.isAdvertisingService(svcUUID);

    if (nameHit || svcHit) {
      candidateAddr = d.getAddress();
      Serial.printf("[BLE] Candidate: %s (name='%s' svcHit=%c)\n",
                    candidateAddr.toString().c_str(), nm.c_str(), svcHit?'Y':'N');
      found = true;
      break;
    }
  }

  scan->clearResults();

  if (!found) {
    Serial.println("[BLE] Target not found.");
    g_isConnecting = false;
    return false;
  }

  Serial.printf("[BLE] Connecting to %s ...\n", candidateAddr.toString().c_str());
  if (!g_client->connect(candidateAddr)) {
    Serial.println("[BLE] Connect() failed.");
    g_isConnecting = false; return false;
  }
  Serial.println("[BLE] Connected.");

  // 找服务
  NimBLERemoteService* svc = g_client->getService(g_serviceUUID.c_str());
  if (!svc) {
    Serial.println("[BLE] Service NOT found on peer.");
    g_client->disconnect(); g_isConnecting = false; return false;
  }

  // 优先用配置的特征；找不到则自动挑一个“可写”的
  g_remoteChr = svc->getCharacteristic(g_charUUID.c_str());
  if (!g_remoteChr) {
    Serial.println("[BLE] Configured characteristic not found, try picking a writable one...");
    std::vector<NimBLERemoteCharacteristic*>* chs = svc->getCharacteristics(true);
    if (chs) {
      for (auto* c : *chs) {
        if (c->canWrite() || c->canWriteNoResponse()) { g_remoteChr = c; break; }
      }
    }
  }

  if (!g_remoteChr) {
    Serial.println("[BLE] No writable characteristic under the service.");
    g_client->disconnect(); g_isConnecting = false; return false;
  }

  if (!g_remoteChr->canWrite() && !g_remoteChr->canWriteNoResponse()) {
    Serial.println("[BLE] Selected characteristic is not writable.");
    g_client->disconnect(); g_isConnecting = false; return false;
  }

  Serial.printf("[BLE] Using characteristic %s (props: %s%s)\n",
                g_remoteChr->getUUID().toString().c_str(),
                g_remoteChr->canWrite() ? "W" : "",
                g_remoteChr->canWriteNoResponse() ? "/WN" : "");
  g_isConnecting = false;
  return true;
}

void ensureConnectedLoop() {
  if (g_client && g_client->isConnected()) return;
  unsigned long now = millis();
  if (now - g_lastScanAttemptMs < RECONNECT_INTERVAL_MS) return;
  g_lastScanAttemptMs = now;
  connectToTarget();
}

/************** 写入 '1'/'0'/'T' **************/
bool writeCommand(char c) {
  if (!g_client || !g_client->isConnected() || !g_remoteChr) {
    Serial.println("[BLE] Not connected."); return false;
  }
  const bool noRsp = g_remoteChr->canWriteNoResponse();
  const uint8_t b = static_cast<uint8_t>(c);
  bool ok = g_remoteChr->writeValue(&b, 1, !noRsp /*withResponse?*/);
  Serial.printf("[BLE] Write '%c' -> %s\n", c, ok ? "OK" : "FAIL");
  return ok;
}

/************** Web 处理 **************/
void handleRoot() {
  server.send(200, "text/html; charset=utf-8", renderIndex());
}

void handleSave() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed"); return;
  }
  String bleName = server.arg("bleName");
  String svcUUID = server.arg("svcUUID");
  String chrUUID = server.arg("chrUUID");
  if (bleName.isEmpty() || svcUUID.isEmpty() || chrUUID.isEmpty()) {
    server.send(400, "text/plain", "Missing parameters"); return;
  }

  g_targetName  = bleName;
  g_serviceUUID = svcUUID;
  g_charUUID    = chrUUID;

  prefs.begin(NS, false);
  prefs.putString(KEY_BLE_NAME, g_targetName);
  prefs.putString(KEY_SVC_UUID,  g_serviceUUID);
  prefs.putString(KEY_CHR_UUID,  g_charUUID);
  prefs.end();

  if (g_client && g_client->isConnected()) g_client->disconnect();
  g_lastScanAttemptMs = 0;

  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "Saved.");
}

void handleStatus() {
  bool connected = (g_client && g_client->isConnected() && g_remoteChr);
  String msg = connected ? "BLE is connected, ready to write."
                         : "Not connected. ESP32 is trying to reconnect.";
  String json = String("{\"connected\":") + (connected ? "true":"false") +
                ",\"bleName\":\"" + g_targetName + "\"" +
                ",\"svcUUID\":\"" + g_serviceUUID + "\"" +
                ",\"chrUUID\":\"" + g_charUUID + "\"" +
                ",\"msg\":\"" + msg + "\"}";
  server.send(200, "application/json", json);
}

void handleLED() {
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"msg\":\"Method Not Allowed\"}"); return;
  }
  String state = server.arg("state");
  char cmd = 0;
  if (state == "on") cmd = '1';
  else if (state == "off") cmd = '0';
  else if (state == "toggle") cmd = 'T';
  else { server.send(400, "application/json", "{\"msg\":\"Use state=on/off/toggle\"}"); return; }

  bool ok = writeCommand(cmd);
  server.send(200, "application/json",
              String("{\"ok\":") + (ok?"true":"false") +
              ",\"msg\":\"" + (ok?"Write succeeded.":"Write failed (not connected?).") + "\"}");
}

/* 列出服务/特征与属性，方便确认 UUID 与可写性 */
void handleDiscover() {
  String out = "{\"list\":[";
  bool first = true;

  if (g_client && g_client->isConnected()) {
    auto *svcs = g_client->getServices(true);
    if (svcs) {
      for (auto *s : *svcs) {
        if (!first) out += ",";
        first = false;
        out += "\"SVC ";
        out += s->getUUID().toString().c_str();
        out += "\"";
        auto *chs = s->getCharacteristics(true);
        if (chs) {
          for (auto *c : *chs) {
            out += ",\"  CHR ";
            out += c->getUUID().toString().c_str();
            out += " props:";
            if (c->canRead()) out += "R";
            if (c->canWrite()) out += "W";
            if (c->canWriteNoResponse()) out += "WN";
            if (c->canNotify()) out += "N";
            if (c->canIndicate()) out += "I";
            out += "\"";
          }
        }
      }
    }
  }
  out += "]}";
  server.send(200, "application/json", out);
}

/************** Wi-Fi + Web 初始化 **************/
void setupWiFiAP() {
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(AP_SSID, AP_PASS)) {
    Serial.println("[WiFi] Failed to start AP.");
  } else {
    Serial.printf("[WiFi] AP started: %s\n", AP_SSID);
    Serial.printf("[WiFi] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  }
}

void setupWeb() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/led", HTTP_POST, handleLED);
  server.on("/discover", HTTP_GET, handleDiscover);
  server.begin();
  Serial.println("[Web] HTTP server started.");
}

/************** BLE 初始化 **************/
void setupBLE() {
  NimBLEDevice::init("ESP32-BLE-Central");
  // 为避免不同核心版本的枚举差异，这里不再强制 setPower
  // 如果你确实要改发射功率，可解注释并用存在的枚举：
  /*
  #if defined(ESP_PWR_LVL_P9)
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  #elif defined(ESP_PWR_LVL_P7)
    NimBLEDevice::setPower(ESP_PWR_LVL_P7);
  #endif
  */
  NimBLEDevice::setSecurityAuth(false, false, true); // 无需配对
}

/************** 载入配置 **************/
void loadConfig() {
  prefs.begin(NS, true);
  g_targetName  = prefs.getString(KEY_BLE_NAME, g_targetName);
  g_serviceUUID = prefs.getString(KEY_SVC_UUID,  g_serviceUUID);
  g_charUUID    = prefs.getString(KEY_CHR_UUID,  g_charUUID);
  prefs.end();

  Serial.println("[CFG] Target: " + g_targetName);
  Serial.println("[CFG] Service: " + g_serviceUUID);
  Serial.println("[CFG] Char   : " + g_charUUID);
}

/************** Arduino 入口 **************/
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP32 Web Config + BLE Central (PIO-ready, NimBLE 1.4.3) ===");

  loadConfig();
  setupWiFiAP();
  setupWeb();
  setupBLE();

  g_lastScanAttemptMs = 0; // 立即尝试连接
}

void loop() {
  server.handleClient();
  ensureConnectedLoop();

  // 串口直接输入 '1'/'0'/'T' 也可控制
  if (Serial.available()) {
    char c = (char)Serial.read();
    if (c=='1' || c=='0' || c=='T') writeCommand(c);
  }

  delay(10);
}
