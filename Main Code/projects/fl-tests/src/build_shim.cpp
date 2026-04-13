// Thin shim that pulls FieldLinkCore's pure-logic sources into the native
// test binary. Only modules with zero hardware dependencies go here —
// everything else requires Arduino.h and lives on-device.

#include "fl_protection.cpp"
