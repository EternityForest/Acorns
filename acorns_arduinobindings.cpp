#include "acorns.h"
#include "Arduino.h"

/*
static SQInteger sqserial_begin(HSQUIRRELVM v)
{

  HardwareSerial * s = 0;
 
  SQInteger baud =9600;

  SQInteger rxpin = 0;
  SQInteger txpin = 0;
  SQInteger i = sq_gettop(v);

  if(i>1)
  {
    sq_getinteger(v,2, * baud);
  }


  if(i>3)
  {
    sq_getinteger(v,4, * rxpin);
  } 
  if(i>4)
  {
    sq_getinteger(v,5, * txpin);
  }
  sq_getinstanceup(v, 1,&s,0);

  //Just to be safe, use the config.
  s->begin(baud,SERIAL_8N2, rxpin, txpin);
}*/

static SQInteger sqmillis(HSQUIRRELVM v)
{
  sq_pushinteger(v, millis());
  return(1);
}

//
static SQInteger sqmicros(HSQUIRRELVM v)
{
  sq_pushinteger(v, micros());
  return(1);
}

static SQInteger sqdelay(HSQUIRRELVM v)
{
 SQInteger i = sq_gettop(v);
  SQInteger d =0;
  if(i==2)
  {
    sq_getinteger(v, 2, &d);
   
    //Delay for the given number of milliseconds
    GIL_UNLOCK;
    delay(d);
    GIL_LOCK;
    return 0;
  }
  return SQ_ERROR;
}


static SQInteger sqanalogread(HSQUIRRELVM v)
{
 SQInteger i = sq_gettop(v);
  SQInteger d =0;
  if(i==2)
  {
    sq_getinteger(v, 2, &d);
   
    //Delay for the given number of milliseconds
    sq_pushinteger(v,  analogRead(d));
    return 1;
  }
  return SQ_ERROR;
}

static SQInteger sqdigitalread(HSQUIRRELVM v)
{
 SQInteger i = sq_gettop(v);
  SQInteger d =0;
  if(i==2)
  {
    sq_getinteger(v, 2, &d);
   
    //Delay for the given number of milliseconds
    sq_pushinteger(v, digitalRead(d));
    return 1;
  }
  return SQ_ERROR;
}

static SQInteger sqdigitalwrite(HSQUIRRELVM v)
{
 SQInteger i = sq_gettop(v);
  SQInteger d =0;
  SQInteger val=0;
  if(i==3)
  {
    sq_getinteger(v, 2, &d);
    sq_getinteger(v, 3, &val);
    digitalWrite(d,val);
    return 0;
  }
  return SQ_ERROR;
}

static SQInteger sqpinmode(HSQUIRRELVM v)
{
 SQInteger i = sq_gettop(v);
  SQInteger d =0;
  SQInteger val=0;
  if(i==3)
  {
    sq_getinteger(v, 2, &d);
    sq_getinteger(v, 3, &val);
    pinMode(d,val);
    return 0;
  }
  return SQ_ERROR;
}

void _Acorns::addArduino()
{
  registerFunction(0, sqdelay,"delay");
  registerFunction(0, sqmicros,"micros");
  registerFunction(0, sqmillis, "millis");
  registerFunction(0, sqdigitalread, "digitalRead");
  registerFunction(0, sqanalogread, "analogRead");
  registerFunction(0, sqdigitalwrite, "digitalWrite");
  registerFunction(0, sqpinmode,"pinMode");
}