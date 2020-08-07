#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "buffer.h"
#include "cache.h"
#include "connection.h"
#include "http.h"
#include "parse.h"
#include "wikigrab.h"

#define DEFAULT_PROG_NAME_MAX		512

#define __noret __attribute__((noreturn))
#define __ctor __attribute__((constructor))
#define __dtor __attribute__((destructor))

static char PROG_NAME[DEFAULT_PROG_NAME_MAX];

wiki_cache_t *http_hcache;
static http_header_t *cookie;

int SOCK_SET_FLAG_ONCE;
int SOCK_SSL_SET_FLAG_ONCE;

static void
__noret usage(int status)
{
	printf(
			"%s <link> [options]\n\n"
			"-Q              show HTTP request header(s)\n"
			"-S              show HTTP response headers(s)\n"
			"--open/-O       open article in text editor when done\n"
			"--txt           format article in plain text file (default)\n"
			"--xml           format article in XML\n"
			"--print/-P      print the parsed article to stdout\n"
			"--help/-h       display this information\n",
			PROG_NAME);

	exit(status);
}

static void
cache_cleanup(void)
{
	wiki_cache_clear_all(http_hcache);
	wiki_cache_destroy(http_hcache);
}

static void
get_runtime_options(int argc, char *argv[])
{
	int i;

	for (i = 1; i < argc; ++i)
	{
		if (!strcmp("--help", argv[i])
		|| !strcmp("-h", argv[i]))
		{
			usage(EXIT_SUCCESS);
		}
		else
		if (!strcmp("--xml", argv[i]))
		{
			set_option(OPT_FORMAT_XML);
			unset_option(OPT_FORMAT_TXT);
		}
		else
		if (!strcmp("--txt", argv[i]))
		{
			set_option(OPT_FORMAT_TXT);
			unset_option(OPT_FORMAT_XML);
		}
		else
		if (!strcmp("--open", argv[i])
		|| !strcmp("-O", argv[i]))
		{
			set_option(OPT_OPEN_FINISH);
		}
		else
		if (!strcmp("-Q", argv[i]))
		{
			set_option(OPT_REQ_HEADER);
		}
		else
		if (!strcmp("-S", argv[i]))
		{
			set_option(OPT_RES_HEADER);
		}
		else
		if (!strcmp("--print", argv[i])
		|| !strcmp("-P", argv[i]))
		{
			set_option(OPT_OUT_TTY);
		}
		else
		{
			continue;
		}
	}

	/*
	 * If none set, use default of txt
	 */
	if (!option_set(OPT_FORMAT_TXT|OPT_FORMAT_XML))
		set_option(OPT_FORMAT_TXT);
}

static int
check_wikigrab_dir(void)
{
	char *home;
	buf_t tmp_buf;

	home = getenv("HOME");

	if (buf_init(&tmp_buf, pathconf("/", _PC_PATH_MAX)) < 0)
		goto fail;
		
	buf_append(&tmp_buf, home);
	buf_append(&tmp_buf, WIKIGRAB_DIR);

	if (access(tmp_buf.buf_head, F_OK) != 0)
	{
		mkdir(tmp_buf.buf_head, S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH);
	}

	buf_destroy(&tmp_buf);
	return 0;

	fail:
	return -1;
}

int
main(int argc, char *argv[])
{
	memset(PROG_NAME, 0, DEFAULT_PROG_NAME_MAX);
	strncpy(PROG_NAME, argv[0], DEFAULT_PROG_NAME_MAX);

	if (argc < 2)
		usage(EXIT_FAILURE);

	SOCK_SET_FLAG_ONCE = 0;
	SOCK_SSL_SET_FLAG_ONCE = 0;

	get_runtime_options(argc, argv);

	if (!strstr(argv[1], "/wiki/"))
	{
		fprintf(stderr,
			"Not a wiki link!\n\n");
		usage(EXIT_FAILURE);
	}

#if 0
	printf(
		"________________________________________\n"
		"\n"
		"          WikiGrab v%s\n"
		"\n"
		" Written by Gary Hannah\n"
		"________________________________________\n\n",
		WIKIGRAB_BUILD);
#endif

	if (check_wikigrab_dir() < 0)
		goto fail;

	struct http_t *http = NULL;
	off_t off;
	int code;
	int exit_ret = EXIT_SUCCESS;
	int host_max;

	host_max = sysconf(_SC_HOST_NAME_MAX);

	http = HTTP_new(0xdeadbeef);
	assert(http);

	http->usingSecure = 1; // use TLS
	http->followRedirects = 1; // automatically follow 3xx status codes
	http->verb = GET;

	http->URL_parse_host(argv[1], http->host);
	http->URL_parse_page(argv[1], http->page);

	if (-1 == http_connect(http))
		goto fail;

	http->send_request(http);
	code = http->recv_response(http);

	if (HTTP_OK != code)
		goto fail_disconnect;

	exit_ret = extract_wiki_article(http_rbuf(http));
	if (exit_ret < 0)
	{
		printf("main: extract_wiki_article error\n");
		goto fail_disconnect;
	}

	http_disconnect(http);
	HTTP_delete(http);
	exit(EXIT_SUCCESS);

fail_disconnect:

	fprintf(stderr, "Disconnecting from remote server\n");
	http_disconnect(http);
	HTTP_delete(http);

fail:
	if (http)
	{
		http_disconnect(http);
		HTTP_delete(http);
	}

	exit(EXIT_FAILURE);
}
