#if defined(ESP8266)
#include <pgmspace.h>
#else
#include <avr/pgmspace.h>
#endif
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Wire.h>
#include <time.h>
#include "FS.h"

#if defined(ESP8266)
#include <pgmspace.h>
#else
#include <avr/pgmspace.h>
#endif
#include <RtcDS3231.h>

#define RTC_ALARM_PIN 13
#define CHIME_PIN 12
#define HEARTBEAT_PIN 2

struct BMChimeConfig {
  String deviceDescription;
  String wiFiSSID;
  String wiFiPassword;
  // From ESP8266WiFi.h
  WiFiMode wiFiMode;
  bool connectWiFiAtReset;
  uint8_t wakeEveryN;
  uint8_t stayAwakeMins;
  uint8_t chimeEveryN; 
  uint8_t chimeOffset; 
} config;

char ssid[64];
char password[64];
const int led = 2;

RtcDS3231 Rtc;
ESP8266WebServer server(80);

uint16_t sleepDuration = 0;

volatile bool interruptFlag = false;

// Utility functions
void alarmISR() {
  interruptFlag = true;
}

void printStringAsHexDump(String str) {
  char buf[64];
  str.toCharArray(buf, 64);
  for (int i = 0; i < str.length(); i++) {
    Serial.print(buf[i], HEX);
    Serial.print(" ");
  }
  Serial.println("");
}

void dateTimeStringFromRtcDateTime(RtcDateTime rtcDateTime, String& dateTimeBuf) {
  char datestring[20];
  snprintf_P(datestring, 
    20,
    PSTR("%02u/%02u/%04u %02u:%02u:%02u"),
    rtcDateTime.Month(),
    rtcDateTime.Day(),
    rtcDateTime.Year(),
    rtcDateTime.Hour(),
    rtcDateTime.Minute(),
    rtcDateTime.Second()
  );

  dateTimeBuf = String(datestring);
}

void getRtcDateTimeString(String& dateTimeBuf) {
  RtcDateTime now = Rtc.GetDateTime();
  dateTimeStringFromRtcDateTime(now, dateTimeBuf);
}

void getNtpDateTimeString(String& dateTimeBuf) {
  struct tm *tmp;
  time_t systemTime = time(nullptr);
  tmp = localtime(&systemTime);

  RtcDateTime systemRtcDateTime = RtcDateTime(
    tmp->tm_year + 1900,
    tmp->tm_mon + 1,
    tmp->tm_mday,
    tmp->tm_hour,
    tmp->tm_min,
    tmp->tm_sec
  );
  dateTimeStringFromRtcDateTime(systemRtcDateTime, dateTimeBuf);
}

uint16_t secondsTilNextN(uint8_t N) {
  // 16 bit return value should be sufficient; we will chime or wake at least once an hour.
  RtcDateTime now = Rtc.GetDateTime();
  
  uint16_t seconds = ((N - (now.Minute() % N)) - 1) * 60;
  seconds += 60 - now.Second();

  return seconds;
}

bool alarmFired(DS3231AlarmFlag& flag) {
  bool wasAlarmed = false;
  if (interruptFlag) { // check our flag that gets sets in the interrupt
    wasAlarmed = true;
    interruptFlag = false; // reset the flag

    // this gives us which alarms triggered and
    // then allows for others to trigger again
    flag = Rtc.LatchAlarmsTriggeredFlags();
  }
  return wasAlarmed;
}


// Web server handler functions

