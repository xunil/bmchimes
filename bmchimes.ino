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

struct BMChimeConfig {
  String wifiSSID;
  String wifiPassword;
  // From ESP8266WiFi.h
  WiFiMode wifiMode;
  bool connectWifiAtReset;
  bool syncNTPAtReset;
  uint8_t wakeEveryN;
  uint8_t chimeEveryN; 
} config;

char ssid[64];
char password[64];
const int led = 2;

RtcDS3231 Rtc;
ESP8266WebServer server(80);

// Web server handler functions

void handleRoot() {
  digitalWrite(led, 1);
  server.send(200, "text/plain", "hello from esp8266!");
  digitalWrite(led, 0);
}

void handleTemp() {
  RtcTemperature dieTemp = Rtc.GetTemperature();
  float dieTempF = dieTemp.AsFloat()*(9/5)+32;
  
  digitalWrite(led, 1);
  String message = "<html>\n<head>\n\t<title>Temperature</title>\n</head>\n<body>\n";
  message += "<h1>Die temperature ";
  message += dieTempF;
  message += "&deg;F</h1>\n";
  message += "</body>\n</html>\n";
  
  server.send(200, "text/html", message);
  digitalWrite(led, 0);  
}

void handleTime() {
  RtcDateTime now = Rtc.GetDateTime();

  String message = "<html>\n<head>\n\t<title>RTC Time</title>\n</head>\n<body>\n";
  message += "<h1>At the tone, the time will be ";
  message += now.Year();
  message += "/";
  message += now.Month();
  message += "/";
  message += now.Day();
  message += " ";
  message += now.Hour();
  message += ":";
  message += now.Minute();
  message += ":";
  message += now.Second();
  message += "\n";
  message += "</body>\n</html>\n";

  server.send(200, "text/html", message);
}

void handleConfig() {
  String message = "<html>\n<head>\n\t<title>Configure</title>\n</head>\n<body>\n";
  message += "<h1>Configuration</h1>";
  message += "<form action=\"/config\" method=\"post\">\n";
  message += "<label>WiFi SSID <input type=\"text\" name=\"wifiSSID\" value=\"";
  message += config.wifiSSID;
  message += "\"/></label><br/>\n";
  message += "<label>WiFi Password <input type=\"text\" name=\"wifiPassword\" value=\"";
  message += config.wifiPassword;
  message += "\"/></label><br/>\n";
  message += "<label>Wifi Mode <select name=\"wifiMode\">\n";
  message += "<option value=\"AP\" ";
  if (config.wifiMode == WIFI_AP) {
    message += "selected";
  }
  message += ">Access Point</option>\n";
  message += "<option value=\"STATION\" ";
  if (config.wifiMode == WIFI_STA) {
    message += "selected";
  }
  message += ">Station</option>\n";
  message += "</select></label><br/>\n";
  message += "<label>Connect to WiFi at reset <input type=\"checkbox\" name=\"connectWifiAtReset\" ";
  if (config.connectWifiAtReset) {
    message += "checked";
  }
  message += "/></label><br/>\n";
  message += "<label>Sync with NTP at reset <input type=\"checkbox\" name=\"syncNTPAtReset\" ";
  if (config.syncNTPAtReset) {
    message += "checked";
  }
  message += "/></label><br/>\n";
  message += "<label>Wake every <input type=\"text\" name=\"wakeEveryN\" value=\"";
  message += config.wakeEveryN;
  message += "\"/> minutes</label><br/>\n";
  message += "<label>Chime every <input type=\"text\" name=\"chimeEveryN\" value=\"";
  message += config.chimeEveryN;
  message += "\"/> minutes</label><br/>\n";
  message += "<input type=\"submit\" value=\"Save\"/>\n";
  message += "</form>\n";
  message += "</body>\n</html>\n";
  
  server.send(200, "text/html", message);
}

void handleNotFound(){
  digitalWrite(led, 1);
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
  digitalWrite(led, 0);
}

// Setup functions

