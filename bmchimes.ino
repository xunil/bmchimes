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

#define RTC_ALARM_PIN 15

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
  dateTimeBuf = rtcDateTime.Year();
  dateTimeBuf += "/";
  dateTimeBuf += rtcDateTime.Month();
  dateTimeBuf += "/";
  dateTimeBuf += rtcDateTime.Day();
  dateTimeBuf += " ";
  dateTimeBuf += rtcDateTime.Hour();
  dateTimeBuf += ":";
  dateTimeBuf += rtcDateTime.Minute();
  dateTimeBuf += ":";
  dateTimeBuf += rtcDateTime.Second();
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

uint16_t secondsTilNextChime() {
  return secondsTilNextN(config.chimeEveryN);
}

// XXX: Accurate?
uint16_t secondsTilNextSleep() {
  return secondsTilNextN(config.wakeEveryN);
}

void enterDeepSleep(uint16_t secondsToSleep) {
  Serial.print("Entering deep sleep for ");
  Serial.print(secondsToSleep);
  Serial.println(" seconds...");
  Serial.flush();
  // deepSleep() expects microseconds
  ESP.deepSleep(secondsToSleep * 1e6);
}

bool shouldSleep() {
  uint16_t secondsTilSleep = secondsTilNextSleep();
  uint16_t secondsTilChime = secondsTilNextChime();
  if (secondsTilSleep <= 1 && secondsTilSleep < secondsTilChime) {
    return true;
  }
  return false;
}

bool shouldChime() {
  if (secondsTilNextChime() <= 1) {
    return true;
  }
  return false;
}

bool alarmFired() {
  bool wasAlarmed = false;
  if (interruptFlag) { // check our flag that gets sets in the interrupt
    wasAlarmed = true;
    interruptFlag = false; // reset the flag

    // this gives us which alarms triggered and
    // then allows for others to trigger again
    DS3231AlarmFlag flag = Rtc.LatchAlarmsTriggeredFlags();

    if (flag & DS3231AlarmFlag_Alarm1) {
      Serial.println("alarm one triggered");
    }
    if (flag & DS3231AlarmFlag_Alarm2) {
      Serial.println("alarm two triggered");
    }
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
    Serial.print("Read key ");
    Serial.print(key);
    Serial.print("; hex: ");
    printStringAsHexDump(key);
    Serial.println("");

    String value = f.readStringUntil('\n');
    value.trim();
    Serial.print("Read value ");
    Serial.print(value);
    Serial.print("; hex: ");
    printStringAsHexDump(value);
    Serial.println("");

    Serial.flush();
    if (key == "DeviceDescription") {
      Serial.print("Setting Device Description");
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

void setRtcWakeupAlarm() {
  uint16_t secondsToStayAwake = config.stayAwakeMins * 60;
  uint16_t secondsUntilWake = secondsTilNextSleep();
  uint16_t secondsToStayAsleep;

  Serial.println("Setting RTC wakeup alarm");
  
  if (secondsUntilWake <= secondsToStayAwake) {
    secondsUntilWake += secondsToStayAwake; 
    secondsToStayAwake *= 2;
  }

  secondsToStayAsleep = secondsUntilWake - secondsToStayAwake;
  
  RtcDateTime now = Rtc.GetDateTime();

  String nowString;
  getRtcDateTimeString(nowString);
  Serial.print("Time is now ");
  Serial.println(nowString);
  Serial.print("secondsToStayAwake = ");
  Serial.println(secondsToStayAwake);
  Serial.print("secondsToStayAsleep = ");
  Serial.println(secondsToStayAsleep);
  Serial.print("secondsUntilWake = ");
  Serial.println(secondsUntilWake);

  uint16_t alarmSecond = now.Second() + ((secondsToStayAwake + secondsToStayAsleep) % 60);
  uint16_t alarmMinute = now.Minute() + ((secondsToStayAwake + secondsToStayAsleep) / 60);

  Serial.print("alarmSecond = ");
  Serial.print(now.Second());
  Serial.print(" + ((");
  Serial.print(secondsToStayAwake);
  Serial.print(" + ");
  Serial.print(secondsToStayAsleep);
  Serial.print(") % 60) == ");
  Serial.println(alarmSecond);

  Serial.print("alarmMinute = ");
  Serial.print(now.Minute());
  Serial.print(" + ((");
  Serial.print(secondsToStayAwake);
  Serial.print(" + ");
  Serial.print(secondsToStayAsleep);
  Serial.print(") % 60) == ");
  Serial.println(alarmMinute);

  if (alarmSecond >= 60) {
    // Seconds overflowed, add to minute
    alarmMinute += (alarmSecond / 60);
    Serial.print("Alarm second overflowed, now ");
    Serial.print(alarmSecond);
    Serial.print("; alarm minute now ");
    Serial.println(alarmMinute);
  }
  if (alarmMinute >= 60) {
    // Minutes overflowed, but it doesn't matter since alarm fires
    // based only on the minutes and seconds
    alarmMinute %= 60;
    Serial.print("Alarm minute overflowed, now ");
    Serial.println(alarmMinute);
  }
  Serial.flush();

  DS3231AlarmOne alarm = DS3231AlarmOne(
    0, // dayOf; irrelevant with MinutesSecondsMatch
    0, // hour; irrelevant with MinutesSecondsMatch
    alarmMinute,
    alarmSecond,
    DS3231AlarmOneControl_MinutesSecondsMatch
  );
  
  Rtc.SetAlarmOne(alarm);
  Rtc.LatchAlarmsTriggeredFlags();
  attachInterrupt(digitalPinToInterrupt(RTC_ALARM_PIN), alarmISR, FALLING);
}

void basicSetup() {
  pinMode(led, OUTPUT);
  digitalWrite(led, LOW);
  Serial.begin(115200);
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
  startWebServer();
}

void loop(void) {
  server.handleClient();
  if (alarmFired()) {
    Serial.println("Alarm fired...");
  }
  /*
  if (shouldSleep()) {
    Serial.println("Should go to sleep now...");
  }
  if (shouldChime()) {
    Serial.println("Should chime now...");
  }
  */
}
