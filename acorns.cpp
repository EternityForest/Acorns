/*Copyright (c) 2018 Daniel Dunn(except noted parts)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.*/

#include "Arduino.h"
#ifdef INC_FREERTOS_H
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif


#include "acorns.h"
#include "minIni.h"

#ifdef ESP32
#include <WiFi.h>
#include <ESPmDNS.h>
#include <dirent.h>
#include <SPIFFS.h>
#endif

#ifdef ESP8266
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <FS.h>
#define esp_random() secureRandom(65536L)
#include "posix_compat.h"
#endif

/************************************************************************************************************/
//Data Structures, forward declarations

//Represents a request to the thread pool to execute the given function
//with the loadedProgram and the arg as its params.
struct Request
{
  void (*f)(loadedProgram *, void *);
  //Pointer tp the target of the request
  struct loadedProgram *program;
  //Object that represents what the interpreter should do.
  //If it === interpreter, it means run loaded code
  void *arg;
};

///declarations

static void deref_prog(loadedProgram *);
static struct loadedProgram *_programForId(const char *id);

/***************************************************/
//The GIL

//The global interpreter lock. Almost any messing
//with of interpreters uses this.
#ifdef INC_FREERTOS_H
SemaphoreHandle_t _acorns_gil_lock;
#endif

//This gets called every 250 instructions in long running squirrel programs to other threads can do things.
void sq_threadyield()
{
  GIL_UNLOCK;
  GIL_LOCK;
}

//When setting the GIL we also set the active program.
//This value is not valid when the GIL is unlocked.
//It is also invalid when there isn't a logical "running program".
//It's purpose is to know what program is running if they all
//share an interpreter.
static struct loadedProgram *activeProgram=0;

//Allows all interpeters to share a context.
static bool sharedMode = false;

void _Acorns::setShared(bool b)
{
  sharedMode=b;
}
/*********************************************************************/
//Random number generation

static uint64_t entropy = 88172645463325252LL;
static uint64_t rng_key = 787987897897LL;

/*
static unsigned long long doRandom(){
  //Every time we call this function, mix in some randomness. We could use the ESP prng,
  //But that's less portable, and we want 64 bits, and I'm not sure what performance is like there.
  //Instead we seed from that occasionally, and continually reseed from micros().
  entropy += micros();
  entropy^=(entropy<<13); entropy^=(entropy>>7); return (entropy^=(entropy<<17));
}
*/