void handleRoot() {
  RtcTemperature dieTemp = Rtc.GetTemperature();
  float dieTempF = dieTemp.AsFloat()*(9/5)+32;

  String rtcDateTimeStr;
  getRtcDateTimeString(rtcDateTimeStr);

  String message = "<html>\n<head>\n\t<title>Chimes Controller</title>\n</head>\n<body>\n";
  message += "<h1>";
  message += config.deviceDescription;
  message += "</h1>\n";
  message += "<h2>Status</h2>\n";
  message += "RTC date and time ";
  message += rtcDateTimeStr;
  message += "<br/>\n";
  message += "Die temperature ";
  message += dieTempF;
  message += "&deg;F<br/>\n";
  if (WiFi.status() == WL_CONNECTED) {
    message += "Connected to WiFi network ";
    message += WiFi.SSID();
    message += "<br/>\n";
  }
  message += "<form action=\"/config\" method=\"get\"><input type=\"submit\" value=\"Configure\"/></form>\n";
  message += "<form action=\"/time\" method=\"get\"><input type=\"submit\" value=\"Manage Time\"/></form>\n";
  message += "<form action=\"/sleep\" method=\"get\"><input type=\"submit\" value=\"Sleep Now\"/></form>\n";
  message += "<form action=\"/reset\" method=\"get\"><input type=\"submit\" value=\"Reset\"/></form>\n";
  message += "</body>\n</html>\n";
  server.send(200, "text/html", message);
}

void handleSleep() {
  String message = "<html>\n<head>\n\t<title>Sleep</title>\n</head>\n<body>\n";
  message += "<h1>Not yet implemented</h1>\n";
  message += "<form action=\"/\" method=\"post\"><input type=\"submit\" value=\"Home\"/></form>\n";
  message += "</body>\n</html>\n";
  
  server.send(200, "text/html", message);
}

void handleReset() {
  String message = "<html>\n<head>\n\t<title>Reset</title>\n</head>\n<body>\n";
  message += "<h1>Resetting!</h1>";
  message += "</body>\n</html>\n";
  
  server.sendHeader("Refresh", "20; url=/");
  server.send(200, "text/html", message);
  delay(2000);
  ESP.restart();
}

void handleTemp() {
  RtcTemperature dieTemp = Rtc.GetTemperature();
  float dieTempF = dieTemp.AsFloat()*(9/5)+32;
  
  String message = "<html>\n<head>\n\t<title>Temperature</title>\n</head>\n<body>\n";
  message += "<h1>Die temperature ";
  message += dieTempF;
  message += "&deg;F</h1>\n";
  message += "<form action=\"/\" method=\"post\"><input type=\"submit\" value=\"Home\"/></form>\n";
  message += "</body>\n</html>\n";
  
  server.send(200, "text/html", message);
}

void handleTime() {
  String message = "<html>\n<head>\n\t<title>Manage Time Settings</title>\n</head>\n<body>\n";
  message += "<h1>Time</h1>\n";
  if (server.method() == HTTP_POST) {
    for (uint8_t i = 0; i < server.args(); i++) {
      if (server.argName(i) == "writeNTP") {
        syncNTPTime();
        writeNtpTimeToRtc();
        message += "<h2>Wrote NTP time to RTC</h2>\n";
      }
    }
  }

  String rtcDateTimeStr;
  String ntpDateTimeStr;
  getRtcDateTimeString(rtcDateTimeStr);
  getNtpDateTimeString(ntpDateTimeStr);

  message += "RTC date and time ";
  message += rtcDateTimeStr;
  message += "<br/>\n";
  message += "NTP date and time ";
  message += ntpDateTimeStr;
  message += "<br/>\n";
  message += "<form action=\"/time\" method=\"post\">\n";
  message += "<input type=\"submit\" value=\"Sync with NTP and write to RTC\"/>\n";
  message += "<input type=\"hidden\" name=\"writeNTP\" value=\"true\"/>\n";
  message += "</form>\n";
  message += "<form action=\"/\" method=\"post\"><input type=\"submit\" value=\"Home\"/></form>\n";
  message += "</body>\n</html>\n";

  server.send(200, "text/html", message);
}

