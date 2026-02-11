#include "fl_ota.h"
#include "fl_storage.h"
#include "fl_comms.h"
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Update.h>

static const char* _otaPassword = "";

void fl_setOtaPassword(const char* pass) {
  _otaPassword = pass;
}

void fl_initArduinoOTA() {
  ArduinoOTA.setHostname(fl_deviceId);
  ArduinoOTA.setPassword(_otaPassword);

  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("OTA: Start updating " + type);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA: Update complete!");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Progress: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  ArduinoOTA.begin();
  Serial.printf("ArduinoOTA ready. Hostname: %s\n", fl_deviceId);
}

void fl_handleOTA() {
  ArduinoOTA.handle();
}

void fl_performRemoteOTA(const char* firmwareUrl) {
  if (!fl_wifiConnected) {
    Serial.println("Cannot update - WiFi not connected");
    return;
  }

  Serial.println("===========================================");
  Serial.println("REMOTE FIRMWARE UPDATE STARTED");
  Serial.printf("URL: %s\n", firmwareUrl);
  Serial.println("===========================================");

  HTTPClient http;
  http.begin(firmwareUrl);

  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("Firmware download failed, HTTP code: %d\n", httpCode);
    http.end();
    return;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0) {
    Serial.println("Invalid content length");
    http.end();
    return;
  }

  Serial.printf("Firmware size: %d bytes\n", contentLength);

  bool canBegin = Update.begin(contentLength);
  if (!canBegin) {
    Serial.println("Not enough space for OTA");
    http.end();
    return;
  }

  WiFiClient * stream = http.getStreamPtr();
  size_t written = 0;
  uint8_t buff[128];
  int lastProgress = 0;

  Serial.println("Starting download...");

  while (http.connected() && (written < (size_t)contentLength)) {
    size_t available = stream->available();

    if (available) {
      int bytesRead = stream->readBytes(buff, min(available, sizeof(buff)));

      if (bytesRead > 0) {
        size_t bytesWritten = Update.write(buff, bytesRead);

        if (bytesWritten != (size_t)bytesRead) {
          Serial.println("Write error!");
          Update.abort();
          http.end();
          return;
        }

        written += bytesWritten;

        int progress = (written * 100) / contentLength;
        if (progress != lastProgress && progress % 10 == 0) {
          Serial.printf("Progress: %d%%\n", progress);
          lastProgress = progress;
        }
      }
    }
    delay(1);
  }

  Serial.printf("Downloaded: %d bytes\n", written);

  if (written != (size_t)contentLength) {
    Serial.println("Download incomplete!");
    Update.abort();
    http.end();
    return;
  }

  if (Update.end()) {
    Serial.println("===========================================");
    Serial.println("FIRMWARE UPDATE SUCCESS!");
    Serial.println("Device will restart in 3 seconds...");
    Serial.println("===========================================");

    http.end();
    delay(3000);
    ESP.restart();
  } else {
    Serial.println("Update failed!");
    Update.printError(Serial);
  }

  http.end();
}
