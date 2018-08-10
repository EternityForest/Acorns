

extern "C"
{
#include <squirrel.h>
#include <sqstdblob.h>
#include <sqstdsystem.h>
#include <sqstdio.h>
#include <sqstdmath.h>
#include <sqstdstring.h>
#include <sqstdaux.h>
}

#include "Arduino.h"
#include "acorns.h"

/************************************************************************************************************/
//Data Structures, forward declarations


//The userdata struct for each loadedProgram interpreter
struct loadedProgram
{

  //Points to the slot in the function table where the pointer to this is stored,
  //So we can zero it when we free it.
  struct loadedProgram ** slot;
  //This is how we can know which program to replace when updating with a new version
  char programID[16];
  //The first 30 bytes of a file identify its "version" so we don't
  //replace things that don't need replacing.
  char hash[30];

  //This is the input buffer that gives us an easy way to send things to a program
  //in excess of the 1500 byte limit for UDP. We might also use it for other stuff later.
  char * inputBuffer;

  //How many bytes are in the input buffer.
  int inputBufferLen;

  //1  or above if the program is busy, don't mess with it in any way except setting/getting vars and making sub-programs.
  //0 means you can delete, replace, etc

  //When a child interpreter runs, it increments all parents and itself.
  //In this way it is kind of like a reference count.
  //Note that it's not the same as GIL, you can yield the GIL but still flag a
  //program as busy so the other tasts don't mess with it.
  char busy;

  HSQUIRRELVM vm;

  //We often use sq_newthread, this is where we store the thread handle so
  //We don't have to clutter up a VM namespace.
  HSQOBJECT threadObj;
  
  //A parent proram, used because we don't want to stop a running program's parent
  struct loadedProgram * parent;

  //A reference count for this struct itself.
  //We only change it under the GIL lock.
  //It's purpose is that the interpreter thread must know if
  //someone has deleted and replaced a VM while we yield.

  //This lets us set the VM to 0 to indicate that a program has ended.
  //And still have this struct around to read that info from.

  //Essentially, this implements zombie processes, handles to things that don't exist.
  char refcount;

};

//Represents a request to the thread pool to execute the given function
//with the loadedProgram and the arg as its params.
struct Request
{
  void (*f)(loadedProgram *, void *);
  //Pointer tp the target of the request
  struct loadedProgram * program;
  //Object that represents what the interpreter should do.
  //If it === interpreter, it means run loaded code
  void * arg;
};

///declarations

static void deref_prog(loadedProgram *);




/**************************************************************************************/
//Free/busy functions

//Mark a program as busy by incrementing the busy reference count of it and all parents
static void _setbusy(struct loadedProgram * p)
{
  while (p)
  {
    p->busy += 1;
    p = p->parent;
  }

}

//Mark a program as busy by decrementing the reference count
static void _setfree(struct loadedProgram * p)
{
  while (p)
  {
    p->busy -= 1;
    p = p->parent;
  }
}



/***************************************************/
//The GIL

//The global interpreter lock. Almost any messing
//with of interpreters uses this.
static SemaphoreHandle_t gil_lock;

//Wait 10 million ticks which is probably days, but still assert it if it fails
#define GIL_LOCK assert(xSemaphoreTake(gil_lock,10000000))
#define GIL_UNLOCK xSemaphoreGive(gil_lock)




/*******************************************************************/
//Thread pool stuff


//This is the thread pool
static TaskHandle_t sqTasks[ACORNS_THREADS];


//The queue going into the thread pool
static QueueHandle_t request_queue;

//Create and send a request to the thread pool
static void _makeRequest(loadedProgram * program, void (*f)(loadedProgram *, void *), void * arg)
{
  struct Request r;

  //Only call under gil because of this.
  //The fact that it is in the queue counts as a reference, and it's up to the thread pool
  //thread to deref it.
  program->refcount++;


  r.program = program;
  r.f = f;
  r.arg = arg;
  xQueueSend(request_queue, &r, portMAX_DELAY);
}


//The loop thar threads in the thread pool actually run
static void InterpreterTask(void *)
{
  struct Request rq;
  struct loadedProgram * ud;

  while (1)
  {
    xQueueReceive(request_queue, &rq, portMAX_DELAY);

    GIL_LOCK;


    while (rq.program->busy)
    {
      GIL_UNLOCK;
      Serial.println("Still busy");
      vTaskDelay(10);
      GIL_LOCK;

      //If someone stopped the program while we were waiting
      if (rq.program->vm == 0)
      {
        goto fexit;
      }
    }

    Serial.print("x");
    _setbusy(rq.program);
    Serial.println("set");
    rq.f(rq.program, rq.arg);
    _setfree(rq.program);

fexit:
    deref_prog(rq.program);
    GIL_UNLOCK;
  }
}


