#include <assert.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "buffer.h"
#include "http.h"
#include "parse.h"
#include "wikigrab.h"

int
value_cache_ctor(void *obj)
{
	assert(obj);

	value_t *val = (value_t *)obj;
	if (!(val->value = calloc(MAX_VALUE_LEN+1, 1)))
		return -1;

	val->vlen = 0;

	return 0;
}

void
value_cache_dtor(void *obj)
{
	assert(obj);

	value_t *val = (value_t *)obj;

	if (val->value)
		free(val->value);

	val->value = NULL;

	return;
}

#define RESET() (p = savep = buf->buf_head)

static void
__remove_html_tags(buf_t *buf)
{
	char *p;
	char *savep;
	char *tail = buf->buf_tail;
	size_t range;

	savep = buf->buf_head;

	p = memchr(savep, 0x3c, (tail - savep));

	if (!p)
		return;

	while(*p == 0x3c)
	{
		savep = p;

		p = memchr(savep, 0x3e, (tail - savep));

		if (!p)
			break;

		++p;

		range = (p - savep);

		buf_collapse(buf, (off_t)(savep - buf->buf_head), range);
		p = savep;
		tail = buf->buf_tail;
	}

	return;
}

static void
__remove_inline_refs(buf_t *buf)
{
	char *p;
	char *savep;
	char *tail = buf->buf_tail;
	size_t range;

	p = savep = buf->buf_head;

	while(1)
	{
		p = strstr(savep, "&#91;");

		if (!p || p >= tail)
			break;

		savep = p;

		p = strstr(savep, "&#93;");

		if (!p)
			p = (savep + strlen("&#91;"));
		else
			p += strlen("&#93;");

		range = (p - savep);
#ifdef DEBUG
		printf("removing LINE_BEGIN|%.*s|LINE_END\n", (int)range, savep);
#endif
		buf_collapse(buf, (off_t)(savep - buf->buf_head), range);
		tail = buf->buf_tail;
		p = savep;
	}
}

static void
__remove_html_encodings(buf_t *buf)
{
	char *p;
	char *savep;
	char *tail = buf->buf_tail;
	size_t range;

	p = savep = buf->buf_tail;

	while(1)
	{
		p = strstr(savep, "&#");

		if (!p)
			break;

		*p++ = 0x20;

		savep = p;

		p = memchr(savep, ';', (tail - savep));

		if (!p)
			p = (savep + 1);
		else
			++p;

		range = (p - savep);
#ifdef DEBUG
		printf("removing LINE_BEGIN|%.*s|LINE_END\n", (int)range, savep);
#endif
		buf_collapse(buf, (off_t)(savep - buf->buf_head), range);
		p = savep;
		tail = buf->buf_tail;
	}
}

struct html_entity_t
{
	char *entity;
	char _char;
};

struct html_entity_t HTML_ENTS[5] =
{
	{ "&quot;", 0x22 },
	{ "&amp;", 0x26 },
	{ "&lt;", 0x3c },
	{ "&gt;", 0x3e },
	{ (char *)NULL, 0 }
};

static void
__replace_html_entities(buf_t *buf)
{
	char *tail = buf->buf_tail;
	char *p;
	char *savep;
	size_t len;
	int i;

	if (option_set(OPT_FORMAT_XML))
		return;

	for (i = 0; HTML_ENTS[i].entity != NULL; ++i)
	{
		p = savep = buf->buf_head;

		while (1)
		{
			p = strstr(savep, HTML_ENTS[i].entity);
			len = strlen(HTML_ENTS[i].entity - 1);

			if (!p || p >= tail)
				break;

			*p++ = HTML_ENTS[i]._char;
			buf_collapse(buf, (off_t)(p - buf->buf_head), len);
			savep = p;
			tail = buf->buf_tail;
		}
	}
	return;
}

