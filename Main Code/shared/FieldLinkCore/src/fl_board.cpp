#include "fl_board.h"
#include "fl_pins.h"

uint8_t fl_do_state = 0xFF;
uint8_t fl_diStatus = 0;

void fl_i2cBusRecovery() {
  // CRITICAL: Release stuck I2C bus from previous crash
  pinMode(FL_I2C_SCL, OUTPUT);
  pinMode(FL_I2C_SDA, INPUT_PULLUP);
  for (int i = 0; i < 9; i++) {
    digitalWrite(FL_I2C_SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(FL_I2C_SCL, HIGH);
    delayMicroseconds(5);
  }
  pinMode(FL_I2C_SCL, INPUT_PULLUP);
  delayMicroseconds(100);
}

void fl_writeDO() {
  Wire.beginTransmission(FL_TCA9554_ADDR);
  Wire.write(0x01);  // Output port register
  Wire.write(fl_do_state);
  Wire.endTransmission();
}

void fl_initDO() {
  // CRITICAL: Set output values BEFORE configuring as outputs
  // This prevents glitches when pins transition from input to output

  // Step 1: Write output port register first (0xFF = all OFF for active-low)
  fl_do_state = 0xFF;
  Wire.beginTransmission(FL_TCA9554_ADDR);
  Wire.write(0x01);  // Output port register
  Wire.write(fl_do_state);
  Wire.endTransmission();

  // Step 2: Set polarity inversion to none
  Wire.beginTransmission(FL_TCA9554_ADDR);
  Wire.write(0x02);  // Polarity inversion register
  Wire.write(0x00);  // No inversion
  Wire.endTransmission();

  // Step 3: NOW configure all pins as outputs
  Wire.beginTransmission(FL_TCA9554_ADDR);
  Wire.write(0x03);  // Configuration register
  Wire.write(0x00);  // All pins as outputs (0 = output)
  Wire.endTransmission();

  // Step 4: Write output state again to ensure it's correct
  fl_writeDO();
  Serial.println("TCA9554 I/O expander initialized");
}

void fl_setDO(uint8_t ch, bool on) {
  uint8_t old_state = fl_do_state;
  // Active-low outputs: clear bit to turn ON, set bit to turn OFF
  if (on) fl_do_state &= ~(1 << ch);
  else    fl_do_state |=  (1 << ch);

  // Only write if state actually changed
  if (fl_do_state != old_state) {
    fl_writeDO();
  }
}

void fl_initDI() {
  pinMode(FL_DI1_PIN, INPUT_PULLUP);  // START button (NO)
  pinMode(FL_DI2_PIN, INPUT_PULLUP);  // STOP button (NC)
  pinMode(FL_DI3_PIN, INPUT_PULLUP);
  pinMode(FL_DI4_PIN, INPUT_PULLUP);
  pinMode(FL_DI5_PIN, INPUT_PULLUP);
  pinMode(FL_DI6_PIN, INPUT_PULLUP);
  pinMode(FL_DI7_PIN, INPUT_PULLUP);
  pinMode(FL_DI8_PIN, INPUT_PULLUP);
  Serial.println("Digital inputs initialized");
}

void fl_readDI() {
  fl_diStatus = 0;
  if (!digitalRead(FL_DI1_PIN)) fl_diStatus |= 0x01;  // DI1 active (inverted)
  if (!digitalRead(FL_DI2_PIN)) fl_diStatus |= 0x02;  // DI2 active (inverted)
  if (!digitalRead(FL_DI3_PIN)) fl_diStatus |= 0x04;  // DI3 active (inverted)
  if (!digitalRead(FL_DI4_PIN)) fl_diStatus |= 0x08;  // DI4 active (inverted)
  if (!digitalRead(FL_DI5_PIN)) fl_diStatus |= 0x10;  // DI5 active (inverted)
  if (!digitalRead(FL_DI6_PIN)) fl_diStatus |= 0x20;  // DI6 active (inverted)
  if (!digitalRead(FL_DI7_PIN)) fl_diStatus |= 0x40;  // DI7 active (inverted)
  if (!digitalRead(FL_DI8_PIN)) fl_diStatus |= 0x80;  // DI8 active (inverted)
}
