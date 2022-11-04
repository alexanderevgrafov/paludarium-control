//include <DNSServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
//#include <ESP8266mDNS.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <Ticker.h>
#include <WiFiClient.h>
#include <WiFiManager.h>  //https://github.com/tzapu/WiFiManager
#include <coredecls.h>    // settimeofday_cb()
#include <sys/time.h>
#include <time.h>  // time() ctime()

#include "ArduinoJson.h"
#include "FS.h"
#include "LittleFS.h"  // LittleFS is declared
#include "MyTicker.h"

#define CONFIG_FILE "conf2"
#define SENSORS_FILE "sensors2"
//#define DATA_FILE "data"
#define DATA_DIR "/d"
#define DATA_DIR_SLASH "/d/"

#define FS_BLOCK_SIZE 8180
//#define FS_BLOCK_SIZE 1020

#define WIFI_CONFIG_DURATION_SEC 150
// #define TZ 3      // (utc+) TZ in hours
// #define DST_MN 0  // use 60mn for summer time in some countries
// #define TZ_MN ((TZ)*60)
#define TZ_SEC 0   //((TZ)*3600)
#define DST_SEC 0  //((DST_MN)*60)

#define SEC 1
#define MAX_SENSORS_COUNT 8
#define TEMP_BYTE_SIZE 4
#define STAMP_BYTE_SIZE 4

#define FILE_CHECK_EACH_HOURS 20
#define TICKERS 3

#define LED_PIN 4       // D2 on board
#define RELAY_PIN 14    // D5 on NodeMCU and WeMos.
#define ONE_WIRE_BUS 5  //D1 on board

//int current_log_id = 2;

struct event_record {
  time_t stamp;
  char event;
  int t[MAX_SENSORS_COUNT];
};

struct sensor_config {
  uint8_t addr[8];
  uint8_t weight;
};

struct config {
  int tl;
  int th;
  unsigned int ton;
  unsigned int toff;
  unsigned int read;
  unsigned int log;
  unsigned int flush;
  uint8_t blink;
};

const int MIN = SEC * 60;
const int LOOP_DELAY = 4 * SEC;
const int SENSORS_READ_EACH = 5 * MIN;
const int LOG_EACH = 10 * MIN;
const int FLUSH_LOG_EACH = 60 * MIN;
const int DATA_BUFFER_SIZE = 150;  // Maximum events we can keep in memory before Internet is back(===real time is known, and we can write a log)
const int PIN_LED = LED_BUILTIN;   // D4 on NodeMCU and WeMos. Controls the onboard LED.

bool initialConfig = false;

const char *strAllowOrigin = "Access-Control-Allow-Origin";
const char *strAllowMethod = "Access-Control-Allow-Method";
const char *strContentType = "application/json";

event_record dataLog[DATA_BUFFER_SIZE];
event_record curSensors;

config conf = {3, 10, 10, 10, 180, 1800, 7200, 1};

Ticker led_sin_ticker;
Ticker timers_aligner;

MyTicker tickers[TICKERS];
bool timersHourAligned = false;

const size_t capacity = JSON_OBJECT_SIZE(7) * 2 + 50;

DynamicJsonDocument doc(capacity);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature DS18B20(&oneWire);
ESP8266WebServer server(80);

sensor_config sensor[MAX_SENSORS_COUNT];

#define LED_WIFI 0
#define LED_R_ON 1
#define LED_R_OFF 2
unsigned led_profiles[3][7] = {
    2, 2, 2, 2, 2, 10, 0,
    450, 1, 5, 1, 0, 0, 0,
    1, 5, 1, 450, 0, 0, 0};
byte led_current_profile = LED_WIFI;
byte led_profile_phase = 0;
bool ledStatus = false;
bool ledStatusPrev = true; //to make sure first cycle turns led off;

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

void pwmLedManager2() {
  if (conf.blink == 0) {
    return;
  }
  if (!led_profiles[led_current_profile][led_profile_phase])
    led_profile_phase = 0;

  if (led_profiles[led_current_profile][led_profile_phase]) {
    led_sin_ticker.once(led_profiles[led_current_profile][led_profile_phase] / 10.0, pwmLedManager2);

    ledStatus = !ledStatus;
    led_profile_phase++;
  }
}

