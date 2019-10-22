#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "buffer.h"
#include "cache.h"
#include "connection.h"
#include "http.h"
#include "wikigrab.h"

/**
 * wiki_cache_http_cookie_ctor - initialise object for the cookie cache
 * @hh: pointer to the object in the cache
 *  -- called in wiki_cache_create()
 */
int
wiki_cache_http_cookie_ctor(void *hh)
{
	http_header_t *ch = (http_header_t *)hh;
	clear_struct(ch);

	ch->name = calloc(HTTP_HNAME_MAX+1, 1);
	ch->value = calloc(HTTP_COOKIE_MAX+1, 1);
	ch->nsize = HTTP_HNAME_MAX+1;
	ch->vsize = HTTP_COOKIE_MAX+1;

	assert(ch->name);
	assert(ch->value);

	return 0;
}

/**
 * wiki_cache_http_cookie_dtor - deconstruct objects before destroying cache
 * @hh: pointer to object in cache
 * -- called in wiki_cache_dealloc()
 */
void
wiki_cache_http_cookie_dtor(void *hh)
{
	assert(hh);

	http_header_t *ch = (http_header_t *)hh;

	free(ch->name);
	ch->name = NULL;
	free(ch->value);
	ch->value = NULL;

	return;
}

static ssize_t
__read_bytes(connection_t *conn, size_t toread)
{
	assert(conn);
	assert(toread > 0);

	ssize_t n;
	SSL *ssl = conn->ssl;
	size_t r = toread;
	buf_t *buf = &conn->read_buf;

	while (r)
	{
		n = buf_read_tls(ssl, buf, r);

		if (n < 0)
			return -1;
		else
		if (!n)
			continue;
		else
			r -= n;
	}

	return toread;
}

#define HTTP_MAX_CHUNK_STR 10

static void
__http_read_until_next_chunk_size(connection_t *conn, buf_t *buf, char **cur_pos)
{
	assert(conn);
	assert(buf);
	assert(cur_pos);
	assert(buf_integrity(buf));

	off_t cur_pos_off = (*cur_pos - buf->buf_head);
	char *q;
	char *tail = buf->buf_tail;

	if (*cur_pos < tail)
	{
		if (**cur_pos == 0x0d)
		{
			while ((**cur_pos == 0x0d || **cur_pos == 0x0a) && *cur_pos < tail)
				++(*cur_pos);

			if (*cur_pos != tail)
			{
				q = *cur_pos;

				while (*q != 0x0d && q < tail)
					++q;

				if (q != tail)
				{
					*cur_pos -= 2;
					return;
				}
			}
		}
	}

	__read_bytes(conn, 2);
	*cur_pos = (buf->buf_head + cur_pos_off);
	tail = buf->buf_tail;
	*cur_pos += 2;
	cur_pos_off += 2;

	while (1)
	{
		__read_bytes(conn, 1);
		tail = buf->buf_tail;
		*cur_pos = (buf->buf_head + cur_pos_off);
		q = memchr(*cur_pos, 0x0a, (tail - *cur_pos));
		if (q)
		{
			*cur_pos -= 2; /* point it back to START_OF_CHUNK_DATA + CHUNK_SIZE */
			break;
		}
	}

	return;
}

#define SKIP_CRNL(p)\
do {\
	while ((*p) == 0x0a || (*p) == 0x0d)\
		++(p);\
} while (0)

