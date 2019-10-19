#ifndef HTML_H
#define HTML_H 1

#include "buffer.h"
#include "cache.h"

int html_get_all(wiki_cache_t *, buf_t *, const char *, const char *) __nonnull((1,2,3,4)) __wur;
int html_get_all_class(wiki_cache_t *, buf_t *, const char *) __nonnull((1,2,3)) __wur;
char *html_get_tag_field(buf_t *, const char *, const char *) __nonnull((1,2,3)) __wur;
char *html_get_tag_content(buf_t *, const char *) __nonnull((1,2)) __wur;
void html_remove_content(buf_t *, char *, char *) __nonnull((1,2,3));
void html_remove_elements_class(buf_t *, const char *) __nonnull((1,2));
void html_remove_elements_id(buf_t *, const char *) __nonnull((1,2));

#endif /* !defined HTML_H */