void setLedProfile(byte profile_num) {
  ledStatus = false;
  led_profile_phase = 0;
  if (conf.blink == 0) {
    return;
  }
  led_current_profile = profile_num;
  pwmLedManager2();
}

void serverSendHeaders() {
  server.sendHeader(strAllowOrigin, "*");
  server.sendHeader(strAllowMethod, "GET, POST, HEAD");
}

void serverSend(String smth) {
  serverSendHeaders();
  server.send(200, strContentType, smth);
}

void genFilename(String *fileName) {
  char buffer[8];
  int index = 0;

  timeTmp = localtime(&nowTime);
  do {
    sprintf(buffer, "%02d%02d%02d",
    (uint8)(timeTmp->tm_year - 100) % 100,
    (uint8)(timeTmp->tm_mon + 1) % 100,
    (uint8)timeTmp->tm_mday % 100);
    *fileName = DATA_DIR_SLASH + String(buffer) + (index > 0 ? ("_" + String(index)) : "");
    index++;
  } while (LittleFS.exists(*fileName));

  SERIAL_PRINT("New file name generated:");
  SERIAL_PRINTLN(*fileName);
}

void setCurrentEvent(char type) {
  curSensors.stamp = start ? nowTime : millis() / 1000;  // записываем число секунд от загрузки если нет настоящего времени (нет интернета)
  curSensors.event = type;
}

String validFileCharacters = "0123456789-[],\"\"onfst";  // supported events are "on", "off", "st"

bool checkFile(String *fileName) {
  File file = LittleFS.open(*fileName, "r");
  char ch;

  if (!file) {
    return false;
  }

  while (file.available()) {
    ch = file.read();
    if (validFileCharacters.indexOf(ch) < 0) {
      SERIAL_PRINTLN("File check failed because of '" + String(ch) + "'");
      return false;
    }
  }

  SERIAL_PRINTLN("File check passed!");
  return true;
}

void writeToFile(String *line, String *fileName) {
  File file;

  //if (!line->length()) return;
  SERIAL_PRINTLN("writeToFile");

  SERIAL_PRINT("File opened to append:");
  SERIAL_PRINTLN(*fileName);
  SERIAL_PRINTLN(*line);

  if ((fileCheckedAt + FILE_CHECK_EACH_HOURS * 60 * 60) < nowTime) {
    SERIAL_PRINTLN("File check");
    if (!checkFile(&currentFileName)) {
      genFilename(&currentFileName);
    }
    fileCheckedAt = nowTime;
  }

  file = LittleFS.open(*fileName, "a");

  if (!file) {
    genFilename(&currentFileName);
    file = LittleFS.open(currentFileName, "a");
  }

  if (file) {
    file.print(*line);

    currentFileSize = file.size();
    SERIAL_PRINT("ResultingSize:");
    SERIAL_PRINTLN(String(currentFileSize));

    file.close();
  }
}

void alignTimersToHour(bool force) {
  // Только если start - признак интернет-времени. Без точного времени невозможно соотносить с часами.
  if (start) {
    if (force || !timersHourAligned) {
      int delta = ceil(nowTime / 3600.0) * 3600 - nowTime;

      if (delta > 20) {
        SERIAL_PRINT("Align to hour required after(sec): ");
        SERIAL_PRINTLN(String(delta));

        timers_aligner.once(delta, [](void) { setTimers(); });
        timersHourAligned = true;
      }
    }
  }
}

void timeSyncCb() {
  gettimeofday(&tv, NULL);

  SERIAL_PRINTLN("--Time sync event--");
  if (start == 0) {
    SERIAL_PRINT("Start time is set == ");
    nowTime = time(nullptr);
    start = nowTime;
    SERIAL_PRINTLN(start);
    flushLogIntoFile();
  }

  alignTimersToHour(false);
}

void putSensorsIntoDataLog() {
  if (dataLogPointer < DATA_BUFFER_SIZE) {
    // Prevent same event type on same timestamp is logged
    if (dataLogPointer > 0) {
      if ((dataLog[dataLogPointer - 1].stamp == curSensors.stamp) && (dataLog[dataLogPointer - 1].event == curSensors.event)) {
        SERIAL_PRINT("Prevented log record duplicate: " + String(curSensors.stamp) + ", ["+ String(curSensors.event)+"]");
        return;
      }
    }
    dataLog[dataLogPointer] = curSensors;
    dataLogPointer++;
  }
}

