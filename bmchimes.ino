#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <time.h>
#include "FS.h"
#include <RtcDS3231.h>
#include <pgmspace.h>
#include "TeeSerial.h"

const int heartbeatPin = 2;
const int rtcAlarmPin = 13;
const int chimePin = 12;
const int chimeStopSwitchPin = 14;
const int statsInterval = 1;
const int statsLinesPerDay = 24 * (60 / statsInterval);

struct BMChimeConfig {
  String deviceDescription;
  String wiFiSSID;
  String wiFiPassword;
  // From ESP8266WiFi.h
  WiFiMode wiFiMode;
  bool connectWiFiAtReset;
  bool sleepEnabled;
  uint16_t wakeEveryNSeconds;
  uint16_t stayAwakeSeconds;
  uint16_t chimeEveryNSeconds; 
  uint16_t chimeOffsetSeconds; 
  uint16_t interChimeDelaySeconds; 
  uint16_t interHourChimeDelaySeconds; 
  uint16_t chimeStopTimeout; 
  bool heartbeatEnabled;
} config;

char ssid[64];
char password[64];
const int led = 2;

RtcDS3231 Rtc;
ESP8266WebServer server(80);

uint16_t sleepDuration = 0;
RtcDateTime wakeAlarmDateTime;
RtcDateTime sleepAlarmDateTime;
RtcDateTime chimeAlarmDateTime;
uint32_t chimeStartMillis = 0;
uint32_t chimeStopMillis = 0;
uint16_t lastChimeDurationMillis = 0;

bool chiming = false;
enum chime_state_t { CHIME_INITIAL = 0, CHIME_SECOND = 1, CHIME_THIRD = 2, CHIME_HOUR = 3 } chimeState = CHIME_INITIAL;
uint8_t chimeHoursRungOut = 0;
#define NUM_CHIME_STATES 4
#define MAX_CHIME_SCHEDULE 15
#define INVALID_TIME 0xffffffff
time_t chimeSchedule[MAX_CHIME_SCHEDULE];

bool shouldCollectStats = false;
volatile bool chimeStopSwitchFlag = false;
volatile bool alarmInterruptFlag = false;

const int heartbeatBlipDuration = 50; // In milliseconds
const int heartbeatBlipInterval = 5 * 1000; // In milliseconds
uint32_t millisAtHeartbeatReset;
uint32_t millisAtLastHeartbeatBlip;
uint8_t heartbeatBlipCount = 0;
int heartbeatPinState = LOW;
uint8_t heartbeatBlipMax = 2 * 2; // 2 blinks - on, off, on, off, hence the multiply by 2

// Utility functions
void alarmISR() {
  alarmInterruptFlag = true;
}

void chimeStopSwitchISR() {
  chimeStopSwitchFlag = true;
}

void printStringAsHexDump(String str) {
  char buf[64];
  str.toCharArray(buf, 64);
  for (int i = 0; i < str.length(); i++) {
    TeeSerial0.print(buf[i], HEX);
    TeeSerial0.print(" ");
  }
}

