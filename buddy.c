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


typedef int level_t;

enum Flag {Free = 0, Taken = 1};

typedef struct BlockHead {
	enum Flag	status;
	level_t		level;
} BlockHead;

typedef struct FreeBlockHead {
	struct BlockHead		header;
	struct FreeBlockHead	*next, *prev;
} FreeBlockHead;


/// Global list of blocks
FreeBlockHead *freeBlocks[LEVELS] = {NULL};

/// Map a new block head
///
/// Traps to OS to allocate a new page
FreeBlockHead *newBlock() {
	FreeBlockHead *new = (FreeBlockHead*) mmap(
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
	
	new->header.status = Free;
	new->header.level = MAX_LEVEL;
	new->next = new->prev = NULL;	// technically not necessary, but this is actually important logically
	
	return new;
}

/// Find a buddy (second half of a larger block) of a given block
///
/// This is done by flipping the bit that differenciates the given block from the body
BlockHead *buddy(BlockHead *block) {
	level_t index = block->level;
	long int mask = 0x1 << (index + MIN);
	return (struct BlockHead*)((long int)block ^ mask);
}

/// Split a given block into two
///
/// Splits the given block in half
/// Lowers the level of the returned and initial blocks
/// Does NOT remove the block from the list
FreeBlockHead *split(FreeBlockHead *block) {
	level_t index = --block->header.level;
	long int mask = 0x1 << (index + MIN);
	FreeBlockHead *new = (FreeBlockHead*)((long int)block | mask);
	// New used to be user data, so we clean it
	new->header.level = index;
	new->header.status = Free;
	new->prev = new->next = NULL;
	
	return new;
}

/// Find the primary block
///
/// Returns the location of the block that has lower memory address
BlockHead *primary(BlockHead *block) {
	level_t index = block->level;
	long int mask = ~0x0 << (1 + index + MIN);	// ~0x0 is everything set to 1 (-1) (0xFFFFFFFFFFFFFFFF)
	return (BlockHead *)((long int)block & mask);
}

/// Merges the 2 buddies
///
/// Inverse of splitting
/// Can be supplied with either of the buddy block
/// Returns the resulting larger block
/// Updates the resulting block level
FreeBlockHead *merge(FreeBlockHead *block) {
	level_t index = block->header.level + 1;
	long int mask = ~0x0 << (index + MIN);
	FreeBlockHead *new = (FreeBlockHead*)((long int)block & mask);
	// Unlike split
	// The location of the new head is the same as the location of the old head
	// So there is no need to clean
	new->header.level = index;
	new->prev = new->next = NULL;
	return new;
}

void *hideHead(BlockHead *block) {
	return (void*)(block + 1);
}

BlockHead *unhideHead(void *memory) {
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
FreeBlockHead *find(int level) {
	if (freeBlocks[level] != NULL) {
		// Turns out we already have a free block of right size
		FreeBlockHead *returnedBlock = freeBlocks[level];
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
			FreeBlockHead *parent = find(level + 1);
			if (parent->prev) parent->prev->next = parent->next;
			if (parent->next) parent->next->prev = parent->prev;
			// We lower the level of the parent, so it doesn't belong to its list any more
			parent->prev = parent->next = NULL;
			freeBlocks[level] = parent;	// We know freeBlocks[level] is NULL
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
void insert(FreeBlockHead *block) {
	level_t level = block->header.level;
	// Since merging pages doesn't make sense check that this isn't a a full page
	if (level != MAX_LEVEL) {
		BlockHead *bud = buddy((BlockHead*)block);
		// This if is not obvious, but we are guaranteed (with correct free use)
		// That the buddy address is not user data, but a buddy head
		// However that buddy might be of a lower level, thus unmergable
		if (bud->status == Free && bud->level == level) {
			FreeBlockHead *freeBuddy = (FreeBlockHead*)bud;
			if (freeBuddy->next) freeBuddy->next->prev = freeBuddy->prev;
			if (freeBuddy->prev) freeBuddy->prev->next = freeBuddy->next;
			if (freeBlocks[level] == freeBuddy) {
				// The buddy is about to be merged, so its level is about to be incremented
				freeBlocks[level] = freeBuddy->next;
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
	block->prev = NULL;
	block->header.status = Free;
	freeBlocks[level] = block;
}

/// Allocate size bytes of memory
void *balloc(size_t size) {
	if (size == 0) return NULL;
	
	int index = level(size);
	check_bounds(index);	// in-source functions do no parameter checking, since the developer is hopefully not an idiot
	BlockHead *block = (BlockHead*)find(index);
	block->status = Taken;
	return hideHead(block);
}

/// Free memory
void bfree(void *memory) {
	if (memory != NULL) {
		BlockHead *block = unhideHead(memory);
		assert(block->status = Taken);
		FreeBlockHead *freeBlock = (FreeBlockHead*)block;
		// used to be user data, so we clean this
		freeBlock->next = freeBlock->prev = NULL;
		insert(freeBlock);
	}
}
