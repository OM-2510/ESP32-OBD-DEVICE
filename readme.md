# ESP32 LFA-Style OBD-II Telemetry Dashboard

## 1. Memory & Sprite Architecture
- **Boot Heap Allocation**: `face` and `work` sprites (224x224 @ 8-bit = ~50 KB each) are allocated at the top of `setup()` **before** `BluetoothSerial` initializes so the ESP32 heap is never fragmented.
- **0 KB Extra Pages**: `PAGE_PARAMS` and `PAGE_CONFIG` render directly to the ST7789 hardware controller using `tft.setTextPadding()` to overwrite old digits cleanly without flicker or extra RAM usage.

## 2. Controls & Navigation (Rotary Encoder on 32/33/27)
- **Switch Pages**: Turn the knob left/right to cycle between:
  1. **LFA Dial Page**: 300° sweep tachometer starting at 6 o'clock, center speedo, TEMP and LOAD bars.
  2. **Parameters Page**: Large speed + RPM header and 8 live ECU sensors (Coolant, Intake, Throttle, Load, MAF, Timing, STFT, LTFT).
  3. **Config Page**: On-board settings menu stored in ESP32 NVS flash.
- **Using the Config Menu**:
  1. Turn the knob to reach the **Config Page**, then **click the knob** to enter the menu.
  2. Turn to highlight a setting, **click** to edit (row turns red), turn to adjust value, and **click** to lock it in.
  3. Select **7. Save & Exit to Dial** to write changes to flash.
- **DTC Fault Alert**:
  - When a fault code is detected (`dtcCount > 0`), the background turns dark red (`COL_DARKRED`) and displays `"DTC FAULT! CHECK PHONE"`.
  - **Single-click the knob** while driving to acknowledge the alert—this restores the black background while keeping the red bottom warning banner visible.

## 3. Customizing the LFA Dial (`src/Config.h` & `src/UiPages.h`)
- **Sweep Angles**: Adjust `ANG_START` (default `0` = 6 o'clock) and `ANG_END` (default `300` = 4 o'clock) in `src/Config.h`.
- **White Band Thickness**: Adjust `R_OUT` (`110`) and `R_IN` (`72`) in `src/Config.h`.
- **Tick Density**: In `UiPages.h -> buildLfaFace()`, the loop steps by `v += 200` (5 sub-ticks per 1000 RPM). Change to `v += 250` for 4 sub-ticks per 1000 RPM.

## 4. Switching from Bench Demo to Car (`OBDII`)
- The firmware boots in **DEMO SIM** mode by default so you can test the LFA needle, pages, and DTC red screen on your desk.
- To connect to your car:
  1. Turn knob to **Page 3 (Config)** -> Click `#5 Data Source` -> switch to `LIVE OBDII`.
  2. Click `#7 Save & Exit` and reboot the ESP32 with your ELM327 plugged into the car's OBD port.