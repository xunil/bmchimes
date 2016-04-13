Controller firmware for ESP8266 modules for Burning Man chimes project.

TODO:
* Add sleep functionality
  * On boot, figure out how long until next chime
  * Figure out how long until next sleep
  * If seconds to next chime is less than seconds to next sleep, skip next sleep
  * Status page should indicate time until next sleep, next chime
* Add chime functionality
  * Add option to chime on specific minute, or on interval from start of hour; will ease testing
  * Chime GPIO output and input (input indicates stop switch tripped)
  * GPIO output should have timeout; if we haven't received a stop signal in N seconds (30? 60?), something is probably wrong
* Test WiFi AP mode
* Add ADC measurement and recording for battery voltage
