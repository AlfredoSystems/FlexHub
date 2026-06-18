/*
 * FlexHub - ESP32-S3
 *
 * Communication Protocol (newline-terminated ASCII over USB Serial at 115200):
 *
 * HTML → ESP32:
 *   CMD:LED:OFF        — LEDs off
 *   CMD:LED:RED        — solid red
 *   CMD:LED:BLUE       — solid blue
 *   CMD:LED:GREEN      — solid green
 *   CMD:LED:PURPLE     — solid purple
 *   CMD:LED:CHASE_RED  — solid red + white comet chase
 *   CMD:LED:CHASE_BLUE — solid blue + white comet chase
 *   CMD:LED:PULSE_RED  — heartbeat pulse, red
 *   CMD:LED:PULSE_BLUE — heartbeat pulse, blue
 *   CMD:MOTOR:ON            — motors on (full forward)
 *   CMD:MOTOR:OFF           — motors off
 *   CMD:DISPLAY:SHIFT:n     — leftmost digit, 0–9
 *   CMD:DISPLAY:CLOCK:nn    — rightmost two digits, 0–99
 *   CMD:DEBUG:ON            — enable debug mode (raw edge events)
 *   CMD:DEBUG:OFF           — disable debug mode
 *
 * ESP32 → HTML:
 *   EVT:BALL           — one ball counted (filtered rises back above 50% — sensor returns HIGH after ball passes)
 *   EVT:BALL_RISE      — raw sensor rising edge  (debug mode only)
 *   EVT:BALL_FALL      — raw sensor falling edge (debug mode only)
 */

#include "USB.h"
#include <FastLED.h>
#include <Alfredo_NoU3.h>
#include <Wire.h>

// --- Configuration ---
static const uint16_t LED_COUNT = 60;  // adjust to your strip length
static const uint8_t LED_PIN = 8;
static const uint8_t PIN_SENSOR = 9;
static const uint8_t PIN_I2C_SDA_HK16 = 4;
static const uint8_t PIN_I2C_SCL_HK16 = 5;
static const float SENSOR_TAU_US = 50000.0f;  // EMA time constant in microseconds
static const uint32_t CHASE_STEP_MS = 35;  // ms per chase frame
static const uint8_t TAIL_LENGTH = 12;     // white comet tail pixels
static const uint32_t PULSE_STEP_MS = 16;  // ~60fps for smooth pulse
static const float motorValue = 0.6f;

// --- Hardware ---
CRGB leds[LED_COUNT];
NoU_Motor motor1(1);
NoU_Motor motor2(2);

// --- Sensor state ---
float    sensorFiltered;
bool     sensorAbove;     // which side of 0.5 the filtered value was on last sample
uint32_t lastSensorUs;    // micros() at last checkBallSensor call, for dt-aware EMA
bool     lastRawSensor = false;   // raw sensor state last sample, for debug edge detection
bool     debugMode = false;

// --- LED mode ---
enum LedMode {
  LED_OFF,
  LED_RED,
  LED_BLUE,
  LED_GREEN,
  LED_PURPLE,
  LED_CHASE_RED,
  LED_CHASE_BLUE,
  LED_PULSE_RED,
  LED_PULSE_BLUE
};
LedMode currentLedMode = LED_OFF;

// --- Chase animation state ---
CRGB chaseBase = CRGB::Red;
uint16_t chaseHead = 0;
uint32_t lastChaseStep = 0;

// --- Pulse animation state ---
CRGB pulseColor = CRGB::Red;
uint32_t lastPulseStep = 0;

// --- HT16K33 display ---
// Layout: [SHIFT] [off] [CLOCK tens] [CLOCK ones]  — digit 1 and colon always dark
static const uint8_t HT16K33_ADDR = 0x70;
static const uint8_t SEG7[10] = {
  0x3F, 0x06, 0x5B, 0x4F, 0x66,  // 0–4
  0x6D, 0x7D, 0x07, 0x7F, 0x6F   // 5–9
};
uint8_t displayShift = 0;  // 0–9
uint8_t displayClock = 0;  // 0–99

// --- Serial buffer ---
String serialBuffer = "";

// ---------------------------------------------------------------------------

void applyLedSolid(CRGB color) {
  fill_solid(leds, LED_COUNT, color);
  FastLED.show();
}

