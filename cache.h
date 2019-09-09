#ifndef WIKI_CACHE_H
#define WIKI_CACHE_H 1

#include <stdint.h>
#include <sys/types.h>

/*
 * 31 ..... 16 15 ..... 0
 *   cache nr     obj nr
 */
#define WIKI_CACHE_SIZE 4096
#define WIKI_CACHE_BITMAP_SIZE (WIKI_CACHE_SIZE / 8)

typedef int (*wiki_cache_ctor_t)(void *);
typedef void (*wiki_cache_dtor_t)(void *);

typedef struct wiki_cache_t
{
	void *cache;
	unsigned char *free_bitmap;
	int capacity;
	int nr_free;
	size_t objsize;
	size_t cache_size;
	uint16_t bitmap_size;
	char *name;
	wiki_cache_ctor_t ctor;
	wiki_cache_dtor_t dtor;
} wiki_cache_t;

wiki_cache_t *wiki_cache_create(char *, size_t, int, wiki_cache_ctor_t, wiki_cache_dtor_t);
void wiki_cache_destroy(wiki_cache_t *) __nonnull((1));
void *wiki_cache_alloc(wiki_cache_t *) __nonnull((1)) __wur;
void wiki_cache_dealloc(wiki_cache_t *, void *) __nonnull((1,2));
int wiki_cache_obj_used(wiki_cache_t *, void *) __nonnull((1,2)) __wur;
int wiki_cache_nr_used(wiki_cache_t *) __nonnull((1)) __wur;
int wiki_cache_capacity(wiki_cache_t *) __nonnull((1)) __wur;
void wiki_cache_clear_all(wiki_cache_t *) __nonnull((1));

#endif /* WIKI_CACHE_H */
