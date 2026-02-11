#ifndef FL_TELEGRAM_H
#define FL_TELEGRAM_H

#include <Arduino.h>

// Set the notification webhook URL (call before begin)
void fl_setNotificationUrl(const char* url);

// Send a JSON payload to the notification webhook via HTTP POST
void fl_sendNotification(const char* jsonPayload);

#endif // FL_TELEGRAM_H
