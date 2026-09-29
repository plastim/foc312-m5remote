// Hardware bring-up for the OSSM M5 Remote carrier: shows every encoder count and every button's raw level, on the
// screen and on USB serial, so the physical knob <-> pin mapping is measured, not assumed. No stim, no Wi-Fi.
// Pins from the OSSM remote's config.h (S3): facts about the board, not code from that project.
#include <Arduino.h>
#include <ESP32Encoder.h>
#include <M5Unified.h>

static const int ENC_PINS[4][2] = {{5, 9}, {18, 17}, {1, 2}, {7, 6}};   // CLK, DT
static const int BTN_PINS[3] = {10, 8, 14};                             // MX, "encoder left", "encoder right"
static const char *const BTN_NAMES[3] = {"MX", "push L (8)", "push R (14)"};

static ESP32Encoder enc[4];
static long last_count[4];
static int last_btn[3];

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
    Serial.begin(115200);
    ESP32Encoder::useInternalWeakPullResistors = puType::up;
    for (int i = 0; i < 4; i++) {
        enc[i].attachHalfQuad(ENC_PINS[i][0], ENC_PINS[i][1]);
        enc[i].setCount(0);
    }
    for (int i = 0; i < 3; i++) pinMode(BTN_PINS[i], INPUT);
    M5.Display.setTextSize(2);
    M5.Display.fillScreen(TFT_BLACK);
}

void loop() {
    M5.update();
    bool changed = false;
    for (int i = 0; i < 4; i++) {
        long c = enc[i].getCount();
        if (c != last_count[i]) {
            Serial.printf("enc%d (pins %d/%d) count %ld\n", i + 1, ENC_PINS[i][0], ENC_PINS[i][1], c);
            last_count[i] = c;
            changed = true;
        }
    }
    for (int i = 0; i < 3; i++) {
        int v = digitalRead(BTN_PINS[i]);
        if (v != last_btn[i]) {
            Serial.printf("%s (pin %d) level %d\n", BTN_NAMES[i], BTN_PINS[i], v);
            last_btn[i] = v;
            changed = true;
        }
    }
    static uint32_t last_draw;
    if (changed || millis() - last_draw > 1000) {
        last_draw = millis();
        M5.Display.setCursor(0, 0);
        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        M5.Display.println("stim remote hwtest\n");
        for (int i = 0; i < 4; i++) M5.Display.printf("enc%d (%2d/%2d): %6ld  \n", i + 1, ENC_PINS[i][0], ENC_PINS[i][1], last_count[i]);
        M5.Display.println();
        for (int i = 0; i < 3; i++) M5.Display.printf("%-12s: %d  \n", BTN_NAMES[i], last_btn[i]);
        M5.Display.printf("\nbattery %d%%   \n", M5.Power.getBatteryLevel());
    }
    delay(5);
}