static size_t
__http_do_chunked_recv(connection_t *conn)
{
	assert(conn);

	char *p;
	char *e;
	off_t chunk_offset;
	buf_t *buf = &conn->read_buf;
	size_t chunk_size;
	size_t save_size;
	size_t overread;
	size_t range;
	char *t;
	static char tmp[HTTP_MAX_CHUNK_STR];

	p = HTTP_EOH(buf);

	while (!p)
	{
		__read_bytes(conn, 1);
		p = HTTP_EOH(buf);
	}

	if (!p)
	{
		fprintf(stderr, "__http_do_chunked_recv: failed to find end of header sentinel\n");
		return -1;
	}

	__http_read_until_next_chunk_size(conn, buf, &p);

	while (1)
	{
		t = p;
		SKIP_CRNL(p);

		range = (p - t);
		if (range)
		{
			buf_collapse(buf, (off_t)(t - buf->buf_head), range);
			p = t;
		}

		e = memchr(p, 0x0d, HTTP_MAX_CHUNK_STR);

		if (!e)
		{
			fprintf(stderr, "__http_do_chunked_recv: failed to find next carriage return\n");

			return -1;
		}

		strncpy(tmp, p, (e - p));
		tmp[e - p] = 0;

		chunk_size = strtoul(tmp, NULL, 16);

		if (!chunk_size)
		{
			--p;
			buf_collapse(buf, (off_t)(p - buf->buf_head), (buf->buf_tail - p));
			break;
		}

		save_size = chunk_size;

		e += 2; /* Skip the \r\n do NOT use SKIP_CRNL(); chunk data could start with these bytes */

		buf_collapse(buf, (off_t)(p - buf->buf_head), (e - p));
		e = p;

		chunk_offset = (e - buf->buf_head);

		overread = (buf->buf_tail - e);

		if (overread >= chunk_size)
		{
			p = (e + save_size);
			__http_read_until_next_chunk_size(conn, buf, &p);
		}
		else
		{
			chunk_size -= overread;
		}

		__read_bytes(conn, chunk_size);

		p = (buf->buf_head + chunk_offset + save_size);
		__http_read_until_next_chunk_size(conn, buf, &p);

#if 0
/*
 * BS=BUF_START ; CS=CHUNK_START ; CE=CHUNK_END ; b=byte
 *
 * |BSbbbbbbbbbbCSbbbbbbbbbbbbbbbbbbbbbbbbbCE\r\n5a8\r\n......
 *                                               ^
 *                                             __next_size
 * This is absolutey where __next_size should be pointing after
 * the below... Something is very wrong if the assertions fail.
 *
 * EDIT:
 * Assertion *(__next_size - 2) == '\r' was failing in a certain
 * case after the final chunk, jumping forward from buf_head
 * chunk_offset + save_size + 2, was pointing ONE byte past the
 * 30 byte: 0d0a0d0a300d0a
 *                    ^
 * To solve this, don't jump forward the extra 2 bytes, and then
 * use SKIP_CRNL to land on the start of the next size string.
 *
 */
#endif
	}

	return 0;
}

int
http_build_request_header(connection_t *conn, const char *http_verb, const char *target)
{
	assert(conn);
	assert(http_verb);
	assert(target);

	buf_t *buf = &conn->write_buf;
	buf_t tbuf;
	static char header_buf[4096];

	if (buf_init(&tbuf, HTTP_URL_MAX) < 0)
		goto fail;

	buf_append(&tbuf, conn->host);

	if (*(tbuf.buf_tail - 1) == '/')
		buf_snip(&tbuf, 1);

/*
 * RFC 7230:
 *
 * HTTP-message = start-line
 *                *( header-field CRLF )
 *                CRLF
 *                [ message body ]
 *
 * start-line = request-line / status-line
 *
 * request-line = method SP request-target SP HTTP-version CRLF
 *
 * Reasons that a server returns a 400 Bad Request:
 *
 * Illegal whitespace between start-line and the first header-field
 * Illegal whitespace between field-name and ":"
 * Usage of deprecated obs-fold rule
 *
 * In the case of an invalid request line, a server can either
 * send a 400 Bad Request or a 301 Moved Permanently with the
 * correct encoding present in the Location header.
 */
	sprintf(header_buf,
			"%s %s HTTP/%s\r\n"
			"User-Agent: %s\r\n"
			"Accept: %s\r\n"
			"Host: %s\r\n"
			"Connection: keep-alive%s",
			http_verb, target, HTTP_VERSION,
			HTTP_USER_AGENT,
			HTTP_ACCEPT,
			tbuf.buf_head,
			HTTP_EOH_SENTINEL);

	buf_append(buf, header_buf);
	buf_destroy(&tbuf);

	return 0;

	fail:
	return -1;
}

int
http_send_request(connection_t *conn)
{
	assert(conn);

	buf_t *buf = &conn->write_buf;

	if (buf_write_tls(conn_tls(conn), buf) == -1)
		goto fail;

	return 0;

	fail:
	return -1;
}