static void
__remove_garbage(buf_t *buf)
{
	buf_t copy_buf;
	char *p;
	char *q;
	char *savep;
	char *tail = buf->buf_tail;
	size_t range;

	buf_init(&copy_buf, DEFAULT_MAX_LINE_SIZE * 2);
	p = savep = buf->buf_head;

	while(1)
	{
		p = memchr(savep, 0x2e, (tail - savep));

		if (!p)
			break;

		savep = p;
		while (!isspace(*p))
			--p;

		q = savep;
		while (!isspace(*q))
			++q;

		range = (q - p);

		buf_append_ex(&copy_buf, p, range);

		if ((*(q-1) == 0x2e) /* End of a line */
		|| (*(q-1) == ')' && *(q-2) == 0x2e) /* End of line in parenthesis */
		|| strstr(copy_buf.buf_head, "&lt;")
		|| strstr(copy_buf.buf_head, "&gt;")
		|| strstr(copy_buf.buf_head, "&quot;")
		|| (isdigit(*(savep-1)) && isdigit(*(savep+1))) /* e.g., "Version 2.0" */
		|| (!memchr(p, '-', range)
		&& !memchr(p, '{', range)
		&& !memchr(p, '}', range)
		&& !memchr(p, '#', range)
		&& !memchr(p, ';', range)
		&& !memchr(p, '(', range)
		&& !memchr(p, ')', range)
		&& !memchr(p, '-', range)))
		{
			p = savep = q;
			continue;
		}

#ifdef DEBUG
		printf("removing LINE_BEGIN|%.*s|LINE_END\n", (int)range, savep);
#endif
		buf_collapse(buf, (off_t)(p - buf->buf_head), range);
		savep = p;
		tail = buf->buf_tail;
	}

	buf_destroy(&copy_buf);

	return;
}

/*
 * buf_shift() could require extending the buffer,
 * and this in turn could result in our data
 * being copied to somewhere else in the heap.
 * So save all the offsets and restore them
 * after the shift.
 */
#define BUF_SHIFT_SAFE(by, optr)\
do {\
	size_t __lstart_off;\
	size_t __lend_off;\
	size_t __nloff;\
	size_t __rightoff;\
	size_t __poff;\
	size_t __spoff;\
	if (line_start)\
		__lstart_off = (line_start - buf->buf_head);\
	if (line_end)\
		__lend_off = (line_end - buf->buf_head);\
	if (new_line)\
		__nloff = (new_line - buf->buf_head);\
	if (right)\
		__rightoff = (right - buf->buf_head);\
	if (p)\
		__poff = (p - buf->buf_head);\
	if (savep)\
		__spoff = (savep - buf->buf_head);\
	buf_shift(buf, (off_t)((optr) - buf->buf_head), (by));\
	if (line_start)\
		line_start = (buf->buf_head + __lstart_off);\
	if (line_end)\
		line_end = (buf->buf_head + __lend_off);\
	if (new_line)\
		new_line = (buf->buf_head + __nloff);\
	if (right)\
		right = (buf->buf_head + __rightoff);\
	if (p)\
		p = (buf->buf_head + __poff);\
	if (savep)\
		savep = (buf->buf_head + __spoff);\
} while(0)