//This function was modified for Acorns.
// *Really* minimal PCG32 code / (c) 2014 M.E. O'Neill / pcg-random.org
// Licensed under Apache License 2.0 (NO WARRANTY, etc. see website)
static uint32_t doRandom()
{
  //This is the modified line, for continual reseeding.
  entropy += micros();

  uint64_t oldstate = entropy;
  // Advance internal state
  entropy = oldstate * 6364136223846793005ULL + (rng_key | 1);
  // Calculate output function (XSH RR), uses old state for max ILP
  uint32_t xorshifted = ((oldstate >> 18u) ^ oldstate) >> 27u;
  uint32_t rot = oldstate >> 59u;
  return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

static SQInteger sqrandom(HSQUIRRELVM v)
{
  SQInteger i = sq_gettop(v);
  SQInteger mn = 0;
  SQInteger mx = 0;
  if (i == 2)
  {
    //Wrong and bad way to generate random numbers. There's a tiny bias if the range
    //isn't a divisor of 2**64. But in practice, 2**64 is big and
    //this isn't for security purposes anyway.
    sq_getinteger(v, 2, &mx);
    sq_pushinteger(v, doRandom() % mx);
    return 1;
  }
  if (i == 3)
  {
    sq_getinteger(v, 2, &mn);
    sq_getinteger(v, 3, &mx);
    sq_pushinteger(v, (doRandom() % mx) + mn);
    return 1;
  }

  //Wrong number of params
  return SQ_ERROR;
}

//***************************/
String readprogstr(const char *ifsh)
{
  const char *p = ifsh;
  int i = 0;
  uint8_t c = 0;

  int len = 0;
  if(ifsh == 0)
  {
    return "NULLPTR";
  }

  do
  {
    //No we cannot just decalare that var the right type.
    //It gives an error on the 8266, which I'm still trying to support
    c = pgm_read_byte((const char PROGMEM *)(p++));
    len++;
  } while (c != 0);

  char *buf = (char *)malloc(len + 2);
  if (buf == 0)
  {
    return "MALLOCERR";
  }

  p = ifsh;
  do
  {
    //No we cannot just decalare that var the right type.
    //It gives an error on the 8266, which I'm still trying to support
    c = pgm_read_byte((const char PROGMEM *)(p++));
    buf[i++] = c;
  } while (c != 0);
  String s = String(buf);
  free(buf);
  return s;
}

/***********************************************************************/
//Directory listing

static HSQOBJECT DirEntryObj;

static SQInteger sqdirectoryiterator(HSQUIRRELVM v)
{

  //Points to the userdata, but that userdata is actually a dir pointer
  DIR **d;
  const char *dirname = 0;

  if (sq_getstring(v, 2, &dirname) == SQ_ERROR)
  {
    sq_throwerror_f(v, F("dir requires one string parameter."));
    return SQ_ERROR;
  }

  //The packed data has the dir name in it after the dir pointer
  sq_newuserdata(v, sizeof(DIR *));
  sq_getuserdata(v, -1, (void **)&d, 0);
  sq_pushobject(v, DirEntryObj);
  sq_setdelegate(v, -2);

  *d = opendir(dirname);

  if (*d == 0)
  {
    return sq_throwerror_f(v, F("Could not open directory"));
  }
  return 1;
}

//Get is a passthrough
static SQInteger sqdirectoryiterator_get(HSQUIRRELVM v)
{
  return 1;
}

static SQInteger sqdirectoryiterator_next(HSQUIRRELVM v)
{
  DIR **d;
  struct dirent *de;

  if (sq_getuserdata(v, 1, (void **)&d, 0) == SQ_ERROR)
  {
    return SQ_ERROR;
  }

  if (*d == 0)
  {
    return sq_throwerror_f(v, F("This directory object is invalid or has been closed"));
  }
  de = readdir(*d);

  if (de)
  {
    sq_pushstring(v, de->d_name, -1);
  }
  else
  {
    sq_pushnull(v);
    closedir(*d);
  }
  return 1;
}

static SQInteger dir_release_hook(SQUserPointer p, SQInteger size)
{
  if (*((DIR **)p) == 0)
  {
    return 0;
  }
  closedir(*((DIR **)(p)));
  *((DIR **)(p)) = 0;
  return 1;
}

/************************************************************************/
//Quotes system

static const char quote_0[] PROGMEM = ("\"The men waited some time at the outpost.\"");
static const char quote_1[] PROGMEM = ("\"This road is longer for some than others.\"");
static const char quote_2[] PROGMEM = ("\"He carefully packed his travelsack before setting out.\"");
static const char quote_3[] PROGMEM = ("\"His staff had been with him on many adventures.\"");
static const char quote_4[] PROGMEM = ("\"From the top of the hill, he could see for miles.\"");
static const char quote_5[] PROGMEM = ("\"She knew better than the others why the river was dry.\"");
static const char quote_6[] PROGMEM = ("\"Only the fireflies lit the path as they made their way through the dark forest.\"");
static const char quote_7[] PROGMEM = ("\"The treasure they sought had been buried years ago.\"");
static const char quote_8[] PROGMEM = ("\"The stone glowed faintly when they passed by the door.\"");
static const char quote_9[] PROGMEM = ("\"The mountain rose before them at the end of the path.\"");
static const char quote_10[] PROGMEM = ("\"Her mother had warned her about this road.\"");
static const char quote_11[] PROGMEM = ("\"The Caravansarai was still miles ahead.\"");
static const char quote_12[] PROGMEM = ("\"His cloak was well-worn and had many small pockets\"");
static const char quote_13[] PROGMEM = ("\"Roads go ever ever on,\nOver rock and under tree,\nBy caves where never sun has shone,\nBy streams that never find the sea;\nOver snow by winter sown,\nAnd through the merry flowers of June,\nOver grass and over stone,\nAnd under mountains in the moon.\"\n-- J. R. R. Tolkien ");
static const char quote_14[] PROGMEM = ("\"The runes read 'I serve but the good,\n        of life and liberty'\"\n    -Leslie Fish, \"The Arizona Sword\"");
static const char quote_15[] PROGMEM = ("\"It's dangerous to go alone! Take this.\"");

const char *const acorn_Quoteslist[] PROGMEM = {quote_0, quote_1, quote_2, quote_3, quote_4, quote_5, quote_6,
                                                quote_7, quote_8, quote_9, quote_10, quote_11, quote_12, quote_13, quote_14, quote_15, 0};

static int numQuotes()
{
  int i = 0;

  while (acorn_Quoteslist[i])
  {
    i++;
  }
  return (i);
}

String acorn_getQuote()
{
  return readprogstr(acorn_Quoteslist[(doRandom() % numQuotes())]);
}

String _Acorns::getQuote()
{
  return readprogstr(acorn_Quoteslist[(doRandom() % numQuotes())]);
}

static SQInteger sqlorem(HSQUIRRELVM v)
{

  sq_pushstring(v, acorn_getQuote().c_str(), -1);
  return 1;
}

/*************************************************************************/
//Misc Arduino

/*
//Warning: this uses undocumented internals of the arduino handle

static SQInteger squartread(HSQUIRRELVM v)
{

   SQInteger ticks = 


    uart_t * uart = &_uart_bus_array[n]
    if(uart == NULL || uart->queue == NULL) {
        return 0;
    }
    uint8_t c;
    if(xQueueReceive(uart->queue, &c, portMAX_DELAY)) {
        return c;
    }
    return 0;
}*/

//This is meant to be part of a squirrel class

/*************************************************************************************/
//Module system

//This is our modules table. It contains weak references to every module that is loaded.
//This means a module can dissapear if all references to it go away!!!
//Beware of bugs!
static HSQOBJECT modulesTable;

//One of the ways that imports are handled, by just letting the user deal with it.
//This function must place the new imported module onto the stack.

//Return 0 if you can't handle the request, 1 if you can, SQ_ERROR for an error
//error.

//This is weak, so the user can override it rather easily.
SQRESULT __attribute__((weak)) sq_userImportFunction(HSQUIRRELVM v, const char *c, char len)
{
  return 0;
}

//Our builtin modules available for import.
//TODO: move into it's own library
SQRESULT sq_builtinImportFunction(HSQUIRRELVM v, const char *c, char len);

SQRESULT sq_builtinImportFunction(HSQUIRRELVM v, const char *c, char len)
{
  return (0);
}

static SQInteger sqimport(HSQUIRRELVM v)
{

  SQInteger s = 0;
  SQInteger i = sq_gettop(v);

  const SQChar *mname;
  if (i == 2)
  {
    if (sq_getstring(v, 2, &mname) == SQ_ERROR)
    {
      sq_throwerror_f(v, F("Name must be a string"));
      return SQ_ERROR;
    }

    s = sq_getsize(v, 2);

    sq_pushobject(v, modulesTable);
    sq_pushstring(v, mname, s);
    i = sq_gettop(v);
    //We have found it in the table of things that are already loaded.
    if (SQ_SUCCEEDED(sq_get(v, -2)))
    {
      return 1;
    }

    //This user import function is expected to put the module we are trying to
    //import onto the stack.
    if (sq_builtinImportFunction(v, mname, s) == 1)
    {

      HSQOBJECT o;
      sq_resetobject(&o);

      //Set the object as a member of the module table.
      //return the object itself
      sq_getstackobj(v, -1, &o);
      sq_pushobject(v, modulesTable);
      sq_pushstring(v, mname, s);
      sq_pushobject(v, o);
      sq_newslot(v, -3, SQFalse);

      sq_pushobject(v, o);
      return 1;
    }

    //This user import function is expected to put the module we are trying to
    //import onto the stack.
    if (sq_userImportFunction(v, mname, s) == 1)
    {

      HSQOBJECT o;
      sq_resetobject(&o);

      //Set the object as a member of the module table.
      //return the object itself
      sq_getstackobj(v, -1, &o);
      sq_pushobject(v, modulesTable);
      sq_pushstring(v, mname, s);
      sq_pushobject(v, o);
      sq_newslot(v, -3, SQFalse);

      sq_pushobject(v, o);
      return 1;
    }

    sq_throwerror_f(v, F("No import handler found"));
    return SQ_ERROR;
  }
  else
  {
    sq_throwerror_f(v, F("import takes exactly one parameter"));
    return SQ_ERROR;
  }
}

/**************************************************************************************/
//Free/busy functions

//Mark a program as busy by incrementing the busy reference count of it and all parents
static void _setbusy(struct loadedProgram *p)
{
  while (p)
  {
    p->busy += 1;
    p = p->parent;
  }
}

//Mark a program as busy by decrementing the reference count
static void _setfree(struct loadedProgram *p)
{
  while (p)
  {
    p->busy -= 1;
    p = p->parent;
  }
}

/*******************************************************************/
//Thread pool stuff

#ifdef INC_FREERTOS_H
//This is the thread pool
static TaskHandle_t sqTasks[ACORNS_THREADS];
//The queue going into the thread pool
static QueueHandle_t request_queue;
#endif

//Create and send a request to the thread pool if using FreeRTOS
//Otherwise, directly execute that thread right then and there.
static void _makeRequest(loadedProgram *program, void (*f)(loadedProgram *, void *), void *arg)
{
  struct Request r;

  //Only call under gil because of this.
  //The fact that it is in the queue counts as a reference, and it's up to the thread pool
  //thread to deref it.
  program->refcount++;

  r.program = program;
  r.f = f;
  r.arg = arg;

#ifdef INC_FREERTOS_H
  xQueueSend(request_queue, &r, portMAX_DELAY);
#else
  r.f(r.program, r.arg);
  deref_prog(r.program);
#endif
}

void _Acorns::makeRequest(const char *id, void (*f)(loadedProgram *, void *), void *arg)
{
  GIL_LOCK;
  loadedProgram *program = _programForId(id);
  activeProgram=program;
  if (program == 0)
  {
    GIL_UNLOCK;
    return;
  }
  _makeRequest(program, f, arg);
  GIL_UNLOCK;
}

#ifdef INC_FREERTOS_H
//The loop thar threads in the thread pool actually run
static void InterpreterTask(void *)
{
  struct Request rq;

  while (1)
  {
    xQueueReceive(request_queue, &rq, portMAX_DELAY);
    GIL_LOCK;
    activeProgram=rq.program;

    while (rq.program->busy)
    {
      GIL_UNLOCK;
      vTaskDelay(100);
      GIL_LOCK;
      activeProgram=rq.program;
      //If someone stopped the program while we were waiting
      if (rq.program->vm == 0)
      {
        goto fexit;
      }
    }

    _setbusy(rq.program);
    rq.f(rq.program, rq.arg);
    _setfree(rq.program);

  fexit:
    deref_prog(rq.program);
    GIL_UNLOCK;
  }
}
#endif

/**********************************************************************************************/
//Callback stuff

void deref_cb(CallbackData *p)
{
  p->refcount--;

  //If either reference is done with it,
  //The callback isn't happening, cleanup right away
  if (p->cleanup)
  {
    p->cleanup(p->prog, p->userpointer);
  }
  p->cleanup = 0;

  if (p->callable)
  {
    if (p->prog)
    {
      sq_release(p->prog->vm, p->callable);
    }
    //Setting the callable to 0 is the flag not
    //To try to call this callback anymore
    p->callable = 0;
  }

  //Deal with the linked list entry in the program
  CallbackData *x = p->prog->callbackRecievers;
  //Just delete if there's no list
  if (p->prog->callbackRecievers)
  {
    p->prog->callbackRecievers = 0;
  }

  //There's callbacks, but the first one isn't this
  else if (x)
  {
    CallbackData *last = x;
    while (x)
    {
      //If we find it, link the one before to the one after
      if (x == p)
      {
        if (x->next)
        {
          last->next = x->next;
        }
        else
        {
          last->next = 0;
        }
      }
      last = x;
    }
  }

  if (p->refcount == 0)
  {
    free(p);
  }
}

static SQInteger cb_release_hook(SQUserPointer p, SQInteger size)
{
  deref_cb(*((CallbackData **)p));
  return 0;
}

//Gets the callable at stack index idx, and return a CallbackData you can use to call it.
//Pushes an opaque subscription object to the stack. The callback is canceled if that ever gets
//garbage collected
struct CallbackData *_Acorns::acceptCallback(HSQUIRRELVM vm, SQInteger idx, void (*cleanup)(struct loadedProgram *, void *))
{
  HSQOBJECT *callable = (HSQOBJECT *)malloc(sizeof(HSQOBJECT));
  sq_resetobject(callable);

  SQObjectType t = sq_gettype(vm, idx);
  if ((t != OT_CLOSURE) && (t != OT_NATIVECLOSURE) && (t != OT_INSTANCE) && (t != OT_USERDATA))
  {
    sq_throwerror(vm, "Supplied object does not appear to be callable.");
  }
  sq_getstackobj(vm, idx, callable);

  //This callback data is a ref to the callable
  sq_addref(vm, callable);

  CallbackData *d = (CallbackData *)malloc(sizeof(CallbackData));

  d->callable = callable;
  d->cleanup = cleanup;

  struct loadedProgram *prg = ((loadedProgram *)sq_getforeignptr(vm));
  //One for the user side, one for the internal side that actually recieves the data.
  d->refcount = 2;

  d->prog = prg;

  if (prg->callbackRecievers == 0)
  {
    prg->callbackRecievers = d;
  }

  else
  {
    CallbackData *p = prg->callbackRecievers;

    while (p)
    {
      p = p->next;
    }
    p->next = d;
  }

  CallbackData **x = 0;
  sq_newuserdata(vm, sizeof(void *));
  sq_getuserdata(vm, -1, (SQUserPointer *)&x, 0);
  sq_setreleasehook(vm, -1, cb_release_hook);
  *x = d;

  return d;
}

//**********************************************************************************8
//program management

static struct loadedProgram *rootInterpreter = 0;

//This is our "program table"
static struct loadedProgram *loadedPrograms[ACORNS_MAXPROGRAMS];

String _Acorns::joinWorkingDir(HSQUIRRELVM v, char *dir)
{
  //Abs path
  if (dir[0] == '/')
  {
    return dir;
  }

  struct loadedProgram *prg = ((loadedProgram *)sq_getforeignptr(v));
  if (prg->workingDir == 0)
  {
    return dir;
  }
  return String(prg->workingDir) + "/" + dir;
}

static SQInteger sqwire_read(HSQUIRRELVM v)
{
  struct loadedProgram *prg = ((loadedProgram *)sq_getforeignptr(v));
  if (prg->workingDir == 0)
  {
    sq_pushstring(v, "", 0);
  }
  sq_pushstring(v, prg->workingDir, -1);
  return 0;
}

//Given a string program ID, return the loadedProgram object
//If it's not loaded.
static struct loadedProgram *_programForId(const char *id)
{
  if (id == 0)
  {
    if (rootInterpreter)
    {
      return rootInterpreter;
    }
    else
    {
      return 0;
    }
  }
  for (int i = 0; i < ACORNS_MAXPROGRAMS; i++)
  {
    if (loadedPrograms[i])
    {
      if (strcmp(loadedPrograms[i]->programID, id) == 0)
      {
        return loadedPrograms[i];
      }
    }
  }
  return 0;
}

static struct loadedProgram **_programSlotForId(const char *id)
{
  if (id == 0)
  {
    if (rootInterpreter)
    {
      return &rootInterpreter;
    }
    else
    {
      return 0;
    }
  }
  for (int i = 0; i < ACORNS_MAXPROGRAMS; i++)
  {
    if (loadedPrograms[i])
    {
      if (strcmp(loadedPrograms[i]->programID, id) == 0)
      {
        return &(loadedPrograms[i]);
      }
    }
  }
  return 0;
}

//Only call under gil
static void deref_prog(loadedProgram *p)
{
  p->refcount--;
  if (p->refcount == 0)
  {
    if (p->inputBuffer)
    {
      free(p->inputBuffer);
      p->inputBuffer = 0;
    }
    free(p);
  }
}

void _Acorns::clearInput(const char *id)
{
  GIL_LOCK;
  loadedProgram *p = _programForId(id);
  activeProgram=p;
  if (p == 0)
  {
    GIL_UNLOCK;
    return;
  }
  if (p->inputBuffer)
  {
    free(p->inputBuffer);
    p->inputBuffer = 0;
    p->inputBufferLen = 0;
  }

  GIL_UNLOCK;
}

void _Acorns::writeToInput(const char *id, const char *data, int len)
{
  writeToInput(id, data, len, -1);
}

///Position is mostly there to allow for idempotent writes. Set to -1 to append to the end.
///But it will fill with garbage if you leave gaps
void _Acorns::writeToInput(const char *id, const char *data, int len, long position)
{
  GIL_LOCK;
  loadedProgram *p = _programForId(id);
  activeProgram=p;
  if (p == 0)
  {
    GIL_UNLOCK;
    return;
  }

  if (len == -1)
  {
    len = strlen(data);
  }

  if (position == -1)
  {
    position = p->inputBufferLen;
  }

  //How much total len is needed for the contents
  long needed = len + position;

  char addlen = 1;
  if (len == 1)
  {
    addlen = 0;
  }

  if (p->inputBuffer == 0)
  {
    p->inputBuffer = (char *)malloc(len + 2);
    p->inputBufferLen = 0;
  }
  else
  {
    char *x = 0;
    //add one for safety and to optimize things with 2 writes where the second is a null pointer.
    //If the len is one, don't add one, because we assume this is the last one.
    if ((x = (char *)realloc(p->inputBuffer, needed + addlen)) == 0)
    {
      GIL_UNLOCK;
      return;
    }
    p->inputBuffer = x;
  }

  memcpy(p->inputBuffer + position, data, len);
  p->inputBufferLen = needed;

  GIL_UNLOCK;
}

static int _closeProgram(const char *id);

//Function that the thread pool runs to run whatever program is on the top of an interpreter's stack
static void runLoaded(loadedProgram *p, void *d)
{
  SQInt32 x = sq_gettop(p->vm);
  sq_pushroottable(p->vm);
  if (sq_call(p->vm, 1, SQFalse, SQTrue) == SQ_ERROR)
  {
    //If the flag saying we should do so is set, close the program on failure.
    if (d == (void *)1)
    {
      //The fact that we are running sets the busy flag which would deadlock when we close it.
      _setfree(p);
      _closeProgram(p->programID);
      //Undo that
      _setbusy(p);
      return;
    }
  }
  //Pop the closure itself, but don't corrupt the stack
  if (x > 1)
  {
    x -= 1;
  }
  sq_settop(p->vm, x);
}

static void _runInputBuffer(loadedProgram *p, void *d)
{

  //Adding the null terminator
  p->inputBuffer[p->inputBufferLen] = 0;
  if (SQ_SUCCEEDED(sq_compilebuffer(p->vm, p->inputBuffer, p->inputBufferLen + 1, _SC("InputBuffer"), SQTrue)))
  {
    runLoaded(p, 0);
    free(p->inputBuffer);
    p->inputBuffer = 0;
    p->inputBufferLen = 0;
  }
  else
  {
    Serial.println(F("Failed to compile code"));
    return;
  }

  free(p->inputBuffer);
  p->inputBuffer = 0;
  p->inputBufferLen = 0;
}

void _Acorns::runInputBuffer(const char *id)
{
  makeRequest(id, _runInputBuffer, 0);
}

//Close a running program, waiting till all children are no longer busy.
static int _closeProgram(const char *id)
{
  entropy += esp_random();
  rng_key += esp_random();
  doRandom();

  loadedProgram **old = _programSlotForId(id);
  //Check if programs are the same

  if (old)
  {

    ///Something can be "busy" without holding the lock if it yields.
    while ((*old)->busy)
    {
      GIL_UNLOCK;
      delay(100);
      GIL_LOCK;
      activeProgram=*old;
      if (*old == 0)
      {
        break;
      }
    }

    if (*old)
    {

      if ((*old)->workingDir)
      {
        free((*old)->workingDir);
        (*old)->workingDir = 0;
      }

      if ((*old)->inputBuffer)
      {
        free((*old)->inputBuffer);
        (*old)->inputBuffer = 0;
      }
      //Close the VM and deref the task handle now that the VM is no longer busy.
      //The way we close the VM is to get rid of references to its thread object.
      if ((*old)->vm)
      {
        sq_release((*old)->vm, &((*old)->threadObj));
        (*old)->vm = 0;
      }
      deref_prog(*old);
      *old = 0;
    }
  }
}

//Get a running program to stop whatever it's doing, but don't actually
//Remove it's process table entry. This just sends the stop request,
//so watch out, it might still be busy for 100 VM instructions or so.
static void _forceclose(const char *id)
{
  loadedProgram *old = _programForId(id);

  if (old == 0)
  {
    return;
  }

  sq_request_forceclose(old->vm);
}

int _Acorns::closeProgram(const char *id)
{
  GIL_LOCK;
  _closeProgram(id);
  GIL_UNLOCK;
  return 0;
}

int _Acorns::closeProgram(const char *id, char force)
{
  GIL_LOCK;
  if (force)
  {
    _forceclose(id);
  }

  _closeProgram(id);
  GIL_UNLOCK;
  return 0;
}

//Sq function to do an unclean stop of a running program.
static SQInteger sqcloseProgram(HSQUIRRELVM v)
{
  const char *id;
  char id2[32];
  int len;
  sq_getstring(v, 2, &id);

  if (sq_getsize(v, 2) > 31)
  {
    return SQ_ERROR;
  }

  memcpy(id2, id, sq_getsize(v, 2));
  id2[sq_getsize(v, 2)] = 0;

  _forceclose(id2);
  _closeProgram(id2);

  return 0;
}

//Load a new program from source code with the given ID, replacing any with the same ID if the
//first 30 bytes are different. The new program will have its own global scope that an inner scope of the root interpreter's.
//You will be able to use getdelegate to get at the root table directly.

//Passing a null to input tries to load the program's input buffer as the replacement for the program.
//If there's no old program or no input buffer, does nothing.
static int _loadProgram(const char *code, const char *id, bool synchronous,
                        void (*errorfunc)(loadedProgram *, const char *),
                        void (*printfunc)(loadedProgram *, const char *), const char *workingDir)

{
  //Don't show the message when we load the empty program just to write things to the buffer
  //Because we don't want to show it twice when we actually load it.
  if (code)
  {
    Serial.print("\nLoading program: ");
    Serial.println(id);
  }
  //Program load times as another entropy
  //Source
  entropy += esp_random();
  rng_key += esp_random();

  void *inputBufToFree = 0;

  struct loadedProgram *old = _programForId(id);
  //Check if programs are the same
  //passing a null pointer tells it to use the input buffer
  if (code == 0)
  {
    if (old)
    {
      if (old->inputBuffer)
      {
        code = old->inputBuffer;
      }
      else
      {
        Serial.println(F("No code or input buffer, cannot load"));
        return 1;
      }
    }
    else
    {
      Serial.println(F("No code or previous program input buffer, cannot load"));
      return 1;
    }
  }

  if (old)
  {

    inputBufToFree = (void *)(old->inputBuffer);
    //Mark it as already dealt with, so that it doesn't get garbage collected
    old->inputBuffer = 0;

    //Check if the versions are the same
    if (memcmp(old->hash, code, PROG_HASH_LEN) == 0)
    {
      Serial.println(F("That exact program version is already loaded, doing nothing."));
      return 0;
    }

    ///Something can be "busy" without holding the lock if it yields.
    while (old->busy)
    {
      GIL_UNLOCK;
      delay(100);
      GIL_LOCK;
    }

    _closeProgram(id);
  }

  //Find a free interpreter slot
  for (int i = 0; i < ACORNS_MAXPROGRAMS; i++)
  {
    if (loadedPrograms[i] == 0)
    {

      //Note that the old struct is still out there in heap until all the refs are gone
      loadedPrograms[i] = (struct loadedProgram *)malloc(sizeof(struct loadedProgram));
      loadedPrograms[i]->parent = rootInterpreter;
      loadedPrograms[i]->refcount = 1;
      loadedPrograms[i]->callbackRecievers = 0;
      loadedPrograms[i]->busy = 0;
      loadedPrograms[i]->inputBuffer = 0;
      loadedPrograms[i]->inputBufferLen = 0;
      loadedPrograms[i]->errorfunc = errorfunc;
      loadedPrograms[i]->printfunc = printfunc;
      loadedPrograms[i]->workingDir = 0;

      if (workingDir)
      {
        loadedPrograms[i]->workingDir = (char *)malloc(strlen(workingDir) + 1);
        strcpy(loadedPrograms[i]->workingDir, workingDir);
      }

      //This is so the dereference function can free the slot in the table
      //By itself
      loadedPrograms[i]->slot = &loadedPrograms[i];

      HSQUIRRELVM vm;
      if (sharedMode==false)
      {
        loadedPrograms[i]->vm = sq_newthread(rootInterpreter->vm, 1024);
      }
      else
      {
         loadedPrograms[i]->vm = rootInterpreter->vm;
      }
      
      vm = loadedPrograms[i]->vm;
      sq_setforeignptr(vm, loadedPrograms[i]);
      sq_resetobject(&loadedPrograms[i]->threadObj);

      //Get the thread handle, ref it so it doesn't go away, then store it in the loadedProgram
      //and pop it. Now the thread is independant
      sq_getstackobj(rootInterpreter->vm, -1, &loadedPrograms[i]->threadObj);
      sq_addref(vm, &loadedPrograms[i]->threadObj);
      sq_pop(rootInterpreter->vm, 1);

      //Make a new table as the root table of the VM, then set root aa it's delegate(The root table that is shared with the parent)
      //then set that new table as our root. This way we can access parent functions but have our own scope.
      sq_newtable(vm);
      sq_pushroottable(vm);
      sq_setdelegate(vm, -2);
      sq_setroottable(vm);

      //Get rid of any garbage, and ensure there's at leas one thomg on the stack
      sq_settop(vm, 1);

      memcpy(loadedPrograms[i]->hash, code, PROG_HASH_LEN);

      //Don't overflow our 16 byte max prog ID
      if (strlen(id) < 16)
      {
        strcpy(loadedPrograms[i]->programID, id);
      }
      else
      {
        memcpy(loadedPrograms[i]->programID, id, 15);
        loadedPrograms[i]->programID[15] == 0;
      }
      loadedPrograms[i]->busy = 0;
      loadedPrograms[i]->vm = vm;

      if (SQ_SUCCEEDED(sq_compilebuffer(vm, code, strlen(code) + 1, _SC(id), SQTrue)))
      {
        if (inputBufToFree)
        {
          free(inputBufToFree);
          inputBufToFree = 0;
        }
        if (synchronous)
        {
          _setbusy(loadedPrograms[i]);
          runLoaded(loadedPrograms[i], (void *)1);
          _setfree(loadedPrograms[i]);
        }
        else
        {
          //That 1 is there as a special flag indicating we should close the program if we can't run it.
          _makeRequest(loadedPrograms[i], runLoaded, (void *)1);
        }
      }
      else
      {
        if (inputBufToFree)
        {
          free(inputBufToFree);
          inputBufToFree = 0;
        }
        //If we can't compile the code, don't load it at all.
        _closeProgram(id);
        Serial.println(F("Failed to compile code"));
      }

      return 0;
    }
  }
  if (inputBufToFree)
  {
    free(inputBufToFree);
    inputBufToFree = 0;
  }
  //err, could not find free slot for program
  Serial.println(F("No free program slots"));
  return 1;
}

int _Acorns::isRunning(const char *id, const char *hash)
{
  GIL_LOCK;
  struct loadedProgram *x = _programForId(id);
  if (x == 0)
  {
    GIL_UNLOCK;
    return 0;
  }
  if (hash)
  {
    if (memcmp(x->hash, hash, PROG_HASH_LEN))
    {
      x = 0;
    }
  }
  GIL_UNLOCK;

  if (x)
  {
    return 1;
  }
  else
  {
    return 0;
  }
}

int _Acorns::isRunning(const char *id)
{
  return isRunning(id, 0);
}

int _Acorns::loadProgram(const char *code, const char *id)
{
  GIL_LOCK;
  _loadProgram(code, id, false, 0, 0, 0);
  GIL_UNLOCK;
  return 0;
}

int _Acorns::runProgram(const char *code, const char *id)
{
  GIL_LOCK;
  _loadProgram(code, id, true, 0, 0, 0);
  GIL_UNLOCK;
  return 0;
}
int _Acorns::runProgram(const char *code, const char *id, void (*onerror)(loadedProgram *, const char *), void (*onprint)(loadedProgram *, const char *), const char *workingDir)
{
  GIL_LOCK;
  _loadProgram(code, id, true, onerror, onprint, workingDir);
  GIL_UNLOCK;
  return 0;
}
//Load whatever is in a program's input buffer as a new program that replaces the old one.
//Force close the old one if forceClose is true, otherwise, just wait for it.
int _Acorns::loadInputBuffer(const char *id, bool forceClose)
{
  GIL_LOCK;
  if (forceClose)
  {
    _forceclose(id);
  }
  _loadProgram(0, id, false, 0, 0, 0);
  GIL_UNLOCK;
  return 0;
}
int _Acorns::loadInputBuffer(const char *id)
{
  GIL_LOCK;
  _loadProgram(0, id, false, 0, 0, 0);
  GIL_UNLOCK;
  return 0;
}

int _Acorns::loadFromFile(const char *fn)
{
  GIL_LOCK;
  FILE *f = fopen(fn, "r");
  if (f)
  {
    fseek(f, 0L, SEEK_END);
    int sz = ftell(f);
    rewind(f);

    char *buf = (char *)malloc(sz + 2);
    int p = 0;

    int chr = fgetc(f);
    while (chr != EOF)
    {
      buf[p] = chr;
      chr = fgetc(f);
      p += 1;
    }
    buf[p] = 0;

    //Find last slash in the path
    const char *slash = fn;
    while (*fn)
    {
      if (*fn == '/')
      {
        slash = fn;
      }
      fn++;
    }
    fclose(f);

    GIL_UNLOCK;
    loadProgram(buf, slash + 1);
    free(buf);
    GIL_LOCK;
  }

  GIL_UNLOCK;
  return 0;
}

int _Acorns::loadFromDir(const char *dir)
{
  GIL_LOCK;
  DIR *d = opendir(dir);
  if (d == 0)
  {
    GIL_UNLOCK;
    return 0;
  }
  char buffer[256];
  struct dirent *de = readdir(d);
  char *fnpart;

  strcpy(buffer, dir);
  if (buffer[strlen(dir) - 1] == '/')
  {
    fnpart = buffer + strlen(dir);
  }
  else
  {
    buffer[strlen(dir)] = '/';
    fnpart = buffer + strlen(dir) + 1;
  }

  while (de)
  {
    //Rather absurd hackery just to put a / before the path that seems to lack one.
    strcpy(fnpart, de->d_name);
    GIL_UNLOCK;
    Serial.print("Loading program from file:");
    Serial.println(buffer);
    loadFromFile(buffer);
    GIL_LOCK;
    de = readdir(d);
  }
  closedir(d);
  GIL_UNLOCK;
  return 0;
}

//****************************************************************************/
//Callbacks management

///*******************************************************************************/
//Functions that are stored in flash until needed

/*
struct LoadableFunction
{
  SQFUNCTION f;
  char * name;
}

*/

//*********************************************************************************
//REPL

static HSQUIRRELVM replvm;
static loadedProgram *replprogram;
static SQChar *replbuffer = 0;
static int replbuffersize = 128;
static int replpointer = 0;
static int startofline = 0;
static int string = 0;
static int blocks = 0;
static char dotprompt = 0;
static char retval = 0;
static char esc = 0;

/*Most of this function is from the original sq.c  see copyright notice in squirrel.h */

void _Acorns::replChar(char c)
{
  if (replbuffer == 0)
  {
    replbuffer = (SQChar *)malloc(128);
  }
  if (c == _SC('\n'))
  {

    if (blocks)
    {
      Serial.print("\n...");
    }
  }
  else
  {
    Serial.write(c);
  }

  if (c == _SC('\n'))
  {
    if (replpointer > 0 && replbuffer[replpointer - 1] == _SC('\\'))
    {
      replbuffer[replpointer - 1] = _SC('\n');
    }
    else if (blocks == 0)
      goto doing;
    replbuffer[replpointer++] = _SC('\n');
  }

  else if (c == _SC('\\'))
  {
    esc = 1;
  }
  else if (string && esc)
  {
    replbuffer[replpointer++] = (SQChar)c;
  }
  else if (c == _SC('}') && !string)
  {
    blocks--;
    replbuffer[replpointer++] = (SQChar)c;
  }
  else if (c == _SC('{') && !string)
  {
    blocks++;
    replbuffer[replpointer++] = (SQChar)c;
  }
  else if (c == _SC('"') || c == _SC('\''))
  {
    string = !string;
    replbuffer[replpointer++] = (SQChar)c;
  }
  else if (replpointer >= 1000 - 1)
  {
    Serial.println(F("sq : input line too long\n"));
    goto resetting;
  }
  else
  {
    replbuffer[replpointer++] = (SQChar)c;
  }
  //Leave some margin, too tired to think about off by one
  if (replpointer > replbuffersize - 3)
  {
    void *x = realloc(replbuffer, replbuffersize * 2);
    if (x)
    {
      replbuffer = (char *)x;
      replbuffersize *= 2;
    }
    else
    {
      Serial.println(F("sq : input line too long\n"));
      goto resetting;
    }
  }

  esc = 0;
  return;
doing:
  replbuffer[replpointer] = _SC('\0');
  GIL_LOCK;
  activeProgram=replprogram;

  if (replbuffer[0] == _SC('='))
  {
    sprintf(sq_getscratchpad(replvm, 1024), _SC("return (%s)"), &replbuffer[1]);
    memcpy(replbuffer, sq_getscratchpad(replvm, -1), (scstrlen(sq_getscratchpad(replvm, -1)) + 1) * sizeof(SQChar));
    retval = 1;
  }
  replpointer = scstrlen(replbuffer);
  if (replpointer > 0)
  {
    SQInteger oldtop = sq_gettop(replvm);
    if (SQ_SUCCEEDED(sq_compilebuffer(replvm, replbuffer, replpointer, _SC("interactive console"), SQTrue)))
    {
      sq_pushroottable(replvm);
      if (SQ_SUCCEEDED(sq_call(replvm, 1, retval, SQTrue)) && retval)
      {
        scprintf(_SC("\n"));
        sq_pushroottable(replvm);
        sq_pushstring(replvm, _SC("print"), -1);
        sq_get(replvm, -2);
        sq_pushroottable(replvm);
        sq_push(replvm, -4);
        sq_call(replvm, 2, SQFalse, SQTrue);
        retval = 0;
      }
    }
    sq_settop(replvm, oldtop);
  }
  GIL_UNLOCK;
resetting:
  //Done with that, make the replbuffer go to the default len if it was expanded
  assert(replbuffer = (SQChar *)realloc(replbuffer, 128));
  replbuffersize = 128;
  replpointer = 0;
  blocks = 0;
  string = 0;
  retval = 0;
  Serial.print("\n>>>");
}

//***********************************************************************************************************/
//INI file config handling
const char cfg_inifile[] = "/spiffs/config.ini";

HSQOBJECT ConfigTable;

static int iniCallback(const char *section, const char *key, const char *value, void *userdata)
{
  (void)userdata; /* this parameter is not used in this example */

  char buf[256];

  char slen = strlen(section);

  //Join section and key with a dot, if section exists.
  if (slen)
  {
    strcpy(buf, section);
    buf[slen] = '.';
    strcpy(buf + slen + 1, key);
  }
  else
  {
    strcpy(buf, key);
  }
  sq_pushobject(rootInterpreter->vm, ConfigTable);
  sq_pushstring(rootInterpreter->vm, buf, -1);
  sq_pushstring(rootInterpreter->vm, value, -1);
  sq_newslot(rootInterpreter->vm, -3, SQFalse);
  sq_pop(rootInterpreter->vm, 1);
  return 1;
}

//This is the fallback getter for the config options
static SQInteger sqgetconfigfromini(HSQUIRRELVM v)
{
  const char *key;

  char buf[256];
  char section[49];

  if (sq_getstring(v, 2, &key) == SQ_ERROR)
  {
    return sq_throwerror_f(v, F("Key must be str"));
  }
  char *x = strchr(key, '.');
  if (x)
  {
    if (x - key > 47)
    {
      return sq_throwerror_f(v, F("Section is too long(max 48 bytes)"));
    }
    memcpy(section, key, (x - key) + 1);
    section[x - key] = 0;
    key = x + 1;
    ini_gets(section, key, "", buf, 256, cfg_inifile);
  }
  else
  {
    ini_gets("", key, "", buf, 256, cfg_inifile);
  }
  sq_pushstring(v, buf, -1);
  return 1;
}

void loadConfig()
{
  sq_resetobject(&ConfigTable);
  sq_pushroottable(rootInterpreter->vm);
  sq_pushstring(rootInterpreter->vm, "config", -1);
  sq_newtableex(rootInterpreter->vm, 2);
  //Create the delegate for the config function;
  sq_newtableex(rootInterpreter->vm, 2);
  sq_pushstring(rootInterpreter->vm, "_get", -1);
  sq_newclosure(rootInterpreter->vm, sqgetconfigfromini, 0); //create a new function
  sq_newslot(rootInterpreter->vm, -3, SQFalse);
  sq_setdelegate(rootInterpreter->vm, -2);

  sq_getstackobj(rootInterpreter->vm, -1, &ConfigTable);
  sq_addref(rootInterpreter->vm, &ConfigTable);
  sq_newslot(rootInterpreter->vm, -3, SQFalse);

  sq_pop(rootInterpreter->vm, 1);

  /*
  //Ensure the existance of the file.
  FILE * f = fopen(cfg_inifile,"r");
  if(f)
  {
    fclose(f);
  }
  else
  {
    return;
  }

  ini_browse(iniCallback, 0, cfg_inifile);
  */
}

static void refreshConfig()
{
  Acorns.tz.setPosix(Acorns.getConfig("time.posixtz", "PST8PDT,M3.2.0,M11.1.0"));
  setInterval(Acorns.getConfig("time.syncinterval", "0").toInt());
  setServer(Acorns.getConfig("time.ntpserver", "pool.ntp.org"));
}

static SQInteger sqwriteconfig(HSQUIRRELVM v)
{
  const char *key;
  const char *val;

  char section[49];

  if (sq_getstring(v, 2, &key) == SQ_ERROR)
  {
    return sq_throwerror_f(v, F("Key must be str"));
  }
  if (sq_getstring(v, 3, &val) == SQ_ERROR)
  {
    if (sq_tostring(v, 3) == SQ_ERROR)
    {
      return sq_throwerror_f(v, F("Requires 2 args"));
    }
    sq_getstring(v, 3, &val);
  }

  char *x = strchr(key, '.');
  if (x)
  {
    if (x - key > 47)
    {
      return sq_throwerror_f(v, F("Section is too long(max 48 bytes)"));
    }
    memcpy(section, key, (x - key) + 1);
    section[x - key] = 0;
    key = x + 1;
    ini_puts(section, key, val, cfg_inifile);
    return 0;
  }

  ini_puts("", key, val, cfg_inifile);
  refreshConfig();
  return 0;
}

void _Acorns::setConfig(String strkey, String value)
{
  const char *key = strkey.c_str();
  const char *x = strchr(key, '.');
  char section[49];

  if (x)
  {
    if (x - key > 47)
    {
      return;
    }
    memcpy(section, key, (x - key) + 1);
    section[x - key] = 0;
    key = x + 1;
    ini_puts(section, key, value.c_str(), cfg_inifile);
  }
  else
  {
    ini_puts("", key, value.c_str(), cfg_inifile);
  }

  refreshConfig();
}

/*First try to get a value from the table itself. Failing that, try to get a value from the .ini file.*/
String _Acorns::getConfig(String key, String d)
{
  char buffer[128];
  getConfig(key.c_str(), d.c_str(), buffer, 128);
  return String(buffer);
}

void _Acorns::getConfig(const char *key, const char *d, char *buf, int maxlen)
{
  char section[64];

  char *x = strchr(key, '.');

  const char *buf2;
  bool found = false;

  sq_pushobject(rootInterpreter->vm, ConfigTable);
  sq_pushstring(rootInterpreter->vm, key, -1);
  if (sq_get(rootInterpreter->vm, -1) != SQ_ERROR)
  {
    if (sq_getsize(rootInterpreter->vm, -1) < maxlen)
    {
      sq_getstring(rootInterpreter->vm, -1, &buf2);
      strcpy(buf, buf2);
      found = true;
    }
    sq_pop(rootInterpreter->vm, 1);
  }
  sq_pop(rootInterpreter->vm, 1);
  if (found)
  {
    return;
  }

  if (x)
  {
    if (x - key > 47)
    {
    }
    else
    {
      memcpy(section, key, (x - key) + 1);
      section[x - key] = 0;
      char *akey = x + 1;
      ini_gets(section, akey, "", buf, maxlen, cfg_inifile);
      if (strlen(buf)==0)
      {
        return;
      }
    }
  }
  if (strlen(d) < maxlen)
  {
    strcpy(buf, d);
  }
}
/***************************************************************************************/
//WiFi
//Connect to wifi based on config file

//wifi event handler

//AsyncWebServer server(80);

/*
static void findLocalNtp()
{

    mdns_result_t * results = NULL;
    esp_err_t err = mdns_query_ptr(service_name, proto, 3000, 20,  &results);
    if(err){
        return;
    }
    if(!results){
        return;
    }

    mdns_result_t * best=results; 
    
    while(results)
    {
      //Find the lowest ASCIIBetical instance name
      if(strcmp(results->instance_name, best->instance_name))
      {
        best= results;
      }
    }

    WiFiUDP ntpUDP;
    NTPClient timeClient(ntpUDP);
    ntpUDP.begin();

    NTPClient.forceUpdate();

    
}
*/

//Configure wifi according to the config file
static void wifiConnect()
{

  char ssid[65];
  char psk[65];
  char wifimode[8];

  //This feature is awful. It wears out memory.
  //Always turn it off
  WiFi.persistent(false);

  //Ensure the existance of the file.
  FILE *f = fopen(cfg_inifile, "r");
  if (f)
  {
    fclose(f);
  }
  else
  {
    return;
  }

  Acorns.getConfig("wifi.ssid", "", ssid, 64);
  Acorns.getConfig("wifi.psk", "", psk, 64);
  Acorns.getConfig("wifi.mode", "sta", wifimode, 8);

  if (strcmp(wifimode, "sta") == 0)
  {
    if (strlen(ssid))
    {
      WiFi.begin(ssid, psk);
      Serial.print("Trying to connect to: ");
      Serial.println(ssid);
    }
  }
  if (strcmp(wifimode, "ap") == 0)
  {
    WiFi.softAP(ssid, psk);
    Serial.print("Serving as access point with SSID: ");
    Serial.println(ssid);
  }
}

#ifndef ESP8266
static void WiFiEvent(WiFiEvent_t event)
{
  switch (event)
  {
  case SYSTEM_EVENT_STA_GOT_IP:
    break;
  case SYSTEM_EVENT_STA_DISCONNECTED:
    wifiConnect();

    break;
  }
}
#endif

//**************************************************************************************/
//General system control

//It's actually down below in this file
extern struct loadedProgram *replprogram;

static void _printfunc(HSQUIRRELVM v, const SQChar *s, ...)
{
  struct loadedProgram *prg = activeProgram;

  char buf[256];
  va_list vl;
  va_start(vl, s);
  vsnprintf(buf, 256, s, vl);
  va_end(vl);
  if (prg == replprogram)
  {
    Serial.println(buf);
  }
  else if(prg==0)
  {
    Serial.println(buf);
  }
  else
  {
    if (prg->printfunc)
    {
      prg->printfunc(prg, buf);
    }
    if (Acorns.printfunc)
    {
      Acorns.printfunc(prg, buf);
    }
    else
    {
      Serial.println(buf);
    }
  }
}

static void _errorfunc(HSQUIRRELVM v, const SQChar *s, ...)
{
  struct loadedProgram *prg = activeProgram;

  char buf[256];
  va_list vl;
  va_start(vl, s);
  vsnprintf(buf, 256, s, vl);
  va_end(vl);

  if (prg == replprogram)
  {
    Serial.println(F(""));
    Serial.print(buf);
  }
  else if(prg==0)
  {
    Serial.println(buf);
  }
  else
  {
    if (prg->errorfunc)
    {
      prg->errorfunc(prg, buf);
    }
    if (Acorns.errorfunc)
    {
      Acorns.errorfunc(prg, buf);
    }
    else
    {
      Serial.println(F(""));
      Serial.print(buf);
    }
  }
}

//Adds the basic standard libraries to the squirrel VM
static void addlibs(HSQUIRRELVM v)
{
  sq_pushroottable(v);
  sqstd_register_bloblib(v);
  sqstd_register_iolib(v);
  sqstd_register_systemlib(v);
  sqstd_register_mathlib(v);
  sqstd_register_stringlib(v);
  sq_pop(v, 1);

}

SQObject sqSerialBaseClass;

void resetLoadedProgram(loadedProgram *p)
{
}

static HSQOBJECT ReplThreadObj;

static SQInteger sqexit(HSQUIRRELVM v)
{
  sq_request_forceclose(v);
  sq_throwerror_f(v, F("exit() function called"));
  return SQ_ERROR;
  return (0);
}

static bool began = false;

void _Acorns::begin()
{
  return _Acorns::begin(0);
}

#ifdef ESP8266
WiFiEventHandler disconnectedEventHandler;
#endif

static SQInteger sqformat(HSQUIRRELVM v)
{
  SPIFFS.format();
  if (SPIFFS.begin() == false)
  {
    sq_throwerror_f(v, F("Failed to format and mount"));
    return SQ_ERROR;
  }
  return 0;
}
bool spiffsPosixBegin();

//This is always there because it's pretty important
static SQInteger sqfreeheap(HSQUIRRELVM v)
{
  sq_pushinteger(v, ESP.getFreeHeap());
  return (1);
}

//Initialize squirrel task management
void _Acorns::begin(const char *prgsdir)
{

  //No need to put load on the NTP servers if the app doesn't need it.
  setInterval(0);

  //Give the system file access
  if (spiffsPosixBegin == false)
  {
    Serial.println(F("SPIFFS mount failed, you can format using spiffsFormat(), but all data will be deleted."));
    Serial.println(F("Functions using the filesystem will not work."));
  }

  if (prgsdir == 0)
  {
    prgsdir = "/spiffs/sqprogs";
  }
  //Run this once and only once
  if (began)
  {
    return;
  }
  began = true;

  Serial.println(F("Acorns: Squirrel for Arduino"));
  Serial.println(F("Based on: http://www.squirrel-lang.org/\n"));

  for (int i = 0; i < ACORNS_MAXPROGRAMS; i++)
  {
    loadedPrograms[i] = 0;
  }

#ifdef INC_FREERTOS_H
  _acorns_gil_lock = xSemaphoreCreateBinary();
  xSemaphoreGive(_acorns_gil_lock);
#endif
  Serial.print("Free Heap: ");
  Serial.print(ESP.getFreeHeap());
  //This will probably seed the RNG far better than anyone should even think of needing
  //For non-crypto stuff.
  rng_key += esp_random();
  rng_key += (uint64_t)esp_random() << 32;

  entropy +=  (uint64_t)esp_random() << 32;
  entropy += esp_random();
  //Start the root interpreter
  rootInterpreter = (struct loadedProgram *)malloc(sizeof(struct loadedProgram));

  rootInterpreter->vm = sq_open(1024); //creates a VM with initial stack size 1024
  rootInterpreter->workingDir = 0;
  //Setup the config system
  Serial.print("Started Interpreter");

  loadConfig();
  Serial.print("Loaded Config");

  //Set the root table dynamic functions delegate;
  /*
  sq_pushroottable(rootInterpreter->vm);
  sq_newtableex(rootInterpreter->vm, 2);
  sq_pushstring(rootInterpreter->vm, "_get", -1);
  sq_newclosure(rootInterpreter->vm, sqgetconfigfromini, 0); //create a new function
  sq_newslot(rootInterpreter->vm, -3, SQFalse);
  sq_setdelegate(rootInterpreter->vm, -2);
  */
  Serial.print("TZ");

  tz.setPosix(Acorns.getConfig("time.posixtz", "PST8PDT,M3.2.0,M11.1.0"));
  Serial.print("Done");

#ifdef ESP8266
  disconnectedEventHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected &event) {
    wifiConnect();
  });
