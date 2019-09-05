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
remove_braces(buf_t *buf)
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
	http_header_t *lastmod;
	size_t range;
	static char scratch_buf[DEFAULT_MAX_LINE_SIZE];
	char *home;

	server = (http_header_t *)wiki_cache_alloc(http_hcache);
	date = (http_header_t *)wiki_cache_alloc(http_hcache);
	lastmod = (http_header_t *)wiki_cache_alloc(http_hcache);

	assert(wiki_cache_obj_used(http_hcache, (void *)server));
	assert(wiki_cache_obj_used(http_hcache, (void *)date));
	assert(wiki_cache_obj_used(http_hcache, (void *)lastmod));

	buf_init(&content_buf, DEFAULT_TMP_BUF_SIZE);
	buf_init(&file_title, pathconf("/", _PC_PATH_MAX));

	home = getenv("HOME");
	buf_append(&file_title, home);
	buf_append(&file_title, WIKIGRAB_DIR);
	buf_append(&file_title, "/");

	RESET();
	GET_TAG_CONTENT("<title", p, savep, tail);

	if (p != savep)
	{
		q = saveq = content_buf.buf_head;

		q = strstr(saveq, " - ");

		if (q)
			buf_collapse(&content_buf, (off_t)(q - content_buf.buf_head), (content_buf.buf_tail - q));

		buf_append(&file_title, content_buf.buf_head);

		q = saveq = file_title.buf_head;

		while (q < file_title.buf_tail)
		{
			if (*q == 0x20
			|| (*q != 0x2f && *q != 0x5f && !isalpha(*q) && !isdigit(*q)))
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
	buf_append(&content_buf, "                   Downloaded via WikiGrab v");
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
	http_fetch_header(buf, "Last-Modified", lastmod, (off_t)0);

	if (date->value[0])
	{
		sprintf(scratch_buf, "%*s", LEFT_ALIGN_WIDTH, "Obtained: ");
		write(out_fd, scratch_buf, strlen(scratch_buf));
		date->value[date->vlen++] = 0x0a;
		date->value[date->vlen] = 0;
		write(out_fd, date->value, date->vlen);
	}

	if (lastmod->value[0])
	{
		sprintf(scratch_buf, "%*s", LEFT_ALIGN_WIDTH, "Last-modified: ");
		write(out_fd, scratch_buf, strlen(scratch_buf));
		lastmod->value[lastmod->vlen++] = 0x0a;
		lastmod->value[lastmod->vlen] = 0;
		write(out_fd, lastmod->value, lastmod->vlen);
	}

	if (server->value[0])
	{
		sprintf(scratch_buf, "%*s", LEFT_ALIGN_WIDTH, "Served-by: ");
		write(out_fd, scratch_buf, strlen(scratch_buf));
		server->value[server->vlen++] = 0x0a;
		server->value[server->vlen] = 0;
		write(out_fd, server->value, server->vlen);

		/*
		 * Get IP address(es) of server.
		 */
		struct addrinfo *ainf = NULL, *aip = NULL;
		struct sockaddr_in sock4;
		struct sockaddr_in6 sock6;
		int gotv4 = 0;
		int gotv6 = 0;

		clear_struct(&sock4);
		clear_struct(&sock6);
		server->value[--server->vlen] = 0;
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

		if (gotv4)
		{
			sprintf(scratch_buf, "%*s", LEFT_ALIGN_WIDTH, "Server-ip-v4: ");
			write(out_fd, scratch_buf, strlen(scratch_buf));
			sprintf(scratch_buf, "%s\n", inet_ntoa(sock4.sin_addr));
			write(out_fd, scratch_buf, strlen(scratch_buf));
			freeaddrinfo(ainf);
			ainf = aip = NULL;
		}

		if (gotv6)
		{
			static char inet6_string[INET6_ADDRSTRLEN];
			sprintf(scratch_buf, "%*s", LEFT_ALIGN_WIDTH, "Server-ip-v6: ");
			write(out_fd, scratch_buf, strlen(scratch_buf));
			inet_ntop(AF_INET6, (char *)sock6.sin6_addr.s6_addr, inet6_string, INET6_ADDRSTRLEN);
			sprintf(scratch_buf, "%s\n", inet6_string);
		}
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
 * BEGIN PARSING THE TEXT FROM THE ARTICLE.
 */

	remove_braces(buf);

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
		buf_append_ex(&content_buf, "\n\n", 2);

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

			if (!p)
				p = tail;
			else
				++p;

			range = (p - savep);
#ifdef DEBUG
			printf("removing LINE_BEGIN|%.*s|LINE_END\n", (int)range, savep);
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
			p = (savep + strlen("&#91;"));
		else
			p += strlen("&#93;");

		range = (p - savep);
#ifdef DEBUG
		printf("removing LINE_BEGIN|%.*s|LINE_END\n", (int)range, savep);
#endif
		buf_collapse(&content_buf, (off_t)(savep - content_buf.buf_head), range);
		tail = content_buf.buf_tail;
		p = savep;
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
			p = (savep + 1);
		else
			++p;

		range = (p - savep);
#ifdef DEBUG
		printf("removing LINE_BEGIN|%.*s|LINE_END\n", (int)range, savep);
#endif
		buf_collapse(&content_buf, (off_t)(savep - content_buf.buf_head), range);
		p = savep;
		tail = content_buf.buf_tail;
	}

	p = savep = content_buf.buf_head;
	tail = content_buf.buf_tail;

	buf_init(&copy_buf, DEFAULT_MAX_LINE_SIZE * 2);

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
		buf_collapse(&content_buf, (off_t)(p - content_buf.buf_head), range);
		savep = p;
		tail = content_buf.buf_tail;
	}

	buf_destroy(&copy_buf);

	p = savep = content_buf.buf_head;
	tail = content_buf.buf_tail;

	while(1)
	{
		if (savep >= tail)
			break;

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
			printf("removing LINE_BEGIN|%.*s|LINE_END\n", (int)range, savep);
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
		buf_write_fd(STDOUT_FILENO, &content_buf);

	assert((content_buf.buf_tail - content_buf.buf_head) == content_buf.data_len);

	format_article(&content_buf);

	buf_write_fd(out_fd, &content_buf);
	close(out_fd);
	out_fd = -1;

	wiki_cache_dealloc(http_hcache, (void *)server);
	wiki_cache_dealloc(http_hcache, (void *)date);
	wiki_cache_dealloc(http_hcache, (void *)lastmod);

	if (option_set(OPT_OPEN_FINISH))
	{
		pid_t child;

		child = fork();

		if (!child)
		{
			execlp("gedit", file_title.buf_head, (char *)0);
		}
	}

	buf_destroy(&content_buf);
	buf_destroy(&file_title);

	return 0;

	out_destroy_file:
	ftruncate(out_fd, (off_t)0);
	unlink(file_title.buf_head);

	out_destroy_bufs:
	buf_destroy(&content_buf);
	buf_destroy(&file_title);

	wiki_cache_dealloc(http_hcache, (void *)server);
	wiki_cache_dealloc(http_hcache, (void *)date);
	wiki_cache_dealloc(http_hcache, (void *)lastmod);
	return -1;
}
