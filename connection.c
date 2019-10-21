#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/conf.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "buffer.h"
#include "cache.h"
#include "connection.h"
#include "http.h"
#include "wikigrab.h"

static sigjmp_buf __timeout_env;
static struct sigaction oact;
static struct sigaction nact;

static void
handle_timeout(int signo)
{
	siglongjmp(__timeout_env, 1);
}

#define SET_TIMEOUT(s)\
do {\
	clear_struct(&oact);\
	clear_struct(&nact);\
	nact.sa_flags = 0;\
	nact.sa_handler = handle_timeout;\
	sigemptyset(&nact.sa_mask);\
	sigaction(SIGALRM, &nact, &oact);\
	alarm((s));\
} while (0)

#define RESET_TIMEOUT()\
do {\
	alarm(0);\
	sigaction(SIGALRM, &oact, NULL);\
} while(0)

void
conn_init(connection_t *conn)
{
	assert(conn);

	clear_struct(conn);
	conn->host = calloc(HTTP_URL_MAX+1, 1);
	conn->page = calloc(HTTP_URL_MAX+1, 1);

	return;
}

void
conn_destroy(connection_t *conn)
{
	assert(conn);

	free(conn->host);
	free(conn->page);
	clear_struct(conn);

	return;
}

inline int conn_socket(connection_t *conn)
{
	return conn->sock;
}

inline SSL *conn_tls(connection_t *conn)
{
	return conn->ssl;
}

#if 0
inline int conn_using_tls(connection_t *conn)
{
	return conn->using_tls;
}
#endif

/**
 * __init_openssl - initialise the openssl library
 */
static inline void
__init_openssl(void)
{
	SSL_library_init();
	SSL_load_error_strings();
	OpenSSL_add_all_algorithms();
	OPENSSL_config(NULL);
	ERR_load_crypto_strings();
}

/**
 * open_connection - set up a connection with the target site
 * @conn: &connection_t that is initialised in this function
 */
int
open_connection(connection_t *conn)
{
	assert(conn);

	struct sockaddr_in sock4;
	struct addrinfo *ainf = NULL;
	struct addrinfo *aip = NULL;

	if (buf_init(&conn->read_buf, HTTP_DEFAULT_READ_BUF_SIZE) < 0)
		goto fail;

	if (buf_init(&conn->write_buf, HTTP_DEFAULT_WRITE_BUF_SIZE) < 0)
		goto fail_release_bufs;

	clear_struct(&sock4);

#ifdef DEBUG
	printf("Getting address of host \"%s\"\n", conn->host);
#endif

	if (getaddrinfo(conn->host, NULL, NULL, &ainf) < 0)
	{
		fprintf(stderr, "open_connection: getaddrinfo error (%s)\n", gai_strerror(errno));
		goto fail;
	}

	for (aip = ainf; aip; aip = aip->ai_next)
	{
		if (aip->ai_family == AF_INET && aip->ai_socktype == SOCK_STREAM)
		{
			memcpy(&sock4, aip->ai_addr, aip->ai_addrlen);
			break;
		}
	}

	if (!aip)
	{
		fprintf(stderr, "open_connection: error obtaining remote host address\n");
		goto fail_release_ainf;
	}

	if (option_set(OPT_USE_TLS))
		sock4.sin_port = htons(HTTPS_PORT_NR);
	else
		sock4.sin_port = htons(HTTP_PORT_NR);

	if ((conn->sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		fprintf(stderr, "open_connection: connect error (%s)\n", strerror(errno));
		goto fail_release_ainf;
	}

	assert(conn->sock > 2);

	fprintf(stdout, "Connecting to %s...\n", inet_ntoa(sock4.sin_addr));


	SET_TIMEOUT(4);
	if (sigsetjmp(__timeout_env, 1) != 0)
	{
		fprintf(stderr, "Operation timed out\n");
		shutdown(conn->sock, SHUT_RDWR);
	}

	if (connect(conn->sock, (struct sockaddr *)&sock4, (socklen_t)sizeof(sock4)) != 0)
	{
		fprintf(stderr, "open_connection: connect error (%s)\n", strerror(errno));
		goto fail_release_ainf;
	}

	if (option_set(OPT_USE_TLS))
	{
		__init_openssl();

		if (!(conn->ssl_ctx = SSL_CTX_new(TLSv1_2_client_method())))
		{
			fprintf(stderr, "open_connection: SSL_CTX_new error\n");
			goto fail_release_ainf;
		}

		if (!(conn->ssl = SSL_new(conn->ssl_ctx)))
		{
			fprintf(stderr, "open_connection: SSL_new error\n");
			goto fail_free_ssl_ctx;
		}

		SSL_set_fd(conn->ssl, conn->sock); /* Set the socket for reading/writing */
		SSL_set_connect_state(conn->ssl); /* Set as client */
	}

	RESET_TIMEOUT();

	fprintf(stdout, "Connected \x1b[38;5;10m✓\x1b[m\n");

	freeaddrinfo(ainf);
	return 0;

	fail_release_bufs:
	buf_destroy(&conn->read_buf);
	buf_destroy(&conn->write_buf);

	fail_free_ssl_ctx:
	SSL_CTX_free(conn->ssl_ctx);
	conn->ssl_ctx = NULL;

	fail_release_ainf:
	freeaddrinfo(ainf);

	fail:
	return -1;
}

void
close_connection(connection_t *conn)
{
	assert(conn);

	shutdown(conn->sock, SHUT_RDWR);
	close(conn->sock);
	conn->sock = -1;

	if (option_set(OPT_USE_TLS))
	{
		SSL_CTX_free(conn->ssl_ctx);
		conn->ssl_ctx = NULL;
		SSL_free(conn->ssl);
		conn->ssl = NULL;
	}

	buf_destroy(&conn->read_buf);
	buf_destroy(&conn->write_buf);

	return;
}

int
conn_switch_to_tls(connection_t *conn)
{
	close_connection(conn);

	size_t host_len = strlen(conn->host);
	char *p;
	char *endp = (conn->host + host_len);

	p = conn->host;

	if (strstr(p, "http"))
	{
		p = memchr(conn->host, '/', endp - conn->host);

		if (*p != '/')
			goto fail;

		p += 2;
	}

#ifdef DEBUG
	printf("Switching to TLS (%s)\n", conn->host);
#endif

	set_option(OPT_USE_TLS);

	if (open_connection(conn) < 0)
		goto fail;

#ifdef DEBUG
	printf("Securely connected to host\n");
#endif

	return 0;

	fail:
	return -1;
}
