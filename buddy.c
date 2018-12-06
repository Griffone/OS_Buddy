#include "buddy.h"

#include <assert.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>

#define MIN					5
#define LEVELS				8
#define MAX_LEVEL			LEVELS -1	// this is how numbers work
#define PAGE				4096

#define check_bounds(x)		assert(x <= MAX_LEVEL)	// redefine this to skip assertion

/// Global list of blocks
struct BlockHead *freeBlocks[LEVELS] = {NULL};

enum Flag {Free = 0, Taken = 1};

typedef int level_t;

struct BlockHead {
	enum Flag			status;			// Is the block taken?
	level_t				level;			// Level of the block (0=lowest (32 bytes), 7=highest (page))
	struct BlockHead	*next, *prev;	// Double-linked list
};

/// Map a new block head
///
/// Traps to OS to allocate a new page
struct BlockHead *newBlock() {
	struct BlockHead *new = (struct BlockHead *) mmap(
													NULL,							// hint for OS memory location, we let it decide
													PAGE,							// size of the newly mapped memory
													PROT_READ | PROT_WRITE,			// access mode
													MAP_PRIVATE | MAP_ANONYMOUS,	// MAP_PRIVATE is COW page independent of other processes,
																					// MAP_ANONYMOUS flags the memory to not be backed by any files
													-1,								// sometimes required to be -1 with MAP_ANONYMOUS, but for the most part ignored
													0);								// offset should be 0 with ANONYMOUS flag
							
	if (new == MAP_FAILED) {
		return NULL;	// this should throw an exception in any reasonable language, but in C malloc is noexcep...
	}
	assert(((long int)new & 0xfff) == 0);	// mmap with MAP_ANONYMOUS flag should be preinitialized to 0
	new->status = Free;
	new->level = MAX_LEVEL;
	
	new->next = new->prev = NULL;	// technically not necessary, but this is actually important logically
	
	return new;
}

/// Find a buddy (second half of a larger block) of a given block
///
/// This is done by flipping the bit that differenciates the given block from the body
struct BlockHead *buddy(struct BlockHead *block) {
	level_t index = block->level;
	long int mask = 0x1 << (index + MIN);
	return (struct BlockHead*)((long int)block ^ mask);
}

/// Split a given block into two
///
/// Splits the given block in half
/// Lowers the level of the returned and initial blocks
/// Does NOT remove the block from the list
struct BlockHead *split(struct BlockHead *block) {
	level_t index = --block->level;
	long int mask = 0x1 << (index + MIN);
	struct BlockHead *new = (struct BlockHead*)((long int)block | mask);
	new->level = index;
	return new;
}

/// Find the primary block
///
/// Returns the location of the block that has lower memory address
struct BlockHead *primary(struct BlockHead *block) {
	level_t index = block->level;
	long int mask = ~0x0 << (1 + index + MIN);	// ~0x0 is everything set to 1 (-1) (0xFFFFFFFFFFFFFFFF)
	return (struct BlockHead *)((long int)block & mask);
}

/// Merges the 2 buddies
///
/// Inverse of splitting
/// Can be supplied with either of the buddy block
/// Returns the resulting larger block
/// Updates the resulting block level
struct BlockHead *merge(struct BlockHead *block) {
	level_t index = block->level;
	long int mask = ~0x0 << (1 + index + MIN);
	struct BlockHead *new = (struct BlockHead*)((long int)block & mask);
	new->level = index + 1;
	return new;
}

void *hideHead(struct BlockHead *block) {
	return (void*)(block + 1);
}

struct BlockHead *unhideHead(void *memory) {
	return ((struct BlockHead*)memory - 1);
}

/// Find the level of the necessary block for a given requested memory amount
///
/// Find find the smallest block size that fits both the requested size of the data as well as the block head for that data
int level(int requestedSize) {
	int total = requestedSize + sizeof(struct BlockHead);
	
	level_t level = 0;
	int size = 1 << MIN;
	while (size < total) {
		size <<= 1;
		level++;
	}
	
	return level;
}

/// Get the next free block of given level
///
/// First checks if there is a free block of given size
/// if there is one - returns it.
/// If there isn't one - recursively tries to find a larger free block
/// If the largest possible block (a full page) is still not found
/// requests the kernel to allocate a new page and unwinds the call stack.
struct BlockHead *find(int level) {
	if (freeBlocks[level] != NULL) {
		// Turns out we already have a free block of right size
		struct BlockHead *returnedBlock = freeBlocks[level];
		// Because of freeing we might have a non-adjacent free page
		freeBlocks[level] = freeBlocks[level]->next;
		return returnedBlock;
	} else {
		// We need to create a new block of the right size
		if (level == MAX_LEVEL) {
			// We don't have a free page, so get a new one
			return newBlock();
		} else {
			// We find a free bigger block and split it
			// Note: since we find a bigger block the buddy of the returned block is free
			// so we append it to the list
			struct BlockHead *parent = find(level + 1);
			if (parent->prev) {
				parent->prev->next = parent->next;
			}
			parent->prev = parent->next = NULL;	// We lower the level of the parent, so it doesn't belong to its list any more
			freeBlocks[level] = parent;
			return split(parent);
		}
	}
}