void readConfig() {
  Serial.println("Starting SPIFFS");
  if (SPIFFS.begin()) {
    Serial.println("SPIFFS initialized");
  } else {
    Serial.println("SPIFFS initialization failed!");
    return;
  }
  Serial.flush();

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
  while (loops < 10 && f.available()) {
    yield();
    Serial.flush();
    String key = f.readStringUntil('=');
    Serial.print("Read key ");
    Serial.println(key);
    String value = f.readStringUntil('\n');
    Serial.print("Read value ");
    Serial.println(value);
    Serial.flush();
    if (key == "WifiSSID") {
      Serial.print("Setting wifi SSID ");
      Serial.println(value);
      config.wifiSSID = value;
    } else if (key == "WifiPassword") {
      Serial.print("Setting wifi password ");
      Serial.println(value);
      config.wifiPassword = value;
    } else if (key == "WifiMode") {
      // Either Station or AP
      Serial.print("Setting wifi mode ");
      Serial.println(value);
      value.toUpperCase();
      if (value == "AP") {
        config.wifiMode = WIFI_AP;
      } else if (value == "STATION") {
        config.wifiMode = WIFI_STA;
      } else {
        Serial.print("Unknown wifi mode ");
        Serial.print(value);
        Serial.println("; defaulting to STATION");
        config.wifiMode = WIFI_STA;
      }
    } else if (key == "ConnectWifiAtReset") {
      // true or false
      Serial.print("Setting connect wifi at reset flag to ");
      Serial.println(value);
      value.toUpperCase();
      if (value == "TRUE") {
        config.connectWifiAtReset = true;
      } else if (value == "FALSE") {
        config.connectWifiAtReset = false;
      } else {
        Serial.print("Unknown boolean value ");
        Serial.print(value);
        Serial.println("; defaulting to TRUE");
        config.connectWifiAtReset = true;
      }
    } else if (key == "SyncNTPAtReset") {
      // true or false
      Serial.print("Setting sync NTP at reset flag to ");
      Serial.println(value);
      value.toUpperCase();
      if (value == "TRUE") {
        config.syncNTPAtReset = true;
      } else if (value == "FALSE") {
        config.syncNTPAtReset = false;
      } else {
        Serial.print("Unknown boolean value ");
        Serial.print(value);
        Serial.println("; defaulting to TRUE");
        config.syncNTPAtReset = true;
      }
    } else if (key == "WakeEveryN") {
      // Use parseInt to determine wake schedule
      Serial.print("Setting wake every N to ");
      Serial.println(value);
      config.wakeEveryN = value.toInt();
    } else if (key == "ChimeEveryN") {
      // Use parseInt to determine chime schedule
      Serial.print("Setting chime every N to ");
      Serial.println(value);
      config.chimeEveryN = value.toInt();
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
  SPIFFS.begin();
  File f = SPIFFS.open("/bmchimes.cfg", "w");
  if (!f) {
      Serial.println("writeConfig: file open failed");
      return;
  }
  f.print("WifiSSID=");
  f.println(config.wifiSSID);
  f.print("WifiPassword=");
  f.println(config.wifiPassword);
  f.print("WifiMode=");
  if (config.wifiMode == WIFI_AP) {
    f.println("AP");
  } else if (config.wifiMode == WIFI_STA) {
    f.println("STATION");
  } else {
    f.println("UNKNOWN");
  }
  f.print("ConnectWifiAtReset=");
  if (config.connectWifiAtReset == true) {
    f.println("TRUE");
  } else {
    f.println("FALSE");
  }
  f.print("SyncNTPAtReset=");
  if (config.syncNTPAtReset == true) {
    f.println("TRUE");
  } else {
    f.println("FALSE");
  }
  f.print("WakeEveryN=");
  f.println(config.wakeEveryN);
  f.print("ChimeEveryN=");
  f.println(config.chimeEveryN);
  f.close();
}

void syncNTPTime() {
  Serial.println("Fetching time from NTP servers");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  // TODO: Add a timeout so this doesn't loop forever
  while (!time(nullptr)) {
    Serial.print(".");
    delay(1000);
  }
  Serial.println("");
}

void connectToWifi() {
  config.wifiSSID.toCharArray(ssid, 64);
  config.wifiPassword.toCharArray(password, 64);
  WiFi.begin(ssid, password);
  Serial.println("");

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(config.wifiSSID);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  if (MDNS.begin("esp8266")) {
    Serial.println("MDNS responder started");
  }
}

void rtcSetup() {
  Rtc.Begin();
  if (!Rtc.IsDateTimeValid()) {
    Serial.println("RTC date/time invalid, will set from local clock.");

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

  if (!Rtc.GetIsRunning()) {
    Serial.println("RTC was not actively running, starting now");
    Rtc.SetIsRunning(true);
  }

  Rtc.Enable32kHzPin(false);
  Rtc.SetSquareWavePin(DS3231SquareWavePin_ModeNone); 
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
  readConfig();
  if (config.connectWifiAtReset) {
    connectToWifi();
    if (config.syncNTPAtReset) {
      syncNTPTime();
    }
  }

  // TODO: Pull system clock sync to RTC out of this function
  rtcSetup();
  startWebServer();
}

void loop(void) {
  server.handleClient();
}
