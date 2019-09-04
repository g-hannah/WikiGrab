#include <assert.h>
#include <pthread.h>
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
	int	cache_nr = 0;
	int capacity = cachep->capacity;
	wiki_cache_t *ptr = cachep;

	while (bm && (*bm & bit))
	{
		bit <<= 1;

		++idx;

		if (!bit)
		{
			++bm;
			bit = 1;
		}

		if (idx >= capacity)
		{
			if (ptr->next)
			{
				ptr = ptr->next;
				idx = 0;
				bit = 1;
				bm = ptr->free_bitmap;
				++cache_nr;
			}
			else
			{
				ptr->next = wiki_cache_create(cachep->name,
							cachep->objsize,
							0,
							cachep->ctor,
							cachep->dtor);

				ptr = ptr->next;
				idx = 0;
				bit = 1;
				bm = ptr->free_bitmap;
				++cache_nr;
			}
		}
	}

	assert(cache_nr < (((WIKI_CACHE_NR_MASK >> WIKI_CACHE_NR_SHIFT) & ~(WIKI_CACHE_NR_MASK)) + 1));
	idx &= ~(WIKI_CACHE_NR_MASK);
	idx |= (cache_nr << WIKI_CACHE_NR_SHIFT);

	if (bm)
		return idx;
	else
		return -1;
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
	off_t offset;
	int capacity;
	size_t objsize = cachep->objsize;
	wiki_cache_t *ptr = cachep;

	capacity = cachep->capacity;
	offset = (((char *)obj - (char *)ptr->cache) / objsize);

	/*
	 * Then the obj belongs to another cache in the linked list.
	 */
	if (offset > (off_t)capacity)
	{
		while (offset > (off_t)capacity)
		{
			ptr = ptr->next;
			offset = ((char *)obj - (char *)ptr->cache);
		}
	}

	unsigned char *bm = ptr->free_bitmap;

	/*
	 * If the object is the, say, 10th object,
	 * then bit 9 represents it, so go up
	 * 9/8 bytes = 1 byte; then move up the
	 * remaining bit.
	 */
	bm += (offset >> 3);

	return (*bm & (1 << (offset & 7))) ? 1 : 0;
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
	clear_struct(cachep);

	cachep->cache = calloc(WIKI_CACHE_SIZE, 1);
	cachep->objsize = size;
	cachep->free_bitmap = calloc(WIKI_CACHE_BITMAP_SIZE, 1);
	int	i;
	int capacity = (WIKI_CACHE_SIZE / size);

	for (i = 0; i < capacity; ++i)
	{
		void *obj = (void *)((char *)cachep->cache + (size * i));

		if (ctor)
			ctor(obj);
	}

	cachep->capacity = capacity;
	cachep->nr_free = capacity;
	cachep->next = NULL;
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
	void *cur_obj = NULL;
	size_t objsize = cachep->objsize;
	wiki_cache_t *ptr = cachep;
	wiki_cache_t *tmp = NULL;

	while (ptr)
	{
		capacity = wiki_cache_capacity(ptr);

		for (i = 0; i < capacity; ++i)
		{
			cur_obj = (void *)((char *)ptr->cache + (objsize * i));

			if (ptr->dtor)
				ptr->dtor(cur_obj);
		}

		tmp = ptr->next;

		free(ptr->cache);
		free(ptr->free_bitmap);
		free(ptr);

		ptr = tmp;
	}

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
	int cache_nr = 0;
	size_t objsize = cachep->objsize;

	cache_nr = (idx >> WIKI_CACHE_NR_SHIFT);
	cache_nr &= 255;
	idx &= ~(WIKI_CACHE_NR_MASK);

	/*
	 * Get a new cache block.
	 */
	if (idx == -1)
	{
		wiki_cache_t *ptr = cachep;

		while (ptr->next)
			ptr = ptr->next;

		ptr->next = wiki_cache_create(
				cachep->name,
				WIKI_CACHE_SIZE,
				0,
				cachep->ctor,
				cachep->dtor);

		slot = ptr->next->cache;
		--(ptr->next->nr_free);
		__wiki_cache_mark_used(ptr, (int)0);

		return slot;
	}

	wiki_cache_t *ptr = cachep;

	while (cache_nr--)
		ptr = ptr->next;

	cache = ptr->cache;
	slot = (void *)((char *)cache + (objsize * idx));
	--(ptr->nr_free);
	__wiki_cache_mark_used(ptr, idx);

	return slot;
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

	off_t obj_off;
	size_t objsize = cachep->objsize;

	obj_off = (off_t)(((char *)slot - (char *)cachep->cache) / objsize);

	__wiki_cache_mark_unused(cachep, obj_off);

	return;
}

void
wiki_cache_clear_all(wiki_cache_t *cachep)
{
	void *obj;
	int i;
	int capacity = cachep->capacity;

	obj = cachep->cache;

	for (i = 0; i < capacity; ++i)
	{
		wiki_cache_dealloc(cachep, obj);
		obj = (void *)((char *)obj + cachep->objsize);
	}

	return;
}
