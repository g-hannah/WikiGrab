#ifndef WIKI_CACHE_H
#define WIKI_CACHE_H 1

#include <stdint.h>
#include <sys/types.h>

/*
 * 31 ..... 16 15 ..... 0
 *   cache nr     obj nr
 */
#define WIKI_CACHE_SIZE 4096

#define WIKI_CACHE_DEC_FREE(c) --((c)->nr_free)
#define WIKI_CACHE_INC_FREE(c) ++((c)->nr_free)

typedef int (*wiki_cache_ctor_t)(void *);
typedef void (*wiki_cache_dtor_t)(void *);

struct active_ptr_ctx
{
	void *ptr_addr;
	void *obj_addr;
	off_t obj_offset;
	off_t ptr_offset;
};

typedef struct wiki_cache_t
{
	void *cache;
	int capacity;
	int nr_free;
	unsigned char *free_bitmap;
	uint16_t bitmap_size;
	struct active_ptr_ctx *active_ptrs;
	int nr_active_ptrs;
	size_t objsize;
	size_t cache_size;
	char *name;
	wiki_cache_ctor_t ctor;
	wiki_cache_dtor_t dtor;
} wiki_cache_t;

wiki_cache_t *wiki_cache_create(char *, size_t, int, wiki_cache_ctor_t, wiki_cache_dtor_t);
void wiki_cache_destroy(wiki_cache_t *) __nonnull((1));
void *wiki_cache_alloc(wiki_cache_t *, void *) __nonnull((1,2)) __wur;
void wiki_cache_dealloc(wiki_cache_t *, void *, void *) __nonnull((1,2,3));
int wiki_cache_obj_used(wiki_cache_t *, void *) __nonnull((1,2)) __wur;
int wiki_cache_nr_used(wiki_cache_t *) __nonnull((1)) __wur;
int wiki_cache_capacity(wiki_cache_t *) __nonnull((1)) __wur;
void wiki_cache_clear_all(wiki_cache_t *) __nonnull((1));

#endif /* WIKI_CACHE_H */
