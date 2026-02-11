#include "fl_web.h"
#include "fl_storage.h"
#include "fl_comms.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <Update.h>

static char _webUser[32] = "admin";
static char _webPass[32] = "";

static AsyncWebServer _server(80);

// Forward declarations from fl_comms.cpp
const char* fl_getFwName();
const char* fl_getFwVersion();
const char* fl_getHwType();

void fl_setWebAuth(const char* user, const char* pass) {
  strncpy(_webUser, user, sizeof(_webUser) - 1);
  strncpy(_webPass, pass, sizeof(_webPass) - 1);
}

bool fl_checkAuth(AsyncWebServerRequest *request) {
  if (strlen(_webPass) == 0) return true;  // No auth if no password set
  if (!request->authenticate(_webUser, _webPass)) {
    request->requestAuthentication();
    return false;
  }
  return true;
}

AsyncWebServer& fl_getWebServer() {
  return _server;
}

void fl_startWebServer() {
  _server.begin();
  Serial.println("Web server started on port 80");
}

void fl_setupBaseWebRoutes() {
  // API: device info
  _server.on("/api/device", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    StaticJsonDocument<512> doc;
    doc["device_id"] = fl_deviceId;
    doc["hardware_type"] = fl_getHwType();
    doc["firmware"] = fl_getFwVersion();
    doc["name"] = fl_getFwName();
    doc["ip"] = WiFi.localIP().toString();
    doc["mac"] = WiFi.macAddress();
    doc["rssi"] = WiFi.RSSI();
    doc["mqtt_connected"] = fl_mqttConnected;
    doc["topic_telemetry"] = fl_topicTelemetry;
    doc["topic_command"] = fl_topicCommand;
    doc["topic_status"] = fl_topicStatus;
    doc["dashboard_url"] = String("https://voltageza.github.io/fieldlink-dashboard/?device=") + fl_deviceId;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // API: MQTT config GET
  _server.on("/api/mqtt", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    StaticJsonDocument<256> doc;
    doc["host"] = fl_mqttHost;
    doc["port"] = fl_mqttPort;
    doc["user"] = fl_mqttUser;
    doc["pass"] = "********";
    doc["tls"] = fl_mqttUseTls;
    doc["connected"] = fl_mqttConnected;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // API: MQTT config POST
  _server.on("/api/mqtt", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    bool changed = false;

    if (request->hasParam("host", true)) {
      strncpy(fl_mqttHost, request->getParam("host", true)->value().c_str(), sizeof(fl_mqttHost) - 1);
      changed = true;
    }
    if (request->hasParam("port", true)) {
      fl_mqttPort = request->getParam("port", true)->value().toInt();
      changed = true;
    }
    if (request->hasParam("user", true)) {
      strncpy(fl_mqttUser, request->getParam("user", true)->value().c_str(), sizeof(fl_mqttUser) - 1);
      changed = true;
    }
    if (request->hasParam("pass", true)) {
      strncpy(fl_mqttPass, request->getParam("pass", true)->value().c_str(), sizeof(fl_mqttPass) - 1);
      changed = true;
    }
    if (request->hasParam("tls", true)) {
      fl_mqttUseTls = request->getParam("tls", true)->value() == "true";
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

  // API: MQTT config reset
  _server.on("/api/mqtt/reset", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    fl_resetMqttConfig();
    request->send(200, "text/plain", "Config reset. Rebooting...");
    delay(1000);
    ESP.restart();
  });

  // MQTT configuration page
  _server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    String html = R"rawliteral(
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
    request->send(200, "text/html", html);
  });

  // Firmware update page
  _server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = R"rawliteral(
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
      <div class="warning">Warning: Device will restart after update.</div>
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
    request->send(200, "text/html", html);
  });

  // API: firmware upload
  _server.on("/api/update", HTTP_POST,
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
