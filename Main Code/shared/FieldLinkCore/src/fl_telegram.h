#ifndef FL_TELEGRAM_H
#define FL_TELEGRAM_H

#include <Arduino.h>

// Configure Telegram bot token and chat ID
void fl_setTelegram(const char* botToken, const char* chatId);

// Send fault notification directly to Telegram group
void fl_sendFaultNotification(int pump, const char* faultType, float current);

#endif
