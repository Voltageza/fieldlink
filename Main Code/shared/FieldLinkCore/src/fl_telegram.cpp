#include "fl_telegram.h"
#include "fl_comms.h"
#include "fl_storage.h"
#include <HTTPClient.h>

static char _webhook_url[256] = "";

void fl_setWebhookUrl(const char* url) {
  strncpy(_webhook_url, url, sizeof(_webhook_url) - 1);
}

void fl_sendWebhook() {
  if (!fl_wifiConnected) {
    Serial.println("Cannot send notification - WiFi not connected");
    return;
  }

  if (_webhook_url[0] == '\0') {
    Serial.println("No webhook URL configured");
    return;
  }

  HTTPClient http;
  http.begin(_webhook_url);
  http.addHeader("Content-Type", "application/json");

  // Build JSON payload with device_id
  String payload = "{\"device_id\":\"";
  payload += fl_DEVICE_ID;
  payload += "\"}";

  Serial.printf("Sending fault notification: %s\n", payload.c_str());

  int httpCode = http.POST(payload);

  if (httpCode > 0) {
    Serial.printf("Notification sent, response code: %d\n", httpCode);
    if (httpCode == HTTP_CODE_OK) {
      String response = http.getString();
      Serial.printf("Response: %s\n", response.c_str());
    }
  } else {
    Serial.printf("Notification failed, error: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
}