String stampToPackedDate(time_t *time) {
  char buffer[12];

  timeTmp = localtime(time);

  sprintf(buffer, "%02d%02d%02d%02d%02d",
  (uint8)(timeTmp->tm_year - 100)%100,
  (uint8)(timeTmp->tm_mon + 1)%100,
  (uint8)timeTmp->tm_mday%100,
  (uint8)timeTmp->tm_hour%100,
  (uint8)timeTmp->tm_min%100
  );
  return String(buffer);
}

void checkCurrentFileName() {
  if (currentFileName.length() == 0) {
    Dir dir = LittleFS.openDir(DATA_DIR);
    time_t maxWriteTime = 0;
    time_t curFileTime;

    while (dir.next()) {
      curFileTime = dir.fileTime();
      if (curFileTime > maxWriteTime) {
        currentFileName = DATA_DIR_SLASH + dir.fileName();
        currentFileSize = dir.fileSize();
        maxWriteTime = curFileTime;
      }
    }

    if (!maxWriteTime) {
      genFilename(&currentFileName);
      currentFileSize = 0;
    }

    SERIAL_PRINT(" #INIT current file:");
    SERIAL_PRINT(currentFileName);
    SERIAL_PRINT(", size:");
    SERIAL_PRINTLN(String(currentFileSize));
  }
}

String genDataLogLine(event_record *record) {
  String data = "";

  if (record->event == 'b') {
    data = ",\"st\"";
  } else {
    for (int k = 0; k < sensorsCount; k++) {
      data += ",";
      data += record->t[k];
    }
    switch (record->event) {
      case 'n':
        data += ",\"on\"";
        break;
      case 'f':
        data += ",\"off\"";
        break;
    }
  }

  // маленькое число в stamp означает что запись была добавлена ДО синхронизации со временем и является числом секунд со старта.
  time_t time = record->stamp > 900000000 ? record->stamp : (time_t)(nowTime - millis() / 1000 + record->stamp);

  return "[" + stampToPackedDate(&time) + data + "]";
}

void flushLogIntoFile() {
  String all = "";

  SERIAL_PRINTLN("Flush log events");

  if (start == 0 || dataLogPointer == 0) {  // мы пишем лог только если знаем настоящее время.
    return;
  }

  checkCurrentFileName();

  all = currentFileSize > 0 ? "," : "[";

  for (int i = 0; i < dataLogPointer; i++) {
    String line = genDataLogLine(&dataLog[i]);

    if (currentFileSize + all.length() + 1 + line.length() > FS_BLOCK_SIZE) {
      if (all.length() > 2) {
        writeToFile(&all, &currentFileName);
      }

      genFilename(&currentFileName);
      currentFileSize = 0;
      all = "[" + line;
    } else {
      all += (i > 0 ? "," : "") + line;
    }
  }

  writeToFile(&all, &currentFileName);

  dataLogPointer = 0;
}

void setRelay(bool set) {
  relayOn = set;

  digitalWrite(RELAY_PIN, relayOn ? HIGH : LOW);

  SERIAL_PRINT("Relay is ");
  SERIAL_PRINTLN(relayOn ? "ON" : "OFF");

  relaySwitchedAt = nowTime;

  setCurrentEvent(relayOn ? 'n' : 'f');

  putSensorsIntoDataLog();
  flushLogIntoFile();
}

void scanSensors() {
  float tC, w, ws = 0, average = 0;

  gettimeofday(&tv, nullptr);
  clock_gettime(0, &tp);
  nowTime = time(nullptr);

  setCurrentEvent('t');

  digitalWrite(PIN_LED, LOW);
  DS18B20.requestTemperatures();

  for (int i = 0; i < sensorsCount; i++) {
    tC = DS18B20.getTempCByIndex(i);
    curSensors.t[i] = (int)round(tC * 10);

    w = sensor[i].weight / 100.0;
    ws += w;

    average += tC * w;
  }

  average = ws ? average / ws : -127;

  digitalWrite(PIN_LED, HIGH);

  if (average < -100 ||  // average -127 mean sensors problems so we better to switch off
      (average >= conf.th && relayOn && nowTime - relaySwitchedAt >= (int)conf.ton)) {
    setRelay(false);
    setLedProfile(LED_R_OFF);
  } else if (
      average > -100  // -127 if contact is broken or if weights are all 0
      && average <= conf.tl && !relayOn && nowTime - relaySwitchedAt >= (int)conf.toff) {
    setRelay(true);
    setLedProfile(LED_R_ON);
  }
}