int
http_recv_response(connection_t *conn)
{
	assert(conn);

	char *p = NULL;
	size_t clen;
	size_t header_len;
	size_t deduct;
	http_header_t *transfer_enc = NULL;
	http_header_t *content_len = NULL;

	content_len = wiki_cache_alloc(http_hcache, &content_len);
	if (!content_len)
		goto fail;

	transfer_enc = wiki_cache_alloc(http_hcache, &transfer_enc);
	if (!transfer_enc)
		goto fail_dealloc;

	while (!p)
	{
		buf_read_tls(conn->ssl, &conn->read_buf, 256);

		p = strstr(conn->read_buf.buf_head, HTTP_EOH_SENTINEL);
	}

	p += strlen(HTTP_EOH_SENTINEL);

	if (http_fetch_header(&conn->read_buf, "Transfer-Encoding", transfer_enc, (off_t)0))
	{
		if (!strncmp("chunked", transfer_enc->value, transfer_enc->vlen))
		{
			if (__http_do_chunked_recv(conn) == -1)
				goto fail_dealloc;

			goto out_dealloc;
		}
	}

	if (http_fetch_header(&conn->read_buf, "Content-Length", content_len, (off_t)0))
	{
		clen = strtoul(content_len->value, NULL, 0);
		header_len = (p - conn->read_buf.buf_head);
		deduct = (conn->read_buf.data_len - header_len);

		clen -= deduct;

		ssize_t n = 0;

		while (clen)
		{
			n = buf_read_tls(conn->ssl, &conn->read_buf, clen);

			if (n < 0)
				goto fail_dealloc;

			clen -= n;
		}
	}
	else
	{
		clen = 0;

		buf_read_tls(conn->ssl, &conn->read_buf, clen);
	}

	assert(conn->read_buf.magic == BUFFER_MAGIC);

	out_dealloc:
	if (content_len)
		wiki_cache_dealloc(http_hcache, content_len, &content_len);
	if (transfer_enc)
		wiki_cache_dealloc(http_hcache, transfer_enc, &transfer_enc);

	return 0;

	fail_dealloc:
	if (content_len)
		wiki_cache_dealloc(http_hcache, content_len, &content_len);
	if (transfer_enc)
		wiki_cache_dealloc(http_hcache, transfer_enc, &transfer_enc);

	fail:
	return -1;
}

int
http_status_code_int(buf_t *buf)
{
	assert(buf);

	char *p = buf->data;
	char *q = NULL;
	char *tail = buf->buf_tail;
	char *head = buf->buf_head;
	static char code_str[16];
	//size_t data_len = buf->data_len;

	/*
	 * HTTP/1.1 200 OK\r\n
	 */

	if (!buf_integrity(buf))
		return -1;

	p = memchr(head, 0x20, (tail - head));
	if (!p)
		return -1;

	++p;

	q = memchr(p, 0x20, (tail - p));
	if (!q)
		return -1;

	strncpy(code_str, p, (q - p));
	code_str[q - p] = 0;

	return atoi(code_str);
}

const char *
http_status_code_string(int code)
{
	switch(code)
	{
		case HTTP_OK:
			return "OK";
			break;
		case HTTP_MOVED_PERMANENTLY:
			return "Moved permanently";
			break;
		case HTTP_FOUND:
			return "Found";
			break;
		case HTTP_BAD_REQUEST:
			return "Bad request";
			break;
		case HTTP_UNAUTHORISED:
			return "Unauthorised";
			break;
		case HTTP_FORBIDDEN:
			return "Forbidden";
			break;
		case HTTP_NOT_FOUND:
			return "Not found";
			break;
		case HTTP_REQUEST_TIMEOUT:
			return "Request timeout";
			break;
		case HTTP_INTERNAL_ERROR:
			return "Internal server error";
			break;
		case HTTP_BAD_GATEWAY:
			return "Bad gateway";
			break;
		case HTTP_SERVICE_UNAV:
			return "Service unavailable";
			break;
		default:
			return "Unknown http status code";
	}
}

ssize_t
http_response_header_len(buf_t *buf)
{
	assert(buf);

	char	*p = buf->data;
	char	*q = NULL;

	if (!buf_integrity(buf))
		return -1;

	q = strstr(p, HTTP_EOH_SENTINEL);

	if (!q)
		return -1;

	q += strlen(HTTP_EOH_SENTINEL);

	return (q - p);
}

char *
http_parse_host(char *url, char *host)
{
	char *p = url;
	//char *q;
	size_t url_len = strlen(url);
	char *endp = (url + url_len);

	p = url;

	if (!strncmp("http:", url, strlen("http:")))
		p += strlen("http://");
	else
	if (!strncmp("https:", url, strlen("https:")))
		p += strlen("https://");

	endp = memchr(p, '/', ((url + url_len) - p));
	if (!endp)
		endp = url + url_len;

	strncpy(host, p, endp - p);
	host[endp - p] = 0;

	return host;
}

char *
http_parse_page(char *url, char *page)
{
	char *p;
	char *q;
	size_t url_len = strlen(url);
	char *endp = (url + url_len);

	p = url;
	q = endp;

	if (*(endp - 1) == '/')
	{
		--endp;
		*endp = 0;
	}

	if (!strncmp("http:", url, 5))
		p += strlen("http://");
	else
	if (!strncmp("https:", url, 6))
		p += strlen("https://");

	q = memchr(p, '/', (endp - p));

	if (!q)
	{
		strncpy(page, "/", 1);
		page[1] = 0;
		return page;
	}

	strncpy(page, q, (endp - q));
	page[endp - q] = 0;

	return page;
}

/**
 * http_check_header - check existence of header
 * @buf: buffer containing header
 * @name: name of the header
 * @off: the offset from within the header to start search
 * @ret_off: offset where header found returned here
 */
