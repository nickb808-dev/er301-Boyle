/* libboyle.swig — SWIG interface for the Boyle ER-301 Object. */

%module boyle_libboyle

%include <od/glue/mod.cpp.swig>

%{
#undef SWIGLUA
#include <Boyle.h>
#include <BoyleMeter.h>
#define SWIGLUA
using namespace boyle;
%}

%include <Boyle.h>
%include <BoyleMeter.h>