void printlnStringAsHexDump(String str) {
  printStringAsHexDump(str);
  TeeSerial0.println("");
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

uint16_t secondsTilNextN(uint16_t N) {
  // 16 bit return value should be sufficient; we will chime or wake at least once an hour.
  RtcDateTime now = Rtc.GetDateTime();

  uint16_t seconds = ((N - (now.Minute() % N)) - 1) * 60;
  seconds += 60 - now.Second();

  return seconds;
}

bool alarmFired(DS3231AlarmFlag& flag) {
  bool wasAlarmed = false;
  if (alarmInterruptFlag) { // check our flag that gets sets in the interrupt
    wasAlarmed = true;
    alarmInterruptFlag = false; // reset the flag

    // this gives us which alarms triggered and
    // then allows for others to trigger again
    flag = Rtc.LatchAlarmsTriggeredFlags();
  }
  return wasAlarmed;
}

void chimeMotorOn() {
  digitalWrite(chimePin, HIGH);
}

void chimeMotorOff() {
  digitalWrite(chimePin, LOW);
}

void startChiming() {
  TeeSerial0.print("Chime state is ");
  switch (chimeState) {
    case CHIME_INITIAL:
      TeeSerial0.println("INITIAL");
      break;
    case CHIME_SECOND:
      TeeSerial0.println("SECOND");
      break;
    case CHIME_THIRD:
      TeeSerial0.println("THIRD");
      break;
    case CHIME_HOUR:
      TeeSerial0.print("HOUR (");
      TeeSerial0.print(chimeHoursRungOut);
      TeeSerial0.println(" hours rung out)");
      break;
    default:
      TeeSerial0.println("UNKNOWN");
  }

  TeeSerial0.println("Chiming!");
  // Set a flag indicating chiming has begun
  chiming = true;
  chimeStopSwitchFlag = false;
  // Store time chiming began
  chimeStartMillis = millis();
  // Trigger chime GPIO
  chimeMotorOn();
}

void stopChiming() {
  chimeStopMillis = millis();
  chimeMotorOff();
  chiming = false;
  lastChimeDurationMillis = chimeStopMillis - chimeStartMillis;
}

void heartbeatReset() {
  millisAtHeartbeatReset = millis();
  millisAtLastHeartbeatBlip = 0;
  heartbeatBlipCount = 0;
  heartbeatPinState = LOW;
  digitalWrite(heartbeatPin, heartbeatPinState);
}

void heartbeat() {
  // There's scratches all around the coin slot
  // Like a heartbeat, baby, trying to wake up
  // But this machine can only swallow money
  // You can't lay a patch by computer design
  // It's just a lot of stupid, stupid signs
  if (heartbeatBlipCount < heartbeatBlipMax) {
    uint32_t millisNow = millis();
    uint32_t millisDelta = millisNow - millisAtHeartbeatReset;
    if (millisDelta % heartbeatBlipDuration == 0 && millisAtLastHeartbeatBlip != millisNow) {
      millisAtLastHeartbeatBlip = millisNow;
      if (heartbeatPinState == LOW) {
        heartbeatPinState = HIGH;
      } else {
        heartbeatPinState = LOW;
      }
      digitalWrite(heartbeatPin, heartbeatPinState);
      heartbeatBlipCount++;
    }
  }
}


// Web server handler functions

void handleRoot() {
  RtcTemperature dieTemp = Rtc.GetTemperature();
  float dieTempF = (dieTemp.AsFloat()*(9.0/5.0))+32.0;

  String rtcDateTimeStr;
  getRtcDateTimeString(rtcDateTimeStr);
  String sleepAlarmDateTimeString;
  String chimeAlarmDateTimeString;
  dateTimeStringFromRtcDateTime(sleepAlarmDateTime, sleepAlarmDateTimeString);
  dateTimeStringFromRtcDateTime(chimeAlarmDateTime, chimeAlarmDateTimeString);

  String message = "<html>\n<head>\n\t<title>Chimes Controller</title>\n</head>\n<body>\n";
  message += "<h1>";
  message += config.deviceDescription;
  message += "</h1>\n";
  message += "<h2>Status</h2>\n";
  message += "RTC date and time ";
  message += rtcDateTimeStr;
  message += "<br/>\n";
  message += "RTC die temperature ";
  message += dieTempF;
  message += "&deg;F<br/>\n";
  if (config.sleepEnabled) {
    message += "Next sleep scheduled at ";
    message += sleepAlarmDateTimeString;
    message += "<br/>\n";
  } else {
    message += "Sleep disabled, no sleep scheduled<br/>\n";
  }
  message += "Next chime scheduled at ";
  message += chimeAlarmDateTimeString;
  message += "<br/>\n";
  if (WiFi.status() == WL_CONNECTED) {
    message += "Connected to WiFi network ";
    message += WiFi.SSID();
    message += "<br/>\n";
  }

  String debugLog;
  TeeSerial0.getBuffer(debugLog);
  message += "<h4>Debug Log</h4>\n";
  message += "<pre>\n";
  message += debugLog;
  message += "</pre>\n";

  message += "<form action=\"/config\" method=\"get\"><input type=\"submit\" value=\"Configure\"/></form>\n";
  message += "<form action=\"/time\" method=\"get\"><input type=\"submit\" value=\"Manage Time\"/></form>\n";
  message += "<form action=\"/stats\" method=\"get\"><input type=\"submit\" value=\"Statistics\"/></form>\n";
  message += "<form action=\"/sleep\" method=\"get\"><input type=\"submit\" value=\"Sleep Now\"/></form>\n";
  message += "<form action=\"/chimenow\" method=\"get\"><input type=\"submit\" value=\"Chime Now\"/></form>\n";
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
  float dieTempF = (dieTemp.AsFloat()*(9.0/5.0))+32.0;
  
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
        // Schedule next chime
        calculateSleepAndChimeTiming();
        setRtcAlarms();
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
    // Checkbox form elements are not passed in the POST unless they are checked;
    // therefore, assume they are unchecked and let the for loop below correct this
    config.sleepEnabled = false;
    config.connectWiFiAtReset = false;
    config.heartbeatEnabled = false;

    for (uint8_t i = 0; i < server.args(); i++) {
      String argName = String(server.argName(i));
      String argValue = String(server.arg(i));
      TeeSerial0.print("handleConfig(): argName = ");
      TeeSerial0.print(argName);
      TeeSerial0.print("; argValue = ");
      TeeSerial0.println(argValue);

      if (argName == "DeviceDescription") {
        TeeSerial0.print("Setting Device Description ");
        TeeSerial0.println(argValue);
        config.deviceDescription = argValue;
      } else if (argName == "WiFiSSID") {
        TeeSerial0.print("Setting WiFi SSID ");
        TeeSerial0.println(argValue);
        config.wiFiSSID = argValue;
      } else if (argName == "WiFiPassword") {
        TeeSerial0.print("Setting WiFi password ");
        TeeSerial0.println(argValue);
        config.wiFiPassword = argValue;
      } else if (argName == "WiFiMode") {
        // Either Station or AP
        TeeSerial0.print("Setting WiFi mode ");
        TeeSerial0.println(argValue);
        argValue.toUpperCase();
        if (argValue == "AP") {
          config.wiFiMode = WIFI_AP;
        } else if (argValue == "STATION") {
          config.wiFiMode = WIFI_STA;
        } else {
          TeeSerial0.print("Unknown WiFi mode ");
          TeeSerial0.print(argValue);
          TeeSerial0.println("; defaulting to STATION");
          config.wiFiMode = WIFI_STA;
        }
      } else if (argName == "ConnectWiFiAtReset") {
        // true or false
        TeeSerial0.print("Setting connect WiFi at reset flag to ");
        TeeSerial0.println(argValue);
        argValue.toUpperCase();
        if (argValue == "TRUE" || argValue == "ON") {
          config.connectWiFiAtReset = true;
        } else if (argValue == "FALSE" || argValue == "OFF") {
          config.connectWiFiAtReset = false;
        } else {
          TeeSerial0.print("Unknown boolean value ");
          TeeSerial0.print(argValue);
          TeeSerial0.println("; defaulting to TRUE");
          config.connectWiFiAtReset = true;
        }
      } else if (argName == "SleepEnabled") {
        // true or false
        TeeSerial0.print("Setting sleep enabled flag to ");
        TeeSerial0.println(argValue);
        argValue.toUpperCase();
        if (argValue == "TRUE" || argValue == "ON") {
          config.sleepEnabled = true;
        } else if (argValue == "FALSE" || argValue == "OFF") {
          config.sleepEnabled = false;
        } else {
          TeeSerial0.print("Unknown boolean value ");
          TeeSerial0.print(argValue);
          TeeSerial0.println("; defaulting to TRUE");
          config.sleepEnabled = true;
        }
      } else if (argName == "WakeEveryNSeconds") {
        TeeSerial0.print("Setting wake every N to ");
        TeeSerial0.println(argValue);
        config.wakeEveryNSeconds = argValue.toInt();
      } else if (argName == "StayAwakeSeconds") {
        TeeSerial0.print("Setting stay awake time to ");
        TeeSerial0.println(argValue);
        config.stayAwakeSeconds = argValue.toInt();
      } else if (argName == "ChimeEveryNSeconds") {
        TeeSerial0.print("Setting chime every N to ");
        TeeSerial0.println(argValue);
        config.chimeEveryNSeconds = argValue.toInt();
      } else if (argName == "ChimeOffset") {
        TeeSerial0.print("Setting chime offset to ");
        TeeSerial0.println(argValue);
        config.chimeOffsetSeconds = argValue.toInt();
      } else if (argName == "InterChimeDelaySeconds") {
        TeeSerial0.print("Setting inter-chime delay to ");
        TeeSerial0.println(argValue);
        config.interChimeDelaySeconds = argValue.toInt();
      } else if (argName == "InterHourChimeDelaySeconds") {
        TeeSerial0.print("Setting inter-hour-chime delay to ");
        TeeSerial0.println(argValue);
        config.interHourChimeDelaySeconds = argValue.toInt();
      } else if (argName == "ChimeStopTimeout") {
        if (argValue.toInt() > 65535) {
          TeeSerial0.print("Value ");
          TeeSerial0.print(argValue);
          TeeSerial0.println(" larger than maximum 65535. Limiting to 65535.");
          config.chimeStopTimeout = 65535;
        } else {
          TeeSerial0.print("Setting chime stop timeout to ");
          TeeSerial0.println(argValue);
          config.chimeStopTimeout = argValue.toInt();
        }
      } else if (argName == "HeartbeatEnabled") {
        // true or false
        TeeSerial0.print("Setting heartbeat enabled flag to ");
        TeeSerial0.println(argValue);
        argValue.toUpperCase();
        if (argValue == "TRUE" || argValue == "ON") {
          config.heartbeatEnabled = true;
        } else if (argValue == "FALSE" || argValue == "OFF") {
          config.heartbeatEnabled = false;
        } else {
          TeeSerial0.print("Unknown boolean value ");
          TeeSerial0.print(argValue);
          TeeSerial0.println("; defaulting to TRUE");
          config.heartbeatEnabled = true;
        }
      } else {
        TeeSerial0.print("Unknown configuration key ");
        TeeSerial0.println(argName);
      }
    }

    // Write updated configuration to SPIFFS
    // But don't bother if this was just an empty POST
    if (server.args() > 0) {
      writeConfig();
      message += "<h4>Configuration updated.</h4>\n";
    }
  }

  message += "<table border=0 cellspacing=10 cellpadding=10>\n";
  message += "<tr><td>\n";
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
  message += "<label>Sleep enabled <input type=\"checkbox\" name=\"SleepEnabled\" ";
  if (config.sleepEnabled) {
    message += "checked";
  }
  message += "/></label><br/>\n";
  message += "<label>Wake every <input type=\"text\" name=\"WakeEveryNSeconds\" value=\"";
  message += config.wakeEveryNSeconds;
  message += "\"/> seconds</label><br/>\n";
  message += "<label>Stay awake for <input type=\"text\" name=\"StayAwakeSeconds\" value=\"";
  message += config.stayAwakeSeconds;
  message += "\"/> seconds</label><br/>\n";
  message += "<label>Chime every <input type=\"text\" name=\"ChimeEveryNSeconds\" value=\"";
  message += config.chimeEveryNSeconds;
  message += "\"/> seconds</label><br/>\n";
  message += "<label>Chime offset <input type=\"text\" name=\"ChimeOffset\" value=\"";
  message += config.chimeOffsetSeconds;
  message += "\"/> seconds</label><br/>\n";
  message += "<label>Inter-chime delay <input type=\"text\" name=\"InterChimeDelaySeconds\" value=\"";
  message += config.interChimeDelaySeconds;
  message += "\"/> seconds</label><br/>\n";
  message += "<label>Inter-hour-chime delay <input type=\"text\" name=\"InterHourChimeDelaySeconds\" value=\"";
  message += config.interHourChimeDelaySeconds;
  message += "\"/> seconds</label><br/>\n";
  message += "<label>Chime stop timeout <input type=\"text\" name=\"ChimeStopTimeout\" value=\"";
  message += config.chimeStopTimeout;
  message += "\"/> milliseconds (max 65535)</label><br/>\n";
  message += "<label>Heartbeat enabled <input type=\"checkbox\" name=\"HeartbeatEnabled\" ";
  if (config.heartbeatEnabled) {
    message += "checked";
  }
  message += "/></label><br/>\n";
  message += "<input type=\"submit\" value=\"Save\"/>\n";
  message += "</form>\n";
  message += "<form action=\"/\" method=\"post\"><input type=\"submit\" value=\"Home\"/></form>\n";
  message += "</td>\n";
  message += "<td>\n";
  message += "<h4>Current config</h4><br/>\n";
  message += "<pre>\n";
  String configFileText;
  if (readConfigToString(configFileText)) {
    message += configFileText;
  } else {
    message += "Error reading config file!";
  }
  message += "</pre>\n";
  message += "</td></tr>\n";
  message += "</table>\n";
  message += "</body>\n</html>\n";
  
  server.send(200, "text/html", message);
}

