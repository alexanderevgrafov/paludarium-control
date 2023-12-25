// include <DNSServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
// #include <ESP8266mDNS.h>
// #include <DallasTemperature.h>
// #include <OneWire.h>
#include <Ticker.h>
#include <WiFiClient.h>
#include <WiFiManager.h> //https://github.com/tzapu/WiFiManager
#include <coredecls.h>   // settimeofday_cb()
#include <sys/time.h>
#include <time.h> // time() ctime()

#include "ArduinoJson.h"
// #include "FS.h"
// #include "LittleFS.h"  // LittleFS is declared
#include "MyTicker.h"

// #define CONFIG_FILE "conf2"
// #define SENSORS_FILE "sensors2"
// #define DATA_FILE "data"
// #define DATA_DIR "/d"
// #define DATA_DIR_SLASH "/d/"

// #define FS_BLOCK_SIZE 8180
// #define FS_BLOCK_SIZE 1020


// #define TZ 3      // (utc+) TZ in hours
// #define DST_MN 0  // use 60mn for summer time in some countries
// #define TZ_MN ((TZ)*60)
#define TZ_SEC 0  //((TZ)*3600)
#define DST_SEC 0 //((DST_MN)*60)

#define SEC 1
// #define MAX_SENSORS_COUNT 8
// #define TEMP_BYTE_SIZE 4
// #define STAMP_BYTE_SIZE 4

// #define FILE_CHECK_EACH_HOURS 20
#define TICKERS 3

//  GPIO numbers for pind D0-4:   16, 5, 4, 0, 2
#define LED_PIN LED_BUILTIN // 4       // D2 on board
#define PIN_LIGHT 16        // GPIO D0
#define PIN_PUMP 5          // GPIO D1
// #define RELAY_PIN 14        // D5 on NodeMCU and WeMos.
// #define ONE_WIRE_BUS 5      // D1 on board

// int current_log_id = 2;

// struct event_record {
//   time_t stamp;
//   char event;
//   int t[MAX_SENSORS_COUNT];
// };

// struct sensor_config
// {
//   uint8_t addr[8];
//   uint8_t weight;
// };

struct config
{
  bool pump;
  bool light;
  // int tl;
  // int th;
  // unsigned int ton;
  // unsigned int toff;
  // unsigned int read;
  // unsigned int log;
  // unsigned int flush;
  uint8_t ledProfile;
};

const int MIN = SEC * 60;
const int LOOP_DELAY = 4 * SEC;
const int SENSORS_READ_EACH = 5 * MIN;
const int LOG_EACH = 10 * MIN;
const int FLUSH_LOG_EACH = 60 * MIN;
const int DATA_BUFFER_SIZE = 150; // Maximum events we can keep in memory before Internet is back(===real time is known, and we can write a log)
const int PIN_LED = LED_BUILTIN;  // D4 on NodeMCU and WeMos. Controls the onboard LED.

bool initialConfig = false;

const char *strAllowOrigin = "Access-Control-Allow-Origin";
const char *strAllowMethod = "Access-Control-Allow-Method";
const char *strContentType = "application/json";

// event_record dataLog[DATA_BUFFER_SIZE];
// event_record curSensors;

config conf = {false, true, 0};

Ticker led_sin_ticker;
Ticker timers_aligner;

MyTicker tickers[TICKERS];
bool timersHourAligned = false;

// const size_t capacity = JSON_OBJECT_SIZE(7) * 2 + 50;

// DynamicJsonDocument doc(capacity);
// OneWire oneWire(ONE_WIRE_BUS);
// DallasTemperature DS18B20(&oneWire);

#define WIFI_CONFIG_DURATION_SEC 240
#define WIFI_RETRY_FIRST_INTERVAL 60
#define WIFI_RETRY_MAX_INTERVAL 7200
ESP8266WebServer server(80);
int wifiConnectAttemptCounter = 0;

// sensor_config sensor[MAX_SENSORS_COUNT];

#define LED_PROFILES_COUNT 3
#define LED_WIFI 0
#define LED_R_ON 1
#define LED_R_OFF 2
unsigned led_profiles[LED_PROFILES_COUNT][7] = {
    2, 2, 2, 2, 2, 10, 0,
    450, 1, 5, 1, 0, 0, 0,
    1, 5, 1, 450, 0, 0, 0};