void setTimers() {
  SERIAL_PRINTLN("Set timers");

  tickers[0].attach(conf.read, scanSensors);
  tickers[1].attach(conf.log, putSensorsIntoDataLog);
  tickers[2].attach(conf.flush, flushLogIntoFile);
}

void sensorsPrepareAddresses() {
  for (byte i = 0; i < sensorsCount; i++) {
    DS18B20.getAddress((uint8_t *)&sensor[i].addr, (uint8_t)i);

    sensor[i].weight = (byte)100 / sensorsCount;
  }
}

void sensorsParseString(String *line, byte *buffer) {
  int ptr = 0, sum = 0;

  for (unsigned i = 0; i < line->length(); i++) {
    char ch = line->charAt(i);

    if (ch >= '0' && ch <= '9')
      sum = sum * 10 + (ch - '0');
    else {
      buffer[ptr++] = (byte)sum;
      sum = 0;
    }

    if (ptr >= MAX_SENSORS_COUNT * 9)
      return;
  }
  buffer[ptr] = (byte)sum;
}

void sensorsBufferToFile(byte *buffer) {
  File file = LittleFS.open(SENSORS_FILE, "w");  // Open it
  file.write(buffer, sensorsCount * 9);
  file.close();

  SERIAL_PRINT("Sensor data saved to file");
}

void sensorsBufferFromFile(byte *buffer) {
  if (LittleFS.exists(SENSORS_FILE)) {
    File file = LittleFS.open(SENSORS_FILE, "r");
    file.readBytes((char *)buffer, sensorsCount * 9);
    file.close();
    // if (bytes < sensorsCount*9 )
    //   SERIAL_PRINTLN("--SERIOUS: sensor data read less than expected--");
    // else
    //   SERIAL_PRINTLN("Sensor data is read from file");
  }
}

void sensorsApplyBufferOn(byte *buffer) {
  for (byte i = 0; i < sensorsCount; i++) {
    for (byte j = 0; j < sensorsCount; j++) {
      int comp = 0;

      while (comp < 8 && sensor[i].addr[j] == buffer[i * 9 + j])
        comp++;

      if (comp == 8) {
        sensor[i].weight = buffer[i * 9 + 8];
        break;
      }
    }
  }
}

void serverSendfile(String fileName) {
  File f = LittleFS.open(DATA_DIR_SLASH + fileName, "r");

  if (f) {
    char buf[2048];
    size_t sent = 0;
    int siz = f.size();

    serverSendHeaders();
    server.setContentLength(siz + 1);
    server.send(200, strContentType, "");

    while (siz > 0) {
      size_t len = std::min((int)(sizeof(buf) - 1), siz);
      f.read((uint8_t *)buf, len);

      server.sendContent(buf, len);
      siz -= len;
      sent += len;
    }
    f.close();

    server.sendContent("]", 1);

  } else {
    serverSendHeaders();
    server.send(404, strContentType, "File Not Found: " + String(DATA_DIR_SLASH) + fileName);
    SERIAL_PRINTLN("Bad open file " + fileName);
  }
}

void parseConfJson(String *json) {
  DeserializationError err = deserializeJson(doc, *json);

  SERIAL_PRINT("Conf parse ");
  SERIAL_PRINTLN(err.c_str());

  if (!err) {
    conf.tl = doc["tl"].as<int>();
    conf.th = doc["th"].as<int>();
    conf.ton = doc["ton"].as<int>();
    conf.toff = doc["toff"].as<int>();
    conf.read = doc["read"].as<int>();
    conf.log = doc["log"].as<int>();
    conf.flush = doc["flush"].as<int>();
    conf.blink = doc["blink"].as<int>();
  }
}