void handleStats() {
  String message = "<html>\n<head>\n\t<title>Statistics</title>\n</head>\n<body>\n";
  if (server.method() == HTTP_POST) {
    for (uint8_t i = 0; i < server.args(); i++) {
      if (server.argName(i) == "delete" && server.arg(i) == "true") {
        SPIFFS.remove("/statistics.csv");
        message += "<h2>Cleared statistics.</h2>\n";
      }
    }
  } else {
    File stats = SPIFFS.open("/statistics.csv", "r");
    if (!stats) {
        TeeSerial0.println("handleStats: file open failed while opening /statistics.csv");
        message += "<h2>No statistics collected yet.</h2>\n";
    } else {
      message += "<table border=1 cellpadding=1 cellspacing=0>";
      message += "<tr>";
      message += "<th>Date</th><th>Battery Voltage</th><th>RTC Temp (&deg;F)</th><th>Chime Duration</th>";
      message += "</tr>";
      while (stats.available()) {
        // time,battery voltage,rtc temp
        String token = stats.readStringUntil(',');
        RtcDateTime statDateTime = RtcDateTime();
        statDateTime.InitWithEpoch32Time(token.toInt());
        token = stats.readStringUntil(',');
        float batteryVoltage = token.toFloat();
        token = stats.readStringUntil(',');
        float rtcTemp = token.toFloat();
        token = stats.readStringUntil('\n');
        uint32_t chimeDuration = token.toInt();

        message += "<tr>";
        message += "<td>";
        String statDateTimeString;
        dateTimeStringFromRtcDateTime(statDateTime, statDateTimeString);
        message += statDateTimeString;
        message += "</td>";

        message += "<td>";
        message += batteryVoltage;
        message += "</td>";

        message += "<td>";
        message += rtcTemp;
        message += "</td>";

        message += "<td>";
        message += chimeDuration;
        message += "</td>";
        message += "</tr>";
      }
      
      message += "</table>";
    }
  }
  
  message += "<form action=\"/stats\" method=\"post\">\n";
  message += "<input type=\"hidden\" name=\"delete\" value=\"true\"/>\n";
  message += "<input type=\"submit\" value=\"Clear Statistics\"/>\n";
  message += "</form>\n";
  message += "<form action=\"/\" method=\"post\"><input type=\"submit\" value=\"Home\"/></form>\n";
  message += "</body>\n</html>\n";
  
  server.sendHeader("Refresh", "60; url=/stats");
  server.send(200, "text/html", message);
}

