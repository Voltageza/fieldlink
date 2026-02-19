#ifndef FL_BOARD_H
#define FL_BOARD_H

#include <Arduino.h>
#include <Wire.h>

// DO register (0xFF = all OFF for active-low outputs)
extern uint8_t fl_do_state;

// Digital input status bitfield (DI1-DI8)
extern uint8_t fl_diStatus;

// I2C bus recovery (call before Wire.begin)
void fl_i2cBusRecovery();

// Initialize TCA9554 I/O expander for digital outputs
void fl_initDO();

// Write current fl_do_state to TCA9554
void fl_writeDO();

// Set individual DO channel (active-low: on=clear bit)
void fl_setDO(uint8_t ch, bool on);

// Initialize digital input pins with pull-ups
void fl_initDI();

// Read all digital inputs into fl_diStatus
void fl_readDI();

#endif
