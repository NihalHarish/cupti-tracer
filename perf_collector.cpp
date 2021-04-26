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
#include <perf_collector.h>

//perf counter syscall
static inline int perf_event_open(struct perf_event_attr * hw,
			    pid_t pid, int cpu, int grp, unsigned long flags)
{
    return syscall(__NR_perf_event_open, hw, pid, cpu, grp, flags);
}

static struct perf_event_attr *pe = NULL;
static int *fds = NULL;
static unsigned int n_counters = 2;
static int perf_events[2] = {PERF_COUNT_SW_TASK_CLOCK, PERF_COUNT_SW_CONTEXT_SWITCHES};
static uint64_t perf_start[2];

int perf_init(){

    pe = (struct perf_event_attr*)calloc(n_counters, sizeof(struct perf_event_attr));
    fds  = (int*) malloc(sizeof(int) * n_counters);
    if (pe == NULL || fds == NULL) {
                printf("Could not allocate space for counter data: ");
                return -1;
         }
    for(int i = 0; i < n_counters; i++){
    	//Configure each perf counter
	pe[i].size        = sizeof(struct perf_event_attr);
	pe[i].type = PERF_TYPE_SOFTWARE;//PERF_TYPE_RAW;
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
		printf("Error opening performance counter\n");
  		return -1;
	}
    }
    perf_read_all(perf_start);
    return 0;
}

void perf_close() {
	uint64_t perf_end[2];
	perf_read_all(perf_end);
  	printf("Task Clocks: %10lu, Context Switches: %10lu\n",perf_end[0] - perf_start[0], perf_end[1] - perf_start[1]);

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
