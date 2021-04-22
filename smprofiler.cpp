#include <Python.h>
#include "cupti_tracer.h"

static PyObject* start(PyObject * self, PyObject * args)
{
  if (!PyArg_Parse(args, "s", &phase))
        return NULL;
  printf("Phase %s\n", phase);
  initTrace();
  Py_INCREF(Py_None);
  return Py_None;
}

static PyObject* stop(PyObject * self, PyObject * args)
{
  finiTrace();
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
