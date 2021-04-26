#include <Python.h>
#include "cupti_tracer.h"
#include "perf_collector.h"

static uint64_t perf_start[2];

static PyObject* start(PyObject * self, PyObject * args)
{
  if (!PyArg_Parse(args, "s", &phase))
        return NULL;
  printf("Phase %s\n", phase);

  //start perf collection
  perf_init();

  //start cupti tracer
  initTrace();

  Py_INCREF(Py_None);
  return Py_None;
}

static PyObject* stop(PyObject * self, PyObject * args)
{
  // finalize cupti tracer
  finiTrace();

  // finalize perf collection
  perf_close();

  Py_INCREF(Py_None);
  return Py_None;
}

static PyMethodDef methods[] = {
    	 {"start", (PyCFunction) start, METH_O, NULL},
	 {"stop", (PyCFunction) stop, METH_NOARGS, NULL},
	{NULL,NULL,0,NULL}
};

static PyModuleDef definitions = {
  PyModuleDef_HEAD_INIT,
  "smprofiler" ,
  "",
  -1,
  methods
};

PyMODINIT_FUNC PyInit_smprofiler() {
    PyObject *module = PyModule_Create(&definitions);
    return module;
}