#else
  WiFi.onEvent(WiFiEvent);
#endif
  wifiConnect();
  ;
  //Lets us advertise a hostname. This already has it's own auto reconnect logic.
  char hostname[32];
  Acorns.getConfig("wifi.hostname", "", hostname, 32);
  if (strlen(hostname))
  {
    Serial.print("MDNS Name: ");
    Serial.println(hostname);
    MDNS.begin(hostname);
  }

  registerFunction(0, sqwriteconfig, "setConfig");
  registerFunction(0, sqlorem, "lorem");
  registerFunction(0, sqrandom, "random");
  registerFunction(0, sqimport, "import");
  registerFunction(0, sqcloseProgram, "forceClose");
  registerFunction(0, sqexit, "exit");
  registerFunction(0, sqformat, "formatSPIFFS");
 


  sqstd_seterrorhandlers(rootInterpreter->vm);
  sq_setprintfunc(rootInterpreter->vm, _printfunc, _errorfunc);


  //addlibs(rootInterpreter->vm);
  Serial.println(F("Added core libraries"));

  //This is part of the class, it's in acorns_aduinobindings
  addArduino(rootInterpreter->vm);

  
  sq_pushroottable(rootInterpreter->vm);
  sq_pushstring(rootInterpreter->vm, "memfree", -1);
  sq_newclosure(rootInterpreter->vm, sqfreeheap, 0); //create a new function
  sq_newslot(rootInterpreter->vm, -3, SQFalse);
  sq_pop(rootInterpreter->vm, 1);

  //Use the root interpeter to create the modules table
  sq_newtableex(rootInterpreter->vm, 8);
  sq_resetobject(&modulesTable);
  sq_getstackobj(rootInterpreter->vm, -1, &modulesTable);
  sq_addref(rootInterpreter->vm, &modulesTable);
  sq_pop(rootInterpreter->vm, 1);

  sq_setforeignptr(rootInterpreter->vm, rootInterpreter);

  //Create the directory iteration code
  sq_newtableex(rootInterpreter->vm, 2);
  sq_pushstring(rootInterpreter->vm, "_nexti", -1);
  sq_newclosure(rootInterpreter->vm, sqdirectoryiterator_next, 0); //create a new function
  sq_newslot(rootInterpreter->vm, -3, SQFalse);

  //The get function that's actually just a passthrough
  sq_pushstring(rootInterpreter->vm, "_get", -1);
  sq_newclosure(rootInterpreter->vm, sqdirectoryiterator_get, 0); //create a new function
  sq_newslot(rootInterpreter->vm, -3, SQFalse);

  sq_resetobject(&DirEntryObj);
  sq_getstackobj(rootInterpreter->vm, -1, &DirEntryObj);
  sq_addref(rootInterpreter->vm, &DirEntryObj);
  sq_pop(rootInterpreter->vm, 1);

  //create the dir function.
  registerFunction(0, sqdirectoryiterator, "dir");

  memcpy(rootInterpreter->hash, "//RootInterpreter123456789abcde", PROG_HASH_LEN);
  rootInterpreter->busy = 0;
  rootInterpreter->inputBuffer = 0;
  rootInterpreter->inputBufferLen = 0;
  rootInterpreter->parent = 0;
  rootInterpreter->errorfunc = 0;

