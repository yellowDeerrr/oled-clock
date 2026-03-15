#include <Arduino.h>
#include <WiFi.h>
#include "time.h"
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <Wire.h>
#include <IRremote.hpp>
#include <Preferences.h>

#include "secrets.h"

#define DECODE_NEC

#define SCREEN_WIDTH      128
#define SCREEN_HEIGHT     64
#define IR_RECEIVE_PIN    2
#define BUZZER_PIN        6
#define BUTTON_PIN        4

#define WIFI_TIMEOUT_MS   10000
#define QUEUE_LENGTH      8
#define UI_TICK_MS        50
#define DISPLAY_REFRESH_MS  200

#define IDLE_SLEEP_MS 5000
#define SLEEP_CHECK_US 800000

const char* NTP_SERVER = "pool.ntp.org";

Preferences prefs;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

enum IRCommand : uint8_t {
  IR_ZERO   = 0x19, IR_ONE  = 0x45, IR_TWO   = 0x46, IR_THREE = 0x47,
  IR_FOUR   = 0x44, IR_FIVE = 0x40, IR_SIX   = 0x43, IR_SEVEN = 0x07,
  IR_EIGHT  = 0x15, IR_NINE = 0x09,
  IR_STAR   = 0x16, IR_HASH = 0x0D,
  IR_LEFT   = 0x08, IR_RIGHT = 0x5A,
  IR_UP     = 0x18, IR_DOWN  = 0x52,
  IR_CENTER = 0x1C
};

const unsigned char alarm_icon[] PROGMEM = {
	0x07, 0xe0, 0x1f, 0xf8, 0x3f, 0xfc, 0x7e, 0x7e, 0x7e, 0x7e, 0xfe, 0x7f, 0xfe, 0x7f, 0xfe, 0x7f, 
	0xff, 0x3f, 0xff, 0x9f, 0xff, 0xdf, 0x7f, 0xfe, 0x7f, 0xfe, 0x3f, 0xfc, 0x1f, 0xf8, 0x07, 0xe0
};

uint8_t hourChangingStep = 1;
uint8_t minuteChangingStep = 1;

struct Alarm {
    int hour;
    int minute;
    bool enabled;
    bool wasTriggered;
    bool isRinging;
};


QueueHandle_t eventQueue;
Alarm alarmClock = {8, 30, true, false, false};

void restoreAlarmSettings(){
  prefs.begin("alarm", true); // true = read-only
  alarmClock.hour    = prefs.getInt("hour", 8);    // 8 = default if not found
  alarmClock.minute  = prefs.getInt("minute", 30);
  alarmClock.enabled = prefs.getBool("enabled", true);
  prefs.end();
}
void displayMessage(const char* line1, const char* line2 = nullptr) {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(WHITE);

  display.setCursor(0, line2 ? 8 : 20);
  display.println(line1);

  if (line2) {
    display.setCursor(0, 36);
    display.println(line2);
  }

  display.display();
}

void displayClock(const char* timeStr, const char* alarmStr, const bool alarmEnabled) {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(WHITE);

  display.setCursor(0, 10);
  display.print(timeStr);

  display.setCursor(0, 35);
  display.print(alarmStr);

  if (alarmEnabled) 
    display.drawBitmap(65, 35, alarm_icon, 16, 16, WHITE);

  display.display();
}

