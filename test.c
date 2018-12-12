#include "buddy.h"
#include "bitmem.h"

#include <assert.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>


#define GIGA				1000000000	// 10^9 (or inverse of nano)
#define TU_PER_SEC			1000000
#define NANOS_PER_TU		(GIGA / TU_PER_SEC)
#define TIME_USED_CLOCK		CLOCK_MONOTONIC

#define TEST_COUNT			11

static char const * const TIME_UNIT = "us";


#define get_now(time)	clock_gettime(TIME_USED_CLOCK, time)

static inline double get_time_since(struct timespec *time) {
	struct timespec now;
	get_now(&now);
	double seconds = now.tv_sec - time->tv_sec;
	long nanos = (now.tv_nsec + (GIGA - time->tv_nsec)) % GIGA;
	return seconds / TU_PER_SEC + (double) nanos / NANOS_PER_TU;
}

/// Benchmark given allocate and free functions
///
/// The first parameter is reference to the allocate function to be tested
/// The second parameter is reference to the free function to be tested
/// The third parameter is a pointer to an array of doubles of size TEST_COUNT to store resulting times in
void benchmark(void *(*allocateFunc)(size_t), void (*freeFunc)(void *), double *times);

int main() {
	/*
	printf("Running test.\n");
	verboseTest();
	printf("Test finished succesfully!\n");
	*/
	
	double default_times[TEST_COUNT] = {0.0};
	double buddy_times[TEST_COUNT] = {0.0};
	double bitmem_times[TEST_COUNT] = {0.0};
	
	struct timespec start_time;
	double duration;
	printf("Benchmarking default memory management:\n");
	get_now(&start_time);
	benchmark(&malloc, &free, default_times);
	duration = get_time_since(&start_time);
	
	printf("\nDefault memory management took total of %f%s\n\n", duration, TIME_UNIT);
	
	printf("Benchmarking buddy memory management:\n");
	get_now(&start_time);
	benchmark(&balloc, &bfree, buddy_times);
	duration = get_time_since(&start_time);
	
	printf("\nBuddy memory management took total of %f%s\n\n", duration, TIME_UNIT);
	
	printf("Benchmarking bitmem memory management:\n");
	get_now(&start_time);
	benchmark(&bm_alloc, &bm_free, bitmem_times);
	duration = get_time_since(&start_time);
	
	printf("\nBitmem memory management took total of %f%s\n\n", duration, TIME_UNIT);
	
	printf("Resulting times:\n");
	printf("test                        ||   default  ||    buddy   ||    bitmem\n");
	printf("tiny allocations            || %8.2f%s || %8.2f%s || %8.2f%s\n",
		default_times[0], TIME_UNIT, buddy_times[0], TIME_UNIT, bitmem_times[0], TIME_UNIT);
	printf("zig-zag                     || %8.2f%s || %8.2f%s || %8.2f%s\n",
		default_times[1], TIME_UNIT, buddy_times[1], TIME_UNIT, bitmem_times[1], TIME_UNIT);
	printf("occasional free             || %8.2f%s || %8.2f%s || %8.2f%s\n",
		default_times[2], TIME_UNIT, buddy_times[2], TIME_UNIT, bitmem_times[2], TIME_UNIT);
	printf("large allocations           || %8.2f%s || %8.2f%s || %8.2f%s\n", 
		default_times[3], TIME_UNIT, buddy_times[3], TIME_UNIT, bitmem_times[3], TIME_UNIT);
	printf("increasing size allocations || %8.2f%s || %8.2f%s || %8.2f%s\n",
		default_times[4], TIME_UNIT, buddy_times[4], TIME_UNIT, bitmem_times[4], TIME_UNIT);
	printf("sweeping free               || %8.2f%s || %8.2f%s || %8.2f%s\n", 
		default_times[5], TIME_UNIT, buddy_times[5], TIME_UNIT, bitmem_times[5], TIME_UNIT);
	printf("clamped allocations         || %8.2f%s || %8.2f%s || %8.2f%s\n",
		default_times[6], TIME_UNIT, buddy_times[6], TIME_UNIT, bitmem_times[6], TIME_UNIT);
	printf("random allocations          || %8.2f%s || %8.2f%s || %8.2f%s\n", 
		default_times[7], TIME_UNIT, buddy_times[7], TIME_UNIT, bitmem_times[7], TIME_UNIT);
	printf("even free                   || %8.2f%s || %8.2f%s || %8.2f%s\n",
		default_times[8], TIME_UNIT, buddy_times[8], TIME_UNIT, bitmem_times[8], TIME_UNIT);
	printf("flipping                    || %8.2f%s || %8.2f%s || %8.2f%s\n",
		default_times[9], TIME_UNIT, buddy_times[9], TIME_UNIT, bitmem_times[9], TIME_UNIT);
	printf("complete cleanup            || %8.2f%s || %8.2f%s || %8.2f%s\n",
		default_times[10], TIME_UNIT, buddy_times[10], TIME_UNIT, bitmem_times[10], TIME_UNIT);
	double default_duration = 0.0;
	double buddy_duration = 0.0;
	double bitmem_duration = 0.0;
	for (int i = 0; i < TEST_COUNT; ++i) {
		default_duration += default_times[i];
		buddy_duration += buddy_times[i];
		bitmem_duration += bitmem_times[i];
	}
	printf("total time                  || %8.2f%s || %8.2f%s || %8.2f%s\n",
		default_duration, TIME_UNIT, buddy_duration, TIME_UNIT, bitmem_duration, TIME_UNIT);
	return 0;
}

