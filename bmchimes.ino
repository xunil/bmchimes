#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Wire.h>
#include <time.h>

#define DS3231_I2C_ADDRESS 0x68

struct Mydatetime {
        byte second;
        byte minute;
        byte hour;
        byte day;
        byte month;
        byte year;
        byte dow;
} datetime;

const char* ssid = "nestsux";
const char* password = "yarly";
const int led = 2;

ESP8266WebServer server(80);

byte decToBcd(byte val){
    return( (val/10*16) + (val%10) );
}
 
byte bcdToDec(byte val){
    return( (val/16*10) + (val%16) );
}

void getRtcTime(struct Mydatetime *dt) {
  Wire.beginTransmission(DS3231_I2C_ADDRESS);
  Wire.write(0);
  Wire.endTransmission();
  Wire.requestFrom(DS3231_I2C_ADDRESS, 7);
  dt->second = bcdToDec(Wire.read());
  dt->minute = bcdToDec(Wire.read());
  dt->hour = bcdToDec(Wire.read() & 0x3f);
  dt->dow = Wire.read();
  dt->day = bcdToDec(Wire.read());
  dt->month = bcdToDec(Wire.read() & 0x1f);
  dt->year = bcdToDec(Wire.read());
}
 
void setRtcTime(struct Mydatetime *dt) {
  Wire.beginTransmission(DS3231_I2C_ADDRESS);
  Wire.write(0);
  Wire.write(decToBcd(dt->second & 0x7F));
  Wire.write(decToBcd(dt->minute & 0x7F));
  Wire.write(decToBcd(dt->hour & 0x3F));
  Wire.write(dt->dow & 0x07);
  Wire.write(decToBcd(dt->day & 0x3F));
  Wire.write(decToBcd(dt->month & 0x1F));
  Wire.write(decToBcd(dt->year));
  Wire.endTransmission();
}

float getDieTemp() {
  Wire.beginTransmission(DS3231_I2C_ADDRESS);
  Wire.write(0x11);
  Wire.endTransmission();
  Wire.requestFrom(DS3231_I2C_ADDRESS, 2);
  int temp2 = Wire.read();
  temp2 <<= 2;
  temp2 |= (Wire.read() >> 6);
  return (float)temp2/4;                        
}

void handleRoot() {
  digitalWrite(led, 1);
  server.send(200, "text/plain", "hello from esp8266!");
  digitalWrite(led, 0);
}

void handleTemp() {
  float dieTemperature = (float)getDieTemp();
  float dieTempF = dieTemperature*(9/5)+32;
  
  digitalWrite(led, 1);
  String message = "<html>\n<head>\n\t<title>Temperature</title>\n</head>\n<body>\n";
  message += "<h1>Die temperature ";
  message += dieTemperature;
  message += "&deg;F</h1>\n";
  message += "</body>\n</html>\n";
  
  server.send(200, "text/html", message);
  digitalWrite(led, 0);  
}

void handleTime() {
  // TODO: Handle arguments to trigger NTP sync
  struct Mydatetime dt;
  getRtcTime(&dt);
  String message = "<html>\n<head>\n\t<title>RTC Time</title>\n</head>\n<body>\n";
  message += "<h1>At the tone, the time will be ";
  message += dt.year + 2000;
  message += "/";
  message += dt.month;
  message += "/";
  message += dt.day;
  message += " ";
  message += dt.hour;
  message += ":";
  message += dt.minute;
  message += ":";
  message += dt.second;
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

void storeNTPTimeToRTC() {
  struct Mydatetime dt;
  struct tm *test;

  Serial.println("Setting RTC time from NTP time");

  time_t now = time(nullptr);
  test  = localtime(&now);
  String message = "Time received from NTP is ";
  message += test->tm_year;
  message += "/";
  message += test->tm_mon;
  message += "/";
  message += test->tm_mday;
  message += " ";
  message += test->tm_hour;
  message += ":";
  message += test->tm_min;
  message += ":";
  message += test->tm_sec;
  message += "\n";
  Serial.print(message);

  dt.year = test->tm_year - 100;
  dt.month = test->tm_mon + 1;
  dt.day = test->tm_mday;
  dt.hour = test->tm_hour;
  dt.minute = test->tm_min;
  dt.second = test->tm_sec;

  setRtcTime(&dt);
  Serial.println("Finished setting RTC time");
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

void basicSetup() {
  pinMode(led, OUTPUT);
  digitalWrite(led, LOW);
  Serial.begin(115200);
  Wire.begin();
}

void setup(void){
  // Simple things like turning on serial, I2C, status LED
  basicSetup();

  // Connect to a WiFi network
  connectToWifi();
  
  // Sync with NTP
  syncNTPTime();
  
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

void loop(void){
  server.handleClient();
}
