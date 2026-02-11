#include "fl_board.h"
#include "fl_pins.h"
#include <Wire.h>

uint8_t fl_do_state = 0xFF;

void fl_initI2C() {
  // I2C bus recovery - release stuck bus from previous crash
  pinMode(I2C_SCL, OUTPUT);
  pinMode(I2C_SDA, INPUT_PULLUP);
  for (int i = 0; i < 9; i++) {
    digitalWrite(I2C_SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(I2C_SCL, HIGH);
    delayMicroseconds(5);
  }
  pinMode(I2C_SCL, INPUT_PULLUP);
  delayMicroseconds(100);

  Wire.begin(I2C_SDA, I2C_SCL);
}

void fl_writeDO() {
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(0x01);  // Output port register
  Wire.write(fl_do_state);
  Wire.endTransmission();
}

void fl_initDO() {
  // CRITICAL: Set output values BEFORE configuring as outputs
  // This prevents glitches when pins transition from input to output

  // Step 1: Write output port register first (0xFF = all OFF for active-low)
  fl_do_state = 0xFF;
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(0x01);
  Wire.write(fl_do_state);
  Wire.endTransmission();

  // Step 2: Set polarity inversion to none
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(0x02);
  Wire.write(0x00);
  Wire.endTransmission();

  // Step 3: Configure all pins as outputs
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(0x03);
  Wire.write(0x00);  // 0 = output
  Wire.endTransmission();

  // Step 4: Write output state again to ensure correct
  fl_writeDO();
  Serial.println("TCA9554 I/O expander initialized");
}

void fl_setDO(uint8_t ch, bool on) {
  uint8_t old_state = fl_do_state;
  // Active-low: clear bit to turn ON, set bit to turn OFF
  if (on) fl_do_state &= ~(1 << ch);
  else    fl_do_state |=  (1 << ch);

  if (fl_do_state != old_state) {
    fl_writeDO();
  }
}

void fl_initDI() {
  pinMode(DI1_PIN, INPUT_PULLUP);
  pinMode(DI2_PIN, INPUT_PULLUP);
  pinMode(DI3_PIN, INPUT_PULLUP);
  pinMode(DI4_PIN, INPUT_PULLUP);
  pinMode(DI5_PIN, INPUT_PULLUP);
  pinMode(DI6_PIN, INPUT_PULLUP);
  pinMode(DI7_PIN, INPUT_PULLUP);
  pinMode(DI8_PIN, INPUT_PULLUP);
  Serial.println("Digital inputs initialized");
}

uint8_t fl_readDI() {
  uint8_t status = 0;
  if (!digitalRead(DI1_PIN)) status |= 0x01;
  if (!digitalRead(DI2_PIN)) status |= 0x02;
  if (!digitalRead(DI3_PIN)) status |= 0x04;
  if (!digitalRead(DI4_PIN)) status |= 0x08;
  if (!digitalRead(DI5_PIN)) status |= 0x10;
  if (!digitalRead(DI6_PIN)) status |= 0x20;
  if (!digitalRead(DI7_PIN)) status |= 0x40;
  if (!digitalRead(DI8_PIN)) status |= 0x80;
  return status;
}

void fl_i2cTest() {
  Serial.println("Testing I2C TCA9554...");
  Wire.beginTransmission(TCA9554_ADDR);
  uint8_t err = Wire.endTransmission();
  Serial.printf("I2C probe result: %d (0=OK)\n", err);

  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(0x01);
  Wire.endTransmission();
  Wire.requestFrom((uint8_t)TCA9554_ADDR, (uint8_t)1);
  if (Wire.available()) {
    uint8_t val = Wire.read();
    Serial.printf("TCA9554 output register: 0x%02X (expected: 0x%02X)\n", val, fl_do_state);
  } else {
    Serial.println("Failed to read from TCA9554");
  }
}
