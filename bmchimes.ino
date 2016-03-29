#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Wire.h>
#include <time.h>

#if defined(ESP8266)
#include <pgmspace.h>
#else
#include <avr/pgmspace.h>
#endif
#include <RtcDS3231.h>


const char* ssid = "nestsux";
const char* password = "yarly";
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
  WiFi.begin(ssid, password);
  Serial.println("");

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
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
  Wire.begin();
}

void startWebServer() {
  // Set up handlers for different pages
  server.on("/time", handleTime);
  server.on("/temp", handleTemp);
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
  connectToWifi();
  syncNTPTime();
  rtcSetup();
  startWebServer();
}

void loop(void) {
  server.handleClient();
}