void handleInfo() {
  String msg = "{";

  nowTime = time(nullptr);

  if (server.arg("cur").length() > 0) {
    float w, ws = 0, average = 0;
    unsigned long upTime = start ? nowTime - start : millis() / 1000;

    if (server.arg("f").length() > 0)
      scanSensors();

    msg += "\"up\":" + String(upTime) + ",\"rel\":" + String((int)relayOn) + ",\"cur\":" + genDataLogLine(&curSensors);

    for (int i = 0; i < sensorsCount; i++) {
      w = sensor[i].weight / 100.0;
      ws += w;
      average += curSensors.t[i] * w / 10;
    }

    average = ws ? average / ws : -127;
    msg += ",\"avg\":" + String(average) + "}";

  } else if (server.arg("last").length() > 0) {
    msg += "\"last\":[";

    if (start != 0 && dataLogPointer != 0) {  // мы пишем лог только если знаем настоящее время.
      for (int i = 0; i < dataLogPointer; i++) {
        String line = genDataLogLine(&dataLog[i]);

        msg += (i > 0 ? "," : "") + line;
      }
    }

    msg += "]}";
  } else {
    FSInfo fs;
    float flag = false;

    LittleFS.info(fs);

    msg += "\"fs\":{\"tot\":";
    msg += fs.totalBytes;
    msg += ",\"used\":";
    msg += fs.usedBytes;
    msg += ",\"block\":";
    msg += fs.blockSize;
    msg += ",\"page\":";
    msg += fs.pageSize;
    msg += "},\"rel\":";
    msg += (int)relayOn;
    msg += ",\"cur\":[";
    for (int i = 0; i < sensorsCount; i++) {
      if (i > 0)
        msg += ",";

      msg += curSensors.t[i];
    }
    msg += "],\"conf\":{\"tl\":";
    msg += conf.tl;
    msg += ",\"th\":";
    msg += conf.th;
    msg += ",\"ton\":";
    msg += conf.ton;
    msg += ",\"toff\":";
    msg += conf.toff;
    msg += ",\"read\":";
    msg += conf.read;
    msg += ",\"log\":";
    msg += conf.log;
    msg += ",\"flush\":";
    msg += conf.flush;
    msg += ",\"blink\":";
    msg += conf.blink;
    msg += "},\"sn\":\"";

    for (int i = 0; i < sensorsCount; i++) {
      if (i > 0)
        msg += ",";

      for (int k = 0; k < 8; k++) {
        msg += String(sensor[i].addr[k]);
        msg += ' ';
      }
      msg += String(sensor[i].weight);
    }

    msg += "\",\"dt\":[";

    Dir dir = LittleFS.openDir(DATA_DIR);
    while (dir.next()) {
      if (flag)
        msg += ",";
      msg += "{\"n\":\"";
      msg += dir.fileName();
      msg += "\",\"s\":";
      msg += dir.fileSize();

      flag = true;
      msg += "}";
    }
    msg += "]}";
  }

  serverSend(msg);
}

void handleConfig() {
    String msg;
    String json = server.arg("set");
    parseConfJson(&json);

    File file = LittleFS.open(CONFIG_FILE, "w");  // Open it
    file.println(server.arg("set"));
    file.close();  // Then close the file again

    SERIAL_PRINT("Conf<--");
    SERIAL_PRINT(server.arg("set"));

    alignTimersToHour(true);

    setLedProfile( relayOn ? LED_R_ON : LED_R_OFF);
    putSensorsIntoDataLog();  // TODO - added this line to log as soon as possible after board restart. If not, first log record can be found after 'conf.log' from restart (and this period is about few hours, which is not nice is final graph)

  handleInfo();
}

void handleSensors() {
    String line = server.arg("sn");
    byte sensBuff[9 * MAX_SENSORS_COUNT];

    sensorsParseString(&line, sensBuff);
    sensorsBufferToFile(sensBuff);
    sensorsApplyBufferOn(sensBuff);

    SERIAL_PRINT("SensConf<--");
    SERIAL_PRINTLN(line);

  handleInfo();
}

void handleGetData() {
  if (server.arg("f").length() > 0) {
    serverSendfile(server.arg("f"));
  } else if (server.arg("d").length() > 0) {
    String path = DATA_DIR_SLASH + server.arg("d");

    if (LittleFS.exists(path)) {
      LittleFS.remove(path);  // Remove it
      serverSend("{\"d\":1}");
    } else {
      serverSend("{\"d\":0}");
    }
  }
}

void handleFormat(){
  int success = LittleFS.format();
  serverSend("{\"formatted\":"+String(success)+"}");
}

