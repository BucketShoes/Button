// Voltage sensing on ADC1. See docs/hardware.md: VBAT_SENSE reads VSYS, not
// the cell. With USB present it reads the charger output.
#pragma once
#include <stdbool.h>

void sense_init(void);
#define SENSE_ERR (-1)    // no reading: the ADC driver failed every conversion (a code, not a voltage)
int sense_vsys_mv(void);  // VSYS (2:1 divider); SENSE_ERR if it couldn't read
int sense_vbus_mv(void);  // USB VBUS (0.6 divider)
// Last VSYS read, for the serial status line: raw ADC average, and the pin voltage before the divider.
void sense_vsys_debug(int *raw, int *pin_mv);
// Times the ADC was found stuck above the pin's possible range and rebuilt (since boot).
int sense_adc_rebuilds(void);
// Rebuild the ADC unit (after a read that can't be right).
void sense_adc_reset(void);
// USB power present, from the VBUS pin's digital level (no ADC conversion).
bool sense_vbus_present(void);