static int
__do_format_txt(buf_t *buf)
{
	assert(buf);

	char *tail = NULL;
	char *line_start = NULL;
	char *line_end = NULL;
	char *p = NULL;
	char *savep = NULL;
	char *left = NULL;
	char *right = NULL;
	char *new_line = NULL;
	size_t line_len;
	size_t delta;
	int	gaps = 0;
	int passes;
	int remainder;
	int volte_face = 0;

	p = buf->buf_head;
	if (*p == 0x0a)
	{
		while (*p == 0x0a && p < tail)
			++p;

		buf_collapse(buf, (off_t)0, (p - buf->buf_head));
	}

	tail = buf->buf_tail;
	line_start = buf->buf_head;

	while (1)
	{
		outer_loop_begin:
		line_end = (line_start + WIKI_ARTICLE_LINE_LENGTH);

		if (line_end > tail)
			line_end = tail;

		if (line_start >= line_end)
			break;

		savep = line_start;

		/*
		 * Remove new lines occuring within our
		 * new line length.
		 */
		while (1)
		{
			if (savep >= line_end)
				break;

			p = memchr(savep, 0x0a, (line_end - savep));

			if (!p)
				break;

		/*
		 * Then it's the end of a paragraph.
		 * Leave it alone and go to 
		 */
			if (*(p+1) == 0x0a)
			{
				while (*p == 0x0a)
					++p;

				line_start = savep = p;
				goto outer_loop_begin;
			}

			*p++ = 0x20;
			savep = p;
		}

		/*
		 * LINE_START + WIKI_ARTICLE_LINE_LENGTH may
		 * happen to already be on a new line.
		 */
		if (*line_end == 0x0a)
		{
			new_line = line_end;

			while (*line_end == 0x0a)
				++line_end;
		}
		else
		if (*line_end == 0x20)
		{
			new_line = line_end;

			*line_end++ = 0x0a;
		}
		else
		if (line_end < tail)
		{
			/*
			 * Find the nearest space backwards.
			 */
			while (*line_end != 0x20 && line_end > (line_start + 1))
				--line_end;

			if (line_end == line_start)
			{
				line_end += WIKI_ARTICLE_LINE_LENGTH;

				BUF_SHIFT_SAFE(1, line_end);
				tail = buf->buf_tail;

				*line_end++ = 0x0a;

				while (*line_end == 0x0a)
					++line_end;

				line_start = line_end;

				continue;
			}

			new_line = line_end;

			*line_end++ = 0x0a;
		}

		if (line_end >= tail)
			line_end = new_line = tail;

		line_len = (new_line - line_start);
		delta = (WIKI_ARTICLE_LINE_LENGTH - line_len);

		/*
		 * Needs justified.
		 */
		if (delta > 0)
		{
			p = savep = line_start;
			gaps = 0;

			while (1)
			{
				p = memchr(savep, 0x20, (new_line - savep));

				if (!p)
					break;

				++gaps;

				while (*p == 0x20)
					++p;

				if (p >= new_line)
					break;

				savep = p;
			}

			if (line_len < (WIKI_ARTICLE_LINE_LENGTH / 3))
			{
				line_start = line_end;
				continue;
			}

			if (!gaps)
			{
				line_start = line_end;
				continue;
			}

			passes = (delta / gaps);
			remainder = (delta % gaps);

			p = savep = line_start;
			while (passes > 0)
			{
				p = memchr(savep, 0x20, (new_line - savep));

				if (!p)
				{
					--passes;
					p = savep = line_start;
					continue;
				}

				BUF_SHIFT_SAFE(1, p);
				tail = buf->buf_tail;
				++line_end;
				++new_line;

				*p++ = 0x20;

				while (*p == 0x20)
					++p;

				if (p >= new_line)
					break;

				savep = p;
			}

			if (remainder)
			{
				left = line_start;
				right = new_line;
				volte_face = 0;

				while (remainder)
				{
					if (!volte_face)
					{
						p = memchr(left, 0x20, (right - left));

						if (!p)
						{
							left = line_start;
							right = (new_line - 1);
							volte_face = 0;
							continue;
						}
					}
					else
					{
						p = right;
						while (*p != 0x20 && p > left)
							--p;

						if (p == left)
						{
							left = line_start;
							right = (new_line - 1);
							volte_face = 0;
							continue;
						}
					}

					BUF_SHIFT_SAFE(1, p);
					tail = buf->buf_tail;

					++line_end;
					++new_line;
					++right;

					*p++ = 0x20;
					--remainder;

					if (!volte_face)
					{
						while (*p == 0x20)
							++p;

						left = p;

						volte_face = 1;
					}
					else
					{
						while (*p == 0x20)
							--p;

						right = p;

						volte_face = 0;
					}
				} /* while(remainder) */
			} /* if (remainder) */
		} /* if (delta > 0) */

		line_start = line_end;

		if (line_end == tail)
			break;
	} /* while(1) */

	__replace_html_entities(buf);

	return 0;
}

