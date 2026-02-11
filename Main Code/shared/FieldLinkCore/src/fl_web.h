#ifndef FL_WEB_H
#define FL_WEB_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// Set web authentication credentials (call before begin)
void fl_setWebAuth(const char* user, const char* pass);

// Authentication check helper for request handlers
bool fl_checkAuth(AsyncWebServerRequest *request);

// Get the web server instance (for adding project-specific routes)
AsyncWebServer& fl_getWebServer();

// Setup shared web routes (/config, /update, /api/device, /api/mqtt, etc.)
void fl_setupBaseWebRoutes();

// Start the web server (call after all routes are added)
void fl_startWebServer();

#endif // FL_WEB_H