//**********************************************************************************8
//program management

static struct loadedProgram* rootInterpreter = 0;

//This is our "program table"
static struct loadedProgram * loadedPrograms[ACORNS_MAXPROGRAMS];



//Given a string program ID, return the loadedProgram object
//If it's not loaded.
static struct loadedProgram* _programForId(const char * id)
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
  for (char i = 0; i < ACORNS_MAXPROGRAMS; i++)
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



//Only call under gil
static void deref_prog(loadedProgram * p)
{
  p->refcount --;
  if (p->refcount == 0)
  {
    free(p);
  }
}



//Function that the thread pool runs to run whatever program is on the top of an interpreter's stack
static void runLoaded(loadedProgram * p, void * d)
{
  Serial.println("runloaded");
  Serial.println((int)p);

  Serial.println((int)(p->vm));
  SQInteger oldtop = sq_gettop(p->vm);
  sq_pushroottable(p->vm);
  sq_call(p->vm, 1, SQFalse, SQTrue);
  sq_settop(p->vm, oldtop);
}


//Close a running program, waiting till all children are no longer busy.
static int _closeProgram(const char * id)
{
  
  loadedProgram * old = _programForId(id);
  //Check if programs are the same

  if (old)
  {
 
    ///Something can be "busy" without holding the lock if it yields.
    while (old->busy)
    {
      GIL_UNLOCK;
      delay(2500);
      GIL_LOCK;
    }

    //Close the VM and deref the task handle now that the VM is no longer busy.
    //The way we close the VM is to get rid of references to its thread object.
    sq_release(old->vm, &old->threadObj);
    old->vm = 0;
    
    deref_prog(old);

  }
}

//Load a new program from source code with the given ID, replacing any with the same ID if the
//first 30 bytes are different. The new program will have its own global scope that an inner scope of the root interpreter's.
//You will be able to use getdelegate to get at the root table directly.
static int _loadProgram(const char * code, const char * id)
{
  loadedProgram * old = _programForId(id);
  //Check if programs are the same

  if (old)
  {
    //Check if the versions are the same
    if (memcmp(old->hash, code, 30) == 0)
    {
      return 0;
    }


    ///Something can be "busy" without holding the lock if it yields.
    while (old->busy)
    {
      GIL_UNLOCK;
      delay(2500);
      GIL_LOCK;
    }

    //Close the VM and deref the task handle now that the VM is no longer busy.
    sq_close(old->vm);
    deref_prog(old);

  }

  //passing a null pointer tells it to use the input buffer
  if (code == 0)
  {
    code = "//comment";
  }

  //Find a free interpreter slot
  for (char i = 0; i < ACORNS_MAXPROGRAMS; i++)
  {
    if (loadedPrograms[i] == 0)
    {

      //Note that the old struct is still out there in heap until all the refs are gone
      loadedPrograms[i] = (struct loadedProgram *)malloc(sizeof(struct loadedProgram));
      loadedPrograms[i]->parent = rootInterpreter;
      loadedPrograms[i]->refcount = 1;

      //This is so the dereference function can free the slot in the table
      //By itself
      loadedPrograms[i]->slot = &loadedPrograms[i];

      HSQUIRRELVM vm;
      vm = sq_newthread(rootInterpreter->vm, 1024);
      sq_resetobject(&loadedPrograms[i]->threadObj);


      //Get the thread handle, ref it so it doesn't go away, then store it in the loadedProgram
      //and pop it. Now the thread is independant
      sq_getstackobj(rootInterpreter->vm,-1, &loadedPrograms[i]->threadObj);
      sq_addref(vm, &loadedPrograms[i]->threadObj);
      sq_pop(rootInterpreter->vm, 1);


      //Make a new table as the root table of the VM, then set root aa it's delegate(The root table that is shared with the parent)
      //then set that new table as our root. This way we can access parent functions but have our own scope.
      sq_newtable(vm);
      sq_pushroottable(vm);
      sq_setdelegate(vm,-2);
      sq_setroottable(vm);
      
      if (SQ_SUCCEEDED(sq_compilebuffer(vm, code, strlen(code) + 1, _SC(id), SQTrue))) {
        loadedPrograms[i]->vm = vm;
        _makeRequest(loadedPrograms[i], runLoaded, 0);
      }
      else
      {
        Serial.println("Failed to compile code");
      }


      memcpy(loadedPrograms[i]->hash, code, 30);
      strcpy(loadedPrograms[i]->programID, id);
      loadedPrograms[i]->programID[strlen(id)] = 0;
      loadedPrograms[i]->busy = 0;
      return 0;
    }
  }

  //err, could not find free slot for program
  return 1;
}


//*********************************************************************************
//REPL


