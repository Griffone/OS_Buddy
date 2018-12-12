#include "bitmem.h"

#include <assert.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>

#define MIN					5
#define LEVELS				8
#define MAX_LEVEL			LEVELS - 1	// this is how counting works
#define PAGE				4096

#define check_bounds(x)		assert(x <= MAX_LEVEL)	// redefine this to skip assertion

#define PAGE_MASK_ADDRESS	0xFFFFFFFFFFFFF000
#define PAGE_MASK_OTHER		~PAGE_MASK_ADDRESS

// Optional safety
#define CLEAN_PAGE_ADDRESS		1
#define ALLOC_ASSERT			1
#define PAGE_IN_RANGE_ASSERT	1
#define ADDRESS_ASSERT			1

typedef unsigned long int bitfield_t;
typedef unsigned int level_t;

typedef struct BlockHead {
	bitfield_t	bitfield;
} BlockHead;

// 32-byte page head
//
// First 16 bytes are 128 block identifiers for 32-byte-sized blocks
// The second 16 bytes are two pointers to other page headers
typedef struct PageHead {
	unsigned char	blocks[16];
	// PAGE_MASK_ADDRESS bits contain address of the next page in list
	// PAGE_MASK_OTHER bits contain number of free bits
	struct PageHead	*next;
	// PAGE_MASK_ADDRESS bits contain address of the previous page in list
	struct PageHead	*prev;
} PageHead;


static PageHead *first_page = NULL;


#define BITFIELD_LEVEL				0x0000000000000007
#define BITFIELD_STATUSFLAG			0x0000000000000008
#define BITFIELD_UNUSED				0xFFFFFFFFFFFFFFF0

#define set_free(bitfield)			bitfield |= BITFIELD_STATUSFLAG
#define set_taken(bitfield) 		bitfield &= ~BITFIELD_STATUSFLAG

#define is_free(bitfield)			(bitfield & BITFIELD_STATUSFLAG)

#define set_level(bitfield, level)	bitfield = (bitfield & ~BITFIELD_LEVEL) | level
#define get_level(bitfield)			(bitfield & BITFIELD_LEVEL)

static inline void set_page(PageHead **pointer, PageHead *page) {
#if CLEAN_PAGE_ADDRESS
	*pointer = (PageHead *)(((long int) *pointer & ~PAGE_MASK_ADDRESS) | ((long int) page & PAGE_MASK_ADDRESS));
#else // CLEAN_PAGE_ADDRESS
	*pointer = (PageHead *)(((long int) *pointer & ~PAGE_MASK_ADDRESS) | (long int) page);
#endif // CLEAN_PAGE_ADDRESS
}

static inline PageHead *get_page(PageHead *pointer) {
	return (PageHead*)((long int) pointer & PAGE_MASK_ADDRESS);
}

static inline void set_other(PageHead **pointer, long int value) {
	*pointer = (PageHead*)(((long int) *pointer & ~PAGE_MASK_OTHER) | (value & PAGE_MASK_OTHER));
}

static inline long int get_other(PageHead *pointer) {
	return (long int)pointer & PAGE_MASK_OTHER;
}


/// Allocate a new page from OS.
///
/// Maps a new Page and initializes its PageHead
PageHead *page_new() {
	struct PageHead *new = (PageHead *) mmap(
		NULL,							// hint for OS memory location, we let it decide
		PAGE,							// size of the newly mapped memory
		PROT_READ | PROT_WRITE,			// access mode
		MAP_PRIVATE | MAP_ANONYMOUS,	// MAP_PRIVATE is COW page independent of other processes,
										// MAP_ANONYMOUS flags the memory to not be backed by any files
		-1,								// sometimes required to be -1 with MAP_ANONYMOUS, but for the most part ignored
		0);								// offset should be 0 with ANONYMOUS flag

	if (new == MAP_FAILED) return NULL;	// this should throw an exception in any reasonable language, but in C malloc is noexcep...
	assert(((long int)new & 0xfff) == 0);	// mmap with MAP_ANONYMOUS flag should be preinitialized to 0
	
	new->blocks[0] = 0xE;	// first bit is '0', as its where this pagehead is
	for (int i = 1; i < 16; ++i) new->blocks[i] = 0xF;
	new->prev = new->next = NULL;
	set_other(&new->next, 127);
	
#if ADDRESS_ASSERT
	assert((void *) new == (void *)((long int) new & PAGE_MASK_ADDRESS));
#endif // ADDRESS_ASSERT
	
	return new;
}

