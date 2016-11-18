#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include <time.h>
#include "FS.h"
#include <RtcDS3231.h>
#include <pgmspace.h>
#include "TeeSerial.h"
#include "StringStream.h"

// Uncomment to enable 15 minute chime, comment #undef below
// #define FIFTEEN_MINUTE_CHIME 1
// Disable 15-minute chime
#undef FIFTEEN_MINUTE_CHIME

// Allows streaming output (<< syntax)
template<class T> inline Print &operator <<(Print &obj, T arg) { obj.print(arg); return obj; }

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
  uint16_t chimeStopTimeout; 
  uint16_t chimeCount;
  uint16_t chimeNumber;
  uint16_t chimeCycleSeconds;
  uint16_t chimeInterInitialSeconds;
  bool chimeEnabled;
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
#define CHIME_INITIAL 0
#define CHIME_SECOND 1
#define CHIME_THIRD 2
#define CHIME_HOUR 3
#define NUM_CHIME_STATES 4
#define MAX_CHIME_SCHEDULE 15
#define INVALID_TIME 0xffffffff
time_t chimeSchedule[MAX_CHIME_SCHEDULE];

bool shouldCollectStats = false;
volatile bool chimingWasEnabled = false;
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
    TeeSerial0 << " ";
  }
}

void printlnStringAsHexDump(String str) {
  printStringAsHexDump(str);
  TeeSerial0 << "\n";
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

uint16_t secondsTilNextN(uint16_t N, RtcDateTime &startingFrom) {
  // 16 bit return value should be sufficient; we will chime or wake at least once an hour.
  uint16_t seconds = ((N - (startingFrom.Minute() % N)) - 1) * 60;
  seconds += 60 - startingFrom.Second();

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
  if (config.chimeEnabled) {
    TeeSerial0 << "Chiming!\n";
    // Set a flag indicating chiming has begun
    chiming = true;
    chimeStopSwitchFlag = false;
    // Store time chiming began
    chimeStartMillis = millis();
    // Trigger chime GPIO
    chimeMotorOn();
  } else {
    TeeSerial0 << "Chiming disabled (but would chime now)...\n";
  }
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

  String html = "<html>\n<head>\n";
  StringStream message = StringStream(html);
  message << "<title>" << config.deviceDescription << " - Chimes Controller</title>\n";
  message << "<style>form { display: inline; }</style>\n";
  message << "</head>\n<body>\n";
  message << "<h1>";
  message << config.deviceDescription;
  message << "</h1>\n";
  message << "<h2>Status</h2>\n";
  message << "This is chime #" << config.chimeNumber << " of " << config.chimeCount << "<br/>\n";
  message << "RTC date and time " << rtcDateTimeStr << "<br/>\n";
  message << "RTC die temperature " << dieTempF << "&deg;F<br/>\n";
  if (config.sleepEnabled) {
    message << "Next sleep scheduled at " << sleepAlarmDateTimeString << "<br/>\n";
  } else {
    message << "Sleep disabled, no sleep scheduled<br/>\n";
  }
  message << "Next chime scheduled at " << chimeAlarmDateTimeString << "<br/>\n";
  message << "Free heap: " << ESP.getFreeHeap() << " bytes<br/>\n";
  message << "Last chime stop switch interval: " << lastChimeDurationMillis << " milliseconds<br/>\n";
  if (WiFi.status() == WL_CONNECTED) {
    message << "Connected to WiFi network " << WiFi.SSID() << "<br/>\n";
  }

  message << "<table>\n";
  message << "<tr>\n";
  message << "<th>Chime Schedule</th>\n";
  message << "<th>Debug Log</th>\n";
  message << "</tr>\n";

  // Chime schedule
  message << "<tr>\n";
  message << "<td>\n";
  for (int i = 0; i < MAX_CHIME_SCHEDULE; i++) {
    RtcDateTime scheduledChimeDateTime = RtcDateTime();
    if (chimeSchedule[i] != 0xffffffff) {
      scheduledChimeDateTime.InitWithEpoch32Time(chimeSchedule[i]);
      String scheduledChimeString;
      dateTimeStringFromRtcDateTime(scheduledChimeDateTime, scheduledChimeString);
      message << scheduledChimeString << "<br/>\n";
    }
  }
  message << "</td>\n";

  // Debug log
  String debugLog;
  yield();
  TeeSerial0.getBuffer(debugLog);
  message << "<td>\n";
  message << "<pre>\n";
  message << debugLog;
  message << "</pre>\n";
  message << "</td>\n";
  message << "</tr>\n";

  // Buttons for the various actions
  message << "<tr>\n";
  message << "<td colspan=\"2\">\n";
  message << "<form action=\"/config\" method=\"get\"><input type=\"submit\" value=\"Configure\"/></form>\n";
  message << "<form action=\"/time\" method=\"get\"><input type=\"submit\" value=\"Manage Time\"/></form>\n";
  message << "<form action=\"/stats\" method=\"get\"><input type=\"submit\" value=\"Statistics\"/></form>\n";
  message << "<form action=\"/debuglog\" method=\"get\"><input type=\"submit\" value=\"Debug Log\"/></form>\n";
  message << "<form action=\"/sleep\" method=\"get\"><input type=\"submit\" value=\"Sleep Now\"/></form>\n";
  message << "<form action=\"/chimenow\" method=\"get\"><input type=\"submit\" value=\"Chime Now\"/></form>\n";
  message << "<form action=\"/reset\" method=\"get\"><input type=\"submit\" value=\"Reset\"/></form>\n";
  message << "</td>\n";
  message << "</tr>\n";
  message << "</table>\n";
  message << "</body>\n</html>\n";

  server.send(200, "text/html", html);
}

void handleSleep() {
  String message = "<html>\n<head>\n\t<title>Sleep</title>\n</head>\n<body>\n";
  message += "<h1>Not yet implemented</h1>\n";
  message += "<form action=\"/\" method=\"post\"><input type=\"submit\" value=\"Home\"/></form>\n";
  message += "</body>\n</html>\n";
  
  server.send(200, "text/html", message);
}

void handleReset() {
  String message = "<html>\n<head>\n";
  message += "<title>";
  message += config.deviceDescription;
  message += " - Reset</title>\n</head>\n<body>\n";
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
  
  String message = "<html>\n<head>\n";
  message += "<title>";
  message += config.deviceDescription;
  message += " - Temperature</title>\n</head>\n<body>\n";
  message += "<h1>Die temperature ";
  message += dieTempF;
  message += "&deg;F</h1>\n";
  message += "<form action=\"/\" method=\"post\"><input type=\"submit\" value=\"Home\"/></form>\n";
  message += "</body>\n</html>\n";
  
  server.send(200, "text/html", message);
}

void handleTime() {
  String message = "<html>\n<head>\n";
  message += "<title>";
  message += config.deviceDescription;
  message += " - Manage Time</title>\n</head>\n<body>\n";
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
  String html = "<html>\n<head>\n";
  StringStream message = StringStream(html);
  message << "<title>";
  message << config.deviceDescription;
  message << " - Configuration</title>\n";
  message << "<style>\n";
  message << "label { font-weight: bold; display: block; width: 200px; float: left; }\n";
  message << "form { display: inline; }\n";
  message << "</style>\n";
  message << "</head>\n<body>\n";
  message << "<h1>Configuration</h1>";

  if (server.method() == HTTP_POST) {
    // User selected save
    // Checkbox form elements are not passed in the POST unless they are checked;
    // therefore, assume they are unchecked and let the for loop below correct this
    config.sleepEnabled = false;
    config.connectWiFiAtReset = false;
    config.chimeEnabled = false;
    config.heartbeatEnabled = false;

    for (uint8_t i = 0; i < server.args(); i++) {
      String argName = String(server.argName(i));
      String argValue = String(server.arg(i));
      TeeSerial0 << "handleConfig(): argName = " << argName << "; argValue = " << argValue << "\n";

      if (argName == "DeviceDescription") {
        TeeSerial0 << "Setting Device Description " << argValue << "\n";
        config.deviceDescription = argValue;
      } else if (argName == "WiFiSSID") {
        TeeSerial0 << "Setting WiFi SSID " << argValue << "\n";
        config.wiFiSSID = argValue;
      } else if (argName == "WiFiPassword") {
        TeeSerial0 << "Setting WiFi password " << argValue << "\n";
        config.wiFiPassword = argValue;
      } else if (argName == "WiFiMode") {
        // Either Station or AP
        TeeSerial0 << "Setting WiFi mode " << argValue << "\n";
        argValue.toUpperCase(); if (argValue == "AP") {
          config.wiFiMode = WIFI_AP;
        } else if (argValue == "STATION") {
          config.wiFiMode = WIFI_STA;
        } else {
          TeeSerial0 << "Unknown WiFi mode " << argValue << "; defaulting to STATION\n";
          config.wiFiMode = WIFI_STA;
        }
      } else if (argName == "ConnectWiFiAtReset") {
        // true or false
        TeeSerial0 << "Setting connect WiFi at reset flag to " << argValue << "\n";
        argValue.toUpperCase();
        if (argValue == "TRUE" || argValue == "ON") {
          config.connectWiFiAtReset = true;
        } else if (argValue == "FALSE" || argValue == "OFF") {
          config.connectWiFiAtReset = false;
        } else {
          TeeSerial0 << "Unknown boolean value " << argValue << "; defaulting to TRUE\n";
          config.connectWiFiAtReset = true;
        }
      } else if (argName == "SleepEnabled") {
        // true or false
        TeeSerial0 << "Setting sleep enabled flag to " << argValue << "\n";
        argValue.toUpperCase();
        if (argValue == "TRUE" || argValue == "ON") {
          config.sleepEnabled = true;
        } else if (argValue == "FALSE" || argValue == "OFF") {
          config.sleepEnabled = false;
        } else {
          TeeSerial0 << "Unknown boolean value " << argValue << "; defaulting to TRUE\n";
          config.sleepEnabled = true;
        }
      } else if (argName == "WakeEveryNSeconds") {
        TeeSerial0 << "Setting wake every N to " << argValue << "\n";
        config.wakeEveryNSeconds = argValue.toInt();
      } else if (argName == "StayAwakeSeconds") {
        TeeSerial0 << "Setting stay awake time to " << argValue << "\n";
        config.stayAwakeSeconds = argValue.toInt();
      } else if (argName == "ChimeEveryNSeconds") {
        TeeSerial0 << "Setting chime every N to " << argValue << "\n";
        config.chimeEveryNSeconds = argValue.toInt();
      } else if (argName == "ChimeStopTimeout") {
        if (argValue.toInt() > 65535) {
          TeeSerial0 << "Value " << argValue << " larger than maximum 65535. Limiting to 65535.\n";
          config.chimeStopTimeout = 65535;
        } else {
          TeeSerial0 << "Setting chime stop timeout to " << argValue << "\n";
          config.chimeStopTimeout = argValue.toInt();
        }
      } else if (argName == "ChimeNumber") {
        TeeSerial0 << "Setting chime number to " << argValue << "\n";
        config.chimeNumber = argValue.toInt();
      } else if (argName == "ChimeCount") {
        TeeSerial0 << "Setting chime count to " << argValue << "\n";
        config.chimeCount = argValue.toInt();
      } else if (argName == "ChimeCycleSeconds") {
        TeeSerial0 << "Setting chime cycle seconds to " << argValue << "\n";
        config.chimeCycleSeconds = argValue.toInt();
      } else if (argName == "ChimeInterInitialSeconds") {
        TeeSerial0 << "Setting chime inter-initial seconds to " << argValue << "\n";
        config.chimeInterInitialSeconds = argValue.toInt();
      } else if (argName == "ChimeEnabled") {
        // true or false
        TeeSerial0 << "Setting chime enabled flag to " << argValue << "\n";
        argValue.toUpperCase();
        if (argValue == "TRUE" || argValue == "ON") {
          config.chimeEnabled = true;
        } else if (argValue == "FALSE" || argValue == "OFF") {
          config.chimeEnabled = false;
        } else {
          TeeSerial0 << "Unknown boolean value " << argValue << "; defaulting to TRUE\n";
          config.chimeEnabled = true;
        }
      } else if (argName == "HeartbeatEnabled") {
        // true or false
        TeeSerial0 << "Setting heartbeat enabled flag to " << argValue << "\n";
        argValue.toUpperCase();
        if (argValue == "TRUE" || argValue == "ON") {
          config.heartbeatEnabled = true;
        } else if (argValue == "FALSE" || argValue == "OFF") {
          config.heartbeatEnabled = false;
        } else {
          TeeSerial0 << "Unknown boolean value " << argValue << "; defaulting to TRUE\n";
          config.heartbeatEnabled = true;
        }
      } else {
        TeeSerial0 << "Unknown configuration key " << argName << "\n";
      }
    }

    // Write updated configuration to SPIFFS
    // But don't bother if this was just an empty POST
    if (server.args() > 0) {
      writeConfig();
      message << "<h4>Configuration updated.</h4>\n";
    }
  }

  message << "<table border=0 cellspacing=10 cellpadding=10>\n";
  message << "<tr><td>\n";
  message << "<form action=\"/config\" method=\"post\">\n";
  message << "<label>Device description</label>\n";
  message << "<input type=\"text\" name=\"DeviceDescription\" value=\"" << config.deviceDescription << "\"/><br/>\n";
  message << "<label>WiFi SSID</label>\n";
  message << "<input type=\"text\" name=\"WiFiSSID\" value=\"" << config.wiFiSSID << "\"/><br/>\n";
  message << "<label>WiFi Password</label>\n";
  message << "<input type=\"text\" name=\"WiFiPassword\" value=\"" << config.wiFiPassword << "\"/><br/>\n";
  message << "<label>WiFi Mode</label>\n";
  message << "<select name=\"WiFiMode\">\n";
  message << "<option value=\"AP\" " << (config.wiFiMode == WIFI_AP ? "selected" : "") << ">Access Point</option>\n";
  message << "<option value=\"STATION\" " << (config.wiFiMode == WIFI_STA ? "selected" : "") << ">Station</option>\n";
  message << "</select><br/>\n";
  message << "<label>Connect to WiFi at reset</label>\n";
  message << "<input type=\"checkbox\" name=\"ConnectWiFiAtReset\" " << (config.connectWiFiAtReset ? "checked" : "") << "/><br/>\n";
  message << "<label>Sleep enabled</label>\n";
  message << "<input type=\"checkbox\" name=\"SleepEnabled\" " << (config.sleepEnabled ? "checked" : "") << "/><br/>\n";
  message << "<label>Wake every</label>\n";
  message << "<input type=\"text\" name=\"WakeEveryNSeconds\" value=\"" << config.wakeEveryNSeconds << "\"/> seconds<br/>\n";
  message << "<label>Stay awake for</label>\n";
  message << "<input type=\"text\" name=\"StayAwakeSeconds\" value=\"" << config.stayAwakeSeconds << "\"/> seconds<br/>\n";
  message << "<label>Chime every</label>\n";
  message << "<input type=\"text\" name=\"ChimeEveryNSeconds\" value=\"" << config.chimeEveryNSeconds << "\"/> seconds<br/>\n";
  message << "<label>Chime stop timeout</label>\n";
  message << "<input type=\"text\" name=\"ChimeStopTimeout\" value=\"" << config.chimeStopTimeout << "\"/> milliseconds (max 65535)<br/>\n";

  message << "<label>This is chime number</label>\n";
  message << "<input type=\"text\" name=\"ChimeNumber\" size=\"4\" value=\"" << config.chimeNumber << "\"/> of ";
  message << "<input type=\"text\" name=\"ChimeCount\" size=\"4\" value=\"" << config.chimeCount << "\"/><br/>\n";

  message << "<label>Chime cycle time</label>\n";
  message << "<input type=\"text\" name=\"ChimeCycleSeconds\" value=\"" << config.chimeCycleSeconds << "\"/> seconds<br/>\n";
  message << "<label>Chime inter-initial time</label>\n";
  message << "<input type=\"text\" name=\"ChimeInterInitialSeconds\" value=\"" << config.chimeInterInitialSeconds << "\"/> seconds<br/>\n";
  message << "<label>Chime enabled</label>\n";
  message << "<input type=\"checkbox\" name=\"ChimeEnabled\" " << (config.chimeEnabled ? "checked" : "") << "/><br/>\n";

  message << "<label>Heartbeat enabled</label>\n";
  message << "<input type=\"checkbox\" name=\"HeartbeatEnabled\" " << (config.heartbeatEnabled ? "checked" : "") << "/><br/>\n";
  message << "<input type=\"submit\" value=\"Save\"/>\n";
  message << "</form>\n";
  message << "<form action=\"/\" method=\"post\"><input type=\"submit\" value=\"Home\"/></form>\n";
  message << "</td>\n";
  message << "<td>\n";
  message << "<strong>Current config</strong><br/>\n";
  message << "<pre>\n";
  String configFileText;
  if (readConfigToString(configFileText)) {
    message << configFileText;
  } else {
    message << "Error reading config file!";
  }
  message << "</pre>\n";
  message << "</td></tr>\n";
  message << "</table>\n";
  message << "</body>\n</html>\n";
  
  server.send(200, "text/html", html);
}

void handleStats() {
  String message = "<html>\n<head>\n";
  message += "<title>";
  message += config.deviceDescription;
  message += " - Statistics</title>\n</head>\n<body>\n";
  message += "<h1>Statistics</h1>\n";
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
        TeeSerial0 << "handleStats: file open failed while opening /statistics.csv\n";
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

void handleDebugLog() {
  String message = "<html>\n<head>\n";
  message += "<title>";
  message += config.deviceDescription;
  message += " - Debug Log</title>\n</head>\n<body>\n";
  message += "<h1>Debug Log</h1>";
  message += "</body>\n</html>\n";
  
  String debugLog;
  yield();
  TeeSerial0.getBuffer(debugLog);
  message += "<pre>\n";
  message += debugLog;
  message += "</pre>\n";

  server.sendHeader("Refresh", "5; url=/debuglog");
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

// Debug functions, not used for now
void listSPIFFSDirectory() {
  TeeSerial0 << "Directory listing of /\n";
  Dir dir = SPIFFS.openDir("/");
  while (dir.next()) {
    TeeSerial0 << "  " << dir.fileName() << "\n";
  }
  TeeSerial0.flush();
}

void dumpSPIFFSInfo() {
FSInfo fs_info;
SPIFFS.info(fs_info);

TeeSerial0 << "Filesystem info\n";
TeeSerial0 << "Total bytes: " << fs_info.totalBytes << "\n";
TeeSerial0 << "Used bytes: " << fs_info.usedBytes << "\n";
TeeSerial0 << "Block size: " << fs_info.blockSize << "\n";
TeeSerial0 << "Page size: " << fs_info.pageSize << "\n";
TeeSerial0 << "Max open files: " << fs_info.maxOpenFiles << "\n";
TeeSerial0 << "Max path length: " << fs_info.maxPathLength << "\n";
TeeSerial0 << "\n";
TeeSerial0.flush();

listSPIFFSDirectory();
}

// Setup functions
void setupSPIFFS() {
  TeeSerial0 << "Starting SPIFFS\n";
  if (SPIFFS.begin()) {
    TeeSerial0 << "SPIFFS initialized\n";
  } else {
    TeeSerial0 << "SPIFFS initialization failed!\n";
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
  config.chimeStopTimeout = 65535;
  config.chimeNumber = 1;
  config.chimeCount = 4;
  config.chimeCycleSeconds = 6;
  config.chimeInterInitialSeconds = 2;
  config.chimeEnabled = true;
  config.heartbeatEnabled = true;
}

bool readConfigToString(String& configFileText) {
  File f = SPIFFS.open("/bmchimes.cfg", "r");
  if (!f) {
      TeeSerial0 << "readConfigToString: file open failed\n";
      return false;
  }
  while (f.available()) {
    yield();
    configFileText += f.readString();
  }
  f.close();
  return true;
}

bool readConfigFile() {
  bool result = false;

  TeeSerial0 << "Opening config file\n";
  File f = SPIFFS.open("/bmchimes.cfg", "r");
  if (!f) {
      TeeSerial0 << "readConfig: file open failed\n";
      return false;
  }
  TeeSerial0.flush();
  TeeSerial0 << "Beginning read of config file\n";
  TeeSerial0.flush();
  result = readConfigFromStream(f);
  TeeSerial0 << "Closing config file\n";
  TeeSerial0.flush();
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
    TeeSerial0 << "Read key " << key << "; hex: ";
    printStringAsHexDump(key);
    TeeSerial0 << "\n";
#endif

    String value = s.readStringUntil('\n');
    value.trim();
#ifdef DEBUG
    TeeSerial0 << "Read value " << value << "; hex: ";
    printStringAsHexDump(value);
    TeeSerial0 << "\n";
#endif

    TeeSerial0.flush();
    if (key == "DeviceDescription") {
      TeeSerial0 << "Setting Device Description " << value << "\n";
      config.deviceDescription = value;
    } else if (key == "WiFiSSID") {
      TeeSerial0 << "Setting WiFi SSID " << value << " (";
      printStringAsHexDump(value);
      TeeSerial0 << ")\n";
      config.wiFiSSID = value;
    } else if (key == "WiFiPassword") {
      TeeSerial0 << "Setting WiFi password " << value << " (";
      printStringAsHexDump(value);
      TeeSerial0 << ")\n";
      config.wiFiPassword = value;
    } else if (key == "WiFiMode") {
      // Either Station or AP
      TeeSerial0 << "Setting WiFi mode " << value << "\n";
      value.toUpperCase();
      if (value == "AP") {
        config.wiFiMode = WIFI_AP;
      } else if (value == "STATION") {
        config.wiFiMode = WIFI_STA;
      } else {
        TeeSerial0 << "Unknown WiFi mode " << value << "; defaulting to STATION" << "\n";
        config.wiFiMode = WIFI_STA;
      }
    } else if (key == "ConnectWiFiAtReset") {
      // true or false
      TeeSerial0 << "Setting connect WiFi at reset flag to " << value << "\n";
      value.toUpperCase();
      if (value == "TRUE") {
        config.connectWiFiAtReset = true;
      } else if (value == "FALSE") {
        config.connectWiFiAtReset = false;
      } else {
        TeeSerial0 << "Unknown boolean value " << value << "; defaulting to TRUE\n";
        config.connectWiFiAtReset = true;
      }
    } else if (key == "SleepEnabled") {
      // true or false
      TeeSerial0 << "Setting sleep enabled flag to " << value << "\n";
      value.toUpperCase();
      if (value == "TRUE") {
        config.sleepEnabled = true;
      } else if (value == "FALSE") {
        config.sleepEnabled = false;
      } else {
        TeeSerial0 << "Unknown boolean value " << value << "; defaulting to TRUE\n";
        config.sleepEnabled = true;
      }
    } else if (key == "WakeEveryNSeconds") {
      TeeSerial0 << "Setting wake every N seconds to " << value << "\n";
      config.wakeEveryNSeconds = value.toInt();
    } else if (key == "StayAwakeSeconds") {
      TeeSerial0 << "Setting stay awake seconds to " << value << "\n";
      config.stayAwakeSeconds = value.toInt();
    } else if (key == "ChimeEveryNSeconds") {
      TeeSerial0 << "Setting chime every N seconds to " << value << "\n";
      config.chimeEveryNSeconds = value.toInt();
    } else if (key == "ChimeNumber") {
      TeeSerial0 << "Setting chime number to " << value << "\n";
      config.chimeNumber = value.toInt();
    } else if (key == "ChimeCount") {
      TeeSerial0 << "Setting chime count to " << value << "\n";
      config.chimeCount = value.toInt();
    } else if (key == "ChimeCycleSeconds") {
      TeeSerial0 << "Setting chime cycle seconds to " << value << "\n";
      config.chimeCycleSeconds = value.toInt();
    } else if (key == "ChimeInterInitialSeconds") {
      TeeSerial0 << "Setting chime inter-initial seconds to " << value << "\n";
      config.chimeInterInitialSeconds = value.toInt();
    } else if (key == "ChimeStopTimeout") {
      if (value.toInt() > 65535) {
        TeeSerial0 << "Value " << value << " larger than maximum 65535. Limiting to 65535.\n";
        config.chimeStopTimeout = 65535;
      } else {
        TeeSerial0 << "Setting chime stop timeout to " << value << "\n";
        config.chimeStopTimeout = value.toInt();
      }
    } else if (key == "ChimeEnabled") {
      // true or false
      TeeSerial0 << "Setting chime enabled flag to " << value << "\n";
      value.toUpperCase();
      if (value == "TRUE") {
        config.chimeEnabled = true;
      } else if (value == "FALSE") {
        config.chimeEnabled = false;
      } else {
        TeeSerial0 << "Unknown boolean value " << value << "; defaulting to TRUE\n";
        config.chimeEnabled = true;
      }
    } else if (key == "HeartbeatEnabled") {
      // true or false
      TeeSerial0 << "Setting heartbeat enabled flag to " << value << "\n";
      value.toUpperCase();
      if (value == "TRUE") {
        config.heartbeatEnabled = true;
      } else if (value == "FALSE") {
        config.heartbeatEnabled = false;
      } else {
        TeeSerial0 << "Unknown boolean value " << value << "; defaulting to TRUE\n";
        config.heartbeatEnabled = true;
      }
    } else {
      TeeSerial0 << "Unknown configuration key " << key << "\n";
    }
    loops++;
  }
  TeeSerial0 << "Config read complete\n";
  return true;
}

void writeConfig() {
  TeeSerial0 << "writeConfig: Opening /bmchimes.cfg for writing" << "\n";
  File f = SPIFFS.open("/bmchimes.cfg", "w");
  if (!f) {
      TeeSerial0 << "writeConfig: file open failed" << "\n";
      return;
  }
  TeeSerial0 << "File successfully opened" << "\n";

  config.deviceDescription.trim();
  TeeSerial0 << "Writing device description: " << config.deviceDescription << "; hex: ";
  printStringAsHexDump(config.deviceDescription);
  TeeSerial0 << "\n";
  f << "DeviceDescription=" << config.deviceDescription << "\n";

  config.wiFiSSID.trim();
  TeeSerial0 << "Writing WiFiSSID: " << config.wiFiSSID << "; hex: ";
  printStringAsHexDump(config.wiFiSSID);
  TeeSerial0 << "\n";
  f << "WiFiSSID=" << config.wiFiSSID << "\n";

  config.wiFiPassword.trim();
  TeeSerial0 << "Writing WiFiPassword: " << config.wiFiPassword << "; hex: ";
  printStringAsHexDump(config.wiFiPassword);
  TeeSerial0 << "\n";
  f << "WiFiPassword=" << config.wiFiPassword << "\n";

  f << "WiFiMode=";
  if (config.wiFiMode == WIFI_AP) {
    f << "AP\n";
  } else if (config.wiFiMode == WIFI_STA) {
    f << "STATION\n";
  } else {
    f << "UNKNOWN\n";
  }
  f << "ConnectWiFiAtReset=" << (config.connectWiFiAtReset ? "TRUE" : "FALSE") << "\n";
  f << "SleepEnabled=" << (config.sleepEnabled ? "TRUE" : "FALSE") << "\n";
  f << "WakeEveryNSeconds=" << config.wakeEveryNSeconds << "\n";
  f << "StayAwakeSeconds=" << config.stayAwakeSeconds << "\n";
  f << "ChimeEveryNSeconds=" << config.chimeEveryNSeconds << "\n";
  f << "ChimeStopTimeout=" << config.chimeStopTimeout << "\n";
  f << "ChimeNumber=" << config.chimeNumber << "\n";
  f << "ChimeCount=" << config.chimeCount << "\n";
  f << "ChimeCycleSeconds=" << config.chimeCycleSeconds << "\n";
  f << "ChimeInterInitialSeconds=" << config.chimeInterInitialSeconds << "\n";
  f << "ChimeEnabled=" << (config.chimeEnabled ? "TRUE" : "FALSE") << "\n";
  f << "HeartbeatEnabled=" << (config.heartbeatEnabled ? "TRUE" : "FALSE") << "\n";
  f.close();
}

float batteryVoltage() {
  const float R7 = 10000.0;
  const float R8 = 665.0;
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
      TeeSerial0 << "collectStats: file open failed while opening /statistics.csv.old\n";
      // Probably our first run. OK to keep going.
  }
  File stats = SPIFFS.open("/statistics.csv", "w");
  if (!stats) {
      TeeSerial0 << "collectStats: file open failed while opening /statistics.csv\n";
      oldStats.close();
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
  TeeSerial0 << "Latest statistics line: ";
  TeeSerial0 << csvBlob;

  if (oldStats) {
    // If there are any old statistics to copy to the new file
    while (oldStats.available() && lines < statsLinesPerDay) {
      csvBlob += oldStats.readStringUntil('\n');
      csvBlob += "\n";
      lines++;
    }
    oldStats.close();
  }

  stats << csvBlob;
  stats.close();
}

void syncNTPTime() {
  TeeSerial0 << "Fetching time from NTP servers\n";
  // Pacific time zone hard coded
  configTime(-(8 * 3600), 3600, "pool.ntp.org", "time.nist.gov");
  // Wait up to a minute for NTP sync
  uint8_t attempts = 0;
  while (attempts <= 120 && !time(nullptr)) {
    TeeSerial0 << ".";
    delay(500);
    attempts++;
  }
  TeeSerial0 << "" << "\n";
  if (!time(nullptr)) {
    TeeSerial0 << "Failed to sync time with NTP servers!\n";
  } else {
    TeeSerial0 << "Successfully synced time with NTP servers.\n";
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
      TeeSerial0 << ".";
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      TeeSerial0 << "\n";
      TeeSerial0 << "Connected to " << config.wiFiSSID << "\n";
      TeeSerial0 << "IP address: " << WiFi.localIP() << "\n";
    } else {
      TeeSerial0 << "\n";
      TeeSerial0 << "Unable to connect to " << config.wiFiSSID << " network.  Giving up.\n";
    }
  } else if (config.wiFiMode == WIFI_AP) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    TeeSerial0 << "Starting WiFi network named " << config.wiFiSSID << "...";
    TeeSerial0.flush();
    if (config.wiFiPassword.length() > 0) {
      WiFi.softAP(ssid, password);
    } else {
      WiFi.softAP(ssid);
    }

    TeeSerial0 << "done.\n";
    TeeSerial0 << "My IP address is " << WiFi.softAPIP() << "\n";
  }
}

void writeNtpTimeToRtc() {
  TeeSerial0 << "Setting RTC time from NTP time\n";
  time_t now = time(nullptr);
  RtcDateTime ntpDateTime = RtcDateTime();
  ntpDateTime.InitWithEpoch32Time(now);

  Rtc.SetDateTime(ntpDateTime);
}

void rtcSetup() {
  Rtc.Begin();
  if (!Rtc.GetIsRunning()) {
    TeeSerial0 << "RTC was not actively running, starting now\n";
    Rtc.SetIsRunning(true);
  }
  Rtc.Enable32kHzPin(false);
  Rtc.SetSquareWavePin(DS3231SquareWavePin_ModeAlarmBoth); 
}

bool timeOverlapsSleepSchedule(time_t start, time_t end) {
  time_t lastScheduledChime = INVALID_TIME;
  for (int i = (MAX_CHIME_SCHEDULE-1); i >= 0; i--) {
    if (chimeSchedule[i] != INVALID_TIME) {
      lastScheduledChime = chimeSchedule[i];
      break;
    }
  }

  if (lastScheduledChime == INVALID_TIME) {
    // Schedule is apparently empty!
    return false;
  }

  if ((start >= chimeSchedule[0] && start <= lastScheduledChime) || (end <= lastScheduledChime && end >= chimeSchedule[0])) {
    return true;
  }

  return false;
}

void calculateSleepTiming(RtcDateTime& now) {
  RtcDateTime startingFrom = now;
  // Doesn't make sense for config.stayAwakeSeconds to be > than config.wakeEveryNSeconds; alert on this in config screen
  int sleepSchedulingAttempts = 0;
  uint16_t secondsToStayAwake = config.stayAwakeSeconds;
  uint16_t secondsTilNextWake, secondsTilWakeAfterNext;
  time_t startingFromTime, wakeTime, nextSleepTime, wakeTimeAfterNext;
  do {
    startingFromTime = startingFrom.Epoch32Time();
    secondsTilNextWake = secondsTilNextN(config.wakeEveryNSeconds / 60, now); // always on a minute boundary
    wakeTime = startingFromTime + secondsTilNextWake;
    nextSleepTime = wakeTime + secondsToStayAwake;
    secondsTilWakeAfterNext = secondsTilNextWake + config.wakeEveryNSeconds;
    wakeTimeAfterNext = startingFromTime + secondsTilWakeAfterNext;
    //time_t sleepTimeAfterNext = wakeTimeAfterNext + secondsToStayAwake;
    TeeSerial0 << "Sleep Scheduling Attempt #" << sleepSchedulingAttempts << "\n";
    TeeSerial0 << "--------------------------------\n";
    TeeSerial0 << "secondsToStayAwake = " << secondsToStayAwake << "\n";
    TeeSerial0 << "secondsTilNextWake = " << secondsTilNextWake << "\n";
    TeeSerial0 << "startingFromTime = " << startingFromTime << "\n";
    TeeSerial0 << "wakeTime = " << wakeTime << "\n";
    TeeSerial0 << "nextSleepTime = " << nextSleepTime << "\n";
    TeeSerial0 << "secondsTilWakeAfterNext = " << secondsTilWakeAfterNext << "\n";
    TeeSerial0 << "wakeTimeAfterNext = " << wakeTimeAfterNext << "\n";
    sleepSchedulingAttempts++;
    startingFrom.InitWithEpoch32Time(wakeTimeAfterNext+1);
  } while (sleepSchedulingAttempts < 3 && timeOverlapsSleepSchedule(nextSleepTime, wakeTimeAfterNext+config.chimeCycleSeconds));

  // set globals
  sleepDuration = wakeTimeAfterNext - nextSleepTime;
  wakeAlarmDateTime = RtcDateTime();
  wakeAlarmDateTime.InitWithEpoch32Time(wakeTime);
  sleepAlarmDateTime = RtcDateTime(nextSleepTime);
  sleepAlarmDateTime.InitWithEpoch32Time(nextSleepTime);
}

void scheduleChimeSequence(RtcDateTime& now) {
  // Clear any existing schedule.
  memset(chimeSchedule, 0xff, sizeof(chimeSchedule));

  time_t nowTime = now.Epoch32Time();
  time_t globalFirstStrikeTime = nowTime + secondsTilNextN(config.chimeEveryNSeconds / 60, now);
  RtcDateTime globalFirstStrikeDateTime = RtcDateTime();
  globalFirstStrikeDateTime.InitWithEpoch32Time(globalFirstStrikeTime);
  uint8_t twelveHour = (globalFirstStrikeDateTime.Hour() % 12 == 0 ? 12 : globalFirstStrikeDateTime.Hour() % 12);
  uint8_t slot = 0;

  for (int strike = 0; strike < twelveHour + 5; strike++) {
    switch (strike) {
      case 0:
      case 1:
      case 2:
        // Initial strikes; formula for offset from first strike time is
        // (strike + (chimeNumber - 1)) * interInitialStrikeDelaySeconds
        chimeSchedule[slot++] = globalFirstStrikeTime + ((strike + (config.chimeNumber - 1)) * config.chimeInterInitialSeconds) + (strike * config.chimeCycleSeconds);
        break;
      case 3:
      case 4:
        // These strikes are silent; don't increment the slot number in the
        // chime schedule, just increment the strike index
        break;
      default:
        // Hour chimes; formula for offset from first strike time is
        // strike * cycleTime
        chimeSchedule[slot++] = globalFirstStrikeTime + (strike * config.chimeCycleSeconds);
    }
  }
}

#ifdef FIFTEEN_MINUTE_CHIME
void fifteenMinuteChime(RtcDateTime& now) {
  // It's a fifteen minute interval, because that's the only time this function is called
  // However, it's up to us to determine whether striking now would interfere with the
  // chime schedule.
  time_t nowTime = now.Epoch32Time();
  // Test to see if this strike would fall within a minute of any scheduled chime; if so, skip it entirely.
  if (timeOverlapsSleepSchedule(nowTime - 30, (nowTime + config.chimeCycleSeconds) + 30)) {
    // Don't want to interfere with the schedule, just skip this chime.
    return;
  }

  // Kick off the strike!
  startChiming();
}
#endif

void calculateSleepAndChimeTiming() {
  RtcDateTime now = Rtc.GetDateTime();
  scheduleChimeSequence(now);
  calculateSleepTiming(now);
  TeeSerial0 << "calculateSleepAndChimeTiming():\n";
  dumpChimeSchedule();
}

void dumpChimeSchedule() {
  TeeSerial0 << "Chime Schedule\n";
  TeeSerial0 << "--------------------------------------\n";
  for (int i = 0; i < MAX_CHIME_SCHEDULE; i++) {
    RtcDateTime scheduledChimeDateTime = RtcDateTime();
    if (chimeSchedule[i] != 0xffffffff) {
      scheduledChimeDateTime.InitWithEpoch32Time(chimeSchedule[i]);
      String scheduledChimeString;
      dateTimeStringFromRtcDateTime(scheduledChimeDateTime, scheduledChimeString);
      TeeSerial0 << scheduledChimeString << "\n";
    }
  }
  TeeSerial0 << "\n";
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

  TeeSerial0 << "Time is now " << nowString << "\n";

  // Alarm one is the chime alarm
  DS3231AlarmOne alarmOne = DS3231AlarmOne(
    0, // dayOf; irrelevant with MinutesSecondsMatch
    chimeAlarmDateTime.Hour(),
    chimeAlarmDateTime.Minute(),
    chimeAlarmDateTime.Second(),
    DS3231AlarmOneControl_HoursMinutesSecondsMatch 
  );
  
  Rtc.SetAlarmOne(alarmOne);
  TeeSerial0 << "Chime scheduled for " << chimeAlarmDateTimeString << "\n";
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
    TeeSerial0 << "sleepDuration = " << sleepDuration << "\n";
    TeeSerial0 << "Sleep scheduled for " << sleepAlarmDateTimeString << "\n";
    TeeSerial0.flush();
  }
  
  Rtc.LatchAlarmsTriggeredFlags();
  attachInterrupt(digitalPinToInterrupt(rtcAlarmPin), alarmISR, FALLING);
}