void handleChimeNow() {
  String message = "<html>\n<head>\n\t<title>Chime Now</title>\n</head>\n<body>\n";
  message += "<h1>Chiming!</h1>";
  message += "</body>\n</html>\n";
  
  server.sendHeader("Refresh", "5; url=/");
  server.send(200, "text/html", message);
  startChiming();
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

// Debug function, not used for now
void dumpSPIFFSInfo() {
  FSInfo fs_info;
  SPIFFS.info(fs_info);

  TeeSerial0.println("Filesystem info");
  TeeSerial0.print("Total bytes: ");
  TeeSerial0.println(fs_info.totalBytes);
  TeeSerial0.print("Used bytes: ");
  TeeSerial0.println(fs_info.usedBytes);
  TeeSerial0.print("Block size: ");
  TeeSerial0.println(fs_info.blockSize);
  TeeSerial0.print("Page size: ");
  TeeSerial0.println(fs_info.pageSize);
  TeeSerial0.print("Max open files: ");
  TeeSerial0.println(fs_info.maxOpenFiles);
  TeeSerial0.print("Max path length: ");
  TeeSerial0.println(fs_info.maxPathLength);
  TeeSerial0.println("");
  TeeSerial0.flush();

  TeeSerial0.println("Directory listing of /");
  Dir dir = SPIFFS.openDir("/");
  while (dir.next()) {
    TeeSerial0.print("  ");
    TeeSerial0.println(dir.fileName());
  }
  TeeSerial0.flush();
}

// Setup functions
void setupSPIFFS() {
  TeeSerial0.println("Starting SPIFFS");
  if (SPIFFS.begin()) {
    TeeSerial0.println("SPIFFS initialized");
  } else {
    TeeSerial0.println("SPIFFS initialization failed!");
    return;
  }
  TeeSerial0.flush();
}

void setConfigDefaults() {
  byte mac[6];
  String macString;

  // Don't like doing this here, but this should only
  // be called during initialization anyway...
  WiFi.mode(WIFI_STA);
  WiFi.macAddress(mac);
  config.deviceDescription = "Unconfigured ";
  macString = String(mac[1], HEX);
  macString.toUpperCase();
  config.deviceDescription += macString;
  config.deviceDescription += ":";
  macString = String(mac[0], HEX);
  macString.toUpperCase();
  config.deviceDescription += macString;
  config.wiFiSSID = config.deviceDescription;
  config.wiFiPassword = "";
  config.wiFiMode = WIFI_AP;
  config.sleepEnabled = false;
  config.wakeEveryNSeconds = 240;
  config.stayAwakeSeconds = 120;
  config.chimeEveryNSeconds = 180;
  config.chimeOffsetSeconds = 0;
  config.interChimeDelaySeconds = 18;
  config.interHourChimeDelaySeconds = 6;
  config.chimeStopTimeout = 65535;
  config.heartbeatEnabled = true;
}

bool readConfigToString(String& configFileText) {
  File f = SPIFFS.open("/bmchimes.cfg", "r");
  if (!f) {
      TeeSerial0.println("readConfigToString: file open failed");
      return false;
  }
  while (f.available()) {
    yield();
    configFileText += f.readString();
  }
  return true;
}

bool readConfigFile() {
  bool result = false;

  TeeSerial0.println("Opening config file");
  File f = SPIFFS.open("/bmchimes.cfg", "r");
  if (!f) {
      TeeSerial0.println("readConfig: file open failed");
      return false;
  }
  TeeSerial0.flush();
  TeeSerial0.println("Beginning read of config file");
  TeeSerial0.flush();
  result = readConfigFromStream(f);
  TeeSerial0.println("Closing config file");
  f.close();
  return result;
}

bool readConfigFromStream(Stream& s) {
  int loops = 0;
  while (loops < 50 && s.available()) {
    yield();
    TeeSerial0.flush();
    String key = s.readStringUntil('=');
    key.trim();
#ifdef DEBUG
    TeeSerial0.print("Read key ");
    TeeSerial0.print(key);
    TeeSerial0.print("; hex: ");
    printStringAsHexDump(key);
    TeeSerial0.println("");
#endif

    String value = s.readStringUntil('\n');
    value.trim();
#ifdef DEBUG
    TeeSerial0.print("Read value ");
    TeeSerial0.print(value);
    TeeSerial0.print("; hex: ");
    printStringAsHexDump(value);
    TeeSerial0.println("");
#endif

    TeeSerial0.flush();
    if (key == "DeviceDescription") {
      TeeSerial0.print("Setting Device Description ");
      TeeSerial0.println(value);
      config.deviceDescription = value;
    } else if (key == "WiFiSSID") {
      TeeSerial0.print("Setting WiFi SSID ");
      TeeSerial0.println(value);
      TeeSerial0.print(" (");
      printStringAsHexDump(value);
      TeeSerial0.println(")");
      config.wiFiSSID = value;
    } else if (key == "WiFiPassword") {
      TeeSerial0.print("Setting WiFi password ");
      TeeSerial0.println(value);
      TeeSerial0.print(" (");
      printStringAsHexDump(value);
      TeeSerial0.println(")");
      config.wiFiPassword = value;
    } else if (key == "WiFiMode") {
      // Either Station or AP
      TeeSerial0.print("Setting WiFi mode ");
      TeeSerial0.println(value);
      value.toUpperCase();
      if (value == "AP") {
        config.wiFiMode = WIFI_AP;
      } else if (value == "STATION") {
        config.wiFiMode = WIFI_STA;
      } else {
        TeeSerial0.print("Unknown WiFi mode ");
        TeeSerial0.print(value);
        TeeSerial0.println("; defaulting to STATION");
        config.wiFiMode = WIFI_STA;
      }
    } else if (key == "ConnectWiFiAtReset") {
      // true or false
      TeeSerial0.print("Setting connect WiFi at reset flag to ");
      TeeSerial0.println(value);
      value.toUpperCase();
      if (value == "TRUE") {
        config.connectWiFiAtReset = true;
      } else if (value == "FALSE") {
        config.connectWiFiAtReset = false;
      } else {
        TeeSerial0.print("Unknown boolean value ");
        TeeSerial0.print(value);
        TeeSerial0.println("; defaulting to TRUE");
        config.connectWiFiAtReset = true;
      }
    } else if (key == "SleepEnabled") {
      // true or false
      TeeSerial0.print("Setting sleep enabled flag to ");
      TeeSerial0.println(value);
      value.toUpperCase();
      if (value == "TRUE") {
        config.sleepEnabled = true;
      } else if (value == "FALSE") {
        config.sleepEnabled = false;
      } else {
        TeeSerial0.print("Unknown boolean value ");
        TeeSerial0.print(value);
        TeeSerial0.println("; defaulting to TRUE");
        config.sleepEnabled = true;
      }
    } else if (key == "WakeEveryNSeconds") {
      TeeSerial0.print("Setting wake every N seconds to ");
      TeeSerial0.println(value);
      config.wakeEveryNSeconds = value.toInt();
    } else if (key == "StayAwakeSeconds") {
      TeeSerial0.print("Setting stay awake seconds to ");
      TeeSerial0.println(value);
      config.stayAwakeSeconds = value.toInt();
    } else if (key == "ChimeEveryNSeconds") {
      TeeSerial0.print("Setting chime every N seconds to ");
      TeeSerial0.println(value);
      config.chimeEveryNSeconds = value.toInt();
    } else if (key == "ChimeOffset") {
      TeeSerial0.print("Setting chime offset to ");
      TeeSerial0.println(value);
      config.chimeOffsetSeconds = value.toInt();
    } else if (key == "InterChimeDelaySeconds") {
      TeeSerial0.print("Setting inter-chime delay to ");
      TeeSerial0.println(value);
      config.interChimeDelaySeconds = value.toInt();
    } else if (key == "InterHourChimeDelaySeconds") {
      TeeSerial0.print("Setting inter-hour-chime delay to ");
      TeeSerial0.println(value);
      config.interHourChimeDelaySeconds = value.toInt();
    } else if (key == "ChimeStopTimeout") {
      if (value.toInt() > 65535) {
        TeeSerial0.print("Value ");
        TeeSerial0.print(value);
        TeeSerial0.print(" larger than maximum 65535. Limiting to 65535.");
        config.chimeStopTimeout = 65535;
      } else {
        TeeSerial0.print("Setting chime stop timeout to ");
        TeeSerial0.println(value);
        config.chimeStopTimeout = value.toInt();
      }
    } else if (key == "HeartbeatEnabled") {
      // true or false
      TeeSerial0.print("Setting heartbeat enabled flag to ");
      TeeSerial0.println(value);
      value.toUpperCase();
      if (value == "TRUE") {
        config.heartbeatEnabled = true;
      } else if (value == "FALSE") {
        config.heartbeatEnabled = false;
      } else {
        TeeSerial0.print("Unknown boolean value ");
        TeeSerial0.print(value);
        TeeSerial0.println("; defaulting to TRUE");
        config.heartbeatEnabled = true;
      }
    } else {
      TeeSerial0.print("Unknown configuration key ");
      TeeSerial0.println(key);
    }
    loops++;
  }
  TeeSerial0.println("Config read complete");
  return true;
}

void writeConfig() {
  TeeSerial0.println("writeConfig: Opening /bmchimes.cfg for writing");
  File f = SPIFFS.open("/bmchimes.cfg", "w");
  if (!f) {
      TeeSerial0.println("writeConfig: file open failed");
      return;
  }
  TeeSerial0.println("File successfully opened");

  config.deviceDescription.trim();
  TeeSerial0.print("Writing device description: ");
  TeeSerial0.print(config.deviceDescription);
  TeeSerial0.print("; hex: ");
  printStringAsHexDump(config.deviceDescription);
  TeeSerial0.println("");
  f.print("DeviceDescription=");
  f.println(config.deviceDescription);

  config.wiFiSSID.trim();
  TeeSerial0.print("Writing WiFiSSID: ");
  TeeSerial0.print(config.wiFiSSID);
  TeeSerial0.print("; hex: ");
  printStringAsHexDump(config.wiFiSSID);
  TeeSerial0.println("");
  f.print("WiFiSSID=");
  f.println(config.wiFiSSID);

  config.wiFiPassword.trim();
  TeeSerial0.print("Writing WiFiPassword: ");
  TeeSerial0.print(config.wiFiPassword);
  TeeSerial0.print("; hex: ");
  printStringAsHexDump(config.wiFiPassword);
  TeeSerial0.println("");
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
  f.print("SleepEnabled=");
  if (config.sleepEnabled == true) {
    f.println("TRUE");
  } else {
    f.println("FALSE");
  }
  f.print("WakeEveryNSeconds=");
  f.println(config.wakeEveryNSeconds);
  f.print("StayAwakeSeconds=");
  f.println(config.stayAwakeSeconds);
  f.print("ChimeEveryNSeconds=");
  f.println(config.chimeEveryNSeconds);
  f.print("ChimeOffset=");
  f.println(config.chimeOffsetSeconds);
  f.print("InterChimeDelaySeconds=");
  f.println(config.interChimeDelaySeconds);
  f.print("InterHourChimeDelaySeconds=");
  f.println(config.interHourChimeDelaySeconds);
  f.print("ChimeStopTimeout=");
  f.println(config.chimeStopTimeout);
  f.print("HeartbeatEnabled=");
  if (config.heartbeatEnabled == true) {
    f.println("TRUE");
  } else {
    f.println("FALSE");
  }
  f.close();
}

float batteryVoltage() {
  const float R7 = 5000.0;
  const float R8 = 220.0;
  uint16_t adcReading = analogRead(A0);
  // Vin = Vout / (R8/(R7+R8))
  float Vout = adcReading / 1024.0;
  float Vin = Vout / (R8/(R7+R8));
  
  return Vin;
}

void collectStats() {
  SPIFFS.remove("/statistics.csv.old");
  SPIFFS.rename("/statistics.csv", "/statistics.csv.old");
  File oldStats = SPIFFS.open("/statistics.csv.old", "r");
  if (!oldStats) {
      TeeSerial0.println("collectStats: file open failed while opening /statistics.csv.old");
      // Probably our first run. OK to keep going.
  }
  File stats = SPIFFS.open("/statistics.csv", "w");
  if (!stats) {
      TeeSerial0.println("collectStats: file open failed while opening /statistics.csv");
      return;
  }

  // Stats line consists of:
  // time,battery voltage,rtc temp,chime duration
  // time will be in Epoch32
  // battery voltage will be direct ADC reading
  // rtc temp will be a float
  // chime duration is the number of milliseconds between chime start and the stop switch triggering
  // Example:
  // 1466529391,13.80,55.75,1348
  String csvBlob;
  int lines = 1;
  RtcDateTime now = Rtc.GetDateTime();
  csvBlob = String(now.Epoch32Time());
  csvBlob += ",";
  csvBlob += String(batteryVoltage(), 2);
  csvBlob += ",";
  RtcTemperature dieTemp = Rtc.GetTemperature();
  float dieTempF = (dieTemp.AsFloat() * (9.0/5.0)) + 32.0;
  csvBlob += String(dieTempF, 2);
  csvBlob += ",";
  csvBlob += String(lastChimeDurationMillis);
  csvBlob += "\n";

  if (oldStats) {
    // If there are any old statistics to copy to the new file
    while (oldStats.available() && lines < statsLinesPerDay) {
      csvBlob += oldStats.readStringUntil('\n');
      csvBlob += "\n";
      lines++;
    }
    oldStats.close();
  }
  stats.print(csvBlob);
  stats.close();
}

void syncNTPTime() {
  TeeSerial0.println("Fetching time from NTP servers");
  // Pacific time zone hard coded
  configTime(-(7 * 3600), -3600, "pool.ntp.org", "time.nist.gov");
  // Wait up to a minute for NTP sync
  uint8_t attempts = 0;
  while (attempts <= 120 && !time(nullptr)) {
    TeeSerial0.print(".");
    delay(500);
    attempts++;
  }
  TeeSerial0.println("");
  if (!time(nullptr)) {
    TeeSerial0.println("Failed to sync time with NTP servers!");
  } else {
    TeeSerial0.println("Successfully synced time with NTP servers.");
  }
}

void connectToWiFi() {
  config.wiFiSSID.toCharArray(ssid, 64);
  config.wiFiPassword.toCharArray(password, 64);
  if (config.wiFiPassword.length() > 0) {
    WiFi.begin(ssid, password);
  } else {
    WiFi.begin(ssid);
  }

  if (config.wiFiMode == WIFI_STA) {
    // Wait up to 1 minute for connection
    uint8_t attempts = 0; 
    while (attempts <= 120 && WiFi.status() != WL_CONNECTED) {
      delay(500);
      TeeSerial0.print(".");
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      TeeSerial0.println("");
      TeeSerial0.print("Connected to ");
      TeeSerial0.println(config.wiFiSSID);
      TeeSerial0.print("IP address: ");
      TeeSerial0.println(WiFi.localIP());
    } else {
      TeeSerial0.println("");
      TeeSerial0.print("Unable to connect to ");
      TeeSerial0.print(config.wiFiSSID);
      TeeSerial0.println(" network.  Giving up.");
    }
  } else if (config.wiFiMode == WIFI_AP) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    TeeSerial0.print("Starting WiFi network named ");
    TeeSerial0.print(config.wiFiSSID);
    TeeSerial0.print("...");
    TeeSerial0.flush();
    if (config.wiFiPassword.length() > 0) {
      WiFi.softAP(ssid, password);
    } else {
      WiFi.softAP(ssid);
    }

    TeeSerial0.println("done.");
    TeeSerial0.print("My IP address is ");
    TeeSerial0.println(WiFi.softAPIP());
  }
}

