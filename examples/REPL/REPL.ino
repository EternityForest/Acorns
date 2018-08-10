/*
  Squirrel REPL! Load this on an ESP32, and type some squirrel code in the serial monitor!
*/
#include "acorns.h"




void setup() {
  Serial.begin(115200);
  Serial.println("**starting up**");
  Acorns.begin();




}

// the loop function runs over and over again forever
void loop() {

  if (Serial.available())
  {
    Acorns.replChar(Serial.read());
  }
}
