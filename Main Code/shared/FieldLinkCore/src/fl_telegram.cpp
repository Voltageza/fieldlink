#include "fl_telegram.h"
#include "fl_comms.h"
#include <HTTPClient.h>

static const char* _webhookUrl = "";

void fl_setNotificationUrl(const char* url) {
  _webhookUrl = url;
}

void fl_sendNotification(const char* jsonPayload) {
  if (!fl_wifiConnected) {
    Serial.println("Cannot send notification - WiFi not connected");
    return;
  }

  if (strlen(_webhookUrl) == 0) {
    Serial.println("Notification URL not configured");
    return;
  }

  HTTPClient http;
  http.begin(_webhookUrl);
  http.addHeader("Content-Type", "application/json");

  Serial.printf("Sending notification: %s\n", jsonPayload);

  int httpCode = http.POST(jsonPayload);

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