struct MemUsage {
	int maxVirtual, maxPhysical;
	int curVirtual, curPhysical;
};

void checkMemoryUsage(struct MemUsage *usage) {
	char buffer[1024] = "";

	FILE* file = fopen("/proc/self/status", "r");
	
	while (fscanf(file, " %1023s", buffer) == 1) {
		if (strcmp(buffer, "VmRSS:") == 0) fscanf(file, " %d", &usage->curPhysical);
		if (strcmp(buffer, "VmSize:") == 0) fscanf(file, " %d", &usage->curVirtual);
	}
	
	if (usage->curPhysical > usage->maxPhysical) usage->maxPhysical = usage->curPhysical;
	if (usage->curVirtual > usage->maxVirtual) usage->maxVirtual = usage->curVirtual;
}

struct MemUsage initial = {0, 0, 0, 0};

void setInitialMemUsage(struct MemUsage *usage) {
	initial.maxVirtual = usage->maxVirtual;
	initial.curVirtual = usage->curVirtual;
	initial.maxPhysical = usage->maxPhysical;
	initial.curPhysical = usage->curPhysical;
}

void printMemUsage(struct MemUsage *usage) {
	printf("{ (virtual: max=%dKB, cur=%dKB), (physical: max=%dKB, cur=%dKB) }",
		usage->maxVirtual - initial.maxVirtual,
		usage->curVirtual - initial.curVirtual,
		usage->maxPhysical - initial.maxPhysical,
		usage->curPhysical - initial.curPhysical);
}

enum flag {open, taken};

typedef struct test_ptr {
	enum flag status;
	void *p;
} test_ptr;

#define assign(ptr, func)					\
	assert(ptr.status == open);				\
	ptr.p = func;							\
	assert(ptr.p != NULL);					\
	*(long int *)ptr.p = (long int) ptr.p;	\
	ptr.status = taken

#define clear(ptr, func)			\
	assert(ptr.status == taken);	\
	func(ptr.p);					\
	ptr.status = open

