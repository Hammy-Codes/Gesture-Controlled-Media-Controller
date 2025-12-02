# Gesture-Controlled Wireless Speaker Controller (ESP32 + VL53L0X)

Built this project for my semester submission , a fully wireless, gesture-controlled media controller powered by an ESP32. What makes it cool is that instead of using a dedicated gesture sensor, I used a VL53L0X distance sensor and calibrated different distance zones to understand hand movements in real time.

So just by moving your hand in front of the device, you can:

Play/Pause

Skip to the next or previous song

Control the volume smoothly

No buttons, no touch — just clean air gestures.
The whole setup runs on Bluetooth + a rechargeable battery pack, so it’s completely portable and works with any phone or laptop.

Honestly, it’s crazy how much you can do with one distance sensor and some smart logic. Super proud of how responsive and smooth it turned out.


## Features

- Play / Pause music using hand gestures  
- Next / Previous track using fast swipe gestures  
- Volume up / down based on hand movement and position  
- Bluetooth BLE HID – controls phone / laptop media keys  
- Fully wireless using 18650 battery + TP4056 + MT3608 boost converter  
- Hybrid idle mode to reduce power usage when no hand is detected

## Hardware Used

- ESP32 Dev Board  
- VL53L0X distance sensor  
- 18650 Li-ion battery (3.7V, 2000mAh)  
- TP4056 Type-C Li-ion charger module (with protection)  
- MT3608 DC-DC boost converter (battery → 5V)  
- Wires, breadboard, switch

## Connections (Summary)

- **VL53L0X → ESP32**
  - VCC → 3.3V  
  - GND → GND  
  - SDA → GPIO 21  
  - SCL → GPIO 22  

- **Power Path**
  - 18650 → TP4056 (B+ / B−)
  - TP4056 OUT+ / OUT− → MT3608 VIN+ / VIN−
  - MT3608 VOUT+ → ESP32 VIN (5V)
  - MT3608 VOUT− → ESP32 GND

## How It Works (Short)

1. VL53L0X continuously measures distance of the user's hand.
2. The code defines different **zones**:
   - Near zone → volume control  
   - Mid zone → play/pause (stable hold)  
   - Far zone → next/previous track (fast swipe)
3. ESP32 sends media key commands via BLE (using `BleKeyboard` library).
4. The device runs wirelessly from a battery and can be charged with USB-C.

## Future Improvements

- Add OLED display to show current mode / volume  
- Add APDS9930 proximity sensor for tap gestures  
- Design a custom PCB and 3D printed enclosure  
- Control smart home devices instead of only media



Made with ❤ using ESP32, Arduino IDE and too much coffee.
