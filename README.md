Controller firmware for ESP8266 modules for Burning Man chimes project.

TODO:
* Add sleep functionality
  * On boot, figure out how long until next chime
  * Figure out how long until next sleep
  * If seconds to next chime is less than seconds to next sleep, skip next sleep
  * Status page should indicate time until next sleep, next chime
  * Set RTC alarm for hh:mm:ss of next sleep
  * Add ISR to trigger on RTC alarm, set sleep flag
  * Check sleep flag every loop() iteration
* Add heartbeat LED: Blip every few seconds in loop()
* Add chime functionality
  * Add option to chime on specific minute, or on interval from start of hour; will ease testing
  * Chime GPIO output and input (input indicates stop switch tripped)
  * GPIO output should have timeout; if we haven't received a stop signal in N seconds (30? 60?), something is probably wrong
  * Test SSR
* WiFi improvements
  * Add config keys for at least 3 WiFi networks and SSIDs
  * Attempt to join each in series for 20 seconds at a time
  * If all three fail, become AP instead
  * Add option to go straight to AP mode
* Add ADC measurement and recording for battery voltage
* Buy LIR2032 batteries for RTCs
* Set system clock from RTC by default
  * Also add button to set system clock from RTC
