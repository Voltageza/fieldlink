#ifndef FL_PINS_H
#define FL_PINS_H

// Waveshare ESP32-S3 POE-ETH-8DI-8DO Board Pin Definitions

// RS485
#define RS485_RX  18
#define RS485_TX  17
#define RS485_DE  21
#define RS485_BAUDRATE 9600
#define MODBUS_ID 1

// W5500 Ethernet (SPI)
#define ETH_CS    16
#define ETH_SCLK  15
#define ETH_MOSI  13
#define ETH_MISO  14
#define ETH_INT   12
#define ETH_RST   39

// I2C (for TCA9554 I/O expander)
#define I2C_SDA   42
#define I2C_SCL   41
#define TCA9554_ADDR  0x20

// DO Channels (0-7 for 8 outputs via TCA9554)
#define DO_CH1  0
#define DO_CH2  1
#define DO_CH3  2
#define DO_CH4  3
#define DO_CH5  4
#define DO_CH6  5
#define DO_CH7  6
#define DO_CH8  7

// Digital Input Pins (directly on ESP32 GPIOs)
#define DI1_PIN  4
#define DI2_PIN  5
#define DI3_PIN  6
#define DI4_PIN  7
#define DI5_PIN  8
#define DI6_PIN  9
#define DI7_PIN  10
#define DI8_PIN  11

#endif // FL_PINS_H
