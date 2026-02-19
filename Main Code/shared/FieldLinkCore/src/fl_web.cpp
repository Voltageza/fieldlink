#include "fl_web.h"
#include "fl_storage.h"
#include "fl_ota.h"
#include <ArduinoJson.h>
#include <Update.h>
#include <WiFi.h>

// Forward-declare from fl_comms to avoid Ethernet.h/ESPAsyncWebServer HTTP_PATCH conflict
extern bool fl_mqttConnected;

AsyncWebServer fl_server(80);

static char _web_user_buf[32] = "admin";
static char _web_pass_buf[64] = "admin";
static const char* _web_user = _web_user_buf;
static const char* _web_pass = _web_pass_buf;
static const char* _dashboard_html = nullptr;

void fl_setWebAuth(const char* user, const char* pass) {
  strncpy(_web_user_buf, user, sizeof(_web_user_buf) - 1);
  strncpy(_web_pass_buf, pass, sizeof(_web_pass_buf) - 1);
  _web_user = _web_user_buf;
  _web_pass = _web_pass_buf;
}

void fl_setDashboardHtml(const char* html) {
  _dashboard_html = html;
}

bool fl_checkAuth(AsyncWebServerRequest *request) {
  if (!request->authenticate(_web_user, _web_pass)) {
    request->requestAuthentication();
    return false;
  }
  return true;
}

