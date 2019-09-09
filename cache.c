#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "cache.h"
#include "http.h"
#include "wikigrab.h"

/**
 * __wiki_cache_next_free_idx - get index of next free object
 * @cachep: pointer to the metadata cache structure
 */
static inline int __wiki_cache_next_free_idx(wiki_cache_t *cachep)
{
	unsigned char *bm = cachep->free_bitmap;
	unsigned char bit = 1;
	int idx = 0;
	int capacity = cachep->capacity;
	uint16_t bitmap_size = cachep->bitmap_size;

	while (bm && (*bm & bit))
	{
		bit <<= 1;

		++idx;

		if (!bit)
		{
			++bm;
			bit = 1;
			--bitmap_size;

			if (!bitmap_size) /* No free space remains */
				return -1;
		}

		if (idx >= capacity)
			return -1;
	}

	return idx;
}

/**
 * __wiki_cache_mark_used - mark an object as used
 * @c: pointer to the metadata cache structure
 * @i: the index of the object in the cache
 */
#define __wiki_cache_mark_used(c, i)	\
do {\
	unsigned char *bm = ((c)->free_bitmap + ((i) >> 3));	\
	(*bm |= (unsigned char)(1 << ((i) & 7)));							\
} while(0)

/**
 * __wiki_cache_mark_unused - mark an object as unused
 * @c: pointer to the metadata cache structure
 * @i: the index of the object in the cache
 */
#define __wiki_cache_mark_unused(c, i)	\
do {\
	unsigned char *bm = ((c)->free_bitmap + ((i) >> 3));	\
	(*bm &= (unsigned char) ~(1 << ((i) & 7)));						\
} while(0)

/**
 * wiki_cache_nr_used - return the number of objects used
 * @cachep: pointer to the metadata cache structure
 */
inline int wiki_cache_nr_used(wiki_cache_t *cachep)
{
	return (cachep->capacity - cachep->nr_free);
}

/**
 * wiki_cache_capacity - return capacity of the cache
 * @cachep: pointer to the metadata cache structure
 */
inline int wiki_cache_capacity(wiki_cache_t *cachep)
{
	return cachep->capacity;
}

/**
 * wiki_cache_obj_used - determine if an object is active or not.
 * @cachep: pointer to the metadata cache structure
 * @obj pointer to the queried cache object
 */
inline int
wiki_cache_obj_used(wiki_cache_t *cachep, void *obj)
{
	int idx;
	int capacity;
	size_t objsize = cachep->objsize;
	unsigned char *bm = cachep->free_bitmap;
	void *cache = cachep->cache;

	capacity = cachep->capacity;
	idx = (((char *)obj - (char *)cache) / objsize);

	/*
	 * Then the obj belongs to another cache in the linked list.
	 */
	if (idx > capacity)
		return -1;

	/*
	 * If the object is the, say, 10th object,
	 * then bit 9 represents it, so go up
	 * 9/8 bytes = 1 byte; then move up the
	 * remaining bit.
	 */
	bm += (idx >> 3);

	return (*bm & (1 << (idx & 7))) ? 1 : 0;
}

/**
 * wiki_cache_create - create a new cache
 * @name: name of the cache for statistics
 * @size: size of the type of object that will be stored in the cache
 * @alignment: minimum alignment of the cache objects
 * @ctor: pointer to a constructor function called on each object
 * @dtor: pointer to a destructor function called on each dealloc()
 */
wiki_cache_t *
wiki_cache_create(char *name,
		size_t size,
		int alignment,
		wiki_cache_ctor_t ctor,
		wiki_cache_dtor_t dtor)
{
	wiki_cache_t	*cachep = malloc(sizeof(wiki_cache_t));
	int capacity = (WIKI_CACHE_SIZE / size);
	int	i;
	void *obj = NULL;
	void *cache = NULL;

	clear_struct(cachep);

	cachep->cache = calloc(WIKI_CACHE_SIZE, 1);
	cachep->objsize = size;
	cachep->free_bitmap = calloc(WIKI_CACHE_BITMAP_SIZE, 1);

	cache = cachep->cache;

	for (i = 0; i < capacity; ++i)
	{
		obj = (void *)((char *)cache + (size * i));

		if (ctor)
			ctor(obj);
	}

	cachep->capacity = capacity;
	cachep->nr_free = capacity;
	cachep->ctor = ctor;
	cachep->dtor = dtor;

#ifdef DEBUG
	printf(
			"Created cache \"%s\"\n"
			"Size of each object=%lu bytes\n"
			"Capacity of cache=%d objects\n"
			"%s\n"
			"%s\n",
			name,
			size,
			capacity,
			ctor ? "constructor provided\n" : "constructor not provided\n",
			dtor ? "destructor provided\n" : "destructor not provided\n");
#endif

	return cachep;
}

