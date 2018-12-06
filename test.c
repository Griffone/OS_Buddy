#include "buddy.h"

#include <assert.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#define durationSince(time)		((double) (clock() - time)) / CLOCKS_PER_SEC * 1000
#define secondsSince(time)		((double) (clock() -time)) / CLOCKS_PER_SEC

void benchmark(void *(*allocateFunc)(size_t), void (*freeFunc)(void *));

int main() {
	/*
	printf("Running test.\n");
	verboseTest();
	printf("Test finished succesfully!\n");
	*/
	
	clock_t startTime;
	double duration;
	printf("Benchmarking default memory management:\n");
	startTime = clock();
	benchmark(&malloc, &free);
	duration = durationSince(startTime);
	
	printf("\nDefault memory management took total of %fms\n\n", duration);
	
	printf("Benchmarking buddy memory management:\n");
	startTime = clock();
	benchmark(&balloc, &bfree);
	duration = durationSince(startTime);
	
	printf("\nBuddy memory management took total of %fms\n\n", duration);
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

struct ptr {
	enum flag status;
	void *p;
};

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
void benchmark(void *(*allocF)(size_t), void (*freeF)(void *)) {
	
	struct MemUsage initialUsage = {0, 0, 0, 0};	
	struct MemUsage memUsage = {0, 0, 0, 0};
	
	clock_t startTime;
	double duration;
	
	struct ptr pointers[1024];
	
	for (int i = 0; i < 1024; ++i) {
		pointers[i].status = open;
	}
	
	checkMemoryUsage(&initialUsage);
	
	printf("Initial memory usage: ");
	printMemUsage(&initialUsage);
	setInitialMemUsage(&initialUsage);
	printf("\n");
	
	printf("benchmarking...\n");
	startTime = clock();
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
	
	duration = durationSince(startTime);
	// exclude io from time usage
	printf("tiny allocations took                     %fms, usage: ", duration);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);
	
	startTime = clock();
	// Tiny zig-zag
	for (int i = 50; i < 100; ++i) {
		if (i % 2 == 0) {
			assign(pointers[i], allocF(100));
		} else {
			assign(pointers[i], allocF(10));
		}
	}	
	duration = durationSince(startTime);
	
	printf("\nzig-zag took                              %fms, usage: ", duration);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);
	
	startTime = clock();
	// cler some, but not really aligned to anything
	for (int i = 3; i < 100; i += 7) {
		clear(pointers[i], freeF);
	}
	duration = durationSince(startTime);

	printf("\nfreeing some items took                   %fms, usage: ", duration);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);
	
	startTime = clock();
	// I'm too lazy to do this cleaner, so I reuse pointers to avoid headache later
	for (int i = 3; i < 100; i += 7) {
		assign(pointers[i], allocF(1000));
	}
	duration = durationSince(startTime);
	
	printf("\nallocating some large blocks took         %fms, usage: ", duration);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);
	
	startTime = clock();
	for (int i = 100; i < 200; ++i) {
		assign(pointers[i], allocF(20 + (i - 100) * 32));
	}
	duration = durationSince(startTime);

	printf("\nallocating increasinly large blocks took  %fms, usage: ", duration);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);
	
	startTime = clock();
	// Sweeping clean
	for (int i = 20; i < 80; ++i) {
		printf("%i ", i);
		if (i == 32) printFreeLists();
		clear(pointers[i], freeF);
	}
	duration = durationSince(startTime);
	
	printf("\nsweeping clean of some objects took       %fms, usage: ", duration);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);

	startTime = clock();
	for (int i = 20; i < 80; ++i) {
		assign(pointers[i], allocF(8 + ((i - 20) * 13) % 64));
	}
	duration = durationSince(startTime);
	
	printf("\nclamped blocks took                       %fms, usage: ", duration);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);
	
	
	printf("\n");
	printFreeLists();
	
	startTime = clock();
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
	duration = durationSince(startTime);
	
	printf("\nlots of random allocations took           %fms, usage: ", duration);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);
	
	startTime = clock();
	for (int i = 0; i < 512; i += 2) {
		clear(pointers[i], freeF);
	}
	duration = durationSince(startTime);
	
	printf("\neven frees took                           %fms, usage: ", duration);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);
	
	startTime = clock();
	for (int i = 0; i < 512; i++) {
		if (i % 2 == 0) {
			assign(pointers[i], allocF(12 + i));
		} else {
			clear(pointers[i], freeF);
		}
	}
	duration = durationSince(startTime);
	
	printf("\nflipping took                             %fms, usage: ", duration);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);

	startTime = clock();
	for (int i = 0; i < 512; i += 2) {
		if (pointers[i].status == open)
			printf("weird at %i\n", i);
		else
			clear(pointers[i], freeF);
	}
	duration = durationSince(startTime);
	
	printf("\nfinal cleanup took                        %fms, usage: ", duration);
	checkMemoryUsage(&memUsage);
	printMemUsage(&memUsage);
	printf("\nbenchmark done\n");
}