/// Insert the block back into the list
///
/// Checks if the buddy of the block is also free
/// If it is - merge and recursively insert the resulting larger block
/// If it isn't - push the fresh block to the freeBlocks list
/// Don't forget to mark the block as free
void insert(struct BlockHead *block) {
	for (level_t i = 0; i < LEVELS; ++i) {
		if (freeBlocks[i] != NULL) {
			if (freeBlocks[i]->level != i) {
				printf("\nCorrupted block (%i): ", i);
				printBlock(freeBlocks[i]);
				printf("\nwhile trying to insert:");
				printBlock(block);
				printf("\n");
				assert(0);
			}
		}
	}
	level_t level = block->level;
	// Since merging pages doesn't make sense check that this isn't a a full page
	if (level != MAX_LEVEL) {
		struct BlockHead *bud = buddy(block);
		// This if is not obvious, but we are guaranteed (with correct free use)
		// That the buddy address is not user data, but a buddy head
		// However that buddy might be of a lower level, thus unmergable
		if (bud->status == Free && bud->level == level) {
			if (bud->prev != NULL) {
				// Remove buddy from the list
				bud->prev->next = bud->next;
			}
			if (freeBlocks[level] == bud) {
				// The buddy is about to be merged, so its level is about to be incremented
				freeBlocks[level] = bud->next;
			}
			block = merge(block);
			return insert(block);	// eventually the biggest free block will be marked as Free, we can avoid doing it eagerly here
		}
	}
	
	// This code is executed if either of the ifs fail
	if (freeBlocks[level] != NULL) {
		freeBlocks[level]->prev = block;
		block->next = freeBlocks[level];
	} else {
		block->next = NULL;
	}
	block->status = Free;
	freeBlocks[level] = block;
}

/// Allocate size bytes of memory
void *balloc(size_t size) {
	if (size == 0) return NULL;
	
	int index = level(size);
	check_bounds(index);	// in-source functions do no parameter checking, since the developer is hopefully not an idiot
	struct BlockHead *block = find(index);
	block->status = Taken;
	return hideHead(block);
}

/// Free memory
void bfree(void *memory) {
	if (memory != NULL) {
		struct BlockHead *block = unhideHead(memory);
		insert(block);
	}
}

void printBlock(struct BlockHead *block) {
	const char *status = (block->status == Free) ? "Free" : "Taken";
	printf("{addr=%p, level=%i, status=%s}", block, block->level, status);
}

void printFreeLists() {
	printf("Free Blocks:\n");
	for (level_t level = MAX_LEVEL; level >= 0; --level) {
		printf("%i:\t", level);
		struct BlockHead *block = freeBlocks[level];
		while (block) {
			printBlock(block);
			printf("-");
			block = block->next;
		}
		printf("{NULL}\n");
	}
}

void verboseTest() {
	printf("Runnign verbose test\n");
	printf("Initial free blocks:\n");
	printFreeLists();
	
	// 12 * 4 = 48 of data + 24 of overhead = 72 required size, fits into 128 byte size (level == 2)
	int *testArray = (int *)balloc(12 * sizeof(int));
	assert(testArray != NULL);
	
	assert(unhideHead(testArray)->level = 2);
	for (int i = 0; i < 12; ++i) {
		testArray[i] = i * 60;
	}
	
	// we want to use a larger sized block, but we expect it to be 256+128 bytes away from testArray
	char *anotherTest = (char *)balloc(128 *sizeof(char));
	assert(anotherTest != NULL);
	assert(unhideHead(anotherTest)->level == 3);
	assert(unhideHead(anotherTest)->status == Taken);
	assert((long int)testArray - (long int)anotherTest == 0x180);
	
	// Now we just get a new page alltogether
	int *page = (int *)balloc(1000 * sizeof(int));
	
	
	printf("\nAfter allocations:\n");
	printFreeLists();
	
	bfree(anotherTest);
	assert(unhideHead(anotherTest)->status == Free);	
	bfree(testArray);
	bfree(page);
	
	printf("\nAfter frees:\n");
	printFreeLists();
	
	char *string = (char *)balloc(sizeof("Some string"));
	assert(*string == 0);
	
	printf("\nAfter new allocation:\n");
	printFreeLists();
	
	bfree(string);
	
	printf("\nAfter final free:\n");
	printFreeLists();
}
