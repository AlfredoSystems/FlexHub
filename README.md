# FlexHub

An ESP32-S3 ball counter controlled over WebSerial from a browser. Reads four beam-break sensors, drives a WS2812B LED strip, and controls two motors via the Alfredo NoU3.

## Hardware

| Component | Detail |
|---|---|
| Microcontroller | ESP32-S3 with Alfredo NoU3 |
| Beam-break sensors | Pins 4, 5, 6, 7 (INPUT_PULLUP) |
| LED strip | WS2812B on pin 8, GRB order |
| Motors | NoU3 motor ports 1 and 2 |

## Dependencies

Install both libraries through the Arduino Library Manager:

- **FastLED** by Daniel Garcia
- **Alfredo_NoU3** by Alfredo Systems

## Flashing

1. Open `FlexHub/FlexHub.ino` in the Arduino IDE.
2. Set `LED_COUNT` at the top of the file to match your strip length.
3. Select board: **ESP32S3 Dev Module** (or your specific NoU3 board variant).
4. Flash. The device will appear as **FlexHub** on the USB serial port.

## Web Controller

Open `index.html` in **Chrome or Edge** (WebSerial is not supported in Firefox).

1. Click **Connect** and select the FlexHub port.
2. Use the LED and motor buttons to control the hardware.
3. The ball count increments automatically each time a sensor fires and can be reset at any time — the count is stored in the browser, not on the ESP32.

## LED Modes

| Mode | Behavior |
|---|---|
| Red / Blue / Green / Purple | Solid color |
| Chase Red | Solid red with a white comet running over it |
| Chase Blue | Solid blue with a white comet running over it |
| Pulse Red | Heartbeat pulse in red |
| Pulse Blue | Heartbeat pulse in blue |
| Off | All LEDs off |

## Communication Protocol

All messages are newline-terminated ASCII over USB Serial at 115200 baud.

**Browser → ESP32**

```
CMD:LED:OFF
CMD:LED:RED
CMD:LED:BLUE
CMD:LED:GREEN
CMD:LED:PURPLE
CMD:LED:CHASE_RED
CMD:LED:CHASE_BLUE
CMD:LED:PULSE_RED
CMD:LED:PULSE_BLUE
CMD:MOTOR:ON
CMD:MOTOR:OFF
```

**ESP32 → Browser**

```
EVT:BALL   # sent once per beam-break falling edge
```