/// Tries to find a block of given level in given page
///
/// Returns initialized BlockHead of correct level of NULL if there isn't enough free space in page
BlockHead *page_take(PageHead *page, level_t level) {
	int free_bits = get_other(page->next);
	if (free_bits < 0x1 << level) return NULL;

	// Evil bitwise manipulation
	// Effectively left-shift with filler ones
	unsigned char mask = ~(~0x1 << level);

	unsigned long int end_offset = (128 - (0x1 << level)) + 1;
	for (unsigned long int offset = 0; offset < end_offset; ++offset) {
		unsigned char this = mask << (offset % 8);
		unsigned char that = ~this;	// flip all bits of this
		if (page->blocks[offset / 8] & this == this
			&& page->blocks[(offset / 8) + 1] & that == that) {
			// We have found our block
			BlockHead *block = (BlockHead *)(page + offset);
			set_taken(block->bitfield);
			set_level(block->bitfield, level);
			
			// We need to flip bits that are in 'this', we already did it with 'that'
			page->blocks[offset / 8] &= that;
			page->blocks[(offset / 8) + 1] &= this;
			
			set_other(&page->next, free_bits - 0x1 << level);
			
			return block;
		}
	}
	
	return NULL;
}

/// Find a new BlockHead of given level
///
/// Internally will iterate over all allocated pages
/// Will allocate a new page if none have a required amount of memory
BlockHead *take(level_t level) {
	if (first_page == NULL)	first_page = page_new();
	
	PageHead *page = first_page;
	
	while (get_page(page->next) != NULL) {
		//printf("Spinning\n");
		BlockHead *block = page_take(page, level);
		
		if (block != NULL) return block;
		
		page = get_page(page->next);
	}
	
	// Minor problem
	// find(page) for last page isn't called
	{
		BlockHead *block = page_take(page, level);
		if (block != NULL) return block;
	}
	
	// None of the pages turned out to have space for a given level
	PageHead *npage = page_new();
	
	set_page(&page->next, npage);
	set_page(&npage->prev, page);
	
	return page_take(npage, level);
}

void free_block(BlockHead *block) {
	PageHead *page = (PageHead *) ((long int)block & PAGE_MASK_ADDRESS);
	
	level_t level = get_level(block->bitfield);
	unsigned char mask = ~(~0x1 << level);
	
	long int offset = (long int)block - (long int)page;

#if PAGE_IN_RANGE_ASSERT
	assert(offset <= 128);
#endif // PAGE_IN_RANGE_ASSERT

	unsigned char this = mask << (offset % 8);
	unsigned char that = ~this;
	
	int free_bits = get_other(page->next);
	free_bits += (0x1 << level);
	page->blocks[offset / 8] |= this;
	page->blocks[offset / 8 + 1] |= that;
	
	// TODO: return page logic here
	set_other(&page->next, free_bits);
}

void *hide_head(BlockHead *block) {
	return (void*)(block + 1);
}

struct BlockHead *unhide_head(void *memory) {
	return ((BlockHead*)memory - 1);
}

/// Find the level of the necessary block for a given requested memory amount
///
/// Find find the smallest block size that fits both the requested size of the data as well as the block head for that data
level_t calc_level(size_t requestedSize) {
	size_t total = requestedSize + sizeof(BlockHead);
	
	level_t level = 0;
	size_t size = 1 << MIN;
	while (size < total) {
		size <<= 1;
		level++;
	}
	
	return level;
}

/// Allocate size bytes of memory
void *bm_alloc(size_t size) {
	if (size == 0) return NULL;
	
	level_t index = calc_level(size);
	check_bounds(index);	// in-source functions do no parameter checking, since the developer is hopefully not an idiot
	
	BlockHead *block = take(index);

#if ALLOC_ASSERT
	assert(!is_free(block->bitfield));
	assert(get_level(block->bitfield) == index);
#endif // ALLOC_ASSERT
	
	return hide_head(block);
}

/// Free memory
void bm_free(void *memory) {
	if (memory != NULL) {
		BlockHead *block = unhide_head(memory);
		assert(!is_free(block->bitfield));
		free_block(block);
	}
}