void scheduleNextChime(RtcDateTime& now) {
  // Schedule next chime
  if (getNextChimeTime(now) == INVALID_TIME) {
    TeeSerial0 << "No valid next chime time found.  Recalculating chime schedule...\n";
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
}

void heartbeatSetup() {
  pinMode(heartbeatPin, OUTPUT);
  digitalWrite(heartbeatPin, LOW);
}

void otaUpdateSetup() {
  ArduinoOTA.setPort(8266);
  String otaHostname = config.deviceDescription;
  for (char c = ' '; c < 127; c++) {
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
      continue;
    otaHostname.replace(c, '-');
  }
  char otaHostnameBuf[64];
  otaHostname.toCharArray(otaHostnameBuf, 64);
  ArduinoOTA.setHostname(otaHostnameBuf);
  ArduinoOTA.setPassword((const char *)"dingdong!"); // Yeah, lame, I know.

  ArduinoOTA.onStart([]() {
    TeeSerial0 << "OTA update starting!\n";
    TeeSerial0 << "Stopping any chime in progress...\n";
    stopChiming();
    TeeSerial0 << "Saving chime enabled state...\n";
    chimingWasEnabled = config.chimeEnabled;
    TeeSerial0 << "Disabling chimes during update...\n";
    config.chimeEnabled = false;
  });
  ArduinoOTA.onEnd([]() {
    TeeSerial0 << "OTA update complete!\n";
    TeeSerial0 << "Restoring chime enabled state...\n";
    config.chimeEnabled = chimingWasEnabled;
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    if ((progress / (total / 100)) % 10 == 0) {
      TeeSerial0 << "OTA update in progress: " << (progress / (total / 100)) << "% complete\n";
    }
  });
  ArduinoOTA.onError([](ota_error_t error) {
    TeeSerial0 << "OTA update error #" << error << ": ";
    if (error == OTA_AUTH_ERROR) TeeSerial0 << "Auth Failed";
    else if (error == OTA_BEGIN_ERROR) TeeSerial0 << "Begin Failed";
    else if (error == OTA_CONNECT_ERROR) TeeSerial0 << "Connect Failed";
    else if (error == OTA_RECEIVE_ERROR) TeeSerial0 << "Receive Failed";
    else if (error == OTA_END_ERROR) TeeSerial0 << "End Failed";
    TeeSerial0 << "\n";
    TeeSerial0 << "Restoring chime enabled state...\n";
    config.chimeEnabled = chimingWasEnabled;
  });
  ArduinoOTA.begin();
}

void basicSetup() {
  pinMode(rtcAlarmPin, INPUT);
  TeeSerial0.begin(74880);
  TeeSerial0 << "\n";
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
  server.on("/debuglog", handleDebugLog);
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
  TeeSerial0 << "HTTP server started\n";
}

void setup(void) {
  basicSetup();
  setupSPIFFS();
  if (!readConfigFile()) {
    TeeSerial0 << "Config read failed, setting defaults\n";
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
  otaUpdateSetup();
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
      TeeSerial0 << "Chime alarm fired!\n";
      dateTimeStringFromRtcDateTime(now, nowString);
      TeeSerial0 << "Time is now " << nowString << "\n";
      if (!chiming) {
        startChiming();
      } else {
        // Hopefully we only get here if someone has triggered a manual chime.
        TeeSerial0 << "Chiming already in progress, skipping scheduled chime.\n";
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
        TeeSerial0 << "Time is now " << nowString << "\n";
        if (sleepDuration == 0) {
          // Sanity check; if we sleep for 0 seconds, the ESP never wakes up.
          TeeSerial0 << "Sleep duration is zero, skipping sleep.\n";
        } else {
          TeeSerial0 << "Sleeping for " << sleepDuration << " seconds\n";
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

#ifdef FIFTEEN_MINUTE_CHIME
      // Single strike at the 15 minute mark, unless it would overlap with the chime schedule
      if (now.Minute() % 15 == 0) {
        fifteenMinuteChime(now);
      }
#endif
    }
  }

  // Check time every loop, turn off chime GPIO if timeout
  if (chiming) {
    // Check for stop conditions (timeout, stop switch)
    if (chimeStopSwitchFlag || (millis() - chimeStartMillis > config.chimeStopTimeout)) {
      // Check chime home switch GPIO flag (set by interrupt), turn off chime GPIO if set
      if (chimeStopSwitchFlag) {
        TeeSerial0 << "Chime stop switch activated, turning off chime motor\n";
      } else {
        TeeSerial0 << "Timeout waiting for chime stop switch activation!\n";
      }
      stopChiming();
      RtcDateTime now = Rtc.GetDateTime();
      scheduleNextChime(now);
    }
  } else {
    if (shouldCollectStats) {
      collectStats();
      shouldCollectStats = false;
    }
  }
  
  server.handleClient();
  ArduinoOTA.handle();

  if (config.heartbeatEnabled) {
    heartbeat();
  }
}
