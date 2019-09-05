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

#if 0
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
#endif

#define RESET() (p = savep = buf->buf_head)

static char tag_content[8192];

char *
get_tag_content(buf_t *buf, const char *tag)
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

char *
get_tag_field(buf_t *buf, const char *tag, const char *field)
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

void
normalise_file_title(buf_t *buf)
{
	assert(buf);

	char *tail = buf->buf_tail;
	char *p = buf->buf_head;

	p = strstr(buf->buf_head, " - Wikipedia");
	if (p)
		buf_collapse(buf, (off_t)(p - buf->buf_head), (buf->buf_tail - p));

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
	char *tail = buf->buf_tail;
	char *p;
	char *q;
	char *savep;
	//char *saveq;
	buf_t file_title;
	buf_t content_buf;
	buf_t tmp_buf;
	buf_t copy_buf;
	http_header_t *server;
	http_header_t *date;
	http_header_t *lastmod;
	size_t range;
	//static char scratch_buf[DEFAULT_MAX_LINE_SIZE];
	static char inet6_string[INET6_ADDRSTRLEN];
	char *large_buffer = NULL;
	char *home;
	struct sockaddr_in sock4;
	struct sockaddr_in6 sock6;
	struct addrinfo *ainf = NULL;
	struct addrinfo *aip = NULL;
	//int gotv4 = 0;
	int gotv6 = 0;

	large_buffer = calloc(8192, 1);

	server = (http_header_t *)wiki_cache_alloc(http_hcache);
	date = (http_header_t *)wiki_cache_alloc(http_hcache);
	lastmod = (http_header_t *)wiki_cache_alloc(http_hcache);

	assert(wiki_cache_obj_used(http_hcache, (void *)server));
	assert(wiki_cache_obj_used(http_hcache, (void *)date));
	assert(wiki_cache_obj_used(http_hcache, (void *)lastmod));

	buf_init(&content_buf, DEFAULT_TMP_BUF_SIZE);
	buf_init(&tmp_buf, DEFAULT_TMP_BUF_SIZE);
	buf_init(&file_title, pathconf("/", _PC_PATH_MAX));

	home = getenv("HOME");
	buf_append(&file_title, home);
	buf_append(&file_title, WIKIGRAB_DIR);
	buf_append(&file_title, "/");

	get_tag_content(buf, "<title");
	buf_append(&file_title, tag_content);
	buf_append(&tmp_buf, tag_content);
	normalise_file_title(&file_title);

	if ((out_fd = open(file_title.buf_head, O_RDWR|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR)) < 0)
		goto out_destroy_bufs;

	http_fetch_header(buf, "Server", server, (off_t)0);
	http_fetch_header(buf, "Date", date, (off_t)0);
	http_fetch_header(buf, "Last-Modified", lastmod, (off_t)0);

	if (server->value[0])
	{
		/*
		 * Get IP address(es) of server.
		 */

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
					//gotv4 = 1;
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

	if (gotv6)
		inet_ntop(AF_INET6, sock6.sin6_addr.s6_addr, inet6_string, INET6_ADDRSTRLEN);
	else
		inet6_string[0] = 0;

	if (option_set(OPT_FORMAT_XML))
	{
		sprintf(large_buffer,
			"<?xml version=\"1.0\" ?>\n"
			"<wiki>\n"
			"<metadata>\n"
			"<meta name=\"Title\" content=\"%s\"/>\n"
			"<meta name=\"Parser\" content=\"WikiGrab v%s\"/>\n"
			"<meta name=\"Server\" content=\"%s\"/>\n"
			"<meta name=\"Server-ipv4\" content=\"%s\"/>\n"
			"<meta name=\"Server-ipv6\" content=\"%s\"/>\n"
			"<meta name=\"Modified\" content=\"%s\"/>\n"
			"<meta name=\"Downloaded\" content=\"%s\"/>\n"
			"</metadata>",
			tmp_buf.buf_head,
			WIKIGRAB_BUILD,
			server->value,
			inet_ntoa(sock4.sin_addr),
			inet6_string[0] ? inet6_string : "none",
			lastmod->value,
			date->value);
	}
	else
	if (option_set(OPT_FORMAT_TXT))
	{
		sprintf(large_buffer,
			"      @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n\n"
			"                    Downloaded via WikiGrab v%s\n\n"
			"      >>>>>>>>>>>>>>>>>>>>>>>>>>>>><<<<<<<<<<<<<<<<<<<<<<<<<<<<<\n\n"
			"%*s%s\n"
			"%*s%s\n"
			"%*s%s\n"
			"%*s%s\n"
			"%*s%s\n"
			"%*s%s\n\n"
			"      @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n\n",
			WIKIGRAB_BUILD,
			LEFT_ALIGN_WIDTH, "Title: ", tmp_buf.buf_head,
			LEFT_ALIGN_WIDTH, "Served-by: ", server->value,
			LEFT_ALIGN_WIDTH, "Server-ip-v4: ", inet_ntoa(sock4.sin_addr),
			LEFT_ALIGN_WIDTH, "Server-ip-v6: ", inet6_string,
			LEFT_ALIGN_WIDTH, "Last-modified: ", lastmod->value,
			LEFT_ALIGN_WIDTH, "Downloaded: ", date->value);
	}

	write(out_fd, large_buffer, strlen(large_buffer));
	buf_clear(&content_buf);


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

		buf_append(&content_buf, "BEGIN_PARA");
		buf_append_ex(&content_buf, savep, (p - savep));
		buf_append(&content_buf, "END_PARA");
		buf_append(&content_buf, "\n");

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

	if (option_set(OPT_FORMAT_XML))
	{
		buf_shift(&content_buf, (off_t)0, strlen("<text>\n"));
		strncpy(content_buf.buf_head, "<text>\n", strlen("<text>\n"));
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

	p = savep = content_buf.buf_head;
	while(1)
	{
		p = strstr(savep, "BEGIN_PARA");
		if (!p)
			break;

		strncpy(p, "<p>", 3);
		p += 3;
		savep = p;
		buf_collapse(&content_buf, (off_t)(p - content_buf.buf_head), (strlen("BEGIN_PARA") - 3));

		p = strstr(savep, "END_PARA");
		strncpy(p, "</p>\n\n", strlen("</p>\n\n"));
		p += strlen("</p>\n\n");
		savep = p;
		buf_collapse(&content_buf, (off_t)(p - content_buf.buf_head), (strlen("END_PARA") - strlen("</p>\n\n")));
	}

	if (option_set(OPT_OUT_TTY))
		buf_write_fd(STDOUT_FILENO, &content_buf);

	assert((content_buf.buf_tail - content_buf.buf_head) == content_buf.data_len);

	format_article(&content_buf);

	if (option_set(OPT_FORMAT_XML))
		buf_append(&content_buf, "</text>\n</wiki>\n");

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
	if (tmp_buf.data)
		buf_destroy(&tmp_buf);

	free(large_buffer);
	large_buffer = NULL;

	return 0;

	out_destroy_file:
	ftruncate(out_fd, (off_t)0);
	unlink(file_title.buf_head);

	out_destroy_bufs:
	buf_destroy(&content_buf);
	buf_destroy(&file_title);
	if (tmp_buf.data)
		buf_destroy(&tmp_buf);

	free(large_buffer);
	large_buffer = NULL;

	wiki_cache_dealloc(http_hcache, (void *)server);
	wiki_cache_dealloc(http_hcache, (void *)date);
	wiki_cache_dealloc(http_hcache, (void *)lastmod);
	return -1;
}
