// ============================================
// FIELDLINK SECRETS - COPY TO secrets.h
// ============================================
// 1. Copy this file to secrets.h
// 2. Replace placeholders with your actual credentials
// 3. NEVER commit secrets.h to git!

#ifndef SECRETS_H
#define SECRETS_H

// MQTT Broker (HiveMQ Cloud)
#define DEFAULT_MQTT_HOST "YOUR_CLUSTER.s1.eu.hivemq.cloud"
#define DEFAULT_MQTT_PORT 8883
#define DEFAULT_MQTT_USER "your-mqtt-username"
#define DEFAULT_MQTT_PASS "your-mqtt-password"

// Telegram Notification Webhook (optional)
#define NOTIFICATION_WEBHOOK_URL "https://your-webhook-url.deno.dev/fault"

// ArduinoOTA Password (change this!)
#define OTA_PASSWORD "change-this-password"

// Local Web Interface Authentication (change this!)
#define WEB_AUTH_USER "admin"
#define WEB_AUTH_PASS "change-this-password"

#endif
