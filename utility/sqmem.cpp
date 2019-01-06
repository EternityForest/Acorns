/*
    see copyright notice in squirrel.h
*/
#include "sqpcheader.h"
#ifndef SQ_EXCLUDE_DEFAULT_MEMFUNCTIONS

#ifdef M5Stack_Core_ESP32
void *sq_vm_malloc(SQUnsignedInteger size){ 
    
    void *x = ps_malloc(size); 
    if(x==0)
        {
            return malloc(size);
        }
    else
        {
            return x;
        }
    }
#else
void *sq_vm_malloc(SQUnsignedInteger size){ return malloc(size); }
#endif


void *sq_vm_realloc(void *p, SQUnsignedInteger SQ_UNUSED_ARG(oldsize), SQUnsignedInteger size){ return realloc(p, size); }

void sq_vm_free(void *p, SQUnsignedInteger SQ_UNUSED_ARG(size)){ free(p); }
#endif