void handleConfig() {
  String message = "<html>\n<head>\n\t<title>Configure</title>\n</head>\n<body>\n";
  message += "<h1>Configuration</h1>";

  if (server.method() == HTTP_POST) {
    // User selected save
    for (uint8_t i = 0; i < server.args(); i++) {
      if (server.argName(i) == "DeviceDescription") {
        Serial.print("Setting Device Description ");
        Serial.println(server.arg(i));
        config.deviceDescription = server.arg(i);
      } else if (server.argName(i) == "WiFiSSID") {
        Serial.print("Setting WiFi SSID ");
        Serial.println(server.arg(i));
        config.wiFiSSID = server.arg(i);
      } else if (server.argName(i) == "WiFiPassword") {
        Serial.print("Setting WiFi password ");
        Serial.println(server.arg(i));
        config.wiFiPassword = server.arg(i);
      } else if (server.argName(i) == "WiFiMode") {
        // Either Station or AP
        Serial.print("Setting WiFi mode ");
        Serial.println(server.arg(i));
        server.arg(i).toUpperCase();
        if (server.arg(i) == "AP") {
          config.wiFiMode = WIFI_AP;
        } else if (server.arg(i) == "STATION") {
          config.wiFiMode = WIFI_STA;
        } else {
          Serial.print("Unknown WiFi mode ");
          Serial.print(server.arg(i));
          Serial.println("; defaulting to STATION");
          config.wiFiMode = WIFI_STA;
        }
      } else if (server.argName(i) == "ConnectWiFiAtReset") {
        // true or false
        Serial.print("Setting connect WiFi at reset flag to ");
        Serial.println(server.arg(i));
        server.arg(i).toUpperCase();
        if (server.arg(i) == "TRUE") {
          config.connectWiFiAtReset = true;
        } else if (server.arg(i) == "FALSE") {
          config.connectWiFiAtReset = false;
        } else {
          Serial.print("Unknown boolean value ");
          Serial.print(server.arg(i));
          Serial.println("; defaulting to TRUE");
          config.connectWiFiAtReset = true;
        }
      } else if (server.argName(i) == "WakeEveryN") {
        Serial.print("Setting wake every N to ");
        Serial.println(server.arg(i));
        config.wakeEveryN = server.arg(i).toInt();
      } else if (server.argName(i) == "StayAwakeMins") {
        Serial.print("Setting stay awake time to ");
        Serial.println(server.arg(i));
        config.stayAwakeMins = server.arg(i).toInt();
      } else if (server.argName(i) == "ChimeEveryN") {
        Serial.print("Setting chime every N to ");
        Serial.println(server.arg(i));
        config.chimeEveryN = server.arg(i).toInt();
      } else if (server.argName(i) == "ChimeOffset") {
        Serial.print("Setting chime offet to ");
        Serial.println(server.arg(i));
        config.chimeOffset = server.arg(i).toInt();
      } else {
        Serial.print("Unknown configuration key ");
        Serial.println(server.argName(i));
      }
    }

    // Write updated configuration to SPIFFS
    // But don't bother if this was just an empty POST
    if (server.args() > 0) {
      writeConfig();
      message += "<h4>Configuration updated.</h4>\n";
    }
  }

  message += "<form action=\"/config\" method=\"post\">\n";
  message += "<label>Device description <input type=\"text\" name=\"DeviceDescription\" value=\"";
  message += config.deviceDescription;
  message += "\"/></label><br/>\n";
  message += "<label>WiFi SSID <input type=\"text\" name=\"WiFiSSID\" value=\"";
  message += config.wiFiSSID;
  message += "\"/></label><br/>\n";
  message += "<label>WiFi Password <input type=\"text\" name=\"WiFiPassword\" value=\"";
  message += config.wiFiPassword;
  message += "\"/></label><br/>\n";
  message += "<label>WiFi Mode <select name=\"WiFiMode\">\n";
  message += "<option value=\"AP\" ";
  if (config.wiFiMode == WIFI_AP) {
    message += "selected";
  }
  message += ">Access Point</option>\n";
  message += "<option value=\"STATION\" ";
  if (config.wiFiMode == WIFI_STA) {
    message += "selected";
  }
  message += ">Station</option>\n";
  message += "</select></label><br/>\n";
  message += "<label>Connect to WiFi at reset <input type=\"checkbox\" name=\"ConnectWiFiAtReset\" ";
  if (config.connectWiFiAtReset) {
    message += "checked";
  }
  message += "/></label><br/>\n";
  message += "<label>Wake every <input type=\"text\" name=\"WakeEveryN\" value=\"";
  message += config.wakeEveryN;
  message += "\"/> minutes</label><br/>\n";
  message += "<label>Stay awake for <input type=\"text\" name=\"WakeEveryN\" value=\"";
  message += config.stayAwakeMins;
  message += "\"/> minutes</label><br/>\n";
  message += "<label>Chime every <input type=\"text\" name=\"ChimeEveryN\" value=\"";
  message += config.chimeEveryN;
  message += "\"/> minutes</label><br/>\n";
  message += "<label>Chime offset <input type=\"text\" name=\"ChimeOffset\" value=\"";
  message += config.chimeOffset;
  message += "\"/> seconds</label><br/>\n";
  message += "<input type=\"submit\" value=\"Save\"/>\n";
  message += "</form>\n";
  message += "<form action=\"/\" method=\"post\"><input type=\"submit\" value=\"Home\"/></form>\n";
  message += "</body>\n</html>\n";
  
  server.send(200, "text/html", message);
}

