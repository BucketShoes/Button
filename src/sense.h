// Voltage sensing on ADC1. See docs/hardware.md: VBAT_SENSE reads VSYS, not
// the cell. With USB present it reads the charger output.
#pragma once

void sense_init(void);
int sense_vsys_mv(void);  // VSYS (2:1 divider)
int sense_vbus_mv(void);  // USB VBUS (0.6 divider)