void writeNtpTimeToRtc() {
  TeeSerial0.println("Setting RTC time from NTP time");
  time_t now = time(nullptr);
  RtcDateTime ntpDateTime = RtcDateTime();
  ntpDateTime.InitWithEpoch32Time(now);

  Rtc.SetDateTime(ntpDateTime);
}

void rtcSetup() {
  Rtc.Begin();
  if (!Rtc.GetIsRunning()) {
    TeeSerial0.println("RTC was not actively running, starting now");
    Rtc.SetIsRunning(true);
  }
  Rtc.Enable32kHzPin(false);
  Rtc.SetSquareWavePin(DS3231SquareWavePin_ModeAlarmBoth); 
}


void calculateSleepTiming(RtcDateTime& now) {
  uint16_t secondsToStayAwake = config.stayAwakeSeconds;
  uint16_t secondsTilNextWake = secondsTilNextN(config.wakeEveryNSeconds / 60); // always on a minute boundary
  time_t nowTime = now.Epoch32Time();
  time_t wakeTime = nowTime + secondsTilNextWake;
  time_t nextSleepTime = wakeTime + secondsToStayAwake;
  uint16_t secondsTilWakeAfterNext = secondsTilNextWake + config.wakeEveryNSeconds;
  time_t wakeTimeAfterNext = nowTime + secondsTilWakeAfterNext;
  time_t sleepTimeAfterNext = wakeTimeAfterNext + secondsToStayAwake;

  // set globals
  sleepDuration = wakeTimeAfterNext - nextSleepTime;
  wakeAlarmDateTime = RtcDateTime();
  wakeAlarmDateTime.InitWithEpoch32Time(wakeTime);
  sleepAlarmDateTime = RtcDateTime(nextSleepTime);
  sleepAlarmDateTime.InitWithEpoch32Time(nextSleepTime);
}

