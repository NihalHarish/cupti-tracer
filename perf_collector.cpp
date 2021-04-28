#include <assert.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/unistd.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <sstream>
#include "perf_collector.h"
#include "smprofiler_timeline.h"

//perf counter syscall
static inline int perf_event_open(struct perf_event_attr * hw,
			    pid_t pid, int cpu, int grp, unsigned long flags)
{
    return syscall(__NR_perf_event_open, hw, pid, cpu, grp, flags);
}

static struct perf_event_attr *pe = NULL;
static int *fds = NULL;
static unsigned int n_counters = 2;
// if running on bare metal instance (e.g. g4dn.metal), one can enable hardware events
static int perf_events[2] = {PERF_COUNT_SW_TASK_CLOCK, PERF_COUNT_SW_CONTEXT_SWITCHES};//, PERF_COUNT_HW_INSTRUCTIONS, PERF_COUNT_HW_CACHE_MISSES, PERF_COUNT_HW_CPU_CYCLES};//, PERF_COUNT_HW_STALLED_CYCLES_FRONTEND, PERF_COUNT_HW_STALLED_CYCLES_BACKEND};
static uint64_t perf_start[2];
static uint64_t start_time;

// phase name provided by user in the python script
static char* phase;

int perf_init(char* phase_name)
{
    phase = phase_name;

    pe = (struct perf_event_attr*)calloc(n_counters, sizeof(struct perf_event_attr));
    fds  = (int*) malloc(sizeof(int) * n_counters);
    if (pe == NULL || fds == NULL) {
                printf("Could not allocate space for counter data: ");
                return -1;
         }

    for(int i = 0; i < n_counters; i++){
    	//Configure each perf counter
	pe[i].size        = sizeof(struct perf_event_attr);
	if (i <= 1)
		pe[i].type = PERF_TYPE_SOFTWARE;
	else
		pe[i].type = PERF_TYPE_HARDWARE;
	pe[i].config = perf_events[i];
	pe[i].disabled = 0;
	pe[i].pinned = 1;
	pe[i].inherit = 1;
//	pe[i].sample_freq = 4000;
//	pe[i].sample_type = PERF_SAMPLE_READ;
//      pe[i].mmap        = 1;
//	pe[i].freq        = 1;

	fds[i] = perf_event_open(&pe[i], getpid(), -1, -1, 0);

	if (fds[i] < 0) {

		printf("Error opening performance counter: %d %d\n", perf_events[i], i);
  		return -1;
	}
    }

    // read perf events
    perf_read_all(perf_start);

    // set start timer
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    start_time = (tv.tv_sec) * 1000000 + tv.tv_usec;

    return 0;
}

void perf_close() {
	uint64_t perf_end[n_counters];
	perf_read_all(perf_end);
  	printf("Task Clocks: %10lu\n",perf_end[0] - perf_start[0]);
	printf("Context Switches: %10lu\n", perf_end[1] - perf_start[1]);
//	printf("Instructions: %10lu\n", perf_end[2] - perf_start[2]);
//	printf("Cache misses: %10lu\n", perf_end[3] - perf_start[3]);
//	printf("Cycles: %10lu\n", perf_end[4] - perf_start[4]);
//	printf("Frontend stalled cycles: %10lu\n", perf_end[5] - perf_start[5]);
//	printf("Backend stalled cycles: %10lu\n", perf_end[6] - perf_start[6]);
        double diff1 = perf_end[2] - perf_start[2];
        double diff2 = perf_end[4] - perf_start[4];

	std::string ss;
  	ss.append(", \"Task Clocks\":");
	ss.append(std::to_string(perf_end[0] - perf_start[0]));
	ss.append(", \"Context Switches\":");
/*        ss.append(std::to_string(perf_end[1] - perf_start[1]));
	ss.append(", \"Instructions\":");
	ss.append(std::to_string(perf_end[2] - perf_start[2]));
	ss.append(", \"Cache misses\":");
	ss.append(std::to_string(perf_end[3] - perf_start[3]));
	ss.append(", \"Cycles\":");
	ss.append(std::to_string(perf_end[4] - perf_start[4]));
	ss.append(", \"IPC\":");
	ss.append(std::to_string(diff1/diff2));*/
  	ss.append(std::to_string(getpid()));

	char event_type = 'X';
        Timeline& tl = Timeline::getInstance();
	struct timeval tv;
        gettimeofday(&tv, nullptr);
        uint64_t current_timestamp = (tv.tv_sec) * 1000000 + tv.tv_usec;

	//record perf metrics in timeline
	tl.SMRecordEvent("perf", phase, start_time, current_timestamp-start_time , (const std::string) ss, event_type);

	for (int i=0; i<n_counters; i++) {
		close(fds[i]);
	}

	if (pe) {
		free(pe);
		pe = NULL;
	}
	if (fds) {
		free(fds);
		fds = NULL;
	}
}


void perf_read_all(uint64_t* vals) {
	for (unsigned int i=0; i<n_counters; i++) {
		uint64_t val;
	        int rc = read(fds[i], &val, sizeof(val));
		vals[i] = val;
	}
}