void handleNotFound(){
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i=0; i<server.args(); i++){
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

// Setup functions
void setupSPIFFS() {
  Serial.println("Starting SPIFFS");
  if (SPIFFS.begin()) {
    Serial.println("SPIFFS initialized");
  } else {
    Serial.println("SPIFFS initialization failed!");
    return;
  }
  Serial.flush();
}

// Debug function, not used for now
void dumpSPIFFSInfo() {
  FSInfo fs_info;
  SPIFFS.info(fs_info);

  Serial.println("Filesystem info");
  Serial.print("Total bytes: ");
  Serial.println(fs_info.totalBytes);
  Serial.print("Used bytes: ");
  Serial.println(fs_info.usedBytes);
  Serial.print("Block size: ");
  Serial.println(fs_info.blockSize);
  Serial.print("Page size: ");
  Serial.println(fs_info.pageSize);
  Serial.print("Max open files: ");
  Serial.println(fs_info.maxOpenFiles);
  Serial.print("Max path length: ");
  Serial.println(fs_info.maxPathLength);
  Serial.println("");
  Serial.flush();

  Serial.println("Directory listing of /");
  Dir dir = SPIFFS.openDir("/");
  while (dir.next()) {
    Serial.print("  ");
    Serial.println(dir.fileName());
  }
  Serial.flush();
}

void readConfig() {
  Serial.println("Opening config file");
  File f = SPIFFS.open("/bmchimes.cfg", "r");
  if (!f) {
      Serial.println("readConfig: file open failed");
      return;
  }
  Serial.flush();
  Serial.println("Beginning read of config file");
  Serial.flush();
  int loops = 0;
  while (loops < 50 && f.available()) {
    yield();
    Serial.flush();
    String key = f.readStringUntil('=');
    key.trim();
#ifdef DEBUG
    Serial.print("Read key ");
    Serial.print(key);
    Serial.print("; hex: ");
    printStringAsHexDump(key);
    Serial.println("");
#endif

    String value = f.readStringUntil('\n');
    value.trim();
#ifdef DEBUG
    Serial.print("Read value ");
    Serial.print(value);
    Serial.print("; hex: ");
    printStringAsHexDump(value);
    Serial.println("");
#endif

    Serial.flush();
    if (key == "DeviceDescription") {
      Serial.print("Setting Device Description ");
      Serial.println(value);
      config.deviceDescription = value;
    } else if (key == "WiFiSSID") {
      Serial.print("Setting WiFi SSID ");
      Serial.println(value);
      config.wiFiSSID = value;
    } else if (key == "WiFiPassword") {
      Serial.print("Setting WiFi password ");
      Serial.println(value);
      config.wiFiPassword = value;
    } else if (key == "WiFiMode") {
      // Either Station or AP
      Serial.print("Setting WiFi mode ");
      Serial.println(value);
      value.toUpperCase();
      if (value == "AP") {
        config.wiFiMode = WIFI_AP;
      } else if (value == "STATION") {
        config.wiFiMode = WIFI_STA;
      } else {
        Serial.print("Unknown WiFi mode ");
        Serial.print(value);
        Serial.println("; defaulting to STATION");
        config.wiFiMode = WIFI_STA;
      }
    } else if (key == "ConnectWiFiAtReset") {
      // true or false
      Serial.print("Setting connect WiFi at reset flag to ");
      Serial.println(value);
      value.toUpperCase();
      if (value == "TRUE") {
        config.connectWiFiAtReset = true;
      } else if (value == "FALSE") {
        config.connectWiFiAtReset = false;
      } else {
        Serial.print("Unknown boolean value ");
        Serial.print(value);
        Serial.println("; defaulting to TRUE");
        config.connectWiFiAtReset = true;
      }
    } else if (key == "WakeEveryN") {
      Serial.print("Setting wake every N to ");
      Serial.println(value);
      config.wakeEveryN = value.toInt();
    } else if (key == "StayAwakeMins") {
      Serial.print("Setting stay awake time to ");
      Serial.println(value);
      config.stayAwakeMins = value.toInt();
    } else if (key == "ChimeEveryN") {
      Serial.print("Setting chime every N to ");
      Serial.println(value);
      config.chimeEveryN = value.toInt();
    } else if (key == "ChimeOffset") {
      Serial.print("Setting chime offset to ");
      Serial.println(value);
      config.chimeOffset = value.toInt();
    } else {
      Serial.print("Unknown configuration key ");
      Serial.println(key);
    }
    loops++;
  }
  Serial.println("Closing config file");
  f.close();
  Serial.println("Config read complete");
}

void writeConfig() {
  Serial.println("writeConfig: Opening /bmchimes.cfg for writing");
  File f = SPIFFS.open("/bmchimes.cfg", "w");
  if (!f) {
      Serial.println("writeConfig: file open failed");
      return;
  }
  Serial.println("File successfully opened");

  config.deviceDescription.trim();
  Serial.print("Writing device description: ");
  Serial.print(config.deviceDescription);
  Serial.print("; hex: ");
  printStringAsHexDump(config.deviceDescription);
  Serial.println("");
  f.print("DeviceDescription=");
  f.println(config.deviceDescription);

  config.wiFiSSID.trim();
  Serial.print("Writing WiFiSSID: ");
  Serial.print(config.wiFiSSID);
  Serial.print("; hex: ");
  printStringAsHexDump(config.wiFiSSID);
  Serial.println("");
  f.print("WiFiSSID=");
  f.println(config.wiFiSSID);

  config.wiFiPassword.trim();
  Serial.print("Writing WiFiPassword: ");
  Serial.print(config.wiFiPassword);
  Serial.print("; hex: ");
  printStringAsHexDump(config.wiFiPassword);
  Serial.println("");
  f.print("WiFiPassword=");
  f.println(config.wiFiPassword);

  f.print("WiFiMode=");
  if (config.wiFiMode == WIFI_AP) {
    f.println("AP");
  } else if (config.wiFiMode == WIFI_STA) {
    f.println("STATION");
  } else {
    f.println("UNKNOWN");
  }
  f.print("ConnectWiFiAtReset=");
  if (config.connectWiFiAtReset == true) {
    f.println("TRUE");
  } else {
    f.println("FALSE");
  }
  f.print("WakeEveryN=");
  f.println(config.wakeEveryN);
  f.print("StayAwakeMins=");
  f.println(config.stayAwakeMins);
  f.print("ChimeEveryN=");
  f.println(config.chimeEveryN);
  f.print("ChimeOffset=");
  f.println(config.chimeOffset);
  f.close();
}

void syncNTPTime() {
  Serial.println("Fetching time from NTP servers");
  // Pacific time zone hard coded
  configTime(-(8 * 3600), -3600, "pool.ntp.org", "time.nist.gov");
  // Wait up to a minute for NTP sync
  uint8_t attempts = 0;
  while (attempts <= 120 && !time(nullptr)) {
    Serial.print(".");
    delay(500);
    attempts++;
  }
  Serial.println("");
  if (!time(nullptr)) {
    Serial.println("Failed to sync time with NTP servers!");
  } else {
    Serial.println("Successfully synced time with NTP servers.");
  }
}

void connectToWiFi() {
  config.wiFiSSID.toCharArray(ssid, 64);
  config.wiFiPassword.toCharArray(password, 64);
  WiFi.begin(ssid, password);
  Serial.println("");

  // Wait up to 1 minute for connection
  uint8_t attempts = 0; 
  while (attempts <= 120 && WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.print("Connected to ");
    Serial.println(config.wiFiSSID);
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    if (MDNS.begin("esp8266")) {
      Serial.println("MDNS responder started");
    }
  } else {
    Serial.println("");
    Serial.print("Unable to connect to ");
    Serial.print(config.wiFiSSID);
    Serial.println(" network.  Giving up.");
  }
}

void writeNtpTimeToRtc() {
  Serial.println("Setting RTC time from NTP time");
  struct tm *tmp;
  time_t now = time(nullptr);
  tmp = localtime(&now);

  RtcDateTime ntpDateTime = RtcDateTime(
    tmp->tm_year + 1900,
    tmp->tm_mon + 1,
    tmp->tm_mday,
    tmp->tm_hour,
    tmp->tm_min,
    tmp->tm_sec
  );

  Rtc.SetDateTime(ntpDateTime);
}

void rtcSetup() {
  Rtc.Begin();
  if (!Rtc.GetIsRunning()) {
    Serial.println("RTC was not actively running, starting now");
    Rtc.SetIsRunning(true);
  }
  Rtc.Enable32kHzPin(false);
  Rtc.SetSquareWavePin(DS3231SquareWavePin_ModeAlarmBoth); 
}

void setRtcSleepAlarm() {
  uint16_t secondsToStayAwake = config.stayAwakeMins * 60;
  uint16_t secondsTilNextWake = secondsTilNextN(config.wakeEveryN);
  uint16_t secondsTilNextChime = secondsTilNextN(config.chimeEveryN);
  RtcDateTime now = Rtc.GetDateTime();
  time_t nowTime = now.Epoch32Time();
  time_t wakeTime = nowTime + secondsTilNextWake;
  time_t chimeTime = nowTime + secondsTilNextChime;
  time_t nextSleepTime = wakeTime + secondsToStayAwake;
  uint16_t secondsTilWakeAfterNext = secondsTilNextWake + (config.wakeEveryN * 60);
  time_t wakeTimeAfterNext = nowTime + secondsTilWakeAfterNext;
  time_t sleepTimeAfterNext = wakeTimeAfterNext + secondsToStayAwake;
  // sleepDuration is global
  sleepDuration = wakeTimeAfterNext - nextSleepTime;
  RtcDateTime wakeAlarmDateTime = RtcDateTime(wakeTime);
  RtcDateTime sleepAlarmDateTime = RtcDateTime(nextSleepTime);
  RtcDateTime chimeAlarmDateTime = RtcDateTime(chimeTime);

  Serial.println("Setting RTC sleep alarm");
  
  String nowString;
  getRtcDateTimeString(nowString);
  Serial.print("Time is now ");
  Serial.println(nowString);
  Serial.print("secondsToStayAwake = ");
  Serial.println(secondsToStayAwake);
  Serial.print("sleepDuration = ");
  Serial.println(sleepDuration);
  
  String sleepAlarmDateTimeString;
  String chimeAlarmDateTimeString;
  dateTimeStringFromRtcDateTime(sleepAlarmDateTime, sleepAlarmDateTimeString);
  dateTimeStringFromRtcDateTime(chimeAlarmDateTime, chimeAlarmDateTimeString);

  Serial.print("Chime scheduled for ");
  Serial.println(chimeAlarmDateTimeString);
  Serial.flush();

  // Determine if the next chime will happen between the next sleep and the next wake
  if (chimeAlarmDateTime >= sleepAlarmDateTime && chimeAlarmDateTime <= wakeAlarmDateTime) {
    // Chime will happen while we are asleep!  Skip the next sleep.
    sleepAlarmDateTime = RtcDateTime(sleepTimeAfterNext);
    Serial.println("Chime scheduled to occur during sleep, rescheduling sleep to avoid conflict");
    Serial.print("Sleep was scheduled for ");
    Serial.println(sleepAlarmDateTimeString);
    dateTimeStringFromRtcDateTime(sleepAlarmDateTime, sleepAlarmDateTimeString);
    
  }

  Serial.print("Sleep now scheduled for ");
  Serial.println(sleepAlarmDateTimeString);
  Serial.flush();

  DS3231AlarmOne alarmOne = DS3231AlarmOne(
    0, // dayOf; irrelevant with MinutesSecondsMatch
    0, // hour; irrelevant with MinutesSecondsMatch
    sleepAlarmDateTime.Minute(),
    sleepAlarmDateTime.Second(),
    DS3231AlarmOneControl_MinutesSecondsMatch
  );
  
  Rtc.SetAlarmOne(alarmOne);

  DS3231AlarmTwo alarmTwo = DS3231AlarmTwo(
    0, // dayOf; irrelevant with HoursMinutesMatch
    chimeAlarmDateTime.Hour(),
    chimeAlarmDateTime.Minute(),
    DS3231AlarmTwoControl_HoursMinutesMatch
  );
  
  Rtc.SetAlarmTwo(alarmTwo);
  Rtc.LatchAlarmsTriggeredFlags();
  attachInterrupt(digitalPinToInterrupt(RTC_ALARM_PIN), alarmISR, FALLING);
}

void setRtcWakeupEveryMinuteAlarm() {
  DS3231AlarmTwo alarm = DS3231AlarmTwo(
    0, // dayOf; irrelevant with MinutesSecondsMatch
    0, // hour; irrelevant with MinutesSecondsMatch
    0,
    DS3231AlarmTwoControl_OncePerMinute
  );
  
  Rtc.SetAlarmTwo(alarm);
  Rtc.LatchAlarmsTriggeredFlags();
  attachInterrupt(digitalPinToInterrupt(RTC_ALARM_PIN), alarmISR, FALLING);
}

void clearRtcAlarms() {
  DS3231AlarmOne alarmOne = DS3231AlarmOne(0, 0, 0, 61,
    DS3231AlarmOneControl_HoursMinutesSecondsDayOfMonthMatch
  );
  Rtc.SetAlarmOne(alarmOne);

  DS3231AlarmTwo alarmTwo = DS3231AlarmTwo(32, 0, 0,
    DS3231AlarmTwoControl_HoursMinutesDayOfMonthMatch
  );
  Rtc.SetAlarmTwo(alarmTwo);
  Rtc.LatchAlarmsTriggeredFlags();
}

void basicSetup() {
  pinMode(HEARTBEAT_PIN, OUTPUT);
  pinMode(CHIME_PIN, OUTPUT);
  pinMode(RTC_ALARM_PIN, INPUT);
  digitalWrite(HEARTBEAT_PIN, LOW);
  digitalWrite(CHIME_PIN, LOW);
  Serial.begin(74880);
  Serial.println("");
  Wire.begin();
}

void startWebServer() {
  // Set up handlers for different pages
  server.on("/", handleRoot);
  server.on("/reset", handleReset);
  server.on("/sleep", handleSleep);
  server.on("/time", handleTime);
  server.on("/temp", handleTemp);
  server.on("/config", handleConfig);
  server.onNotFound(handleNotFound);

  /***
   *** Different approach to handler functions
  server.on("/inline", [](){
    server.send(200, "text/plain", "this works as well");
  });
  ***/

  // Start the web server
  server.begin();
  Serial.println("HTTP server started");
}

void setup(void) {
  basicSetup();
  setupSPIFFS();
  readConfig();
  if (config.connectWiFiAtReset) {
    connectToWiFi();
  }

  rtcSetup();
  clearRtcAlarms();
  setRtcSleepAlarm();
  // Used for testing.
  //setRtcWakeupEveryMinuteAlarm();
  startWebServer();
}

void loop(void) {
  String nowString;
  DS3231AlarmFlag flag;

  server.handleClient();
  if (alarmFired(flag)) {
    getRtcDateTimeString(nowString);
    Serial.print("Time is now ");
    Serial.println(nowString);
    Serial.print("Alarm ");
    if (flag & DS3231AlarmFlag_Alarm1) {
      Serial.println("one fired");
      Serial.print("Sleeping for ");
      Serial.print(sleepDuration);
      Serial.println(" seconds");
      // deepSleep expects a number in microseconds
      ESP.deepSleep(sleepDuration * 1e6);
    }

    if (flag & DS3231AlarmFlag_Alarm2) {
      Serial.println("two fired");
      // Time to Chime!
      Serial.println("Would chime now!");
    }
  }
}
