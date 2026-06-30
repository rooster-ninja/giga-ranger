SX1280 RANGING CALIBRATION FIRMWARE
LILYGO T3-S3 V1.3 — Giga Ranger Project
================================================================================

FLASH INSTRUCTIONS
------------------

1. Plug in SLAVE board via USB, then:

   cd ~/firmware_calibration
   ~/.local/bin/pio run -e slave -t upload

2. Plug in MASTER board via USB, then:

   ~/.local/bin/pio run -e master -t upload

3. Open serial monitor on master (115200 baud):

   ~/.local/bin/pio device monitor

   If you have two USB ports, monitor the master board specifically:

   ~/.local/bin/pio device monitor --port /dev/ttyUSB0   (adjust port as needed)


CALIBRATION PROCEDURE
---------------------

Signal chain (assemble before starting monitor):

  [T3-S3 Master] -- SMA -- [40 dB atten] -- [1 m RG-316] -- [40 dB atten] -- SMA -- [T3-S3 Slave]

Both boards should be inside steel chassis. 80 dB total attenuation required.

Run A:
  - Flash and connect as above
  - Master collects 500 ranging exchanges, prints each result (metres)
  - CalibrationValue_A printed at end of run

Run B (role reversal):
  - Swap USB connections between boards
  - Re-flash: slave board gets -e master, master board gets -e slave
  - Repeat — collect CalibrationValue_B

Final value:
  CalibrationValue = (CalibrationValue_A + CalibrationValue_B) / 2

Write to production firmware:
  radio.setRangingCalibration(YOUR_AVERAGED_VALUE);


CABLE CONSTANTS
---------------
  Cable:    DigiKey J10302-ND / 415-0031-M1.0
  Type:     RG-316 MIL-DTL-17 (Amphenol CIT) — confirmed from jacket
  Physical: 1.000 m
  VF:       0.695
  Elec:     0.695 m  (3.86 ranging counts)


BUILD ENVIRONMENT (verified)
-----------------------------
  OS:         Arch Linux x86_64
  PlatformIO: 6.1.19  (~/.local/bin/pio)
  RadioLib:   7.7.1
  Platform:   espressif32 7.0.1
  Framework:  Arduino

  master env: SUCCESS  RAM 6.1%  Flash 8.8%
  slave env:  SUCCESS  RAM 6.1%  Flash 8.8%


PIN ASSIGNMENTS (T3-S3 V1.3, SX1280) — confirmed from schematic
-----------------------------------------------------------------
  CS/NSS  GPIO7     MOSI  GPIO6
  SCK     GPIO5     MISO  GPIO3
  DIO1    GPIO33    BUSY  GPIO34
  RESET   GPIO8