byte led_current_profile = LED_WIFI;
byte led_profile_phase = 0;
int curPinStatus = 0;
int prevPinStatus = 0;
bool ledStatus = false;
bool ledStatusPrev = true; // to make sure first cycle turns led off;

timeval tv;
timespec tp;
struct tm *timeTmp;

time_t nowTime;
time_t start = 0;
time_t relaySwitchedAt = 0;
time_t fileCheckedAt = 0;

String currentFileName;
long currentFileSize;

int sensorsCount = 0;
int dataLogPointer = 0;
bool relayOn = false;

extern "C" int clock_gettime(clockid_t unused, struct timespec *tp);
void WiFiSetup(void);
void setTimers(void);
void flushLogIntoFile(void);

#define SERIAL_DEBUG 1
#if SERIAL_DEBUG
#define WIFI_CHECK_PERIOD 2
#define SERIAL_PRINT(msg) Serial.print(msg);
#define SERIAL_PRINTLN(msg) Serial.println(msg);
#else
#define WIFI_CHECK_PERIOD 20
#define SERIAL_PRINT(msg) ;
#define SERIAL_PRINTLN(msg) ;
#endif

void pwmLedManager2()
{
  if (conf.ledProfile == 0)
  {
    return;
  }
  if (!led_profiles[led_current_profile][led_profile_phase])
    led_profile_phase = 0;

  if (led_profiles[led_current_profile][led_profile_phase])
  {
    led_sin_ticker.once(led_profiles[led_current_profile][led_profile_phase] / 10.0, pwmLedManager2);

    ledStatus = !ledStatus;
    led_profile_phase++;
  }
}

void setLedProfile(byte profile_num)
{
  ledStatus = false;
  led_profile_phase = 0;
  if (profile_num == 0)
  {
    return;
  }
  led_current_profile = profile_num - 1;
  pwmLedManager2();
}

void serverSendHeaders()
{
  server.sendHeader(strAllowOrigin, "*");
  server.sendHeader(strAllowMethod, "GET, POST, HEAD");
}

void serverSend(String smth)
{
  serverSendHeaders();
  server.send(200, strContentType, smth);
}

void alignTimersToHour(bool force)
{
  // Только если start - признак интернет-времени. Без точного времени невозможно соотносить с часами.
  if (start)
  {
    if (force || !timersHourAligned)
    {
      int delta = ceil(nowTime / 3600.0) * 3600 - nowTime;

      if (delta > 20)
      {
        SERIAL_PRINT("Align to hour required after(sec): ");
        SERIAL_PRINTLN(String(delta));

        timers_aligner.once(delta, [](void)
                            { setTimers(); });
        timersHourAligned = true;
      }
    }
  }
}

void timeSyncCb()
{
  gettimeofday(&tv, NULL);

  SERIAL_PRINTLN("--Time sync event--");
  if (start == 0)
  {
    SERIAL_PRINT("Start time is set == ");
    nowTime = time(nullptr);
    start = nowTime;
    SERIAL_PRINTLN(start);
    // flushLogIntoFile();
  }

  alignTimersToHour(false);
}

void setTimers()
{
  SERIAL_PRINTLN("Set timers");

  // tickers[0].attach(conf.read, scanSensors);
  // tickers[1].attach(conf.log, putSensorsIntoDataLog);
  // tickers[2].attach(conf.flush, flushLogIntoFile);
}

void changePinStatus()
{
  SERIAL_PRINTLN("ChangePinStatus...");

  setLedProfile(conf.ledProfile);
  digitalWrite(PIN_LIGHT, conf.light ? HIGH : LOW);
  digitalWrite(PIN_PUMP, conf.pump ? HIGH : LOW);
}

String configToJson()
{
  String msg = "{";
  msg += "\"pump\":" + String(conf.pump ? "1" : "0");
  msg += ",\"light\":" + String(conf.light ? "1" : "0");
  msg += ",\"ledProfile\":" + String(conf.ledProfile);
  msg += "}";
  return msg;
}

void handleInfo()
{
  String msg = "{";

  nowTime = time(nullptr);

  unsigned long upTime = start ? nowTime - start : millis() / 1000;

  msg += "\"up\":" + String(upTime) + ",\"conf\":" + configToJson() + '}';

  serverSend(msg);
}

