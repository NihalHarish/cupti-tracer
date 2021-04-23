
#include <stdio.h>
#include <cuda.h>
#include <cupti.h>
#include "activity_definitions.h"
#include <iostream>
#include "smprofiler_timeline.h"

//libunwind MACRO for local unwind optimization
#define UNW_LOCAL_ONLY
#include "libunwind.h"

#define CUPTI_CALL(call)                                                    \
  do {                                                                      \
    CUptiResult _status = call;                                             \
    if (_status != CUPTI_SUCCESS) {                                         \
      const char *errstr;                                                   \
      cuptiGetResultString(_status, &errstr);                               \
      fprintf(stderr, "%s:%d: error: function %s failed with error %s.\n",  \
              __FILE__, __LINE__, #call, errstr);                           \
      exit(-1);                                                             \
    }                                                                       \
  } while (0)

#define BUF_SIZE (32 * 1024)
#define ALIGN_SIZE (8)
#define ALIGN_BUFFER(buffer, align)                                            \
  (((uintptr_t) (buffer) & ((align)-1)) ? ((buffer) + (align) - ((uintptr_t) (buffer) & ((align)-1))) : (buffer))

// start timestamp
static uint64_t start_timestamp;

// phase name provided by user in the python script
static char* phase;

CUpti_SubscriberHandle subscriber;

Timeline& tl = Timeline::getInstance();


static void print_activity(CUpti_Activity *record)
{
  switch (record->kind)
  {
  case CUPTI_ACTIVITY_KIND_DEVICE:
    {
      CUpti_ActivityDevice2 *device = (CUpti_ActivityDevice2 *) record;
      printf("Phase %s DEVICE %s (%u), capability %u.%u, global memory (bandwidth %u GB/s, size %u MB), "
             "multiprocessors %u, clock %u MHz\n",
	     phase,
             device->name, device->id,
             device->computeCapabilityMajor, device->computeCapabilityMinor,
             (unsigned int) (device->globalMemoryBandwidth / 1024 / 1024),
             (unsigned int) (device->globalMemorySize / 1024 / 1024),
             device->numMultiprocessors, (unsigned int) (device->coreClockRate / 1000));
      break;
    }
  case CUPTI_ACTIVITY_KIND_DEVICE_ATTRIBUTE:
    {
      CUpti_ActivityDeviceAttribute *attribute = (CUpti_ActivityDeviceAttribute *)record;
      printf("Phase %s DEVICE_ATTRIBUTE %u, device %u, value=0x%llx\n",
             phase, attribute->attribute.cupti, attribute->deviceId, (unsigned long long)attribute->value.vUint64);
      break;
    }
  case CUPTI_ACTIVITY_KIND_CONTEXT:
    {
      CUpti_ActivityContext *context = (CUpti_ActivityContext *) record;
      printf("Phase %s CONTEXT %u, device %u, compute API %s, NULL stream %d\n",
             phase, context->contextId, context->deviceId,
             get_compute_api_string((CUpti_ActivityComputeApiKind) context->computeApiKind),
             (int) context->nullStreamId);
      break;
    }
  case CUPTI_ACTIVITY_KIND_MEMCPY:
    {
      CUpti_ActivityMemcpy2 *memcpy = (CUpti_ActivityMemcpy2 *) record;
      printf("Phase %s MEMCPY %s [ %llu - %llu ] device %u, context %u, stream %u, size %llu, correlation %u\n",
              phase, get_memcopy_events_string((CUpti_ActivityMemcpyKind)memcpy->copyKind),
              (unsigned long long) (memcpy->start - start_timestamp),
              (unsigned long long) (memcpy->end - start_timestamp),
              memcpy->deviceId, memcpy->contextId, memcpy->streamId,
              (unsigned long long)memcpy->bytes, memcpy->correlationId);
      break;
    }
  case CUPTI_ACTIVITY_KIND_MEMSET:
    {
      CUpti_ActivityMemset *memset = (CUpti_ActivityMemset *) record;
      printf("Phase %s MEMSET value=%u [ %llu - %llu ] device %u, context %u, stream %u, correlation %u\n",
             phase, memset->value,
             (unsigned long long) (memset->start - start_timestamp),
             (unsigned long long) (memset->end - start_timestamp),
             memset->deviceId, memset->contextId, memset->streamId,
             memset->correlationId);
      break;
    }
  case CUPTI_ACTIVITY_KIND_KERNEL:
  case CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL:
    {
      const char* kindString = (record->kind == CUPTI_ACTIVITY_KIND_KERNEL) ? "KERNEL" : "CONC KERNEL";
      CUpti_ActivityKernel3 *kernel = (CUpti_ActivityKernel3 *) record;
      tl.SMRecordEvent(phase, kernel->name, kernel->start, kernel->end - kernel->start,  "X");
      printf("Phase %s %s \"%s\" [ %llu - %llu ] device %u, context %u, stream %u, correlation %u\n",
             phase, kindString,
             kernel->name,
             (unsigned long long) (kernel->start - start_timestamp),
             (unsigned long long) (kernel->end - start_timestamp),
             kernel->deviceId, kernel->contextId, kernel->streamId,
             kernel->correlationId);
      printf("    grid [%u,%u,%u], block [%u,%u,%u], shared memory (static %u, dynamic %u)\n",
             kernel->gridX, kernel->gridY, kernel->gridZ,
             kernel->blockX, kernel->blockY, kernel->blockZ,
             kernel->staticSharedMemory, kernel->dynamicSharedMemory);
      break;
    }
  case CUPTI_ACTIVITY_KIND_DRIVER:
    {
      CUpti_ActivityAPI *api = (CUpti_ActivityAPI *) record;
      tl.SMRecordEvent(phase, "DRIVER", api->start, api->end - api->start,  "X");
      printf("Phase %s DRIVER cbid=%u [ %llu - %llu ] process %u, thread %u, correlation %u\n",
             phase, api->cbid,
             (unsigned long long) (api->start - start_timestamp),
             (unsigned long long) (api->end - start_timestamp),
             api->processId, api->threadId, api->correlationId);
      break;
    }
  case CUPTI_ACTIVITY_KIND_RUNTIME:
    {
      CUpti_ActivityAPI *api = (CUpti_ActivityAPI *) record;
      tl.SMRecordEvent(phase, "DRIVER", api->start, api->end - api->start,  "X");
      printf("Phase %s RUNTIME cbid=%u [ %llu - %llu ] process %u, thread %u, correlation %u\n",
             phase, api->cbid,
             (unsigned long long) (api->start - start_timestamp),
             (unsigned long long) (api->end - start_timestamp),
             api->processId, api->threadId, api->correlationId);
      break;
    }
  case CUPTI_ACTIVITY_KIND_NAME:
    {
      CUpti_ActivityName *name = (CUpti_ActivityName *) record;
      switch (name->objectKind)
      {
      case CUPTI_ACTIVITY_OBJECT_CONTEXT:
        printf("Phase %s NAME  %s %u %s id %u, name %s\n",
               phase, 
	       get_activity_object_string(name->objectKind),
               get_activity_object_id_string(name->objectKind, &name->objectId),
               get_activity_object_string(CUPTI_ACTIVITY_OBJECT_DEVICE),
               get_activity_object_id_string(CUPTI_ACTIVITY_OBJECT_DEVICE, &name->objectId),
               name->name);
        break;
      case CUPTI_ACTIVITY_OBJECT_STREAM:
        printf("Phase %s NAME %s %u %s %u %s id %u, name %s\n",
               phase, 
	       get_activity_object_string(name->objectKind),
               get_activity_object_id_string(name->objectKind, &name->objectId),
               get_activity_object_string(CUPTI_ACTIVITY_OBJECT_CONTEXT),
               get_activity_object_id_string(CUPTI_ACTIVITY_OBJECT_CONTEXT, &name->objectId),
               get_activity_object_string(CUPTI_ACTIVITY_OBJECT_DEVICE),
               get_activity_object_id_string(CUPTI_ACTIVITY_OBJECT_DEVICE, &name->objectId),
               name->name);
        break;
      default:
        printf("Phase %s NAME %s id %u, name %s\n",
               phase,
	       get_activity_object_string(name->objectKind),
               get_activity_object_id_string(name->objectKind, &name->objectId),
               name->name);
        break;
      }
      break;
    }
  case CUPTI_ACTIVITY_KIND_MARKER:
    {
      CUpti_ActivityMarker2 *marker = (CUpti_ActivityMarker2 *) record;
      printf("Phase %s MARKER id %u [ %llu ], name %s, domain %s\n",
             phase, marker->id, (unsigned long long) marker->timestamp, marker->name, marker->domain);
      break;
    }
  case CUPTI_ACTIVITY_KIND_MARKER_DATA:
    {
      CUpti_ActivityMarkerData *marker = (CUpti_ActivityMarkerData *) record;
      printf("Phase %s MARKER_DATA id %u, color 0x%x, category %u, payload %llu/%f\n",
             phase, marker->id, marker->color, marker->category,
             (unsigned long long) marker->payload.metricValueUint64,
             marker->payload.metricValueDouble);
      break;
    }
  case CUPTI_ACTIVITY_KIND_SYNCHRONIZATION:
    {
	  CUpti_ActivitySynchronization *activity_sync = (CUpti_ActivitySynchronization *) record;
	  tl.SMRecordEvent(phase, get_sync_events_string(activity_sync->type), activity_sync->start, activity_sync->end - activity_sync->start,  "X");
	  printf("Phase %s SYNC %s [ %llu, %llu ] contextId %d streamID %d cudaEventId %d correlationId %d\n", 
			  phase, 
			  get_sync_events_string(activity_sync->type),
			  (unsigned long long) activity_sync->start - start_timestamp,
			  (unsigned long long) activity_sync->end - start_timestamp,
			  activity_sync->contextId,
			  activity_sync->streamId,
			  activity_sync->cudaEventId,
			  activity_sync->correlationId);
          break;
    }
  case CUPTI_ACTIVITY_KIND_PC_SAMPLING:
      {
        CUpti_ActivityPCSampling2 *psRecord = (CUpti_ActivityPCSampling2 *)record;

        printf("source %u, functionId %u, pc 0x%x, corr %u, samples %u \n",
          psRecord->sourceLocatorId,
          psRecord->functionId,
          psRecord->pcOffset,
          psRecord->correlationId,
          psRecord->samples);
        break;
      }
  default:
    printf("unknown\n");
    break;
  }
}

void CUPTIAPI bufferRequested(uint8_t **buffer, size_t *size, size_t *maxNumRecords)
{
  uint8_t *bfr = (uint8_t *) malloc(BUF_SIZE + ALIGN_SIZE);
  if (bfr == NULL) {
    printf("Error: out of memory\n");
    exit(-1);
  }

  *size = BUF_SIZE;
  *buffer = ALIGN_BUFFER(bfr, ALIGN_SIZE);
  *maxNumRecords = 0;
}

void CUPTIAPI bufferCompleted(CUcontext ctx, uint32_t streamId, uint8_t *buffer, size_t size, size_t validSize)
{
  CUptiResult status;
  CUpti_Activity *record = NULL;

  if (validSize > 0) {
    do {
      status = cuptiActivityGetNextRecord(buffer, validSize, &record);
      if (status == CUPTI_SUCCESS) {
        print_activity(record);
      }
      else if (status == CUPTI_ERROR_MAX_LIMIT_REACHED)
        break;
      else {
        CUPTI_CALL(status);
      }
    } while (1);

  }

  free(buffer);
}


//Callback called on every CUDA API call entry
static void OnDriverApiEnter(CUpti_CallbackDomain domain, CUpti_driver_api_trace_cbid cbid, const CUpti_CallbackData *cbdata)
{
	// get timestamp
	uint64_t tsc;
	if (cuptiDeviceGetTimestamp(cbdata->context, &tsc) == CUPTI_SUCCESS){
		printf("Enter API callback %llu %s %s \n", (unsigned long long) tsc-start_timestamp, cbdata->symbolName, cbdata->functionName);
	}
	//CUpti_driver_api_trace_cbid cbid_new = (CUpti_driver_api_trace_cbid) cbid; 
        printf("cbid %d", cbid);
	switch (cbid) {
	case CUPTI_DRIVER_TRACE_CBID_cuLaunchKernel: 
		{
			printf("symbolName %s correlationID %d\n", cbdata->symbolName, cbdata->correlationId);
			break;
		}

	case CUPTI_DRIVER_TRACE_CBID_cuLaunchCooperativeKernel:
	case CUPTI_DRIVER_TRACE_CBID_cuLaunchCooperativeKernelMultiDevice: {
       	    printf("symbolName %s correlationID %d\n", cbdata->symbolName, cbdata->correlationId);
	    break;
     	    }
	}
	
        /***
        // libunwind variables
	unw_word_t ip;
        unw_cursor_t cursor;
        unw_context_t uc;
        char funcName[128];

        uint32_t correlationId = cbdata->correlationId;

	// stack unwinding with libunwind. record first 20 function calls in the stack.
	unw_getcontext(&uc);

	if (unw_init_local(&cursor, &uc) <0)
      		printf("unw_init_local failed\n");

    	int count = 0;
	while (unw_step(&cursor) > 0 && count < 20) {
      		unw_get_reg(&cursor, UNW_REG_IP, &ip);
      		unw_get_proc_name(&cursor, funcName, sizeof(funcName), NULL);
		printf("%d 0x%016lx %s\n", count, (unsigned long)ip, funcName);
      		count++;
    }***/
}

//Callback called on every CUDA API call exit
static void OnDriverApiExit(CUpti_CallbackDomain domain, CUpti_CallbackId cbid, const CUpti_CallbackData *cbdata)
{
	uint64_t tsc;
	if (cuptiDeviceGetTimestamp(cbdata->context, &tsc) == CUPTI_SUCCESS){
		printf("Exit API callback %llu %s %s \n", (unsigned long long) tsc-start_timestamp, cbdata->symbolName, cbdata->functionName);		
        }
}

//registered callback
static void CUPTIAPI trace_callback(void *userdata, CUpti_CallbackDomain domain,  CUpti_CallbackId cbid, const void *cbdata)
{
  const CUpti_CallbackData *cbInfo = (CUpti_CallbackData *)cbdata;
  if (cbInfo->callbackSite == CUPTI_API_ENTER){
	OnDriverApiEnter(domain, (CUpti_driver_api_trace_cbid) cbid, cbInfo);
  }
  else if (cbInfo->callbackSite == CUPTI_API_EXIT)
  {
	  if (cbid == CUPTI_DRIVER_TRACE_CBID_cuLaunchCooperativeKernelMultiDevice){
		printf("correlationID: %d", cbInfo->correlationId);
	  }
	  OnDriverApiExit(domain, cbid, cbInfo);
  }
  if (domain == CUPTI_CB_DOMAIN_RESOURCE) {
	printf("Callback %s \n", cbInfo->symbolName);
    	printf("Callback %s \n", cbInfo->functionName);
	}
 }

void
initTrace()
{
  // enable activities
  CUPTI_CALL(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_DEVICE));
  CUPTI_CALL(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_CONTEXT));
  CUPTI_CALL(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_DRIVER));
  CUPTI_CALL(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_RUNTIME));
  CUPTI_CALL(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_MEMCPY));
  CUPTI_CALL(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_MEMSET));
  CUPTI_CALL(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_NAME));
  CUPTI_CALL(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_MARKER));
  CUPTI_CALL(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL));  
  CUPTI_CALL(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_SYNCHRONIZATION)); 
