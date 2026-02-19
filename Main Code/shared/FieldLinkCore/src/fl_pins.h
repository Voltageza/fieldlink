#ifndef FL_PINS_H
#define FL_PINS_H

// RS485
#define FL_RS485_RX   18
#define FL_RS485_TX   17
#define FL_RS485_DE   21
#define FL_RS485_BAUD 9600
#define FL_MODBUS_ID  1

// W5500 Ethernet (SPI)
#define FL_ETH_CS    16
#define FL_ETH_SCLK  15
#define FL_ETH_MOSI  13
#define FL_ETH_MISO  14
#define FL_ETH_INT   12
#define FL_ETH_RST   39

// WAVESHARE I2C PINS (for TCA9554 I/O expander)
#define FL_I2C_SDA       42
#define FL_I2C_SCL       41
#define FL_TCA9554_ADDR  0x20

// DO CHANNELS (0-7 for 8 outputs)
#define FL_DO_CONTACTOR_CH  0   // Main contactor relay
#define FL_DO_RUN_LED_CH    1   // RUN indicator (green)
#define FL_DO_FAULT_LED_CH  2   // FAULT indicator (red)
#define FL_DO_FAULT_CH      4   // Fault alarm output (physical DO5)

// WAVESHARE DIGITAL INPUT PINS (directly connected to ESP32 GPIOs)
#define FL_DI1_PIN  4   // START button (NO - Normally Open)
#define FL_DI2_PIN  5   // STOP button (NC - Normally Closed)
#define FL_DI3_PIN  6   // LOCAL/REMOTE selector
#define FL_DI4_PIN  7
#define FL_DI5_PIN  8
#define FL_DI6_PIN  9
#define FL_DI7_PIN  10
#define FL_DI8_PIN  11

#endif
