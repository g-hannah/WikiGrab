#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "buffer.h"
#include "format.h"
#include "http.h"
#include "parse.h"
#include "wikigrab.h"

/**
 * Copy contents of HTML tag into buffer - append LF
 */
#define GET_TAG_CONTENT(tag, p, s, t)\
do {\
	buf_clear(&content_buf);\
	(p) = strstr((s), (tag));\
	if ((p))\
	{\
		(s) = (p);\
		(p) = memchr((s), 0x3e, (t) - (s));\
		if ((p))\
		{\
			++(p);\
			(s) = (p);\
			(p) = memchr((s), 0x3c, (t) - (s));\
			if (!(p))\
				return -1;\
			buf_append_ex(&content_buf, (s), ((p) - (s)));\
			buf_append_ex(&content_buf, "\n", 1);\
			++(p);\
			(s) = (p)++;\
		}\
	}\
	else\
		(p) = (s);\
} while(0)

#define GET_INTAG_CONTENT(tag, name, p, s, t)\
do {\
	buf_clear(&content_buf);\
	(p) = strstr((s), (tag));\
	if ((p))\
	{\
		(s) = (p);\
		(p) = strstr((s), (name));\
		if ((p))\
		{\
			(s) = (p);\
			(p) = memchr((s), '"', (t) - (s));\
			if (!(p))\
				goto out_destroy_file;\
			++(p);\
			(s) = (p);\
			(p) = memchr((s), '"', (t) - (s));\
			if (!(p))\
				goto out_destroy_file;\
			buf_append_ex(&content_buf, (s), ((p) - (s)));\
			buf_append_ex(&content_buf, "\n", 1);\
			++(p);\
			(s) = (p)++;\
		}\
	}\
	else\
		(p) = (s);\
} while(0)

#define RESET() (p = savep = buf->buf_head)

