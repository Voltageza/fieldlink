#ifndef FL_BOARD_H
#define FL_BOARD_H

#include <Arduino.h>

// DO register state (0xFF = all OFF for active-low outputs)
extern uint8_t fl_do_state;

// Initialize I2C bus with recovery sequence
void fl_initI2C();

// Initialize TCA9554 I/O expander for digital outputs
void fl_initDO();

// Write current DO state to TCA9554
void fl_writeDO();

// Set individual DO channel (active-low: on=true clears bit)
void fl_setDO(uint8_t ch, bool on);

// Initialize all 8 digital input pins with internal pull-ups
void fl_initDI();

// Read all 8 digital inputs as a bitfield (bit set = active/low)
uint8_t fl_readDI();

// I2C probe test for TCA9554 (prints result to Serial)
void fl_i2cTest();

#endif // FL_BOARD_H
