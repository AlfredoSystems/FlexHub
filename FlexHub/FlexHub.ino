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
 *   CMD:MOTOR:ON       — motors on (full forward)
 *   CMD:MOTOR:OFF      — motors off
 *
 * ESP32 → HTML:
 *   EVT:BALL           — one ball counted (sent once per beam-break falling edge)
 */

#include "USB.h"
#include <FastLED.h>
#include <Alfredo_NoU3.h>

// --- Configuration ---
static const uint16_t LED_COUNT      = 60;  // adjust to your strip length
static const uint8_t  LED_PIN        = 8;
static const uint8_t  SENSOR_COUNT   = 4;
static const uint8_t  SENSOR_PINS[SENSOR_COUNT] = {4, 5, 6, 7};
static const uint32_t DEBOUNCE_MS    = 50;
static const uint32_t CHASE_STEP_MS  = 35;  // ms per chase frame
static const uint8_t  TAIL_LENGTH    = 12;  // white comet tail pixels
static const uint32_t PULSE_STEP_MS  = 16;  // ~60fps for smooth pulse

// --- Hardware ---
CRGB leds[LED_COUNT];
NoU_Motor motor1(1);
NoU_Motor motor2(2);

// --- Sensor state ---
bool     lastSensorState[SENSOR_COUNT];
uint32_t lastChangeTime[SENSOR_COUNT];

// --- LED mode ---
enum LedMode {
    LED_OFF,
    LED_RED, LED_BLUE, LED_GREEN, LED_PURPLE,
    LED_CHASE_RED, LED_CHASE_BLUE,
    LED_PULSE_RED, LED_PULSE_BLUE
};
LedMode currentLedMode = LED_OFF;

// --- Chase animation state ---
CRGB     chaseBase     = CRGB::Red;
uint16_t chaseHead     = 0;
uint32_t lastChaseStep = 0;

// --- Pulse animation state ---
CRGB     pulseColor    = CRGB::Red;
uint32_t lastPulseStep = 0;

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
            chaseBase.b + (uint8_t)((uint16_t)(255 - chaseBase.b) * t / 255)
        );
    }
    FastLED.show();

    chaseHead = (chaseHead + 1) % LED_COUNT;
}

// --- Pulse: heartbeat brightness envelope ---
// Two-beat pattern (lub-dub) with a pause: total period 900ms

uint8_t heartbeatBrightness() {
    uint32_t t = millis() % 900;
    if (t < 80)  return (uint8_t)(t * 255 / 80);           // beat 1 rise
    if (t < 180) return (uint8_t)(255 - (t - 80) * 180 / 100); // beat 1 fall → 75
    if (t < 260) return (uint8_t)(75 + (t - 180) * 180 / 80);  // beat 2 rise
    if (t < 480) return (uint8_t)(255 * (480 - t) / 220);   // beat 2 fall → 0
    return 0;                                                // rest
}

void startPulse(CRGB color) {
    pulseColor    = color;
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
        (uint8_t)((uint16_t)pulseColor.b * bri / 255)
    );
    fill_solid(leds, LED_COUNT, c);
    FastLED.show();
}

// ---------------------------------------------------------------------------

void setMotors(bool on) {
    float speed = on ? 1.0f : 0.0f;
    motor1.set(speed);
    motor2.set(speed);
}

void processCommand(const String& cmd) {
    if      (cmd == "CMD:LED:OFF")        { currentLedMode = LED_OFF;        applyLedSolid(CRGB::Black); }
    else if (cmd == "CMD:LED:RED")        { currentLedMode = LED_RED;        applyLedSolid(CRGB::Red);   }
    else if (cmd == "CMD:LED:BLUE")       { currentLedMode = LED_BLUE;       applyLedSolid(CRGB::Blue);  }
    else if (cmd == "CMD:LED:GREEN")      { currentLedMode = LED_GREEN;      applyLedSolid(CRGB::Green); }
    else if (cmd == "CMD:LED:PURPLE")     { currentLedMode = LED_PURPLE;     applyLedSolid(CRGB::Purple);}
    else if (cmd == "CMD:LED:CHASE_RED")  { currentLedMode = LED_CHASE_RED;  startChase(CRGB::Red);      }
    else if (cmd == "CMD:LED:CHASE_BLUE") { currentLedMode = LED_CHASE_BLUE; startChase(CRGB::Blue);     }
    else if (cmd == "CMD:LED:PULSE_RED")  { currentLedMode = LED_PULSE_RED;  startPulse(CRGB::Red);      }
    else if (cmd == "CMD:LED:PULSE_BLUE") { currentLedMode = LED_PULSE_BLUE; startPulse(CRGB::Blue);     }
    else if (cmd == "CMD:MOTOR:ON")       { setMotors(true);  }
    else if (cmd == "CMD:MOTOR:OFF")      { setMotors(false); }
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

void checkSensors() {
    uint32_t now = millis();
    for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
        bool state = (bool)digitalRead(SENSOR_PINS[i]);
        if (state != lastSensorState[i] && (now - lastChangeTime[i]) >= DEBOUNCE_MS) {
            lastChangeTime[i] = now;
            if (state == LOW) { // falling edge = beam broken = ball
                Serial.println("EVT:BALL");
            }
            lastSensorState[i] = state;
        }
    }
}

// ---------------------------------------------------------------------------

void setup() {
    USB.productName("FlexHub");
    USB.begin();
    Serial.begin(115200);

    NoU3.begin();

    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, LED_COUNT);
    FastLED.setBrightness(255);
    FastLED.show();

    for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
        pinMode(SENSOR_PINS[i], INPUT_PULLUP);
        lastSensorState[i] = (bool)digitalRead(SENSOR_PINS[i]);
        lastChangeTime[i]  = 0;
    }

    setMotors(false);
}

void loop() {
    readSerial();
    checkSensors();

    switch (currentLedMode) {
        case LED_CHASE_RED:
        case LED_CHASE_BLUE: updateChase(); break;
        case LED_PULSE_RED:
        case LED_PULSE_BLUE: updatePulse(); break;
        default: break;
    }
}
