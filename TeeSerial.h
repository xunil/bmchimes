#ifndef _TEE_SERIAL_H_INCLUDED_
#define _TEE_SERIAL_H_INCLUDED_

#include "HardwareSerial.h"

#define TEE_BUFFER_SIZE 1024

class TeeSerial : public HardwareSerial {
public:
  TeeSerial(int uart_nr) : HardwareSerial(uart_nr), start(0), length(0), position(0) { }
  size_t write(uint8_t) override;
  void getBuffer(String& str);

private:
  char teeBuffer[TEE_BUFFER_SIZE];
  int start;
  int length;
  int position;
};

extern TeeSerial TeeSerial0;
#endif // _TEE_SERIAL_H_INCLUDED_