int
extract_wiki_article(buf_t *buf)
{
	int out_fd = -1;
	char *tail = buf->buf_tail;
	char *p;
	char *q;
	char *savep;
	char *saveq;
	buf_t file_title;
	buf_t content_buf;
	buf_t copy_buf;
	http_header_t *server;
	http_header_t *date;
	size_t range;
	static char scratch_buf[DEFAULT_MAX_LINE_SIZE];

	server = (http_header_t *)wiki_cache_alloc(http_hcache);
	date = (http_header_t *)wiki_cache_alloc(http_hcache);

	assert(wiki_cache_obj_used(http_hcache, (void *)server));
	assert(wiki_cache_obj_used(http_hcache, (void *)date));

	buf_init(&content_buf, DEFAULT_TMP_BUF_SIZE);
	buf_init(&file_title, pathconf("/", _PC_PATH_MAX));

	RESET();
	GET_TAG_CONTENT("<title", p, savep, tail);

	if (p != savep)
	{
		q = saveq = content_buf.buf_head;

		q = strstr(saveq, " - ");

		if (q)
			buf_collapse(&content_buf, (off_t)(q - content_buf.buf_head), (content_buf.buf_tail - q));

		buf_copy(&file_title, &content_buf);

		q = saveq = file_title.buf_head;

		while (q < file_title.buf_tail)
		{
			if (*q == 0x20
			|| (!isalpha(*q) && !isdigit(*q)))
			{
				*q++ = 0x5f;
				if (*(q-2) == 0x5f)
				{
					--q;
					buf_collapse(&file_title, (off_t)(q - file_title.buf_head), (size_t)1);
				}
				continue;
			}

			++q;
		}

		while (!isalpha(*(file_title.buf_tail - 1)) && !isdigit(*(file_title.buf_tail - 1)))
			buf_snip(&file_title, 1);

		buf_append_ex(&file_title, ".txt", 4);

		if ((out_fd = open(file_title.buf_head, O_RDWR|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR)) < 0)
			goto out_destroy_bufs;

		buf_init(&copy_buf, content_buf.data_len);
		buf_copy(&copy_buf, &content_buf);
	}
	else
	{
		goto out_destroy_bufs;
	}

	buf_clear(&content_buf);

	buf_append(&content_buf, "      @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n\n");
	buf_append(&content_buf, "                      Grabbed by WikiGrab v");
	buf_append(&content_buf, WIKIGRAB_BUILD);
	buf_append(&content_buf, "\n\n");
	buf_append(&content_buf, "      >>>>>>>>>>>>>>>>>>>>>>>>>>>>><<<<<<<<<<<<<<<<<<<<<<<<<<<<<\n\n");

	write(out_fd, content_buf.buf_head, content_buf.data_len);
	
	sprintf(scratch_buf, "%*s", LEFT_ALIGN_WIDTH, "Title: ");
	write(out_fd, scratch_buf, strlen(scratch_buf));
	buf_append(&copy_buf, "\n");
	write(out_fd, copy_buf.buf_head, copy_buf.data_len);

	buf_destroy(&copy_buf);

	http_fetch_header(buf, "Server", server, (off_t)0);
	http_fetch_header(buf, "Date", date, (off_t)0);

	if (date->value[0])
	{
		sprintf(scratch_buf, "%*s", LEFT_ALIGN_WIDTH, "Obtained: ");
		write(out_fd, scratch_buf, strlen(scratch_buf));
		date->value[date->vlen++] = 0x0a;
		date->value[date->vlen] = 0;
		write(out_fd, date->value, date->vlen);
	}

	if (server->value[0])
	{
		sprintf(scratch_buf, "%*s", LEFT_ALIGN_WIDTH, "Served-by: ");
		write(out_fd, scratch_buf, strlen(scratch_buf));
		server->value[server->vlen++] = 0x0a;
		server->value[server->vlen] = 0;
		write(out_fd, server->value, server->vlen);
	}

	RESET();
	GET_INTAG_CONTENT("<meta name=\"generator\"", "content", p, savep, tail);
	if (p != savep)
	{
		sprintf(scratch_buf, "%*s", LEFT_ALIGN_WIDTH, "Generated-by: ");
		write(out_fd, scratch_buf, strlen(scratch_buf));
		write(out_fd, content_buf.buf_head, content_buf.data_len);
	}

	RESET();
	GET_INTAG_CONTENT("<meta", "charset", p, savep, tail);
	if (p != savep)
	{
		sprintf(scratch_buf, "%*s", LEFT_ALIGN_WIDTH, "Charset: ");
		write(out_fd, scratch_buf, strlen(scratch_buf));
		write(out_fd, content_buf.buf_head, content_buf.data_len);
	}

	buf_clear(&content_buf);

	buf_append(&content_buf, "\n      @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n\n");
	write(out_fd, content_buf.buf_head, content_buf.data_len);

	/*
	 * Get the paragraphs of the text.
	 */

	RESET();
	p = strstr(savep, "\"mw-content-text\"");
	if (!p)
		goto out_destroy_file;

	buf_clear(&content_buf);
	savep = p;
	while (1)
	{
		p = strstr(savep, "<p");
		if (!p)
			break;

		savep = p;
		p = strstr(savep, "</p");

		if (!p)
			break;

		buf_append_ex(&content_buf, savep, (p - savep));
		buf_append_ex(&content_buf, "\n\n", 1);

		savep = p;
	}

	p = savep = content_buf.buf_head;
	tail = content_buf.buf_tail;

	/*
	 * Remove all the HTML tags.
	 */
	while(1)
	{
		p = memchr(savep, '<', (tail - savep));

		if (!p)
			break;

		while (*p == '<')
		{
			savep = p;
			p = memchr(savep, '>', (tail - savep));

			++p;

			range = (p - savep);
#ifdef DEBUG
			printf("removing %.*s\n", (int)range, savep);
#endif
			buf_collapse(&content_buf, (off_t)(savep - content_buf.buf_head), range);
			p = savep;
			tail = content_buf.buf_tail;
		}
	}

	/*
	 * Remove reference numbers (i.e., "[55]").
	 */
	p = savep = content_buf.buf_head;
	while(1)
	{
		p = strstr(savep, "&#91;");

		if (!p)
			break;

		savep = p;

		p = strstr(savep, "&#93;");

		if (!p)
			break;

		p += strlen("&#93;");

		range = (p - savep);
#ifdef DEBUG
		printf("removing %.*s\n", (int)range, savep);
#endif
		buf_collapse(&content_buf, (off_t)(savep - content_buf.buf_head), range);
		p = savep;
		tail = content_buf.buf_tail;
	}

	p = savep = content_buf.buf_head;
	tail = content_buf.buf_tail;
	while(1)
	{
		p = strstr(savep, "&#");

		if (!p)
			break;

		*p++ = 0x20;

		savep = p;

		p = memchr(savep, ';', (tail - savep));

		if (!p)
			break;

		++p;

		range = (p - savep);
#ifdef DEBUG
		printf("removing %.*s\n", (int)range, savep);
#endif
		buf_collapse(&content_buf, (off_t)(savep - content_buf.buf_head), range);
		p = savep;
		tail = content_buf.buf_tail;
	}

	p = savep = content_buf.buf_head;
	tail = content_buf.buf_tail;
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

		if ((*(q-1) == 0x2e)
		|| (*(q-1) == ')' && *(q-2) == 0x2e)
		|| (isdigit(*(savep-1)) && isdigit(*(savep+1)))
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
		printf("removing %.*s\n", (int)range, p);
#endif
		buf_collapse(&content_buf, (off_t)(p - content_buf.buf_head), range);
		savep = p;
		tail = content_buf.buf_tail;
	}

	p = savep = content_buf.buf_head;
	tail = content_buf.buf_tail;
	while(1)
	{
		p = memchr(savep, 0x7b, (tail - savep));

		if (!p)
			break;

		if (!isspace(*(p+1)))
		{
			if (!isspace(*(p-1)))
			{
				savep = p;

				while (!isspace(*p))
					--p;

				++p;

				q = memchr(savep, 0x20, (tail - savep));

				range = (q - p);
#ifdef DEBUG
				printf("removing %.*s\n", (int)range, p);
#endif
				buf_collapse(&content_buf, (off_t)(p - content_buf.buf_head), range);
				savep = q = p;
				tail = content_buf.buf_tail;
			}
			else
			{
				savep = p;

				p = memchr(savep, 0x20, (tail - savep));

				if (p)
				{
					range = (p - savep);
#ifdef DEBUG
					printf("removing %*.s\n", (int)range, savep);
#endif
					buf_collapse(&content_buf, (off_t)(savep - content_buf.buf_head), range);
					p = savep;
					tail = content_buf.buf_tail;
				}
			}
		}
		else
		{
			savep = ++p;
		}
	}

	p = savep = content_buf.buf_head;
	tail = content_buf.buf_tail;

	while(1)
	{
		p = memchr(savep, 0x20, (tail - savep));

		if (!p)
			break;

		savep = p;

		while (*p == 0x20)
			++p;

		range = (p - savep);

		if (range > 1)
		{
			++savep;

			range = (p - savep);
#ifdef DEBUG
			printf("removing %*s\n", (int)range, savep);
#endif
			buf_collapse(&content_buf, (off_t)(savep - content_buf.buf_head), range);
			p = savep;
			tail = content_buf.buf_tail;
		}
		else
		{
			savep = p;
		}
	}

	if (option_set(OPT_OUT_TTY))
		write(STDOUT_FILENO, content_buf.buf_head, content_buf.data_len);

	assert((content_buf.buf_tail - content_buf.buf_head) == content_buf.data_len);

	format_article(&content_buf);

	write(out_fd, content_buf.buf_head, content_buf.data_len);
	close(out_fd);
	out_fd = -1;

	buf_destroy(&content_buf);
	buf_destroy(&file_title);

	wiki_cache_dealloc(http_hcache, (void *)server);
	wiki_cache_dealloc(http_hcache, (void *)date);
	return 0;

	out_destroy_file:
	ftruncate(out_fd, (off_t)0);
	unlink(file_title.buf_head);

	out_destroy_bufs:
	buf_destroy(&content_buf);
	buf_destroy(&file_title);

	wiki_cache_dealloc(http_hcache, (void *)server);
	wiki_cache_dealloc(http_hcache, (void *)date);
	return -1;
}
