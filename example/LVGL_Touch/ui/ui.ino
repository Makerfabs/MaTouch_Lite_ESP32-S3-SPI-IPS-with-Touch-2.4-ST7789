#include <lvgl.h>
#include <TFT_eSPI.h>
#include <ui.h>

#include <SPI.h>
#include <Wire.h>
#include "Adafruit_SGP30.h"
#include "Adafruit_SHT31.h"
#include "NS2009.h"

#include <WiFi.h>
#include "time.h"

#define I2C_SDA_PIN 5
#define I2C_SCL_PIN 4
#define TOUCH_SDA_PIN 10
#define TOUCH_SCL_PIN 9

const char* ssid       = "Makerfabs_SH";
const char* password   = "20160704";

const char* ntpServer = "ntp.aliyun.com";
const long  gmtOffset_sec = 8 * 3600;
const int   daylightOffset_sec = 0;

Adafruit_SHT31 sht31;
Adafruit_SGP30 sgp;
NS2009 touch(0x48, false, false);

unsigned long lastSensorRead = 0;
unsigned long lastTouchTime = 0;
bool screenOn = true;
const unsigned long SCREEN_OFF_MS = 10000;

float h = 0.0;
float t = 0.0;
int TVOC = 0;
int eCO2 = 0;

String currentTime = "";
String currentDate = "";
unsigned long lastTimeUpdate = 0;
const unsigned long TIME_UPDATE_MS = 1000;

static const uint16_t screenWidth  = 240;
static const uint16_t screenHeight = 320;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[ screenWidth * 160 ];
static lv_color_t buf2[ screenWidth * 160 ];

TFT_eSPI tft = TFT_eSPI(screenWidth, screenHeight);

#if LV_USE_LOG != 0
void my_print(const char * buf)
{
    Serial.printf(buf);
    Serial.flush();
}
#endif

void my_disp_flush( lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p )
{
    uint32_t w = ( area->x2 - area->x1 + 1 );
    uint32_t h = ( area->y2 - area->y1 + 1 );

    tft.startWrite();
    tft.setAddrWindow( area->x1, area->y1, w, h );
    tft.pushColors( ( uint16_t * )&color_p->full, w * h, true );
    tft.endWrite();

    lv_disp_flush_ready( disp );
}

void my_touchpad_read( lv_indev_drv_t * indev_driver, lv_indev_data_t * data )
{
    touch.Scan();

    if (touch.Touched) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = touch.X;
        data->point.y = touch.Y;
        lastTouchTime = millis();
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

uint32_t getAbsoluteHumidity(float temperature, float humidity)
{
    const float absoluteHumidity = 216.7f * ((humidity / 100.0f) * 6.112f * exp((17.62f * temperature) / (243.12f + temperature)) / (273.15f + temperature));
    const uint32_t absoluteHumidityScaled = static_cast<uint32_t>(1000.0f * absoluteHumidity);
    return absoluteHumidityScaled;
}

void updateLocalTimeText()
{
    time_t now = time(nullptr);

    if (now < 100000) {
        currentTime = "--:--:--";
        currentDate = "---- -- --";
        return;
    }

    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    char timeStr[16];
    strftime(timeStr, 16, "%H:%M:%S", &timeinfo);
    currentTime = String(timeStr);

    char dateStr[32];
    strftime(dateStr, 32, "%Y-%m-%d", &timeinfo);
    currentDate = String(dateStr);
}

void initWiFi()
{
    WiFi.begin(ssid, password);
    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED && timeout < 15) {
        delay(500);
        timeout++;
    }
}

void sensor_init()
{
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(100000UL);
    delay(100);

    if (!sht31.begin(0x44)) {
        Serial.println("SHT31 not found");
    }
    if (!sgp.begin()) {
        Serial.println("SGP30 not found");
    }
}

void updateSensorData()
{
    float humidity = sht31.readHumidity();
    float temperature = sht31.readTemperature();

    if (!isnan(temperature)) t = temperature;
    if (!isnan(humidity)) h = humidity;

    sgp.setHumidity(getAbsoluteHumidity(t, h));

    if (sgp.IAQmeasure()) {
        TVOC = sgp.TVOC;
        eCO2 = sgp.eCO2;
    }

    static char lastT[32] = "";
    static char lastH[32] = "";
    static char lastC[32] = "";
    static char lastOC[32] = "";
    char text[32];

    sprintf(text, "%.1f °C", t);
    if (strcmp(text, lastT) != 0) {
        strcpy(lastT, text);
        lv_label_set_text(ui_Label2, text);
    }

    sprintf(text, "%.1f %%", h);
    if (strcmp(text, lastH) != 0) {
        strcpy(lastH, text);
        lv_label_set_text(ui_Label4, text);
    }

    sprintf(text, "%d ppm", eCO2);
    if (strcmp(text, lastC) != 0) {
        strcpy(lastC, text);
        lv_label_set_text(ui_Label6, text);
    }

    sprintf(text, "%d ppb", TVOC);
    if (strcmp(text, lastOC) != 0) {
        strcpy(lastOC, text);
        lv_label_set_text(ui_Label8, text);
    }
}

void updateTimeScreen()
{
    if(currentTime == "") return;
  
    static char lastTime[32] = "";
    char timeText[32];
    currentTime.toCharArray(timeText, 32);

    if (strcmp(timeText, lastTime) != 0) {
        strcpy(lastTime, timeText);
        lv_label_set_text(ui_Label9, timeText);
    }
}

void checkScreenAutoOff() {
    static bool firstRun = true;
    if(firstRun){
        lastTouchTime = millis();
        firstRun = false;
    }

    if (screenOn && millis() - lastTouchTime > SCREEN_OFF_MS) {
        screenOn = false;
        digitalWrite(TFT_BL, LOW);
    }
    else if (!screenOn && touch.Touched) {
        screenOn = true;
        digitalWrite(TFT_BL, HIGH);
        lastTouchTime = millis();
    }
}

void setup()
{
    Serial.begin(115200);

    // 
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    initWiFi();
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    lv_init();

    tft.begin();
    tft.setRotation(0);

    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, screenWidth * 160);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    sensor_init();

    Wire1.begin(TOUCH_SDA_PIN, TOUCH_SCL_PIN);
    Wire1.setClock(100000);
    touch.Calibrate(CALIBRATE_MIN_X, CALIBRATE_MAX_X, CALIBRATE_MIN_Y, CALIBRATE_MAX_Y);

    ui_init();
    updateSensorData();
}

void loop()
{
    lv_timer_handler();

    if (millis() - lastSensorRead > 2000) {
        lastSensorRead = millis();
        updateSensorData();
    }

    if (millis() - lastTimeUpdate > TIME_UPDATE_MS) {
        lastTimeUpdate = millis();
        updateLocalTimeText();
        updateTimeScreen();
    }

    checkScreenAutoOff();
    delay(5);
}