static HSQUIRRELVM replvm;
static SQChar  replbuffer[1024];
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
  if (c == _SC('\n')) {

    if (blocks)
    {
      Serial.print("\n...");
    }
  }
  else
  {
    Serial.write(c);
  }

  if (c == _SC('\n')) {
    if (replpointer > 0 && replbuffer[replpointer - 1] == _SC('\\'))
    {
      replbuffer[replpointer - 1] = _SC('\n');

    }
    else if (blocks == 0)goto doing;
    replbuffer[replpointer++] = _SC('\n');
  }

  else if(c == _SC('\\'))
  {
    esc = 1;
  }
  else if(string && esc)
  {
       replbuffer[replpointer++] = (SQChar)c; 
  }
  else if (c == _SC('}') && !string) {
    blocks--;
    replbuffer[replpointer++] = (SQChar)c;
  }
  else if (c == _SC('{') && !string) {
    blocks++;
    replbuffer[replpointer++] = (SQChar)c;
  }
  else if (c == _SC('"') || c == _SC('\'')) {
    string = !string;
    replbuffer[replpointer++] = (SQChar)c;
  }
  else if (replpointer >= 1024 - 1) {
    Serial.print("sq : input line too long\n");
    goto resetting;
  }
  else {
    replbuffer[replpointer++] = (SQChar)c;
  }

  esc=0;
  return;
doing:
  replbuffer[replpointer] = _SC('\0');

  if (replbuffer[0] == _SC('=')) {
    sprintf(sq_getscratchpad(replvm, 1024), _SC("return (%s)"), &replbuffer[1]);
    memcpy(replbuffer, sq_getscratchpad(replvm, -1), (scstrlen(sq_getscratchpad(replvm, -1)) + 1)*sizeof(SQChar));
    retval = 1;
  }
  replpointer = scstrlen(replbuffer);
  if (replpointer > 0) {
    SQInteger oldtop = sq_gettop(replvm);
    if (SQ_SUCCEEDED(sq_compilebuffer(replvm, replbuffer, replpointer, _SC("interactive console"), SQTrue))) {
      sq_pushroottable(replvm);
      if (SQ_SUCCEEDED(sq_call(replvm, 1, retval, SQTrue)) &&  retval) {
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
resetting:
  replpointer = 0;
  blocks = 0;
  string = 0;
  retval = 0;
  Serial.print("\n>>>");

}


//**************************************************************************************/
//General system control


static void printfunc(HSQUIRRELVM SQ_UNUSED_ARG(v), const SQChar *s, ...)
{
  char buf[256];
  va_list vl;
  va_start(vl, s);
  vsnprintf(buf, 256, s, vl);
  va_end(vl);
  Serial.print(buf);
}

static void errorfunc(HSQUIRRELVM SQ_UNUSED_ARG(v), const SQChar *s, ...)
{
  char buf[256];
  va_list vl;
  va_start(vl, s);
  vsnprintf(buf, 256, s, vl);
  va_end(vl);
  Serial.println("");
  Serial.print(buf);
}



//Adds the basic standard libraries to the squirrel VM
static void addlibs(HSQUIRRELVM v)
{
  sq_setprintfunc(v, printfunc, errorfunc);
  sq_pushroottable(v);
  sqstd_register_bloblib(v);
  sqstd_register_iolib(v);
  sqstd_register_systemlib(v);
  sqstd_register_mathlib(v);
  sqstd_register_stringlib(v);

  //aux library
  //sets error handlers
  sqstd_seterrorhandlers(v);
  sq_pop(v, 1);
}


//Initialize squirrel task management
void _Acorns::begin()
{
  for (char i = 0; i < ACORNS_MAXPROGRAMS; i++)
  {
    loadedPrograms[i] == 0;
  }


  //Start the root interpreter
  const char * code =  "'The only line in this root program currently is this comment";
  rootInterpreter = (struct loadedProgram *)malloc(sizeof(struct loadedProgram));

  rootInterpreter->vm = sq_open(1024); //creates a VM with initial stack size 1024
  memcpy(rootInterpreter->hash, code, 30);
  rootInterpreter->busy = 0;

  gil_lock = xSemaphoreCreateBinary( );
  xSemaphoreGive(gil_lock);
  request_queue = xQueueCreate( 25, sizeof(struct Request));

  for (char i = 0; i < ACORNS_THREADS; i++)
  {
    xTaskCreatePinnedToCore(InterpreterTask,
                            "SquirrelVM",
                            6000,
                            0,
                            1,
                            &sqTasks[i],
                            1
                           );
  }

  rootInterpreter->vm = sq_open(1024);
  Serial.println("Initialized root interpreter");

  addlibs(rootInterpreter->vm);
  Serial.println("Added core libraries");

  replvm = sq_newthread(rootInterpreter->vm, 1024);
  Serial.println("Started REPL interpreter");

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
