// Voltage sensing on ADC1. See docs/hardware.md: VBAT_SENSE reads VSYS, not
// the cell. With USB present it reads the charger output.
#pragma once

void sense_init(void);
int sense_vsys_mv(void);  // VSYS (2:1 divider)
int sense_vbus_mv(void);  // USB VBUS (0.6 divider)
// Last VSYS read, for the serial status line: raw ADC average, and the pin voltage before the divider.
void sense_vsys_debug(int *raw, int *pin_mv);