void scheduleChimeSequence(RtcDateTime& now) {
  time_t nowTime = now.Epoch32Time();

  // Clear any existing schedule.
  memset(chimeSchedule, 0xff, sizeof(chimeSchedule));
  uint16_t secondsTilNextChime;
  time_t lastChimeTime;
  static time_t chimeTime;
  uint16_t scheduledHourChimes = 0;
  uint8_t twelveHour = (now.Hour() % 12 == 0 ? 12 : now.Hour() % 12);
  chime_state_t scheduleChimeState;
  for ( int i = 0, scheduleChimeState = CHIME_INITIAL;
        i < MAX_CHIME_SCHEDULE, scheduleChimeState <= CHIME_HOUR && scheduledHourChimes < twelveHour;
        i++) {
    switch (scheduleChimeState) {
      case CHIME_INITIAL:
        secondsTilNextChime = secondsTilNextN(config.chimeEveryNSeconds / 60) + config.chimeOffsetSeconds;
        chimeSchedule[i] = nowTime + secondsTilNextChime;
        break;
      case CHIME_SECOND:
      case CHIME_THIRD:
        secondsTilNextChime = config.interChimeDelaySeconds + config.chimeOffsetSeconds;
        chimeSchedule[i] = chimeSchedule[i-1] + secondsTilNextChime;
        break;
      case CHIME_HOUR:
        secondsTilNextChime = config.interHourChimeDelaySeconds;
        chimeSchedule[i] = chimeSchedule[i-1] + secondsTilNextChime;
        break;
    }


    if (scheduleChimeState == CHIME_HOUR) {
      scheduledHourChimes++;
    } else {
      scheduleChimeState = (chime_state_t)(((int)scheduleChimeState + 1) % NUM_CHIME_STATES);
    }
  }

  /* XXX 
  // Determine if the next chime will happen between the next sleep and the next wake
  if (chimeAlarmDateTime >= sleepAlarmDateTime && chimeAlarmDateTime <= wakeAlarmDateTime) {
    // Chime will happen while we are asleep!  Skip the next sleep.
    sleepAlarmDateTime.InitWithEpoch32Time(sleepTimeAfterNext);
  }
  */
}