//  CUPTI_CALL(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_PC_SAMPLING));

  // register callback
//  CUPTI_CALL(cuptiSubscribe(&subscriber, (CUpti_CallbackFunc)trace_callback, NULL));
//  CUPTI_CALL(cuptiEnableDomain(1, subscriber,  CUPTI_CB_DOMAIN_RUNTIME_API));
//  CUPTI_CALL(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_OVERHEAD))  
//  CUPTI_CALL(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_FUNCTION)); 
//  CUPTI_CALL(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_SOURCE_LOCATOR));
//  CUPTI_CALL(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_INSTRUCTION_EXECUTION));
//  Register callbacks for buffer requests and for buffers completed by CUPTI.
  CUPTI_CALL(cuptiActivityRegisterCallbacks(bufferRequested, bufferCompleted));

  size_t attrValue = 0, attrValueSize = sizeof(size_t);
  CUPTI_CALL(cuptiActivityGetAttribute(CUPTI_ACTIVITY_ATTR_DEVICE_BUFFER_SIZE, &attrValueSize, &attrValue));
  printf("%s = %llu B\n", "CUPTI_ACTIVITY_ATTR_DEVICE_BUFFER_SIZE", (long long unsigned)attrValue);
  attrValue *= 2;
  CUPTI_CALL(cuptiActivitySetAttribute(CUPTI_ACTIVITY_ATTR_DEVICE_BUFFER_SIZE, &attrValueSize, &attrValue));

  CUPTI_CALL(cuptiActivityGetAttribute(CUPTI_ACTIVITY_ATTR_DEVICE_BUFFER_POOL_LIMIT, &attrValueSize, &attrValue));
  printf("%s = %llu\n", "CUPTI_ACTIVITY_ATTR_DEVICE_BUFFER_POOL_LIMIT", (long long unsigned)attrValue);
  attrValue *= 2;
  CUPTI_CALL(cuptiActivitySetAttribute(CUPTI_ACTIVITY_ATTR_DEVICE_BUFFER_POOL_LIMIT, &attrValueSize, &attrValue));

  CUPTI_CALL(cuptiGetTimestamp(&start_timestamp));
}

void finiTrace()
{
   // Force flush any remaining activity buffers before termination of the application
   CUPTI_CALL(cuptiActivityFlushAll(1));
  // CUPTI_CALL(cuptiUnsubscribe(subscriber));
}

