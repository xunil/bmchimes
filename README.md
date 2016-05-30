Controller firmware for ESP8266 modules for Burning Man chimes project.

Using RtcDS3231 library https://github.com/Makuna/Rtc/

TODO:
* Add chime functionality
  * Just need to add config option to control chime timeout
* WiFi improvements
  * Add config keys for at least 3 WiFi networks and SSIDs
  * Attempt to join each in series for 20 seconds at a time
  * If all three fail, become AP instead
  * Add option to go straight to AP mode
* Add ADC measurement and recording for battery voltage
