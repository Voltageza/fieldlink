#include "fl_telegram.h"
#include "fl_comms.h"
#include "fl_storage.h"
#include <HTTPClient.h>

static char _bot_token[64] = "";
static char _chat_id[24] = "";

void fl_setTelegram(const char* botToken, const char* chatId) {
  strncpy(_bot_token, botToken, sizeof(_bot_token) - 1);
  strncpy(_chat_id, chatId, sizeof(_chat_id) - 1);
}

void fl_sendFaultNotification(int pump, const char* faultType, float current) {
  if (!fl_wifiConnected) {
    Serial.println("Cannot send notification - WiFi not connected");
    return;
  }

  if (_bot_token[0] == '\0' || _chat_id[0] == '\0') {
    Serial.println("Telegram not configured");
    return;
  }

  // Build Telegram Bot API URL
  char url[128];
  snprintf(url, sizeof(url),
    "https://api.telegram.org/bot%s/sendMessage", _bot_token);

  // Build JSON payload with escaped newlines for Telegram API
  char payload[512];
  snprintf(payload, sizeof(payload),
    "{\"chat_id\":\"%s\","
    "\"parse_mode\":\"Markdown\","
    "\"text\":\"*FAULT ALERT*\\n\\n"
    "Device: *%s*\\n"
    "Pump: *%d*\\n"
    "Fault: *%s* (%.1fA)\\n\\n"
    "Open FieldLogic to view details and reset.\"}",
    _chat_id, fl_DEVICE_ID, pump, faultType, current);

  Serial.printf("Sending Telegram to chat %s\n", _chat_id);

  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(payload);

  if (httpCode > 0) {
    Serial.printf("Telegram sent, response: %d\n", httpCode);
  } else {
    Serial.printf("Telegram failed: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
}