static void
__do_format_json(buf_t *buf)
{
	char *p;
	char *savep;
	char *tail = buf->buf_tail;
	size_t range;

	p = savep = buf->buf_head;

	while(1)
	{
		p = memchr(savep, 0x22, (tail - savep));

		if (!p)
			break;

		buf_shift(buf, (off_t)(p - buf->buf_head), (size_t)1);
		*p++ = '/';

		savep = ++p;
		tail = buf->buf_tail;
	}

	p = savep = buf->buf_head;
	while(1)
	{
		p = memchr(savep, 0x0a, (tail - savep));

		if (!p)
			break;

		savep = p;

		while (*p == 0x0a)
			*p++ = 0x20;

		range = (p - savep);

		if (range > 1)
		{
			++savep;

			buf_collapse(buf, (off_t)(savep - buf->buf_head), (p - savep));
			p = savep;
			tail = buf->buf_tail;
		}
	}
}

static char tag_content[8192];

static char *
__get_tag_content(buf_t *buf, const char *tag)
{
	assert(buf);
	assert(tag);

	char *tail = buf->buf_tail;
	char *p;
	char *savep;

	p = strstr(buf->buf_head, tag);

	if (!p)
		return NULL;

	savep = p;

	p = memchr(savep, 0x3e, (tail - savep));

	if (!p)
		return NULL;

	savep = ++p;

	p = memchr(savep, 0x3c, (tail - savep));

	if (!p)
		return NULL;

	strncpy(tag_content, savep, (p - savep));
	tag_content[p - savep] = 0;

	return tag_content;
}

static char *
__get_tag_field(buf_t *buf, const char *tag, const char *field)
{
	assert(buf);
	assert(tag);
	assert(field);

	char *tail = buf->buf_tail;
	char *p;
	char *savep;

	p = strstr(buf->buf_head, tag);

	if (!p)
		return NULL;

	savep = p;

	p = strstr(savep, field);

	if (!p)
		return NULL;

	savep = p;

	/* e.g. "content=\"..."
	 *       s         p
	 */
	p += (strlen(field) + 2);

	savep = p;

	p = memchr(savep, 0x22, (tail - savep));

	if (!p)
		return NULL;

	strncpy(tag_content, savep, (p - savep));
	tag_content[p - savep] = 0;

	return tag_content;
}

static int
__remove_braces(buf_t *buf)
{
	assert(buf);

	char *tail = buf->buf_tail;
	char *p;
	char *savep;
	char *outer_start;
	char *outer_end;
	char *search_from;
	int depth = 0;
	size_t range;

/*
 * 1. FIND LEFT BRACE
 * 2. IF NO LEFT BRACE, GOTO 9, ELSE FIND NEXT RIGHT BRACE
 * 3. SEARCH [LEFTBRACE+1,RIGHT_BRACE) FOR MORE LEFT BRACES
 * 4. IF MORE LEFT BRACES, FIND EQUAL NUMBER OF RIGHT BRACES FROM [RIGHTBRACE+1...TAIL), ELSE GOTO 7
 * 5. SEARCH RANGE [RIGHTBRACE+1...NEW RIGHT BRACE) FOR MORE LEFT BRACES
 * 6. GOTO 4
 * 7. REMOVE RANGE [FIRST LEFT BRACE...LAST RIGHT BRACE]
 * 8. GO TO START OF BUFFER, GOTO 1
 * 9. END
 */

	while(1)
	{
		p = savep = buf->buf_head;
		depth = 0;

		outer_start = memchr(savep, 0x7b, (tail - savep));

		if (!outer_start)
			goto out;

		search_from = (outer_start + 1);
		outer_end = memchr(search_from, 0x7d, (tail - search_from));

		if (!outer_end)
		{
			outer_end = tail;
			goto out_collapse;
		}

		++outer_end;

		for(;;)
		{
			savep = search_from;
			depth = 0;

			while(1)
			{
				p = memchr(savep, 0x7b, (outer_end - savep));

				if (!p)
					break;

				++depth;

				savep = ++p;

				if (p >= outer_end)
					break;
			}

			if (depth == 0) /* then we've found the outermost right brace for our initial left brace */
			{
#ifdef DEBUG
				printf("removing \x1b[38;5;9mLINE_START\x1b[m%.*s\x1b[38;5;9mLINE_END\x1b[m\n", (int)(outer_end - outer_start), outer_start);
#endif
				buf_collapse(buf, (off_t)(outer_start - buf->buf_head), (outer_end - outer_start));
				break;
			}

			search_from = outer_end;
			savep = search_from;

			while (depth > 0)
			{
				if (*outer_end != 0x7d)
					outer_end = memchr(savep, 0x7d, (tail - savep));

				if (!outer_end)
				{
					outer_end = tail;
					goto out_collapse;
				}

				--depth;

				++outer_end;
				savep = outer_end;

				if (savep >= tail)
					break;
			}
		} /* for(;;) */
	} /* while(1) */


	/*
	 * We jump here if we never found a matching closing
	 * right brace, so we just clear from the outer
	 * left brace to the tail of the buffer.
	 */
	out_collapse:
	range = (outer_end - outer_start);
	buf_collapse(buf, (off_t)(outer_start - buf->buf_head), range);

	out:
	return 0;
}