/// Run a benchmark using given allocator and deallocator
void benchmark(void *(*allocF)(size_t), void (*freeF)(void *), double *times) {
	
	struct MemUsage initialUsage = {0, 0, 0, 0};
	struct MemUsage memUsage = {0, 0, 0, 0};
	
	struct timespec start_time;
	
	test_ptr pointers[1024];
	
	for (int i = 0; i < 1024; ++i) {
		pointers[i].status = open;
	}
	
	checkMemoryUsage(&initialUsage);
	
	printf("Initial memory usage: ");
	printMemUsage(&initialUsage);
	setInitialMemUsage(&initialUsage);
	printf("\n");
	
	printf("benchmarking...\n");
	get_now(&start_time);
	for (int i = 0; i < 50; ++i) {
		// Pretend some weird regular structure on the heap
		switch (i % 3) {
			case 0:
				assign(pointers[i], allocF(8));
				break;
				
			case 1:
				assign(pointers[i], allocF(16));
				break;
			
			case 2:
				assign(pointers[i], allocF(64));
				break;
		}
	}
	times[0] = get_time_since(&start_time);
	
	// exclude io from time usage
	printf("tiny allocations took                     %f%s, usage: ", times[0], TIME_UNIT);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);
	
	get_now(&start_time);
	// Tiny zig-zag
	for (int i = 50; i < 100; ++i) {
		if (i % 2 == 0) {
			assign(pointers[i], allocF(100));
		} else {
			assign(pointers[i], allocF(10));
		}
	}
	times[1] = get_time_since(&start_time);
	
	printf("\nzig-zag took                              %f%s, usage: ", times[1], TIME_UNIT);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);
	
	get_now(&start_time);
	// cler some, but not really aligned to anything
	for (int i = 3; i < 100; i += 7) {
		clear(pointers[i], freeF);
	}
	times[2] = get_time_since(&start_time);

	printf("\nfreeing some items took                   %f%s, usage: ", times[2], TIME_UNIT);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);
	
	get_now(&start_time);
	// I'm too lazy to do this cleaner, so I reuse pointers to avoid headache later
	for (int i = 3; i < 100; i += 7) {
		assign(pointers[i], allocF(1000));
	}
	times[3] = get_time_since(&start_time);
	
	printf("\nallocating some large blocks took         %f%s, usage: ", times[3], TIME_UNIT);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);
	
	get_now(&start_time);
	for (int i = 100; i < 200; ++i) {
		assign(pointers[i], allocF(20 + (i - 100) * 32));
	}
	times[4] = get_time_since(&start_time);

	printf("\nallocating increasinly large blocks took  %f%s, usage: ", times[4], TIME_UNIT);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);
	
	get_now(&start_time);
	// Sweeping clean
	for (int i = 20; i < 80; ++i) {
		clear(pointers[i], freeF);
	}
	times[5] = get_time_since(&start_time);
	
	printf("\nsweeping clean of some objects took       %f%s, usage: ", times[5], TIME_UNIT);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);

	get_now(&start_time);
	for (int i = 20; i < 80; ++i) {
		assign(pointers[i], allocF(8 + ((i - 20) * 13) % 64));
	}
	times[6] = get_time_since(&start_time);
	
	printf("\nclamped blocks took                       %f%s, usage: ", times[6], TIME_UNIT);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);
	
	get_now(&start_time);
	for (int i = 200; i < 512; ++i) {
		switch (i % 8) {
			case 0:
				assign(pointers[i], allocF(5 + ((i - 200) * 31) % 117));
				break;
				
			case 1:
			case 2:
			case 3:
			case 4:
				assign(pointers[i], allocF(64));
				break;
				
			case 5:
			case 6:
				assign(pointers[i], allocF(i));
				break;
				
			case 7:
				assign(pointers[i], allocF(2000));
				break;
		}
	}
	times[7] = get_time_since(&start_time);
	
	printf("\nlots of random allocations took           %f%s, usage: ", times[7], TIME_UNIT);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);
	
	get_now(&start_time);
	for (int i = 0; i < 512; i += 2) {
		clear(pointers[i], freeF);
	}
	times[8] = get_time_since(&start_time);
	
	printf("\neven frees took                           %f%s, usage: ", times[8], TIME_UNIT);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);
	
	get_now(&start_time);
	for (int i = 0; i < 512; i++) {
		if (i % 2 == 0) {
			assign(pointers[i], allocF(12 + i));
		} else {
			clear(pointers[i], freeF);
		}
	}
	times[9] = get_time_since(&start_time);
	
	printf("\nflipping took                             %f%s, usage: ", times[9], TIME_UNIT);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);

	get_now(&start_time);
	for (int i = 0; i < 512; i += 2) {
		if (pointers[i].status == open)
			printf("weird at %i\n", i);
		else
			clear(pointers[i], freeF);
	}
	times[10] = get_time_since(&start_time);
	
	printf("\nfinal cleanup took                        %f%s, usage: ", times[10], TIME_UNIT);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);
	printf("\nbenchmark done\n");
}
