#include <stdio.h>
#include <iostream>
#include <cuda.h>
#include <cupti.h>


//libunwind MACRO for local unwind optimization
#define UNW_LOCAL_ONLY
#include "libunwind.h"

void cupti_tracer_init(char *phase);
void cupti_tracer_close();