void configFromFile() {
  File file;
  if (LittleFS.exists(CONFIG_FILE)) {
    int fileSize = 0;
    String json;
    File file = LittleFS.open(CONFIG_FILE, "r");  // Open it

    fileSize = file.size();

    SERIAL_PRINT("Conf file size ");
    SERIAL_PRINTLN(fileSize);

    if (fileSize > 20 && fileSize < 150) {
      json = file.readString();
      parseConfJson(&json);
      SERIAL_PRINT("Conf <-- ");
      SERIAL_PRINTLN(json);
    }
    file.close();  // Then close the file again
  }
}

void isWiFiConnected() {
  SERIAL_PRINTLN("WiFi");
  SERIAL_PRINTLN(WiFi.localIP().toString());
  /*
  while (WiFi.localIP().toString() == "0.0.0.0") {

P.S. I, also, found that in the BasicOTA example, it uses while (WiFi.waitForConnectResult()
  */
  if (WiFi.localIP().toString() == "0.0.0.0") {
    SERIAL_PRINTLN("No wifi located - set time for next period. ");
  //  SERIAL_PRINTLN(WiFi.status());


    timers_aligner.once(WIFI_CHECK_PERIOD, isWiFiConnected);
  } else {
    settimeofday_cb(timeSyncCb);
    configTime(TZ_SEC, DST_SEC, "pool.ntp.org");

    server.on("/conf", handleConfig);
    server.on("/sens", handleSensors);
    server.on("/data", handleGetData);
    server.on("/info", handleInfo);
    server.on("/formatFS", handleFormat);

    server.begin();

    SERIAL_PRINT("IP is ");
    SERIAL_PRINTLN(WiFi.localIP().toString());
    WiFi.printDiag(Serial);
  }
}

void WiFiSetup() {
  WiFiManager wifiManager;

  setLedProfile(LED_WIFI);

  SERIAL_PRINTLN("Waiting wifi");
  WiFi.mode(WIFI_STA);
  delay(1000);

  //WiFi.begin("GreenBox", "sobaka-enot");

  wifiManager.setConfigPortalTimeout(240);
  if (WiFi.SSID() != "")
    wifiManager.setConfigPortalTimeout(WIFI_CONFIG_DURATION_SEC);  //If no access point name has been previously entered disable timeout.

  wifiManager.autoConnect("ESP8266_HeatController");

//ToDo------
  // SERIAL_PRINTLN("Connecting");
  // while (WiFi.status() != WL_CONNECTED) {
  //   delay(2500);
  //   SERIAL_PRINTLN(WiFi.status());
  // }
  // SERIAL_PRINTLN();

  // SERIAL_PRINTLN("Connected, IP address: ");
  // SERIAL_PRINTLN(WiFi.localIP());

  isWiFiConnected();

  //digitalWrite(PIN_LED, HIGH); // Turn led off as we are not in configuration mode.
  // For some unknown reason webserver can only be started once per boot up
  // so webserver can not be used again in the sketch.
}

void setup() {
  byte sensBuff[9 * MAX_SENSORS_COUNT];

  pinMode(PIN_LED, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);

  Serial.begin(115200);
  SERIAL_PRINTLN("\n Starting");
  setCurrentEvent('b');
  putSensorsIntoDataLog();

  analogWrite(LED_PIN, 300);  //Just light up for setup period

  DS18B20.begin();
  LittleFS.begin();

  WiFiSetup();

  sensorsCount = DS18B20.getDeviceCount();
  sensorsCount = MAX_SENSORS_COUNT > sensorsCount ? sensorsCount : MAX_SENSORS_COUNT;

  configFromFile();

  sensorsPrepareAddresses();
  sensorsBufferFromFile(sensBuff);
  sensorsApplyBufferOn(sensBuff);

  setLedProfile(LED_R_OFF);

  setTimers();

  scanSensors();
  putSensorsIntoDataLog();
}

void loop() {
  int i;
  server.handleClient();

  if (ledStatus != ledStatusPrev) {
    analogWrite(LED_PIN, ledStatus ? 600 : 0);
    ledStatusPrev = ledStatus;
  }

  for (i = 0; i < TICKERS; i++)
    if (tickers[i].armed())
      tickers[i].run();
}