static void
__normalise_file_title(buf_t *buf)
{
	assert(buf);

	char *tail = buf->buf_tail;
	char *p = buf->buf_head;

	p = buf->buf_head;
	while (p < tail)
	{
		if (*p == 0x20
		|| (*p != 0x2f && *p != 0x5f && !isalpha(*p) && !isdigit(*p)))
		{
			*p++ = 0x5f;
			if (*(p-2) == 0x5f)
			{
				--p;
				buf_collapse(buf, (off_t)(p - buf->buf_head), (size_t)1);
				tail = buf->buf_tail;
			}

			continue;
		}

		++p;
	}

	while (!isalpha(*(buf->buf_tail - 1)) && !isdigit(*(buf->buf_tail - 1)))
		buf_snip(buf, 1);

	if (option_set(OPT_FORMAT_XML))
		buf_append(buf, ".xml");
	else
	if (option_set(OPT_FORMAT_JSON))
		buf_append(buf, ".json");
	else
		buf_append(buf, ".txt");

	return;
}

int
extract_wiki_article(buf_t *buf)
{
	int out_fd = -1;
	char *p;
	char *savep;
	buf_t file_title;
	buf_t content_buf;
	http_header_t *server;
	http_header_t *date;
	http_header_t *lastmod;
	static char inet6_string[INET6_ADDRSTRLEN];
	char *buffer = NULL;
	char *home;
	struct sockaddr_in sock4;
	struct sockaddr_in6 sock6;
	struct addrinfo *ainf = NULL;
	struct addrinfo *aip = NULL;
	int gotv4 = 0;
	int gotv6 = 0;
	wiki_cache_t *value_cache = NULL;
	struct article_header article_header;
	size_t vlen;

	if (!(buffer = calloc(DEFAULT_TMP_BUF_SIZE, 1)))
		goto fail;

	clear_struct(&article_header);

	/*
	 * Extract data for display
	 * at top of the article.
	 */
	value_cache = wiki_cache_create(
			"value_cache",
			sizeof(value_t),
			0,
			value_cache_ctor,
			value_cache_dtor);

	article_header.title = (value_t *)wiki_cache_alloc(value_cache);
	article_header.server_name = (value_t *)wiki_cache_alloc(value_cache);
	article_header.server_ipv4 = (value_t *)wiki_cache_alloc(value_cache);
	article_header.server_ipv6 = (value_t *)wiki_cache_alloc(value_cache);
	article_header.generator = (value_t *)wiki_cache_alloc(value_cache);
	article_header.lastmod = (value_t *)wiki_cache_alloc(value_cache);
	article_header.downloaded = (value_t *)wiki_cache_alloc(value_cache);
	article_header.content_len = (value_t *)wiki_cache_alloc(value_cache);

	/* Extract these values from the HTTP response header */
	server = (http_header_t *)wiki_cache_alloc(http_hcache);
	date = (http_header_t *)wiki_cache_alloc(http_hcache);
	lastmod = (http_header_t *)wiki_cache_alloc(http_hcache);

	http_fetch_header(buf, "Server", server, (off_t)0);
	http_fetch_header(buf, "Date", date, (off_t)0);
	http_fetch_header(buf, "Last-Modified", lastmod, (off_t)0);

	strcpy(article_header.server_name->value, server->value);
	article_header.server_name->vlen = server->vlen;

	strcpy(article_header.downloaded->value, date->value);
	article_header.downloaded->vlen = date->vlen;

	strcpy(article_header.lastmod->value, lastmod->value);
	article_header.lastmod->vlen = lastmod->vlen;

	wiki_cache_dealloc(http_hcache, (void *)server);
	wiki_cache_dealloc(http_hcache, (void *)date);
	wiki_cache_dealloc(http_hcache, (void *)lastmod);

	buf_init(&content_buf, DEFAULT_TMP_BUF_SIZE);
	buf_init(&file_title, pathconf("/", _PC_PATH_MAX));

	home = getenv("HOME");
	buf_append(&file_title, home);
	buf_append(&file_title, WIKIGRAB_DIR);
	buf_append(&file_title, "/");

	__get_tag_content(buf, "<title");
	buf_append(&file_title, tag_content);
	buf_snip(&file_title, strlen(" - Wikipedia"));

	vlen = strlen(tag_content);
	strncpy(article_header.title->value, tag_content, vlen);
	article_header.title->value[vlen] = 0;
	article_header.title->vlen = vlen;

	/*
	 * Replace spaces (and any non-underscore non-ascii chars) with underscores.
	 */
	__normalise_file_title(&file_title);

	__get_tag_field(buf, "<meta name=\"generator\"", "content");

	vlen = strlen(tag_content);
	strncpy(article_header.generator->value, tag_content, vlen);
	article_header.generator->value[vlen] = 0;
	article_header.generator->vlen = vlen;

	if ((out_fd = open(file_title.buf_head, O_RDWR|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR)) < 0)
		goto out_destroy_bufs;

	if (server->value[0])
	{
		clear_struct(&sock4);
		clear_struct(&sock6);

		getaddrinfo(server->value, NULL, NULL, &ainf);
		if (ainf)
		{
			for (aip = ainf; aip; aip = aip->ai_next)
			{
				if (aip->ai_socktype == SOCK_STREAM
				&& aip->ai_family == AF_INET)
				{
					memcpy(&sock4, aip->ai_addr, aip->ai_addrlen);
					gotv4 = 1;
					break;
				}
			}

			for (aip = ainf; aip; aip = aip->ai_next)
			{
				if (aip->ai_family == AF_INET6)
				{
					memcpy(&sock6, aip->ai_addr, aip->ai_addrlen);
					gotv6 = 1;
					break;
				}
			}
		}
	}

	if (gotv4)
		strcpy(article_header.server_ipv4->value, inet_ntoa(sock4.sin_addr));
	else
		strcpy(article_header.server_ipv4->value, "None");

	if (gotv6)
		strcpy(article_header.server_ipv6->value, inet_ntop(AF_INET6, sock6.sin6_addr.s6_addr, inet6_string, INET6_ADDRSTRLEN));
	else
		strcpy(article_header.server_ipv6->value, "None");

	buf_clear(&content_buf);

/*
 * BEGIN PARSING THE TEXT FROM THE ARTICLE.
 */

	__remove_braces(buf);

	RESET();
	p = strstr(savep, "\"mw-content-text\"");
	if (!p)
		goto out_destroy_file;

	buf_clear(&content_buf);

	p = savep = content_buf.buf_head;

	/*
	 * Copy everything between <p></p> tags.
	 */
	while (1)
	{
		p = strstr(savep, "<p");

		if (!p)
			break;

		savep = p;
		p = strstr(savep, "</p");

		if (!p)
			break;

		if (option_set(OPT_FORMAT_XML))
			buf_append(&content_buf, BEGIN_PARA_MARK);

		buf_append_ex(&content_buf, savep, (p - savep));

		if (option_set(OPT_FORMAT_XML))
			buf_append(&content_buf, END_PARA_MARK);

		buf_append(&content_buf, "\n");

		savep = p;
	}

	__remove_html_tags(&content_buf);
	__remove_inline_refs(&content_buf);
	__remove_html_encodings(&content_buf);
	__replace_html_entities(&content_buf);
	__remove_garbage(&content_buf);

	/*
	 * Replace the _BEGIN_PARA_ and _END_PARA_
	 * with <paragraph> and </paragraph>
	 * respectively.
	 */
	if (option_set(OPT_FORMAT_XML))
	{
		static char *opener = "<paragraph>";
		static char *closer = "</paragraph>\n\n";
		size_t omark_len = strlen(BEGIN_PARA_MARK);
		size_t cmark_len = strlen(END_PARA_MARK);
		size_t opener_len = strlen(opener);
		size_t closer_len = strlen(closer);
		ssize_t shift = 0;

		p = savep = content_buf.buf_head;
		while(1)
		{
			p = strstr(savep, BEGIN_PARA_MARK);

			if (!p)
				break;

			shift = (opener_len - omark_len);

			if (shift > 0)
				buf_shift(&content_buf, (off_t)(p - content_buf.buf_head), shift);

			strncpy(p, opener, opener_len);

			p += opener_len;

			if (shift < 0)
				buf_collapse(&content_buf, (off_t)(p - content_buf.buf_head), (size_t)(0 - shift));

			savep = p;

			p = strstr(savep, END_PARA_MARK);

			shift = (closer_len - cmark_len);

			if (shift > 0)
				buf_shift(&content_buf, (off_t)(p - content_buf.buf_head), (size_t)shift);

			strncpy(p, closer, closer_len);

			p += closer_len;

			if (shift < 0)
				buf_collapse(&content_buf, (off_t)(p - content_buf.buf_head), (size_t)(0 - shift));

			savep = p;
		}
	}

	sprintf(article_header.content_len->value, "%lu", content_buf.data_len);
	article_header.content_len->vlen = strlen(article_header.content_len->value);

	if (option_set(OPT_FORMAT_XML))
	{
		sprintf(buffer,
			"%s\n"
			"<wiki>\n"
			"\t<metadata>\n"
			"\t\t<meta name=\"Title\" content=\"%s\"/>\n"
			"\t\t<meta name=\"Parser\" content=\"WikiGrab v%s\"/>\n"
			"\t\t<meta name=\"Server\" content=\"%s\"/>\n"
			"\t\t<meta name=\"Server-ipv4\" content=\"%s\"/>\n"
			"\t\t<meta name=\"Server-ipv6\" content=\"%s\"/>\n"
			"\t\t<meta name=\"Generator\" content=\"%s\"/>\n"
			"\t\t<meta name=\"Modified\" content=\"%s\"/>\n"
			"\t\t<meta name=\"Downloaded\" content=\"%s\"/>\n"
			"\t\t<meta name=\"Length\" content=\"%s\"/>\n"
			"\t</metadata>\n"
			"\t<text>\n",
			XML_START_LINE,
			article_header.title->value,
			WIKIGRAB_BUILD,
			article_header.server_name->value,
			article_header.server_ipv4->value,
			article_header.server_ipv6->value,
			article_header.generator->value,
			article_header.lastmod->value,
			article_header.downloaded->value,
			article_header.content_len->value);

			buf_append(&content_buf, "</text>\n</wiki>\n");
	}
	else
	if (option_set(OPT_FORMAT_JSON))
	{
		sprintf(buffer,
			"{\n"
			"\t\"Title\": \"%s\",\n"
			"\t\"Parser\": \"WikiGrab v%s\",\n"
			"\t\"Server\": {\n"
			"\t\t\t\"Name\": \"%s\",\n"
			"\t\t\t\"IPV4\": \"%s\",\n"
			"\t\t\t\"IPV6\": \"%s\",\n"
			"\t},\n"
			"\t\"Generator\": \"%s\",\n"
			"\t\"LastModified\": \"%s\",\n"
			"\t\"Downloaded\": \"%s\",\n"
			"\t\"Length\": \"%s\",\n"
			"\t\"Content\":\n"
			"\"",
			article_header.title->value,
			WIKIGRAB_BUILD,
			article_header.server_name->value,
			article_header.server_ipv4->value,
			article_header.server_ipv6->value,
			article_header.generator->value,
			article_header.lastmod->value,
			article_header.downloaded->value,
			article_header.content_len->value);

			__do_format_json(&content_buf);
			buf_append(&content_buf, "\"\n}\n");
	}
	else
	{
		sprintf(buffer,
			"      @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n\n"
			"                    Downloaded via WikiGrab v%s\n\n"
			"      >>>>>>>>>>>>>>>>>>>>>>>>>>>>><<<<<<<<<<<<<<<<<<<<<<<<<<<<<\n\n"
			"%*s%s\n"
			"%*s%s\n"
			"%*s%s\n"
			"%*s%s\n"
			"%*s%s\n"
			"%*s%s\n"
			"%*s%s\n"
			"%*s%s\n\n"
			"      @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n\n",
			WIKIGRAB_BUILD,
			LEFT_ALIGN_WIDTH, "Title: ", article_header.title->value,
			LEFT_ALIGN_WIDTH, "Served-by: ", article_header.server_name->value,
			LEFT_ALIGN_WIDTH, "Generated-by: ", article_header.generator->value,
			LEFT_ALIGN_WIDTH, "Server-ip-v4: ", article_header.server_ipv4->value,
			LEFT_ALIGN_WIDTH, "Server-ip-v6: ", article_header.server_ipv6->value,
			LEFT_ALIGN_WIDTH, "Last-modified: ", article_header.lastmod->value,
			LEFT_ALIGN_WIDTH, "Downloaded: ", article_header.downloaded->value,
			LEFT_ALIGN_WIDTH, "Content-length: ", article_header.content_len->value);

		__do_format_txt(&content_buf);
	}

	write(out_fd, buffer, strlen(buffer));
	buf_write_fd(out_fd, &content_buf);
	close(out_fd);
	out_fd = -1;

	if (option_set(OPT_OPEN_FINISH))
	{
		pid_t child;

		child = fork();

		if (!child)
		{
			execlp("gedit", file_title.buf_head, (char *)0);
		}
	}

	wiki_cache_dealloc(value_cache, (void *)article_header.title);
	wiki_cache_dealloc(value_cache, (void *)article_header.server_name);
	wiki_cache_dealloc(value_cache, (void *)article_header.server_ipv4);
	wiki_cache_dealloc(value_cache, (void *)article_header.server_ipv6);
	wiki_cache_dealloc(value_cache, (void *)article_header.generator);
	wiki_cache_dealloc(value_cache, (void *)article_header.content_len);
	wiki_cache_dealloc(value_cache, (void *)article_header.lastmod);
	wiki_cache_dealloc(value_cache, (void *)article_header.downloaded);

	wiki_cache_destroy(value_cache);

	buf_destroy(&content_buf);
	buf_destroy(&file_title);

	free(buffer);
	buffer = NULL;

	return 0;

	out_destroy_file:

	ftruncate(out_fd, (off_t)0);
	unlink(file_title.buf_head);

	out_destroy_bufs:

	buf_destroy(&content_buf);
	buf_destroy(&file_title);

	free(buffer);
	buffer = NULL;

	wiki_cache_dealloc(value_cache, (void *)article_header.title);
	wiki_cache_dealloc(value_cache, (void *)article_header.server_name);
	wiki_cache_dealloc(value_cache, (void *)article_header.server_ipv4);
	wiki_cache_dealloc(value_cache, (void *)article_header.server_ipv6);
	wiki_cache_dealloc(value_cache, (void *)article_header.generator);
	wiki_cache_dealloc(value_cache, (void *)article_header.content_len);
	wiki_cache_dealloc(value_cache, (void *)article_header.lastmod);
	wiki_cache_dealloc(value_cache, (void *)article_header.downloaded);

	wiki_cache_destroy(value_cache);

	fail:
	return -1;
}