void calculateSleepAndChimeTiming() {
  RtcDateTime now = Rtc.GetDateTime();
  calculateSleepTiming(now);
  scheduleChimeSequence(now);
  TeeSerial0.println("calculateSleepAndChimeTiming():");
  dumpChimeSchedule();
}

void dumpChimeSchedule() {
  TeeSerial0.println("Chime Schedule");
  TeeSerial0.println("--------------------------------------");
  for (int i = 0; i < MAX_CHIME_SCHEDULE; i++) {
    RtcDateTime scheduledChimeDateTime = RtcDateTime();
    if (chimeSchedule[i] != 0xffffffff) {
      scheduledChimeDateTime.InitWithEpoch32Time(chimeSchedule[i]);
      String scheduledChimeString;
      dateTimeStringFromRtcDateTime(scheduledChimeDateTime, scheduledChimeString);
      TeeSerial0.println(scheduledChimeString);
    }
  }
  TeeSerial0.println("");
}

time_t getNextChimeTime(RtcDateTime& now) {
  RtcDateTime candidateChimeDateTime = RtcDateTime();
  for (int i = 0; i < MAX_CHIME_SCHEDULE; i++) {
    if (chimeSchedule[i] != INVALID_TIME) {
      candidateChimeDateTime.InitWithEpoch32Time(chimeSchedule[i]);
      if (candidateChimeDateTime > now) {
        return chimeSchedule[i];
      }
    }
  }
  return INVALID_TIME;
}

void setRtcAlarms() {
  // Chime alarm is second-sensitive, has to be an exact alarm
  // Sleep alarm isn't, can just run on the 00 second of a given minute
  // Other less-critical tasks such as stats collection are also seconds-insensitive,
  // can run on a once-a-minute basis.
  RtcDateTime now = Rtc.GetDateTime();
  chimeAlarmDateTime = RtcDateTime();
  time_t nextChimeTime = getNextChimeTime(now);
  chimeAlarmDateTime.InitWithEpoch32Time(nextChimeTime);

  String nowString;
  getRtcDateTimeString(nowString);
  String sleepAlarmDateTimeString;
  dateTimeStringFromRtcDateTime(sleepAlarmDateTime, sleepAlarmDateTimeString);
  String chimeAlarmDateTimeString;
  dateTimeStringFromRtcDateTime(chimeAlarmDateTime, chimeAlarmDateTimeString);

  TeeSerial0.print("Time is now ");
  TeeSerial0.println(nowString);

  // Alarm one is the chime alarm
  DS3231AlarmOne alarmOne = DS3231AlarmOne(
    0, // dayOf; irrelevant with MinutesSecondsMatch
    chimeAlarmDateTime.Hour(),
    chimeAlarmDateTime.Minute(),
    chimeAlarmDateTime.Second(),
    DS3231AlarmOneControl_HoursMinutesSecondsMatch 
  );
  
  Rtc.SetAlarmOne(alarmOne);
  TeeSerial0.print("Chime scheduled for ");
  TeeSerial0.println(chimeAlarmDateTimeString);
  TeeSerial0.flush();

  // Alarm two is the once-per-minute alarm
  DS3231AlarmTwo alarmTwo = DS3231AlarmTwo(
    0,
    0,
    0,
    DS3231AlarmTwoControl_OncePerMinute
  );
  
  Rtc.SetAlarmTwo(alarmTwo);
  if (config.sleepEnabled) {
    TeeSerial0.print("sleepDuration = ");
    TeeSerial0.println(sleepDuration);
    TeeSerial0.print("Sleep scheduled for ");
    TeeSerial0.println(sleepAlarmDateTimeString);
    TeeSerial0.flush();
  }
  
  Rtc.LatchAlarmsTriggeredFlags();
  attachInterrupt(digitalPinToInterrupt(rtcAlarmPin), alarmISR, FALLING);
}