#ifdef INC_FREERTOS_H
  request_queue = xQueueCreate(25, sizeof(struct Request));

  int numThreads = 4;
  
  //In shared mode there is 1 interpreter and thus 1 thread
  if (sharedMode)
  {
    numThreads = ACORNS_THREADS;
  }
  else
  {
    numThreads=1;
  }
  

  for (int i = 0; i < ACORNS_THREADS; i++)
  {
    xTaskCreatePinnedToCore(InterpreterTask,
                            "SquirrelVM",
                            4096,
                            0,
                            1,
                            &sqTasks[i],
                            1);
  }
#endif

  Serial.println(F("Initialized root interpreter."));

  if (sharedMode)
  {
    replvm= (rootInterpreter->vm);
  }
  else
  {
    replvm = sq_newthread(rootInterpreter->vm, 1024);
  }
  
  replprogram = (struct loadedProgram *)malloc(sizeof(struct loadedProgram));
  sq_setforeignptr(replvm, replprogram);
  replprogram->busy = 0;
  replprogram->callbackRecievers = 0;
  replprogram->parent = rootInterpreter;
  replprogram->vm = replvm;
  replprogram->inputBuffer = 0;
  replprogram->inputBufferLen = 0;
  replprogram->errorfunc = 0;
  replprogram->printfunc = 0;
  replprogram->workingDir = 0;

  sq_resetobject(&ReplThreadObj);
  sq_getstackobj(rootInterpreter->vm, -1, &ReplThreadObj);
  sq_addref(rootInterpreter->vm, &ReplThreadObj);

  //Clear the stack, just in case. It's important the ome thing we leave
  //Be the repl VM.
  sq_settop(rootInterpreter->vm, 1);
  loadFromDir(prgsdir);
  Serial.print("Free Heap: ");
  Serial.print(ESP.getFreeHeap());
  Serial.println(F("\nStarted REPL interpreter\n"));
  //All booted
  Serial.println(acorn_getQuote());
  Serial.print("\n>>>");
}

