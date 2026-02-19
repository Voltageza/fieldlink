#ifndef FL_TELEGRAM_H
#define FL_TELEGRAM_H

#include <Arduino.h>

// Set webhook URL for notifications
void fl_setWebhookUrl(const char* url);

// Send fault notification via webhook
void fl_sendWebhook();

#endif