void scheduleNextChime(RtcDateTime& now) {
  // Schedule next chime
  if (getNextChimeTime(now) == INVALID_TIME) {
    TeeSerial0.println("No valid next chime time found.  Recalculating chime schedule...");
    calculateSleepAndChimeTiming();
  }
  setRtcAlarms();
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

void chimeSetup() {
  pinMode(chimePin, OUTPUT);
  digitalWrite(chimePin, LOW);
  pinMode(chimeStopSwitchPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(chimeStopSwitchPin), chimeStopSwitchISR, FALLING);
  chimeState = CHIME_INITIAL;
}

void heartbeatSetup() {
  pinMode(heartbeatPin, OUTPUT);
  digitalWrite(heartbeatPin, LOW);
}

void basicSetup() {
  pinMode(rtcAlarmPin, INPUT);
  TeeSerial0.begin(74880);
  TeeSerial0.println("");
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
  server.on("/stats", handleStats);
  server.on("/chimenow", handleChimeNow);
  server.onNotFound(handleNotFound);

  /***
   *** Different approach to handler functions
  server.on("/inline", [](){
    server.send(200, "text/plain", "this works as well");
  });
  ***/

  // Start the web server
  server.begin();
  TeeSerial0.println("HTTP server started");
}

void setup(void) {
  basicSetup();
  setupSPIFFS();
  if (!readConfigFile()) {
    TeeSerial0.println("Config read failed, setting defaults");
    setConfigDefaults();
    writeConfig();
    readConfigFile();
  }
  connectToWiFi();

  rtcSetup();
  clearRtcAlarms();
  calculateSleepAndChimeTiming();
  setRtcAlarms();
  chimeSetup();
  if (config.heartbeatEnabled) {
    heartbeatSetup();
    heartbeatReset();
  }
  startWebServer();
}

void loop(void) {
  String nowString;
  DS3231AlarmFlag flag;

  // Probably unnecessary, but doesn't hurt anything.
  yield();

  if (config.heartbeatEnabled && millis() % heartbeatBlipInterval == 0) {
    heartbeatReset();
  }

  if (alarmFired(flag)) {
    RtcDateTime now = Rtc.GetDateTime();
    if (flag & DS3231AlarmFlag_Alarm1) {
      // Chime alarm fired
      TeeSerial0.println("Chime alarm fired!");
      dateTimeStringFromRtcDateTime(now, nowString);
      TeeSerial0.print("Time is now ");
      TeeSerial0.println(nowString);
      if (!chiming) {
        startChiming();
      } else {
        // Hopefully we only get here if someone has triggered a manual chime.
        TeeSerial0.println("Chiming already in progress, skipping scheduled chime.");
      }
    }

    if (flag & DS3231AlarmFlag_Alarm2) {
      // Once-per-minute alarm fired
      if (config.sleepEnabled
            && sleepAlarmDateTime.Hour() == now.Hour()
            && sleepAlarmDateTime.Minute() == now.Minute()
            && sleepAlarmDateTime.Second() == now.Second()) {
        // sleep now
        dateTimeStringFromRtcDateTime(now, nowString);
        TeeSerial0.print("Time is now ");
        TeeSerial0.println(nowString);
        if (sleepDuration == 0) {
          // Sanity check; if we sleep for 0 seconds, the ESP never wakes up.
          TeeSerial0.println("Sleep duration is zero, skipping sleep.");
        } else {
          TeeSerial0.print("Sleeping for ");
          TeeSerial0.print(sleepDuration);
          TeeSerial0.println(" seconds");
          // deepSleep expects a number in microseconds
          ESP.deepSleep(sleepDuration * 1e6);
        }
      }

      // Collect statistics - this is time consuming, don't do it while chiming.
      // Instead, set a flag.  Timing of stats collection is far from critical.
      if (now.Minute() % statsInterval == 0) {
        shouldCollectStats = true;
      }

      // Update chime schedule
      scheduleNextChime(now);
    }
  }

  // Check time every loop, turn off chime GPIO if timeout
  if (chiming) {
    // Check for stop conditions (timeout, stop switch)
    if (chimeStopSwitchFlag || (millis() - chimeStartMillis > config.chimeStopTimeout)) {
      // Check chime home switch GPIO flag (set by interrupt), turn off chime GPIO if set
      if (chimeStopSwitchFlag) {
        TeeSerial0.println("Chime stop switch activated, turning off chime motor");
      } else {
        TeeSerial0.println("Timeout waiting for chime stop switch activation!");
      }
      stopChiming();
      RtcDateTime now = Rtc.GetDateTime();
      if (chimeState == CHIME_HOUR) {
        chimeHoursRungOut++;
        uint8_t twelveHour = (now.Hour() % 12 == 0 ? 12 : now.Hour() % 12);
        if (chimeHoursRungOut >= twelveHour) {
          chimeHoursRungOut = 0;
          chimeState = CHIME_INITIAL;
        }
      } else {
        // Advance to the next chime state.
        chimeState = (chime_state_t)(((int)chimeState + 1) % NUM_CHIME_STATES);
      }
      scheduleNextChime(now);
    }
  } else {
    if (shouldCollectStats) {
      collectStats();
      shouldCollectStats = false;
    }
  }
  
  server.handleClient();
  if (config.heartbeatEnabled) {
    heartbeat();
  }
}
