#ifndef FL_WEB_H
#define FL_WEB_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// Web server instance
extern AsyncWebServer fl_server;

// Set web authentication credentials
void fl_setWebAuth(const char* user, const char* pass);

// Set dashboard HTML (PROGMEM pointer)
void fl_setDashboardHtml(const char* html);

// Authentication check helper
bool fl_checkAuth(AsyncWebServerRequest *request);

// Register library-provided web routes (call before fl_server.begin())
void fl_setupWebRoutes();

#endif
