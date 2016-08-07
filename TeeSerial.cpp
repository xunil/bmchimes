#include "TeeSerial.h"

size_t TeeSerial::write(uint8_t c) {
  HardwareSerial::write(c);

  teeBuffer[position] = c;
  length = (length + 1 < TEE_BUFFER_SIZE ? length + 1 : TEE_BUFFER_SIZE);
  position = (position + 1) % TEE_BUFFER_SIZE;
  if (length == TEE_BUFFER_SIZE) {
    start = position;
  }
}

void TeeSerial::getBuffer(String& str) {
  // One additional byte so the string ends in a null.
  char tmpBuffer[TEE_BUFFER_SIZE+1];

  memset(tmpBuffer, 0, TEE_BUFFER_SIZE+1);
  memcpy(tmpBuffer, teeBuffer+start, TEE_BUFFER_SIZE-start);
  if (start > 0) {
    memcpy(tmpBuffer+(TEE_BUFFER_SIZE-start), teeBuffer, start);
  }

  str = String(tmpBuffer);
}

TeeSerial TeeSerial0(UART0);