/**
 * wiki_cache_destroy - destroy a cache
 * @cachep: pointer to the metadata cache structure
 */
void
wiki_cache_destroy(wiki_cache_t *cachep)
{
	assert(cachep);

	int	i;
	int capacity = wiki_cache_capacity(cachep);
	void *obj = NULL;
	size_t objsize = cachep->objsize;

	obj = cachep->cache;

	for (i = 0; i < capacity; ++i)
	{
		if (cachep->dtor)
			cachep->dtor(obj);

		obj = (void *)((char *)obj + objsize);
	}

	free(cachep->cache);
	free(cachep->free_bitmap);
	free(cachep);

	return;
}

/**
 * wiki_cache_alloc - allocate an object from a cache
 * @cachep: pointer to the metadata cache structure
 */
void *
wiki_cache_alloc(wiki_cache_t *cachep)
{
	assert(cachep);

	void *cache = cachep->cache;
	void *slot = NULL;
	int idx = __wiki_cache_next_free_idx(cachep);
	size_t objsize = cachep->objsize;
	size_t cache_size = cachep->cache_size;
	uint16_t bitmap_size = cachep->bitmap_size;
	int capacity = cachep->capacity;
	int i;


	if (idx != -1)
	{
		__wiki_cache_mark_used(cachep, idx);

		--(cachep->nr_free);

		slot = (void *)((char *)cache + (idx * objsize));

		return slot;
	}
	else
	{
		if (!(cachep->cache = realloc(cachep->cache, cache_size * 2)))
		{
			fprintf(stderr, "wiki_cache_alloc: realloc error for ->cache (%s)\n", strerror(errno));
			goto fail;
		}

		if (!(cachep->free_bitmap = realloc(cachep->free_bitmap, bitmap_size * 2)))
		{
			fprintf(stderr, "wiki_cache_alloc: realloc error for ->free_bitmap (%s)\n", strerror(errno));
			goto fail;
		}

		unsigned char *bm = (cachep->free_bitmap + cachep->bitmap_size);

		for (i = 0; i < bitmap_size; ++i)
			*bm++ = 0;

		cachep->bitmap_size *= 2;
		cachep->cache_size *= 2;
		cachep->nr_free += capacity;
		cachep->capacity *= 2;

		idx = __wiki_cache_next_free_idx(cachep);

		__wiki_cache_mark_used(cachep, idx);

		--(cachep->nr_free);

		cache = cachep->cache;

		slot = (void *)((char *)cache + (idx * objsize));
		return slot;
	}


	fail:
	return NULL;
}

/**
 * wiki_cache_dealloc - return an object to the cache
 * @cachep: pointer to the metadata cache structure
 * @slot: the object to be returned
 */
void
wiki_cache_dealloc(wiki_cache_t *cachep, void *slot)
{
	assert(cachep);
	assert(slot);

	int obj_idx;
	size_t objsize = cachep->objsize;
	
	obj_idx = (int)(((char *)slot - (char *)cachep->cache) / objsize);

	__wiki_cache_mark_unused(cachep, obj_idx);
	++(cachep->nr_free);

	return;
}

void
wiki_cache_clear_all(wiki_cache_t *cachep)
{
	void *obj = NULL;
	int i;
	int capacity = cachep->capacity;

	obj = cachep->cache;

	for (i = 0; i < capacity; ++i)
	{
		if (wiki_cache_obj_used(cachep, obj))
			wiki_cache_dealloc(cachep, obj);

		obj = (void *)((char *)obj + cachep->objsize);
	}

	return;
}