int
http_check_header(buf_t *buf, const char *name, off_t off, off_t *ret_off)
{
	assert(buf);
	assert(name);

	char *check_from = buf->buf_head + off;
	char *p;

	if ((p = strstr(check_from, name)))
	{
		*ret_off = (off_t)(p - buf->buf_head);
		return 1;
	}
	else
	{
		return 0;
	}
}

/**
 * http_get_header - find and return a line in an HTTP header
 * @buf: the buffer containing the HTTP header
 * @name: the name of the header (e.g., "Set-Cookie")
 */
char *
http_fetch_header(buf_t *buf, const char *name, http_header_t *hh, off_t whence)
{
	assert(buf);
	assert(name);
	assert(hh);
	assert(hh->name);
	assert(hh->value);

	char *check_from = buf->buf_head + whence;
	char *tail = buf->buf_tail;
	char *p;
	char *q;
	char *hend;

	p = strstr(check_from, name);

	if (!p)
		return NULL;

	hend = strstr(check_from, HTTP_EOH_SENTINEL);
	if (!hend)
	{
		fprintf(stderr,
				"http_get_header: failed to find end of header sentinel\n");
		errno = EPROTO;
		goto out_clear_ret;
	}

	q = memchr(p, ':', (tail - p));
	if (!q)
		return NULL;

	if (!strncmp("Set-Cookie", p, q - p))
	{
		size_t _nlen = strlen("Cookie");
		strncpy(hh->name, "Cookie", _nlen);
		hh->name[_nlen] = 0;
		hh->nlen = _nlen;;
	}
	else
	{
		strncpy(hh->name, p, (q - p));
		hh->name[q - p] = 0;
		hh->nlen = (q - p);
	}

	p = (q + 2);
	if (*(p-1) != ' ')
		--p;

	q = memchr(p, 0x0d, (tail - p));
	if (!q)
		goto out_clear_ret;

	strncpy(hh->value, p, (q - p));
	hh->value[q - p] = 0;
	hh->vlen = (q - p);

	return hh->value;

	out_clear_ret:
	memset(hh->name, 0, hh->nsize);
	memset(hh->value, 0, hh->vsize);
	hh->nlen = 0;
	hh->vlen = 0;

	//fail:
	return NULL;
}

int
http_append_header(buf_t *buf, http_header_t *hh)
{
	assert(buf);
	assert(hh);

	char *p;
	char *head = buf->buf_head;

	p = strstr(head, HTTP_EOH_SENTINEL);

	if (!p)
	{
		fprintf(stderr,
				"http_append_header: failed to find end of header sentinel\n");
		errno = EPROTO;
		return -1;
	}

	p += 2;

	buf_t tmp;

	if (buf_init(&tmp, HTTP_COOKIE_MAX+strlen("Cookie: ")) < 0)
		goto fail;

	buf_append(&tmp, hh->name);
	buf_append(&tmp, ": ");
	buf_append(&tmp, hh->value);
	buf_append(&tmp, "\r\n");

	buf_shift(buf, (off_t)(p - head), tmp.data_len);

	strncpy(p, tmp.buf_head, tmp.data_len);

	buf_destroy(&tmp);

	return 0;

	fail:
	return -1;
}

static http_header_t *generic_hp;

int
http_parse_header(buf_t *buf, wiki_cache_t *cachep)
{
	assert(buf);
	assert(cachep);

	char *p;
	char *savep;
	char *line;
	char *tail = buf->buf_tail;
	char *head = buf->buf_head;
	size_t len;

	p = head;

	while (p < tail)
	{
		savep = p;

		line = memchr(savep, 0x0d, tail - savep);

		if (!line)
			break;

		p = memchr(savep, ':', line - savep);

		if (!p)
			break;

		generic_hp = (http_header_t *)wiki_cache_alloc(cachep, &generic_hp);
		assert(wiki_cache_obj_used(cachep, (void *)generic_hp));

		len = (p - savep);
		strncpy(generic_hp->name, savep, len);
		generic_hp->name[len] = 0;
		generic_hp->nlen = len;

		++p;

		while ((*p == ' ' || *p == '\t') && p < tail)
			++p;

		if (p == tail)
			break;

		savep = p;

		len = (line - p);
		strncpy(generic_hp->value, p, len);
		generic_hp->value[len] = 0;
		generic_hp->vlen = len;

		while ((*p == 0x0a || *p == 0x0d) && p < tail)
			++p;

		if (p == tail)
			break;
	}

	return 0;
}

#if 0
int
http_state_add_cookies(http_state_t *state, char *cookies)
{
	assert(state);

	int i;
	int nr_cookies = state->nr_cookies;

	if (nr_cookies)
	{
		for (i = 0; i < nr_cookies; ++i)
			free(state->cookies[i]);

		free(state->cookies);
	}
}
#endif