// --- Chase: solid base color with white comet running over it ---

void startChase(CRGB base) {
  chaseBase = base;
  chaseHead = 0;
  fill_solid(leds, LED_COUNT, base);
  FastLED.show();
}

void updateChase() {
  uint32_t now = millis();
  if (now - lastChaseStep < CHASE_STEP_MS) return;
  lastChaseStep = now;

  fill_solid(leds, LED_COUNT, chaseBase);

  for (uint8_t i = 0; i < TAIL_LENGTH; i++) {
    uint16_t idx = (chaseHead - i + LED_COUNT) % LED_COUNT;
    // t=255 at head (i=0), t=0 at tail tip — blends white→base color
    uint8_t t = (uint8_t)(255 * (TAIL_LENGTH - i - 1) / (TAIL_LENGTH - 1));
    leds[idx] = CRGB(
      chaseBase.r + (uint8_t)((uint16_t)(255 - chaseBase.r) * t / 255),
      chaseBase.g + (uint8_t)((uint16_t)(255 - chaseBase.g) * t / 255),
      chaseBase.b + (uint8_t)((uint16_t)(255 - chaseBase.b) * t / 255));
  }
  FastLED.show();

  chaseHead = (chaseHead + 1) % LED_COUNT;
}

// --- Pulse: heartbeat brightness envelope ---
// Two-beat pattern (lub-dub) with a pause: total period 900ms

uint8_t heartbeatBrightness() {
  uint32_t t = millis() % 900;
  if (t < 80) return (uint8_t)(t * 255 / 80);                 // beat 1 rise
  if (t < 180) return (uint8_t)(255 - (t - 80) * 180 / 100);  // beat 1 fall → 75
  if (t < 260) return (uint8_t)(75 + (t - 180) * 180 / 80);   // beat 2 rise
  if (t < 480) return (uint8_t)(255 * (480 - t) / 220);       // beat 2 fall → 0
  return 0;                                                   // rest
}

void startPulse(CRGB color) {
  pulseColor = color;
  lastPulseStep = 0;
}

void updatePulse() {
  uint32_t now = millis();
  if (now - lastPulseStep < PULSE_STEP_MS) return;
  lastPulseStep = now;

  uint8_t bri = heartbeatBrightness();
  CRGB c(
    (uint8_t)((uint16_t)pulseColor.r * bri / 255),
    (uint8_t)((uint16_t)pulseColor.g * bri / 255),
    (uint8_t)((uint16_t)pulseColor.b * bri / 255));
  fill_solid(leds, LED_COUNT, c);
  FastLED.show();
}

// ---------------------------------------------------------------------------
// HT16K33 display

void displayInit() {
  Wire.beginTransmission(HT16K33_ADDR);
  Wire.write(0x21);  // oscillator on
  Wire.endTransmission();

  Wire.beginTransmission(HT16K33_ADDR);
  Wire.write(0x81);  // display on, no blink
  Wire.endTransmission();

  Wire.beginTransmission(HT16K33_ADDR);
  Wire.write(0xEF);  // max brightness
  Wire.endTransmission();

  displayUpdate();
}

void displayUpdate() {
  // HT16K33 RAM: 5 positions × 2 bytes = 10 bytes
  // pos 0=digit0, pos 1=digit1, pos 2=colon, pos 3=digit2, pos 4=digit3
  Wire.beginTransmission(HT16K33_ADDR);
  Wire.write(0x00);  // start at RAM address 0
  Wire.write(SEG7[displayShift]);
  Wire.write(0x00);  // digit 0: SHIFT
  Wire.write(0x00);
  Wire.write(0x00);  // digit 1: always off
  Wire.write(0x00);
  Wire.write(0x00);  // colon:   always off
  Wire.write(SEG7[displayClock / 10]);
  Wire.write(0x00);  // digit 2: clock tens
  Wire.write(SEG7[displayClock % 10]);
  Wire.write(0x00);  // digit 3: clock ones
  Wire.endTransmission();
}

// ---------------------------------------------------------------------------

void setMotors(bool on) {
  float speed = on ? motorValue : 0.0f;
  motor1.set(speed);
  motor2.set(speed);
}

