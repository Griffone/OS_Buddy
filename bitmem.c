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

typedef struct PageDescriptor {
	// We have 128 actual blocks (lowest level)
	// But we then have a bit for each level to optimize allocation timing
	// This is free memory wise (the page descriptor is 32 bytes already and needs a BlockHead, so 40 bytes takes the same size
	bitfield_t				blocks[2];
	struct PageDescriptor	*next, *prev;
	void 					*page;
} PageDescriptor;


static struct {
	struct PageDescriptor *front, *back;
} pages;

// Note: There might be an obvious mathematic interpretation if one reorders some bits,
// but I didn't find one
static int const LEVEL_OFFSETS = {
	0,		// level 0:   0-127
	128,	// level 1: 128-191
	192,	// level 2: 192-223
	244,	// level 3: 224-239
	240,	// level 4: 240-247
	248,	// level 5: 248-251
	252,	// level 6: 252-253
	254,	// level 7: 254-254
	255		// Last size, might be used by internal algorithms (ie, upper bound for level 7)
};

#define BITFIELD_LEVEL				0x0000000000000007
#define BITFIELD_STATUSFLAG			0x0000000000000800
#define BITFIELD_UNUSED				0x00000000000007F8
#define BITFIELD_ADDRESS			0xFFFFFFFFFFFFF000
#define BITFIELD_OTHER				0x0000000000000FFF

#define set_free(bitfield)			bitfield |= BITFIELD_STATUSFLAG
#define set_taken(bitfield) 		bitfield &= ~BITFIELD_STATUSFLAG

#define is_free(bitfield)			(bitfield & BITFIELD_STATUSFLAG)

#define set_level(bitfield, level)	bitfield = (bitfield & ~BITFIELD_LEVEL) | level
#define get_level(bitfield)			(bitfield & BITFIELD_LEVEL)

static inline void set_page(bitfield_t *bitfield, PageDescriptor *page) {
#if CLEAN_PAGE_ADDRESS
	*bitfield = (*bitfield & ~BITFIELD_ADDRESS) | ((bitfield_t) page & BITFIELD_ADDRESS);
#else // CLEAN_PAGE_ADDRESS
	*bitfield = (*bitfield & ~BITFIELD_ADDRESS) | (bitfield_t) page;
#endif // CLEAN_PAGE_ADDRESS
}

static inline PageDescriptor *get_page(bitfield_t bitfield) {
	return (PageDescriptor*)(bitfield & BITFIELD_ADDRESS);
}

static inline void set_other(bitfield_t *bitfield, long int value) {
	*bitfield = (*bitfield & ~BITFIELD_OTHER) | (value & BITFIELD_OTHER);
}

static inline long int get_other(bitfield_t bitfield) {
	return (bitfield & BITFIELD_OTHER);
}

static inline void push_page(PageDescriptor *page) {
	if (pages.back != NULL) {
		pages.back->next = page;
		page->prev = pages.back;
		pages.back = page;
	} else {
		pages.back = pages.front = page;
	}
}

static inline PageDescriptor *pop_page(PageDescriptor *page) {
	if (page->prev != NULL) page->prev->next = page->next;
	if (page->next != NULL) page->next->prev = page->prev;
	if (pages.front == page) pages.front = page->next;
	if (pages.back == page) pages.back = page->prev;
}

static inline int get_bit(bitfield_t *bitfields, int bit) {
	int offset = bit / (sizeof(bitfield_t) * 8);
	bitfield_t mask = 1 << bit % (sizeof(bitfield_t) * 8);
	return bitfields[offset] & mask == mask;
}

static inline void set_bit(bitfield_t *bitfields, int bit, int value) {
	int offset = bit / (sizeof(bitfield_t) * 8);
	bitfield_t mask = 1 << bit % (sizeof(bitfield_t) * 8);
	bitfield_t val = value << bit % (sizeof(bitfield_t) * 8);
	bitfields[offset] = (bitfields[offset] & ~mask) | value;
}


static void init() __attribute__((constructor));


/// Allocate a new page from OS.
///
/// Maps a new Page and initializes its PageHead
void *map_new_page() {
	void *new = mmap(
		NULL,							// hint for OS memory location, we let it decide
		PAGE,							// size of the newly mapped memory
		PROT_READ | PROT_WRITE,			// access mode
		MAP_PRIVATE | MAP_ANONYMOUS,	// MAP_PRIVATE is COW page independent of other processes,
										// MAP_ANONYMOUS flags the memory to not be backed by any files
		-1,								// sometimes required to be -1 with MAP_ANONYMOUS, but for the most part ignored
		0);								// offset should be 0 with ANONYMOUS flag

	if (new == MAP_FAILED) return NULL;	// this should throw an exception in any reasonable language, but in C malloc is noexcep...
	assert(((long int)new & 0xfff) == 0);	// mmap with MAP_ANONYMOUS flag should be preinitialized to 0
	
#if ADDRESS_ASSERT
	assert(new == (void *)((long int) new & PAGE_MASK_ADDRESS));
#endif // ADDRESS_ASSERT
	
	return new;
}

void init() {
	void *first = map_new_page();
	
	PageDescriptor *page = (PageDescriptor*)first;
	pages.front = pages.back = page;
	
	page->page = first;
	
}

/// Tries to find a block of given level in given page
///
/// Returns initialized BlockHead of correct level of NULL if there isn't enough free space in page
BlockHead *page_take(PageHead *page, level_t level) {
	// Still uses buddy algorithm to optimise sarch time
	bitfield_t bits = 1 << level;
	bitfield_t pos = 0;
	for (bitfield_t bit = 0; bit < 128; ++bit) {
		if (!get_bit(page->blocks, bit)) {
			pos = bit + 1;
		} else if (bit - pos == bits) {
			
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
	
	long int offset = ((long int)block - (long int)page) / sizeof(BlockHead);

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
int block_count(size_t requestedSize) {
	return (requestedSize + sizeof(BlockHead)) / 32;
}

/// Allocate size bytes of memory
void *bm_alloc(size_t size) {
	if (size == 0) return NULL;
	
	int blocks = block_count(size);
	
	assert(blocks < 128);
	
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