SQInteger _Acorns::registerFunction(const char *id, SQFUNCTION f, const char *fname)
{
  GIL_LOCK;
  loadedProgram *p = _programForId(id);
  sq_pushroottable(p->vm);
  sq_pushstring(p->vm, fname, -1);
  sq_newclosure(p->vm, f, 0); //create a new function
  sq_newslot(p->vm, -3, SQFalse);
  sq_pop(p->vm, 1); //pops the root table
  GIL_UNLOCK;
  return 0;
}


struct DynamicFunction{
  char * name;
  SQFUNCTION f;
  struct DynamicFunction * next;
};

static struct DynamicFunction dynFunc ={0,0,0};


//Create a dynamic function. Not actually a closure, they are created on
//Demand and consume almost no memory.
SQInteger _Acorns::registerDynamicFunction(SQFUNCTION f, const char *fname)
{
  struct DynamicFunction *d=&dynFunc;
  GIL_LOCK;
  while(d)
  {
    if(d->name==0)
    {
     d->f=f;
     d->name=(char*)malloc(strlen(fname)+1);
     strcpy(d->name, fname);
     return 0;
    }
    if(d->next==0)
    {
      struct DynamicFunction *x=(DynamicFunction*)malloc(sizeof(DynamicFunction));
      d->next =x;
      x->f=f;
      x->name=(char*)malloc(strlen(fname)+1);
      strcpy(x->name, fname);
      return 0;
    }
    d=d->next;
  }
  GIL_UNLOCK;
  return 0;
}





