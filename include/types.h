#ifndef FROSTBYTE_TYPES_H
#define FROSTBYTE_TYPES_H

typedef unsigned char uint8;
typedef signed char int8;
typedef unsigned short uint16;
typedef signed short int16;
typedef unsigned int uint32;
typedef signed int int32;
typedef unsigned long uint64;
typedef signed long int64;

typedef unsigned long usize;
typedef signed long isize;
typedef unsigned long uintptr;

typedef enum { false = 0, true = 1 } bool;

#define nullptr ((void*)0)

#endif