void tryToGetWorldTime(){
  WiFi.begin(SSID, PASSWORD);

  unsigned long wifiStart = millis();

  while (WiFi.status() != WL_CONNECTED) {

    if (millis() - wifiStart > WIFI_TIMEOUT_MS) {
      Serial.println("WiFi timeout");
      displayMessage("No WiFi");
      break;
    }

    Serial.print(".");
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  if (WiFi.status() == WL_CONNECTED) {

    configTzTime(
      time_zone_location,
      NTP_SERVER
    );

    Serial.println("\nWiFi connected");
    delay(500);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) 
      Serial.println(&timeinfo, "Time synced: %H:%M:%S");
 
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}



void saveAlarm() {
  prefs.begin("alarm", false);
  prefs.putInt("hour",    alarmClock.hour);
  prefs.putInt("minute",  alarmClock.minute);
  prefs.putBool("enabled", alarmClock.enabled);
  prefs.end();
}


void IRTask(void* pvParameters) {
  static bool buttonWasPressed = false;
  IrReceiver.begin(IR_RECEIVE_PIN, ENABLE_LED_FEEDBACK);

  for (;;) {

    if (IrReceiver.decode()) {

      bool isRepeat = IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT;
      uint8_t rawCmd = IrReceiver.decodedIRData.command;

      IrReceiver.resume();

      if (!isRepeat) {
        IRCommand cmd = (IRCommand)rawCmd;
        xQueueSend(eventQueue, &cmd, pdMS_TO_TICKS(10));
      }
    }

    bool buttonPressed = !digitalRead(BUTTON_PIN);

    if (buttonPressed && !buttonWasPressed) {
        IRCommand cmd = IR_CENTER;
        xQueueSend(eventQueue, &cmd, pdMS_TO_TICKS(10));
    }
    buttonWasPressed = buttonPressed;

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void UITask(void* pvParameters) {

  struct tm timeinfo;
  unsigned long lastUpdate = 0;
  bool changingHours = true;

  unsigned long lastActivity = millis();

  displayMessage("Ready");

  for (;;) {
    IRCommand cmd;

    if (xQueueReceive(eventQueue, &cmd, pdMS_TO_TICKS(UI_TICK_MS))) {
      lastActivity = millis(); // reset idle timer

      switch (cmd) {
        
        case IR_DOWN:
            if (alarmClock.enabled || alarmClock.isRinging) break;

            if (changingHours) {
                alarmClock.hour = (alarmClock.hour + 24 - hourChangingStep) % 24;
            } else {
                alarmClock.minute = (alarmClock.minute + 60 - minuteChangingStep) % 60;
            }
            break;

        case IR_UP:
            if (alarmClock.enabled || alarmClock.isRinging) break;

            if (changingHours) {
                alarmClock.hour = (alarmClock.hour + hourChangingStep) % 24;
            } else {
                alarmClock.minute = (alarmClock.minute + minuteChangingStep) % 60;
            }
            break;

        case IR_RIGHT:
        case IR_LEFT:
            changingHours = !changingHours;
            break;

        case IR_CENTER:
          if (alarmClock.isRinging) {
            noTone(BUZZER_PIN);
            alarmClock.enabled   = false;
            alarmClock.isRinging = false;
          } else {
            alarmClock.enabled = !alarmClock.enabled;
          }
          saveAlarm();
          break;
      }
    }

    unsigned long now = millis();

    if (now - lastUpdate < DISPLAY_REFRESH_MS) {
      continue;
    }

    if (!getLocalTime(&timeinfo)) {
      displayMessage("No NTP", "check wifi");
      delay(100);
      tryToGetWorldTime();
      continue;
    }

    lastUpdate = now;
    
    if (!alarmClock.isRinging &&
        now - lastActivity > IDLE_SLEEP_MS) {
      esp_sleep_enable_timer_wakeup(SLEEP_CHECK_US);
      esp_sleep_enable_gpio_wakeup();
      gpio_wakeup_enable((gpio_num_t)BUTTON_PIN, GPIO_INTR_LOW_LEVEL);
      gpio_wakeup_enable((gpio_num_t)IR_RECEIVE_PIN, GPIO_INTR_LOW_LEVEL);

      esp_light_sleep_start();   // sleep here
    }
   
    if (alarmClock.isRinging){
      static unsigned long lastToggle = 0;
      static bool buzzerState = false;

      const int BEEP_INTERVAL = 300;

      if(now - lastToggle >= BEEP_INTERVAL){
        buzzerState = !buzzerState;

        if(buzzerState)
          tone(BUZZER_PIN, 1000);
        else
          noTone(BUZZER_PIN);

        lastToggle = now;
      }
    }
    if (alarmClock.enabled &&
        timeinfo.tm_hour == alarmClock.hour &&
        timeinfo.tm_min  == alarmClock.minute &&
        !alarmClock.wasTriggered) 
    {
      alarmClock.wasTriggered = true;
      alarmClock.isRinging = true;
    }

    if (timeinfo.tm_hour != alarmClock.hour ||
      timeinfo.tm_min  != alarmClock.minute) {
      alarmClock.wasTriggered = false;
    }

    char buffer[9];
    char bufferAlarm[9];

    snprintf(buffer, sizeof(buffer),
             "%02d:%02d:%02d",
             timeinfo.tm_hour,
             timeinfo.tm_min,
             timeinfo.tm_sec);

    snprintf(bufferAlarm, sizeof(bufferAlarm),
             "%02d:%02d",
             alarmClock.hour,
             alarmClock.minute);

    displayClock(buffer, bufferAlarm, alarmClock.enabled && !alarmClock.wasTriggered);
  }
}


void setup() {
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire.begin(8, 9);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 init failed");

    for (;;) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  restoreAlarmSettings();

  displayMessage("Connecting", "WiFi...");

  tryToGetWorldTime();

  eventQueue = xQueueCreate(QUEUE_LENGTH, sizeof(IRCommand));

  configASSERT(eventQueue);

  xTaskCreatePinnedToCore(
    IRTask,
    "IRTask",
    4096,
    nullptr,
    1,
    nullptr,
    0
  );

  xTaskCreatePinnedToCore(
    UITask,
    "UITask",
    4096,
    nullptr,
    1,
    nullptr,
    1
  );
}

void loop() {
  vTaskSuspend(nullptr);
}