//This is the fallback getter for the dyn funcs
static SQInteger sqgetdynamicfunc(HSQUIRRELVM v)
{
  const char *key;
  if (sq_getstring(v, 2, &key) == SQ_ERROR)
  {
    return sq_throwerror_f(v, F("Key must be str"));
  }
  return 1;

  //Search the linked list
  struct DynamicFunction *d=&dynFunc;
  while(d)
  {
    if(d->name==0)
    {
      return sq_throwerror_f(v, F("No entry by that name"));
    }
    if(strcmp(d->name,key)==0)
    {
      break;
    }
    d=d->next;
  }

  if (d==0)
  {
    return sq_throwerror_f(v, F("No entry by that name"));
  }

  sq_newclosure(v, d->f, 0); //create a new function
  return 0;
}


SQInteger _Acorns::setIntVariable(const char *id, long long value, const char *fname)
{
  GIL_LOCK;
  loadedProgram *p = _programForId(id);
  sq_pushroottable(p->vm);
  sq_pushstring(p->vm, fname, -1);
  sq_pushinteger(p->vm, value);
  sq_newslot(p->vm, -3, SQFalse);
  sq_pop(p->vm, 1); //pops the root table
  GIL_UNLOCK;
  return 0;
}

//*******************************************************/
//Compatibility

//Squirrel needs this for something
int __attribute__((weak)) system(const char *string)
{
}

///********************************************************/
//API Class instance
_Acorns Acorns;