void processCommand(const String& cmd) {
  if (cmd == "CMD:LED:OFF") {
    currentLedMode = LED_OFF;
    applyLedSolid(CRGB::Black);
  } else if (cmd == "CMD:LED:RED") {
    currentLedMode = LED_RED;
    applyLedSolid(CRGB::Red);
  } else if (cmd == "CMD:LED:BLUE") {
    currentLedMode = LED_BLUE;
    applyLedSolid(CRGB::Blue);
  } else if (cmd == "CMD:LED:GREEN") {
    currentLedMode = LED_GREEN;
    applyLedSolid(CRGB::Green);
  } else if (cmd == "CMD:LED:PURPLE") {
    currentLedMode = LED_PURPLE;
    applyLedSolid(CRGB::Purple);
  } else if (cmd == "CMD:LED:CHASE_RED") {
    currentLedMode = LED_CHASE_RED;
    startChase(CRGB::Red);
  } else if (cmd == "CMD:LED:CHASE_BLUE") {
    currentLedMode = LED_CHASE_BLUE;
    startChase(CRGB::Blue);
  } else if (cmd == "CMD:LED:PULSE_RED") {
    currentLedMode = LED_PULSE_RED;
    startPulse(CRGB::Red);
  } else if (cmd == "CMD:LED:PULSE_BLUE") {
    currentLedMode = LED_PULSE_BLUE;
    startPulse(CRGB::Blue);
  } else if (cmd == "CMD:MOTOR:ON") {
    setMotors(true);
  } else if (cmd == "CMD:MOTOR:OFF") {
    setMotors(false);
  } else if (cmd.startsWith("CMD:DISPLAY:SHIFT:")) {
    displayShift = (uint8_t)constrain(cmd.substring(18).toInt(), 0, 9);
    displayUpdate();
  } else if (cmd.startsWith("CMD:DISPLAY:CLOCK:")) {
    displayClock = (uint8_t)constrain(cmd.substring(18).toInt(), 0, 99);
    displayUpdate();
  } else if (cmd == "CMD:DEBUG:ON") {
    debugMode = true;
  } else if (cmd == "CMD:DEBUG:OFF") {
    debugMode = false;
  }
}

void readSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      serialBuffer.trim();
      if (serialBuffer.length() > 0) processCommand(serialBuffer);
      serialBuffer = "";
    } else if (c != '\r') {
      serialBuffer += c;
    }
  }
}

void checkBallSensor() {
  uint32_t now = micros();
  float dt = (float)(now - lastSensorUs);
  lastSensorUs = now;

  bool raw = (bool)digitalRead(PIN_SENSOR);

  if (debugMode && raw != lastRawSensor) {
    Serial.println(raw ? "EVT:BALL_RISE" : "EVT:BALL_FALL");
  }
  lastRawSensor = raw;

  // dt-aware EMA: alpha = dt / (tau + dt)
  float alpha = dt / (SENSOR_TAU_US + dt);
  sensorFiltered = alpha * (raw ? 1.0f : 0.0f) + (1.0f - alpha) * sensorFiltered;

  bool above = sensorFiltered >= 0.5f;
  if (above && !sensorAbove) Serial.println("EVT:BALL");
  sensorAbove = above;
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  Wire.begin(PIN_I2C_SDA_HK16, PIN_I2C_SCL_HK16, 100000);
  Wire1.begin(PIN_I2C_SDA_IMU, PIN_I2C_SCL_IMU, 400000);

  NoU3.beginMotors();
  NoU3.beginServiceLight();
  NoU3.setServiceLight(LIGHT_ENABLED);

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, LED_COUNT);
  FastLED.setBrightness(255);
  FastLED.show();

  pinMode(PIN_SENSOR, INPUT_PULLDOWN);
  lastRawSensor  = (bool)digitalRead(PIN_SENSOR);
  sensorFiltered = lastRawSensor ? 1.0f : 0.0f;
  sensorAbove    = lastRawSensor;
  lastSensorUs   = micros();

  setMotors(false);
  displayInit();
}

void loop() {
  readSerial();
  checkBallSensor();

  switch (currentLedMode) {
    case LED_CHASE_RED:
    case LED_CHASE_BLUE: updateChase(); break;
    case LED_PULSE_RED:
    case LED_PULSE_BLUE: updatePulse(); break;
    default: break;
  }
}