// MQTT Configuration page HTML
static const char CONFIG_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>FieldLink - MQTT Config</title>
  <style>
    body { font-family: -apple-system, sans-serif; background: #1a1a2e; color: #eee; padding: 20px; }
    .container { max-width: 400px; margin: 0 auto; }
    h1 { color: #00d4ff; font-size: 24px; }
    .card { background: #16213e; border-radius: 12px; padding: 20px; margin: 20px 0; }
    label { display: block; margin: 15px 0 5px; color: #888; font-size: 12px; text-transform: uppercase; }
    input, select { width: 100%; padding: 12px; border: 1px solid #333; border-radius: 6px; background: #0f0f23; color: #fff; font-size: 16px; box-sizing: border-box; }
    input:focus { border-color: #00d4ff; outline: none; }
    button { width: 100%; padding: 14px; border: none; border-radius: 6px; font-size: 16px; font-weight: bold; cursor: pointer; margin-top: 10px; }
    .btn-primary { background: #00d4ff; color: #000; }
    .btn-danger { background: #ff4757; color: #fff; }
    .btn-secondary { background: #333; color: #fff; }
    .status { padding: 10px; border-radius: 6px; margin: 10px 0; text-align: center; }
    .status.connected { background: #00ff8820; color: #00ff88; }
    .status.disconnected { background: #ff475720; color: #ff4757; }
    .device-id { font-family: monospace; font-size: 20px; color: #00d4ff; text-align: center; padding: 10px; background: #0f0f23; border-radius: 6px; }
  </style>
</head>
<body>
  <div class="container">
    <h1>FieldLink Config</h1>
    <div class="device-id" id="deviceId">Loading...</div>
    <div class="status disconnected" id="mqttStatus">MQTT: Checking...</div>
    <div class="card">
      <h3>MQTT Broker</h3>
      <label>Host</label>
      <input type="text" id="host" placeholder="broker.example.com">
      <label>Port</label>
      <input type="number" id="port" value="8883">
      <label>Username</label>
      <input type="text" id="user" placeholder="username">
      <label>Password</label>
      <input type="password" id="pass" placeholder="password">
      <label>Use TLS/SSL</label>
      <select id="tls">
        <option value="true">Yes (Port 8883)</option>
        <option value="false">No (Port 1883)</option>
      </select>
      <button class="btn-primary" onclick="saveConfig()">Save and Reboot</button>
      <button class="btn-danger" onclick="resetConfig()">Reset to Defaults</button>
    </div>
    <div class="card">
      <button class="btn-secondary" onclick="location.href='/update'">Firmware Update</button>
      <button class="btn-secondary" onclick="location.href='/'">Back to Dashboard</button>
    </div>
  </div>
  <script>
    async function loadConfig() {
      try {
        var res = await fetch('/api/mqtt');
        var cfg = await res.json();
        document.getElementById('host').value = cfg.host;
        document.getElementById('port').value = cfg.port;
        document.getElementById('user').value = cfg.user;
        document.getElementById('tls').value = cfg.tls ? 'true' : 'false';
        document.getElementById('mqttStatus').textContent = 'MQTT: ' + (cfg.connected ? 'Connected' : 'Disconnected');
        document.getElementById('mqttStatus').className = 'status ' + (cfg.connected ? 'connected' : 'disconnected');
        var devRes = await fetch('/api/device');
        var dev = await devRes.json();
        document.getElementById('deviceId').textContent = dev.device_id;
      } catch(e) { console.error(e); }
    }
    async function saveConfig() {
      var data = new URLSearchParams();
      data.append('host', document.getElementById('host').value);
      data.append('port', document.getElementById('port').value);
      data.append('user', document.getElementById('user').value);
      data.append('pass', document.getElementById('pass').value);
      data.append('tls', document.getElementById('tls').value);
      try {
        var res = await fetch('/api/mqtt', { method: 'POST', body: data });
        alert(await res.text());
      } catch(e) { alert('Error: ' + e); }
    }
    async function resetConfig() {
      if (confirm('Reset MQTT config to defaults?')) {
        try {
          var res = await fetch('/api/mqtt/reset', { method: 'POST' });
          alert(await res.text());
        } catch(e) { alert('Error: ' + e); }
      }
    }
    loadConfig();
  </script>
</body>
</html>
)rawliteral";

// Firmware Update page HTML
static const char UPDATE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>FieldLink - Firmware Update</title>
  <style>
    body { font-family: -apple-system, sans-serif; background: #1a1a2e; color: #eee; padding: 20px; }
    .container { max-width: 400px; margin: 0 auto; }
    h1 { color: #00d4ff; font-size: 24px; }
    .card { background: #16213e; border-radius: 12px; padding: 20px; margin: 20px 0; }
    .device-id { font-family: monospace; font-size: 20px; color: #00d4ff; text-align: center; padding: 10px; background: #0f0f23; border-radius: 6px; margin-bottom: 20px; }
    .version { text-align: center; color: #888; margin-bottom: 20px; }
    input[type="file"] { width: 100%; padding: 12px; border: 2px dashed #00d4ff; border-radius: 6px; background: #0f0f23; color: #fff; cursor: pointer; }
    input[type="file"]:hover { background: #1a1a3e; }
    button { width: 100%; padding: 14px; border: none; border-radius: 6px; font-size: 16px; font-weight: bold; cursor: pointer; margin-top: 10px; }
    .btn-primary { background: #00d4ff; color: #000; }
    .btn-secondary { background: #333; color: #fff; }
    .btn-primary:disabled { opacity: 0.5; cursor: not-allowed; }
    .progress { width: 100%; height: 30px; background: #0f0f23; border-radius: 6px; margin: 20px 0; overflow: hidden; display: none; }
    .progress-bar { height: 100%; background: linear-gradient(90deg, #00d4ff, #00ff88); width: 0%; transition: width 0.3s; text-align: center; line-height: 30px; color: #000; font-weight: bold; }
    .status { padding: 10px; border-radius: 6px; margin: 10px 0; text-align: center; display: none; }
    .status.success { background: #00ff8820; color: #00ff88; display: block; }
    .status.error { background: #ff475720; color: #ff4757; display: block; }
    .warning { background: #ff9f4320; color: #ff9f43; padding: 10px; border-radius: 6px; margin: 10px 0; font-size: 14px; }
  </style>
</head>
<body>
  <div class="container">
    <h1>Firmware Update</h1>
    <div class="device-id" id="deviceId">Loading...</div>
    <div class="version">Current Version: <span id="version">--</span></div>
    <div class="card">
      <h3>Upload New Firmware</h3>
      <div class="warning">Warning: Device will restart after update. Ensure pump is stopped before proceeding.</div>
      <input type="file" id="fileInput" accept=".bin">
      <div class="progress" id="progressBar">
        <div class="progress-bar" id="progressBarFill">0%</div>
      </div>
      <div class="status" id="status"></div>
      <button class="btn-primary" id="uploadBtn" onclick="uploadFirmware()">Upload Firmware</button>
      <button class="btn-secondary" onclick="location.href='/config'">Back to Config</button>
    </div>
  </div>
  <script>
    async function loadInfo() {
      try {
        const res = await fetch('/api/device');
        const dev = await res.json();
        document.getElementById('deviceId').textContent = dev.device_id;
        document.getElementById('version').textContent = dev.firmware;
      } catch(e) { console.error(e); }
    }
    async function uploadFirmware() {
      const fileInput = document.getElementById('fileInput');
      const file = fileInput.files[0];
      if (!file) { alert('Please select a firmware file (.bin)'); return; }
      if (!file.name.endsWith('.bin')) { alert('Please select a valid .bin firmware file'); return; }
      if (!confirm('Upload firmware and restart device?')) return;
      const uploadBtn = document.getElementById('uploadBtn');
      const progressBar = document.getElementById('progressBar');
      const progressBarFill = document.getElementById('progressBarFill');
      const status = document.getElementById('status');
      uploadBtn.disabled = true;
      fileInput.disabled = true;
      progressBar.style.display = 'block';
      status.style.display = 'none';
      const formData = new FormData();
      formData.append('firmware', file);
      try {
        const xhr = new XMLHttpRequest();
        xhr.upload.addEventListener('progress', (e) => {
          if (e.lengthComputable) {
            const percent = (e.loaded / e.total) * 100;
            progressBarFill.style.width = percent + '%';
            progressBarFill.textContent = Math.round(percent) + '%';
          }
        });
        xhr.addEventListener('load', () => {
          if (xhr.status === 200) {
            status.className = 'status success';
            status.textContent = 'Update successful! Device will restart...';
            status.style.display = 'block';
            setTimeout(() => { location.href = '/'; }, 10000);
          } else {
            status.className = 'status error';
            status.textContent = 'Update failed: ' + xhr.responseText;
            status.style.display = 'block';
            uploadBtn.disabled = false;
            fileInput.disabled = false;
          }
        });
        xhr.addEventListener('error', () => {
          status.className = 'status error';
          status.textContent = 'Upload failed. Check connection.';
          status.style.display = 'block';
          uploadBtn.disabled = false;
          fileInput.disabled = false;
        });
        xhr.open('POST', '/api/update');
        xhr.send(formData);
      } catch(e) {
        status.className = 'status error';
        status.textContent = 'Error: ' + e.message;
        status.style.display = 'block';
        uploadBtn.disabled = false;
        fileInput.disabled = false;
      }
    }
    loadInfo();
  </script>
</body>
</html>
)rawliteral";

void fl_setupWebRoutes() {
  // Dashboard (requires auth)
  fl_server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    if (_dashboard_html) {
      request->send_P(200, "text/html", _dashboard_html);
    } else {
      request->send(200, "text/plain", "No dashboard configured");
    }
  });

  // API endpoint for device info
  fl_server.on("/api/device", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    StaticJsonDocument<512> doc;
    doc["device_id"] = fl_DEVICE_ID;
    doc["hardware_type"] = fl_getHwType();
    doc["firmware"] = fl_getFwVersion();
    doc["name"] = fl_getFwName();
    doc["ip"] = WiFi.localIP().toString();
    doc["mac"] = WiFi.macAddress();
    doc["rssi"] = WiFi.RSSI();
    doc["mqtt_connected"] = fl_mqttConnected;
    doc["topic_telemetry"] = fl_TOPIC_TELEMETRY;
    doc["topic_command"] = fl_TOPIC_COMMAND;
    doc["topic_status"] = fl_TOPIC_STATUS;
    doc["dashboard_url"] = String("https://voltageza.github.io/fieldlink-dashboard/?device=") + fl_DEVICE_ID;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // MQTT config GET
  fl_server.on("/api/mqtt", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    StaticJsonDocument<256> doc;
    doc["host"] = fl_mqtt_host;
    doc["port"] = fl_mqtt_port;
    doc["user"] = fl_mqtt_user;
    doc["pass"] = "********";  // Don't expose password
    doc["tls"] = fl_mqtt_use_tls;
    doc["connected"] = fl_mqttConnected;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // MQTT config POST
  fl_server.on("/api/mqtt", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    bool changed = false;
    if (request->hasParam("host", true)) {
      strncpy(fl_mqtt_host, request->getParam("host", true)->value().c_str(), sizeof(fl_mqtt_host) - 1);
      changed = true;
    }
    if (request->hasParam("port", true)) {
      fl_mqtt_port = request->getParam("port", true)->value().toInt();
      changed = true;
    }
    if (request->hasParam("user", true)) {
      strncpy(fl_mqtt_user, request->getParam("user", true)->value().c_str(), sizeof(fl_mqtt_user) - 1);
      changed = true;
    }
    if (request->hasParam("pass", true)) {
      strncpy(fl_mqtt_pass, request->getParam("pass", true)->value().c_str(), sizeof(fl_mqtt_pass) - 1);
      changed = true;
    }
    if (request->hasParam("tls", true)) {
      fl_mqtt_use_tls = request->getParam("tls", true)->value() == "true";
      changed = true;
    }
    if (changed) {
      fl_saveMqttConfig();
      request->send(200, "text/plain", "Config saved. Rebooting...");
      delay(1000);
      ESP.restart();
    } else {
      request->send(400, "text/plain", "No parameters provided");
    }
  });

  // MQTT config reset
  fl_server.on("/api/mqtt/reset", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    fl_resetMqttConfig();
    request->send(200, "text/plain", "Config reset. Rebooting...");
    delay(1000);
    ESP.restart();
  });

  // MQTT Config page
  fl_server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    request->send_P(200, "text/html", CONFIG_HTML);
  });

  // Firmware Update page
  fl_server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", UPDATE_HTML);
  });

  // Firmware upload endpoint
  fl_server.on("/api/update", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (!fl_checkAuth(request)) return;
      bool updateSuccess = !Update.hasError();
      AsyncWebServerResponse *response = request->beginResponse(200, "text/plain",
        updateSuccess ? "Update Success! Rebooting..." : "Update Failed!");
      response->addHeader("Connection", "close");
      request->send(response);
      if (updateSuccess) {
        delay(1000);
        ESP.restart();
      }
    },
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (!index) {
        Serial.printf("HTTP OTA Update Start: %s\n", filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(Serial);
          request->send(500, "text/plain", "Update init failed");
          return;
        }
      }
      if (Update.write(data, len) != len) {
        Update.printError(Serial);
        request->send(500, "text/plain", "Update write failed");
        return;
      }
      if (final) {
        if (Update.end(true)) {
          Serial.printf("HTTP OTA Update Success: %u bytes\n", index + len);
        } else {
          Update.printError(Serial);
          request->send(500, "text/plain", "Update end failed");
        }
      }
    }
  );
}
