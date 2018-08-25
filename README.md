# Acorns: Squirrel for the ESP32


This is a port of Squirrel to the ESP32. It gives you a REPL in the Arduino serial terminal, and the underlying library aims to provide
an easy API for loading and unloading programs and having multiple running at once.

Try it out with the REPL example sketch, and be warned there's probably bugs and the API isn't stable yet.

This projet is under the MIT license, except the PCG random number generator which is Apache.(See http://www.pcg-random.org/)

## Process Model

Acorns processes are independant VMs that immediately run whatever code you load into them. They have
their own scope which has the root interpreter's scope as a delegate, so you can access true global functions.

They have a process ID which is up to 16 bytes long, and is a null terminated string. They also have a "hash"
value, which is just the first 30 bytes of code.

If you try to load new code into a program that already exists, if the hashes are the same nothing happens. If they are not,
the old program is stopped(after it is no longer busy), and the new one is loaded.


## Configuration File

Acorns can be configured via a file "/spiffs/config.ini". If present, all key/value pairs in the "Acorns" section will be placed
in the "config" dict, which is globally available. This table is just a table, setting keys doesn't make the change persist,
but you can do that with setConfig("key", "value").

Rembember that at the moment, numbers are converted to strings, but that behavior may change, so explicitly convert numbers to what
you want them to be. Values used by the system as opposed to user code will always begin with "sys.", and it is suggested that you use a meaningful
prefix for your values.


In the future, to save RAM, I might not import the config entries that are used by the system.


### Config Entries
These config entries control the behavior of Acorns itself.

#### wifi.ssid
#### wifi.psk
#### wifi.mode
This can be "ap" or "sta"(the default)
### wifi.hostname
If present, this domain name will be advertised using mDNS. You don't need to include the .local at the end.




## Squirrel Language Functions
I've tried to stay close to Arduino where possible. From within Squirrel(In addition to standard squirrel stuff), you have access to:

### random(max), random(min, max)
Get an integer in the given range. Might have a tiny bit of bias, so don't use it for security.
Unlike Arduino, there's no repeatability to this sequence, and the system mixes in a bit of entropy at each call
to keep it unpredictable enough for games, simulations, etc.

Uses the PCG algorithm, and the micros() value.

### millis()/micros()
As Arduino's millis()/micros(). Returns time since boot.

### delay(d)
As Arduino's delay. Releases the GIL, so other threads can run while you wait.

### import(str)

Takes the name of something to import, and tries a variety of strategies to locate the object. Should that object have been previously imported,
This will return that, however the system *may* unload an imported object at any time if there are no references.

Right now the only strategy is the user's import hook.

### pinMode/digitalWrite/digitalRead/analogRead

As the Arduino functions. HIGH, LOW, INPUT, INPUT_PULLUP, and OUTPUT are defined as global variables.

### system.memfree()
Returns the number of bytes of heap remaining. We deviate from the arduino API for the system namespace because they use platform
specific naming, and because fewer namespaces mean less RAM use.

### system.restart()
Completely restart the ESP.


### Serial.begin(baud,[config, rxpin, txpin])
Configure the main serial port(Note that this is the port used for the REPL). config is ignored.

### Serial.write(c)
Write a byte as to the main serial port.


### forceClose(id)
Given a program's ID, close a running program. Note that this will force close even a busy program. It cannot close a program until it exits any C functions it might be doing.
I might change the name of this function.

For the curious, this is possible through a patched version of the squirrel language that allows you to raise non-handlable exceptions.
We also patch the VM to yield the GIL every 250 opcodes.


### stream.writes(str)
In addition to Squirrel's standard functions for read/writing to blobs and files, writes(str) allows directly writing a string.

### stream.reads(size)
In addition to Squirrel's standard functions for read/writing to blobs and files, reads(size) alows reading up to size bytes as a str.

## API

 
### Acorns.makeRequest(const char * id, f, void * arg)

Takes the ID of a program, looks that programs loadedProgram struct up, and calls the function with the program and the arg.

This function call happens under the global interpreter lock, with the VMs busy flag set. It is not safe to make another request from within that function,
but you can safely call squirrel API methods on program->vm.

The type of the function must be: void (*f)(loadedProgram *, void *)

### Acorns.loadProgram(const char * code, const char * id)

Loads some source code into a new program, replacing any old one with that ID, and immediately runs it in a background thread pool thread.

### Acorns.closeProgram(const char * id)

Waits for a program to finish, then stops it.

### Acorns.replChar(char)

Takes a character of input to the REPL loop. This loop has it's own program, and any output is printed to Serial.

### CallbackData * Acorns.acceptCallback(HSQUIRRELVM vm, SQInteger idx, void(*cleanup)(CallbackData * p, void * arg), void * cleanupArg )

Only call from within a native function in Squirrel or a makeRequest funtion.

Takes a squirrel callable object at the stack position,
and returns a CallbackData that can be used to call it from within a makeRequest.

Also places a subscription object on the stack. You have to do something with it, because if it
gets GCed, it cancels the subscription.

When the callbackdata object loses either of the 2 references it begins with, the cleanup function will be called if it isn't null.
When it loses all refeferences, it's memory will be freed.

Callbackdata has the folowing properties:

#### vm
The HSQUIRRELVM * to the VM that requested the callback. It may be set to NULL by squirrel
at any point outside the GIL.

#### callable
The HSQUIRRELOBJ that is to be called. Push it onto the stack within a makeRequest function to call it. It may be set to NULL by squirrel
at any point outside the GIL.


### Acorns.writeToInput(char * id, char * data, int len)
Write the data to the given program's input buffer. The program must exist. If len is -1, use strlen.

### Acorns.runInputBuffer(char * id)
Tell a given program to compile and run it's input buffer. This lets you issue commands in the context of a running program,
and it's a handy way to load code via the network in small packets.

### Acorns.clearInputBuffer(char * id)
Clears the input buffer of a loaded program.


### SQRESULT sq_userImportFunction(HSQUIRRELVM v, const char * c, char len) __attribute__((weak))

Define this function, it takes a string and a length. If you have that object, push it to the stack and return 1,
otherwise return 0. The system will handle all caching.