void handleSet()
{
  if (server.arg("l").length()>0) {
    conf.light = server.arg("l").toInt() > 0;
  }
  if (server.arg("p").length()>0) {
    conf.pump = server.arg("p").toInt() > 0;
  }
  if (server.arg("b").length()>0) {
    conf.ledProfile = server.arg("b").toInt() % (LED_PROFILES_COUNT + 1);
  }
  SERIAL_PRINT("SetConf<---");

  changePinStatus();

  handleInfo();
}

boolean isWiFiConnected()
{
  SERIAL_PRINTLN("WiFi");
  SERIAL_PRINTLN(WiFi.localIP().toString());
  /*
  while (WiFi.localIP().toString() == "0.0.0.0") {

P.S. I, also, found that in the BasicOTA example, it uses while (WiFi.waitForConnectResult()
  */
  if (WiFi.localIP().toString() == "0.0.0.0")
  {
    //  SERIAL_PRINTLN(WiFi.status());
    return false;
  }

  return true;
}

void wifiConnectionCycle()
{
  if (isWiFiConnected())
  {
    settimeofday_cb(timeSyncCb);
    configTime(TZ_SEC, DST_SEC, "pool.ntp.org");

    server.on("/set", handleSet);
    server.on("/info", handleInfo);

    server.begin();

    SERIAL_PRINT("IP is ");
    SERIAL_PRINTLN(WiFi.localIP().toString());
    WiFi.printDiag(Serial);
    return;
  }

  wifiConnectAttemptCounter++;
  int interval = max(wifiConnectAttemptCounter * WIFI_RETRY_FIRST_INTERVAL, WIFI_RETRY_MAX_INTERVAL);
  timers_aligner.once(interval, wifiConnectionCycle);

  SERIAL_PRINTLN("No wifi located - set next attempt after " + String(interval) );

  WiFi.begin( "BlackCatTbilisi", "KuraRiver");
}

void WiFiSetup()
{

WiFiManager wifiManager;
  setLedProfile(LED_WIFI);

  SERIAL_PRINTLN("Waiting wifi");
  WiFi.mode(WIFI_STA);
  delay(1000);

  // WiFi.begin("GreenBox", "sobaka-enot");

  wifiManager.setConfigPortalTimeout(240);
  // if (WiFi.SSID() != "")
  //   wifiManager.setConfigPortalTimeout(WIFI_CONFIG_DURATION_SEC); // If no access point name has been previously entered disable timeout.

  wifiManager.autoConnect("Paludarium_ESP8266");
  //wifiManager.setConnectTimeout(300);

  // ToDo------
  //  SERIAL_PRINTLN("Connecting");
  //  while (WiFi.status() != WL_CONNECTED) {
  //    delay(2500);
  //    SERIAL_PRINTLN(WiFi.status());
  //  }
  //  SERIAL_PRINTLN();

  // SERIAL_PRINTLN("Connected, IP address: ");
  // SERIAL_PRINTLN(WiFi.localIP());

  wifiConnectionCycle();

  // digitalWrite(PIN_LED, HIGH); // Turn led off as we are not in configuration mode.
  //  For some unknown reason webserver can only be started once per boot up
  //  so webserver can not be used again in the sketch.
}

void setup()
{
  // byte sensBuff[9 * MAX_SENSORS_COUNT];

  pinMode(PIN_LED, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(PIN_LIGHT, OUTPUT);
  pinMode(PIN_PUMP, OUTPUT);

  Serial.begin(115200);
  SERIAL_PRINTLN("\n Starting");
  // setCurrentEvent('b');
  // putSensorsIntoDataLog();

  analogWrite(LED_PIN, 300); // Just light up for setup period

  // DS18B20.begin();
  // LittleFS.begin();

  WiFiSetup();

  // sensorsCount = DS18B20.getDeviceCount();
  // sensorsCount = MAX_SENSORS_COUNT > sensorsCount ? sensorsCount : MAX_SENSORS_COUNT;

  // configFromFile();

  // sensorsPrepareAddresses();
  // sensorsBufferFromFile(sensBuff);
  // sensorsApplyBufferOn(sensBuff);

  setLedProfile(LED_R_OFF);

  setTimers();

  digitalWrite(PIN_LIGHT, HIGH);
}

void loop()
{
  int i;
  server.handleClient();

  if (ledStatus != ledStatusPrev)
  {
    SERIAL_PRINTLN(ledStatus);
    analogWrite(LED_PIN, ledStatus ? 0 : 600);
    ledStatusPrev = ledStatus;
  }

  for (i = 0; i < TICKERS; i++)
    if (tickers[i].armed())
      tickers[i].run();
}
