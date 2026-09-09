#pragma once
#include <cstddef>
#include <cstdint>
// Experiment ABI. Module/state schema is identical across optimized topologies.
// No ownership changes in compute. The library creating state destroys it.
namespace ps {
using Schema=const char*(*)();
using Bytes=std::size_t(*)();
using Create=void*(*)(int);
using Free=void(*)(void*);
using InitTables=void(*)(int);
using Zone=float*(*)(void*,int,const char*);
using Control=void(*)(void*);
using Tick=void(*)(void*,int,float*,float*);
using Block=void(*)(void*,int,int,float**,float**);
using Compute=void(*)(void*,int,float**);
}
