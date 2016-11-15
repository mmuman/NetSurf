/*
 * Copyright 2006 Daniel Silverstone <dsilvers@digital-scurf.org>
 * Copyright 2007 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2003 Phil Mellor <monkeyson@users.sourceforge.net>
 *
 * This file is part of NetSurf.
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * implementation of fetching of data from http and https schemes.
 *
 * This implementation uses libcurl's 'multi' interface.
 *
 * The CURL handles are cached in the curl_handle_ring.
 */

/* must come first to ensure winsock2.h vs windows.h ordering issues */
#include "utils/inet.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>
#include <openssl/ssl.h>

#include <libwapcaplet/libwapcaplet.h>
#include <nsutils/time.h>

#include "utils/corestrings.h"
#include "utils/nsoption.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/utils.h"
#include "utils/ring.h"
#include "utils/useragent.h"
#include "utils/file.h"
#include "utils/string.h"
#include "netsurf/fetch.h"
#include "netsurf/misc.h"
#include "desktop/gui_internal.h"

#include "content/fetch.h"
#include "content/fetchers.h"
#include "content/fetchers/curl.h"
#include "content/gopher.h"
#include "content/urldb.h"

/** maximum number of progress notifications per second */
#define UPDATES_PER_SECOND 2

/** maximum number of X509 certificates in chain for TLS connection */
#define MAX_CERTS 10

/** SSL certificate info */
struct cert_info {
	X509 *cert;		/**< Pointer to certificate */
	long err;		/**< OpenSSL error code */
};

/** Information for a single fetch. */
struct curl_fetch_info {
	struct fetch *fetch_handle; /**< The fetch handle we're parented by. */
	CURL * curl_handle;	/**< cURL handle if being fetched, or 0. */
	bool had_headers;	/**< Headers have been processed. */
	bool abort;		/**< Abort requested. */
	bool stopped;		/**< Download stopped on purpose. */
	bool only_2xx;		/**< Only HTTP 2xx responses acceptable. */
	bool downgrade_tls;	/**< Downgrade to TLS <= 1.0 */
	nsurl *url;		/**< URL of this fetch. */
	lwc_string *host;	/**< The hostname of this fetch. */
	struct curl_slist *headers;	/**< List of request headers. */
	char *location;		/**< Response Location header, or 0. */
	unsigned long content_length;	/**< Response Content-Length, or 0. */
	char *cookie_string;	/**< Cookie string for this fetch */
	char *realm;		/**< HTTP Auth Realm */
	char *post_urlenc;	/**< Url encoded POST string, or 0. */
	long http_code; /**< HTTP result code from cURL. */
	struct curl_httppost *post_multipart;	/**< Multipart post data, or 0. */
	uint64_t last_progress_update;	/**< Time of last progress update */
	int cert_depth; /**< deepest certificate in use */
	struct cert_info cert_data[MAX_CERTS];	/**< HTTPS certificate data */
	struct gopher_state *gopher;	/**< gopher-specific state */
};

/** curl handle cache entry */
struct cache_handle {
	CURL *handle; /**< The cached cURL handle */
	lwc_string *host; /**< The host for which this handle is cached */

	struct cache_handle *r_prev; /**< Previous cached handle in ring. */
	struct cache_handle *r_next; /**< Next cached handle in ring. */
};

/** Global cURL multi handle. */
CURLM *fetch_curl_multi;

/** Curl handle with default options set; not used for transfers. */
static CURL *fetch_blank_curl;

/** Ring of cached handles */
static struct cache_handle *curl_handle_ring = 0;

/** Count of how many schemes the curl fetcher is handling */
static int curl_fetchers_registered = 0;

/** Flag for runtime detection of openssl usage */
static bool curl_with_openssl;

/** Error buffer for cURL. */
static char fetch_error_buffer[CURL_ERROR_SIZE];

/** Proxy authentication details. */
static char fetch_proxy_userpwd[100];


/**
 * Initialise a cURL fetcher.
 */
static bool fetch_curl_initialise(lwc_string *scheme)
{
	LOG("Initialise cURL fetcher for %s", lwc_string_data(scheme));
	curl_fetchers_registered++;
	return true; /* Always succeeds */
}


/**
 * Finalise a cURL fetcher.
 *
 * \param scheme The scheme to finalise.
 */
static void fetch_curl_finalise(lwc_string *scheme)
{
	struct cache_handle *h;

	curl_fetchers_registered--;
	LOG("Finalise cURL fetcher %s", lwc_string_data(scheme));
	if (curl_fetchers_registered == 0) {
		CURLMcode codem;
		/* All the fetchers have been finalised. */
		LOG("All cURL fetchers finalised, closing down cURL");

		curl_easy_cleanup(fetch_blank_curl);

		codem = curl_multi_cleanup(fetch_curl_multi);
		if (codem != CURLM_OK)
			LOG("curl_multi_cleanup failed: ignoring");

		curl_global_cleanup();
	}

	/* Free anything remaining in the cached curl handle ring */
	while (curl_handle_ring != NULL) {
		h = curl_handle_ring;
		RING_REMOVE(curl_handle_ring, h);
		lwc_string_unref(h->host);
		curl_easy_cleanup(h->handle);
		free(h);
	}
}


/**
 * Check if this fetcher can fetch a url.
 *
 * \param url The url to check.
 * \return true if the fetcher supports the url else false.
 */
static bool fetch_curl_can_fetch(const nsurl *url)
{
	return nsurl_has_component(url, NSURL_HOST);
}


/**
 * Convert a list of struct ::fetch_multipart_data to a list of
 * struct curl_httppost for libcurl.
 */
static struct curl_httppost *
fetch_curl_post_convert(const struct fetch_multipart_data *control)
{
	struct curl_httppost *post = 0, *last = 0;
	CURLFORMcode code;
	nserror ret;

	for (; control; control = control->next) {
		if (control->file) {
			char *leafname = NULL;
			ret = guit->file->basename(control->value, &leafname, NULL);
			if (ret != NSERROR_OK) {
				continue;
			}

			/* We have to special case filenames of "", so curl
			 * a) actually attempts the fetch and
			 * b) doesn't attempt to open the file ""
			 */
			if (control->value[0] == '\0') {
				/* dummy buffer - needs to be static so
				 * pointer's still valid when we go out
				 * of scope (not that libcurl should be
				 * attempting to access it, of course).
				 */
				static char buf;

				code = curl_formadd(&post, &last,
					CURLFORM_COPYNAME, control->name,
					CURLFORM_BUFFER, control->value,
					/* needed, as basename("") == "." */
					CURLFORM_FILENAME, "",
					CURLFORM_BUFFERPTR, &buf,
					CURLFORM_BUFFERLENGTH, 0,
					CURLFORM_CONTENTTYPE,
						"application/octet-stream",
					CURLFORM_END);
				if (code != CURL_FORMADD_OK)
					LOG("curl_formadd: %d (%s)", code, control->name);
			} else {
				char *mimetype = guit->fetch->mimetype(control->value);
				code = curl_formadd(&post, &last,
					CURLFORM_COPYNAME, control->name,
					CURLFORM_FILE, control->rawfile,
					CURLFORM_FILENAME, leafname,
					CURLFORM_CONTENTTYPE,
					(mimetype != 0 ? mimetype : "text/plain"),
					CURLFORM_END);
				if (code != CURL_FORMADD_OK)
					LOG("curl_formadd: %d (%s=%s)", code, control->name, control->value);
				free(mimetype);
			}
			free(leafname);
		}
		else {
			code = curl_formadd(&post, &last,
					CURLFORM_COPYNAME, control->name,
					CURLFORM_COPYCONTENTS, control->value,
					CURLFORM_END);
			if (code != CURL_FORMADD_OK)
				LOG("curl_formadd: %d (%s=%s)", code, control->name, control->value);
		}
	}

	return post;
}


/**
 * Start fetching data for the given URL.
 *
 * The function returns immediately. The fetch may be queued for later
 * processing.
 *
 * A pointer to an opaque struct curl_fetch_info is returned, which can be
 * passed to fetch_abort() to abort the fetch at any time. Returns 0 if memory
 * is exhausted (or some other fatal error occurred).
 *
 * The caller must supply a callback function which is called when anything
 * interesting happens. The callback function is first called with msg
 * FETCH_HEADER, with the header in data, then one or more times
 * with FETCH_DATA with some data for the url, and finally with
 * FETCH_FINISHED. Alternatively, FETCH_ERROR indicates an error occurred:
 * data contains an error message. FETCH_REDIRECT may replace the FETCH_HEADER,
 * FETCH_DATA, FETCH_FINISHED sequence if the server sends a replacement URL.
 *
 * Some private data can be passed as the last parameter to fetch_start, and
 * callbacks will contain this.
 */
static void *
fetch_curl_setup(struct fetch *parent_fetch,
		 nsurl *url,
		 bool only_2xx,
		 bool downgrade_tls,
		 const char *post_urlenc,
		 const struct fetch_multipart_data *post_multipart,
		 const char **headers)
{
	struct curl_fetch_info *fetch;
	struct curl_slist *slist;
	int i;

	fetch = malloc(sizeof (*fetch));
	if (fetch == NULL)
		return 0;

	fetch->fetch_handle = parent_fetch;

	LOG("fetch %p, url '%s'", fetch, nsurl_access(url));

	/* construct a new fetch structure */
	fetch->curl_handle = NULL;
	fetch->had_headers = false;
	fetch->abort = false;
	fetch->stopped = false;
	fetch->only_2xx = only_2xx;
	fetch->downgrade_tls = downgrade_tls;
	fetch->headers = NULL;
	fetch->url = nsurl_ref(url);
	fetch->host = nsurl_get_component(url, NSURL_HOST);
	fetch->location = NULL;
	fetch->content_length = 0;
	fetch->http_code = 0;
	fetch->cookie_string = NULL;
	fetch->realm = NULL;
	fetch->post_urlenc = NULL;
	fetch->post_multipart = NULL;
	if (post_urlenc) {
		fetch->post_urlenc = strdup(post_urlenc);
	} else if (post_multipart) {
		fetch->post_multipart = fetch_curl_post_convert(post_multipart);
	}
	fetch->last_progress_update = 0;
	fetch->gopher = NULL;

	/* TLS defaults */
	memset(fetch->cert_data, 0, sizeof(fetch->cert_data));
	fetch->cert_depth = -1;

	if ((fetch->host == NULL) ||
	    (post_multipart != NULL && fetch->post_multipart == NULL) ||
	    (post_urlenc != NULL && fetch->post_urlenc == NULL)) {
		goto failed;
	}

#define APPEND(list, value) \
	slist = curl_slist_append(list, value);		\
	if (slist == NULL)				\
		goto failed;				\
	list = slist;

	/* remove curl default headers */
	APPEND(fetch->headers, "Pragma:");

	/* when doing a POST libcurl sends Expect: 100-continue" by default
	 * which fails with lighttpd, so disable it (see bug 1429054) */
	APPEND(fetch->headers, "Expect:");

	if ((nsoption_charp(accept_language) != NULL) &&
	    (nsoption_charp(accept_language)[0] != '\0')) {
		char s[80];
		snprintf(s, sizeof s, "Accept-Language: %s, *;q=0.1",
			 nsoption_charp(accept_language));
		s[sizeof s - 1] = 0;
		APPEND(fetch->headers, s);
	}

	if (nsoption_charp(accept_charset) != NULL &&
	    nsoption_charp(accept_charset)[0] != '\0') {
		char s[80];
		snprintf(s, sizeof s, "Accept-Charset: %s, *;q=0.1",
			 nsoption_charp(accept_charset));
		s[sizeof s - 1] = 0;
		APPEND(fetch->headers, s);
	}

	if (nsoption_bool(do_not_track) == true) {
		APPEND(fetch->headers, "DNT: 1");
	}

	/* And add any headers specified by the caller */
	for (i = 0; headers[i] != NULL; i++) {
		APPEND(fetch->headers, headers[i]);
	}

	return fetch;

#undef APPEND

failed:
	if (fetch->host != NULL)
		lwc_string_unref(fetch->host);

	nsurl_unref(fetch->url);
	free(fetch->post_urlenc);
	if (fetch->post_multipart)
		curl_formfree(fetch->post_multipart);
	curl_slist_free_all(fetch->headers);
	free(fetch);
	return NULL;
}


void * fetch_curl_setup_gopher(struct fetch *parent_fetch, nsurl *url,
		 bool only_2xx, bool downgrade_tls, const char *post_urlenc,
		 const struct fetch_multipart_data *post_multipart,
		 const char **headers)
{
	struct curl_fetch_info *f;
	const char *mime;
	char type;

	f = fetch_curl_setup(parent_fetch, url, only_2xx, downgrade_tls,
		post_urlenc, post_multipart, headers);

	f->gopher = gopher_state_create(f->url, f->fetch_handle);
	if (f->gopher == NULL) {
		fetch_curl_free(f);
		return NULL;
	}

	if (url_gopher_type(nsurl_access(url), &f->gopher->type) != URL_FUNC_OK
			|| f->gopher->type == GOPHER_TYPE_NONE) {
		f->http_code = 404;
		fetch_set_http_code(f->fetch_handle, f->http_code);
		LOG(("fetch %p, gopher error for '%s'", f, nsurl_access(url)));
	}

	mime = gopher_type_to_mime(f->gopher->type);
	/* TODO: add a better API,
	 * fetch_filetype() is wrongly assuming unknown files to be HTML.
	 */
	if (mime == NULL)
		mime = fetch_filetype(nsurl_access(url));

	if (mime) {
		char s[80];
		fetch_msg msg;

		LOG(("fetch %p, gopher mime is '%s'", f, mime));
		snprintf(s, sizeof s, "Content-type: %s\r\n", mime);
		s[sizeof s - 1] = 0;
		msg.type = FETCH_HEADER;
		msg.data.header_or_data.buf = (const uint8_t *) s;
		msg.data.header_or_data.len = strlen(s);
		fetch_send_callback(&msg, f->fetch_handle);
	}

	return f;
}


/**
 * OpenSSL Certificate verification callback
 *
 * Called for each certificate in a chain being verified. OpenSSL
 * calls this in deepest first order from the certificate authority to
 * the peer certificate at position 0.
 *
 * Each certificate is stored in the fetch context the first time it
 * is presented. If an error is encountered it is only returned for
 * the peer certificate at position 0 allowing the enumeration of the
 * entire chain not stopping early at the depth of the erroring
 * certificate.
 *
 * \param verify_ok 0 if the caller has already determined the chain
 *                   has errors else 1
 * \param x509_ctx certificate context being verified
 * \return 1 to indicate verification should continue and 0 to indicate
 *          verification should stop.
 */
static int
fetch_curl_verify_callback(int verify_ok, X509_STORE_CTX *x509_ctx)
{
	int depth;
	struct curl_fetch_info *fetch;

	depth = X509_STORE_CTX_get_error_depth(x509_ctx);
	fetch = X509_STORE_CTX_get_app_data(x509_ctx);

	/* record the max depth */
	if (depth > fetch->cert_depth) {
		fetch->cert_depth = depth;
	}

	/* certificate chain is excessively deep so fail verification */
	if (depth >= MAX_CERTS) {
		X509_STORE_CTX_set_error(x509_ctx,
					 X509_V_ERR_CERT_CHAIN_TOO_LONG);
		return 0;
	}

	/* save the certificate by incrementing the reference count and
	 * keeping a pointer.
	 */
	if (!fetch->cert_data[depth].cert) {
		fetch->cert_data[depth].cert = X509_STORE_CTX_get_current_cert(x509_ctx);
		fetch->cert_data[depth].cert->references++;
		fetch->cert_data[depth].err = X509_STORE_CTX_get_error(x509_ctx);
	}

	/* allow certificate chain to be completed */
	if (depth > 0) {
		verify_ok = 1;
	} else {
		/* search for deeper certificates in the chain with errors */
		for (depth = fetch->cert_depth; depth > 0; depth--) {
			if (fetch->cert_data[depth].err != 0) {
				/* error in previous certificate so fail verification */
				verify_ok = 0;
				X509_STORE_CTX_set_error(x509_ctx, fetch->cert_data[depth].err);
			}
		}
	}

	return verify_ok;
}


/**
 * OpenSSL certificate chain verification callback
 *
 * Verifies certificate chain by calling standard implementation after
 * setting up context for the certificate callback.
 *
 * \param x509_ctx The certificate store to validate
 * \param parm The fetch context.
 * \return 1 to indicate verification success and 0 to indicate verification failure.
 */
static int fetch_curl_cert_verify_callback(X509_STORE_CTX *x509_ctx, void *parm)
{
	int ok;

	/* Store fetch struct in context for verify callback */
	ok = X509_STORE_CTX_set_app_data(x509_ctx, parm);

	/* verify the certificate chain using standard call */
	if (ok) {
		ok = X509_verify_cert(x509_ctx);
	}

	return ok;
}


/**
 * cURL SSL setup callback
 *
 * \param curl_handle The curl handle to perform the ssl operation on.
 * \param _sslctx The ssl context.
 * \param parm The callback context.
 * \return A curl result code.
 */
static CURLcode
fetch_curl_sslctxfun(CURL *curl_handle, void *_sslctx, void *parm)
{
	struct curl_fetch_info *f = (struct curl_fetch_info *) parm;
	SSL_CTX *sslctx = _sslctx;
	long options = SSL_OP_ALL | SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3;

	/* set verify callback for each certificate in chain */
	SSL_CTX_set_verify(sslctx, SSL_VERIFY_PEER, fetch_curl_verify_callback);

	/* set callback used to verify certificate chain */
	SSL_CTX_set_cert_verify_callback(sslctx,
					 fetch_curl_cert_verify_callback,
					 parm);

	if (f->downgrade_tls) {
		/* Disable TLS 1.1/1.2 if the server can't cope with them */
#ifdef SSL_OP_NO_TLSv1_1
		options |= SSL_OP_NO_TLSv1_1;
#endif
#ifdef SSL_OP_NO_TLSv1_2
		options |= SSL_OP_NO_TLSv1_2;
#endif
#ifdef SSL_MODE_SEND_FALLBACK_SCSV
		/* Ensure server rejects the connection if downgraded too far */
		SSL_CTX_set_mode(sslctx, SSL_MODE_SEND_FALLBACK_SCSV);
#endif
	}

	SSL_CTX_set_options(sslctx, options);

	return CURLE_OK;
}


/**
 * Set options specific for a fetch.
 *
 * \param f The fetch to set options on.
 * \return A curl result code.
 */
static CURLcode fetch_curl_set_options(struct curl_fetch_info *f)
{
	CURLcode code;
	const char *auth;

#undef SETOPT
#define SETOPT(option, value) { \
	code = curl_easy_setopt(f->curl_handle, option, value);	\
	if (code != CURLE_OK)					\
		return code;					\
	}

	SETOPT(CURLOPT_URL, nsurl_access(f->url));
	SETOPT(CURLOPT_PRIVATE, f);
	SETOPT(CURLOPT_WRITEDATA, f);
	SETOPT(CURLOPT_WRITEHEADER, f);
	SETOPT(CURLOPT_PROGRESSDATA, f);
	SETOPT(CURLOPT_REFERER, fetch_get_referer_to_send(f->fetch_handle));
	SETOPT(CURLOPT_HTTPHEADER, f->headers);
	if (f->post_urlenc) {
		SETOPT(CURLOPT_HTTPPOST, NULL);
		SETOPT(CURLOPT_HTTPGET, 0L);
		SETOPT(CURLOPT_POSTFIELDS, f->post_urlenc);
	} else if (f->post_multipart) {
		SETOPT(CURLOPT_POSTFIELDS, NULL);
		SETOPT(CURLOPT_HTTPGET, 0L);
		SETOPT(CURLOPT_HTTPPOST, f->post_multipart);
	} else {
		SETOPT(CURLOPT_POSTFIELDS, NULL);
		SETOPT(CURLOPT_HTTPPOST, NULL);
		SETOPT(CURLOPT_HTTPGET, 1L);
	}

	f->cookie_string = urldb_get_cookie(f->url, true);
	if (f->cookie_string) {
		SETOPT(CURLOPT_COOKIE, f->cookie_string);
	} else {
		SETOPT(CURLOPT_COOKIE, NULL);
	}

	if ((auth = urldb_get_auth_details(f->url, NULL)) != NULL) {
		SETOPT(CURLOPT_HTTPAUTH, CURLAUTH_ANY);
		SETOPT(CURLOPT_USERPWD, auth);
	} else {
		SETOPT(CURLOPT_USERPWD, NULL);
	}

	/* set up proxy options */
	if (nsoption_bool(http_proxy) &&
	    (nsoption_charp(http_proxy_host) != NULL) &&
	    (strncmp(nsurl_access(f->url), "file:", 5) != 0)) {
		SETOPT(CURLOPT_PROXY, nsoption_charp(http_proxy_host));
		SETOPT(CURLOPT_PROXYPORT, (long) nsoption_int(http_proxy_port));

#if LIBCURL_VERSION_NUM >= 0x071304
		/* Added in 7.19.4 */
		/* setup the omission list */
		SETOPT(CURLOPT_NOPROXY, nsoption_charp(http_proxy_noproxy));
#endif

		if (nsoption_int(http_proxy_auth) != OPTION_HTTP_PROXY_AUTH_NONE) {
			SETOPT(CURLOPT_PROXYAUTH,
			       nsoption_int(http_proxy_auth) ==
					OPTION_HTTP_PROXY_AUTH_BASIC ?
					(long) CURLAUTH_BASIC :
					(long) CURLAUTH_NTLM);
			snprintf(fetch_proxy_userpwd,
					sizeof fetch_proxy_userpwd,
					"%s:%s",
				 nsoption_charp(http_proxy_auth_user),
				 nsoption_charp(http_proxy_auth_pass));
			SETOPT(CURLOPT_PROXYUSERPWD, fetch_proxy_userpwd);
		}
	} else {
		SETOPT(CURLOPT_PROXY, NULL);
	}

	/* Disable SSL session ID caching, as some servers can't cope. */
	SETOPT(CURLOPT_SSL_SESSIONID_CACHE, 0);

	if (urldb_get_cert_permissions(f->url)) {
		/* Disable certificate verification */
		SETOPT(CURLOPT_SSL_VERIFYPEER, 0L);
		SETOPT(CURLOPT_SSL_VERIFYHOST, 0L);
		if (curl_with_openssl) {
			SETOPT(CURLOPT_SSL_CTX_FUNCTION, NULL);
			SETOPT(CURLOPT_SSL_CTX_DATA, NULL);
		}
	} else {
		/* do verification */
		SETOPT(CURLOPT_SSL_VERIFYPEER, 1L);
		SETOPT(CURLOPT_SSL_VERIFYHOST, 2L);
		if (curl_with_openssl) {
			SETOPT(CURLOPT_SSL_CTX_FUNCTION, fetch_curl_sslctxfun);
			SETOPT(CURLOPT_SSL_CTX_DATA, f);
		}
	}

	return CURLE_OK;
}

/**
 * Initiate a fetch from the queue.
 *
 * \param fetch fetch to use to fetch content.
 * \param handle CURL handle to be used to fetch the content.
 * \return true if the fetch was successfully initiated else false.
 */
static bool
fetch_curl_initiate_fetch(struct curl_fetch_info *fetch, CURL *handle)
{
	CURLcode code;
	CURLMcode codem;

	fetch->curl_handle = handle;

	/* Initialise the handle */
	code = fetch_curl_set_options(fetch);
	if (code != CURLE_OK) {
		fetch->curl_handle = 0;
		return false;
	}

	/* add to the global curl multi handle */
	codem = curl_multi_add_handle(fetch_curl_multi, fetch->curl_handle);
	assert(codem == CURLM_OK || codem == CURLM_CALL_MULTI_PERFORM);

	return true;
}


/**
 * Find a CURL handle to use to dispatch a job
 */
static CURL *fetch_curl_get_handle(lwc_string *host)
{
	struct cache_handle *h;
	CURL *ret;
	RING_FINDBYLWCHOST(curl_handle_ring, h, host);
	if (h) {
		ret = h->handle;
		lwc_string_unref(h->host);
		RING_REMOVE(curl_handle_ring, h);
		free(h);
	} else {
		ret = curl_easy_duphandle(fetch_blank_curl);
	}
	return ret;
}


/**
 * Dispatch a single job
 */
static bool fetch_curl_start(void *vfetch)
{
	struct curl_fetch_info *fetch = (struct curl_fetch_info*)vfetch;
	return fetch_curl_initiate_fetch(fetch,
			fetch_curl_get_handle(fetch->host));
}

/**
 * Cache a CURL handle for the provided host (if wanted)
 */
static void fetch_curl_cache_handle(CURL *handle, lwc_string *host)
{
#if LIBCURL_VERSION_NUM >= 0x071e00
	/* 7.30.0 or later has its own connection caching; suppress ours */
	curl_easy_cleanup(handle);
	return;
#else
	struct cache_handle *h = 0;
	int c;
	RING_FINDBYLWCHOST(curl_handle_ring, h, host);
	if (h) {
		/* Already have a handle cached for this hostname */
		curl_easy_cleanup(handle);
		return;
	}
	/* We do not have a handle cached, first up determine if the cache is full */
	RING_GETSIZE(struct cache_handle, curl_handle_ring, c);
	if (c >= nsoption_int(max_cached_fetch_handles)) {
		/* Cache is full, so, we rotate the ring by one and
		 * replace the oldest handle with this one. We do this
		 * without freeing/allocating memory (except the
		 * hostname) and without removing the entry from the
		 * ring and then re-inserting it, in order to be as
		 * efficient as we can.
		 */
		if (curl_handle_ring != NULL) {
			h = curl_handle_ring;
			curl_handle_ring = h->r_next;
			curl_easy_cleanup(h->handle);
			h->handle = handle;
			lwc_string_unref(h->host);
			h->host = lwc_string_ref(host);
		} else {
			/* Actually, we don't want to cache any handles */
			curl_easy_cleanup(handle);
		}

		return;
	}
	/* The table isn't full yet, so make a shiny new handle to add to the ring */
	h = (struct cache_handle*)malloc(sizeof(struct cache_handle));
	h->handle = handle;
	h->host = lwc_string_ref(host);
	RING_INSERT(curl_handle_ring, h);
#endif
}


/**
 * Abort a fetch.
 */
static void fetch_curl_abort(void *vf)
{
	struct curl_fetch_info *f = (struct curl_fetch_info *)vf;
	assert(f);
	LOG("fetch %p, url '%s'", f, nsurl_access(f->url));
	if (f->curl_handle) {
		f->abort = true;
	} else {
		fetch_remove_from_queues(f->fetch_handle);
		fetch_free(f->fetch_handle);
	}
}


/**
 * Clean up the provided fetch object and free it.
 *
 * Will prod the queue afterwards to allow pending requests to be initiated.
 */
static void fetch_curl_stop(struct curl_fetch_info *f)
{
	CURLMcode codem;

	assert(f);
	LOG("fetch %p, url '%s'", f, nsurl_access(f->url));

	if (f->curl_handle) {
		/* remove from curl multi handle */
		codem = curl_multi_remove_handle(fetch_curl_multi,
				f->curl_handle);
		assert(codem == CURLM_OK);
		/* Put this curl handle into the cache if wanted. */
		fetch_curl_cache_handle(f->curl_handle, f->host);
		f->curl_handle = 0;
	}

	fetch_remove_from_queues(f->fetch_handle);
}


/**
 * Free a fetch structure and associated resources.
 */
static void fetch_curl_free(void *vf)
{
	struct curl_fetch_info *f = (struct curl_fetch_info *)vf;
	int i;

	if (f->curl_handle) {
		curl_easy_cleanup(f->curl_handle);
	}
	nsurl_unref(f->url);
	lwc_string_unref(f->host);
	free(f->location);
	free(f->cookie_string);
	free(f->realm);
	if (f->headers) {
		curl_slist_free_all(f->headers);
	}
	free(f->post_urlenc);
	if (f->post_multipart) {
		curl_formfree(f->post_multipart);
	}

	for (i = 0; i < MAX_CERTS && f->cert_data[i].cert; i++) {
		f->cert_data[i].cert->references--;
		if (f->cert_data[i].cert->references == 0) {
			X509_free(f->cert_data[i].cert);
		}
	}

	if (f->gopher)
		gopher_state_free(f->gopher);

	free(f);
}


/**
 * Find the status code and content type and inform the caller.
 *
 * Return true if the fetch is being aborted.
 */
static bool fetch_curl_process_headers(struct curl_fetch_info *f)
{
	long http_code;
	CURLcode code;
	fetch_msg msg;

	f->had_headers = true;

	if (!f->http_code) {
		code = curl_easy_getinfo(f->curl_handle, CURLINFO_HTTP_CODE,
					 &f->http_code);
		fetch_set_http_code(f->fetch_handle, f->http_code);
		assert(code == CURLE_OK);
	}
	http_code = f->http_code;
	LOG("HTTP status code %li", http_code);

	if (http_code == 304 && !f->post_urlenc && !f->post_multipart) {
		/* Not Modified && GET request */
		msg.type = FETCH_NOTMODIFIED;
		fetch_send_callback(&msg, f->fetch_handle);
		return true;
	}

	/* handle HTTP redirects (3xx response codes) */
	if (300 <= http_code && http_code < 400 && f->location != 0) {
		LOG("FETCH_REDIRECT, '%s'", f->location);
		msg.type = FETCH_REDIRECT;
		msg.data.redirect = f->location;
		fetch_send_callback(&msg, f->fetch_handle);
		return true;
	}

	/* handle HTTP 401 (Authentication errors) */
	if (http_code == 401) {
		msg.type = FETCH_AUTH;
		msg.data.auth.realm = f->realm;
		fetch_send_callback(&msg, f->fetch_handle);
		return true;
	}

	/* handle HTTP errors (non 2xx response codes) */
	if (f->only_2xx && strncmp(nsurl_access(f->url), "http", 4) == 0 &&
			(http_code < 200 || 299 < http_code)) {
		msg.type = FETCH_ERROR;
		msg.data.error = messages_get("Not2xx");
		fetch_send_callback(&msg, f->fetch_handle);
		return true;
	}

	if (f->abort)
		return true;

	return false;
}

/**
 * setup callback to allow the user to examine certificates which have
 * failed to validate during fetch.
 */
static void
curl_start_cert_validate(struct curl_fetch_info *f,
			 struct cert_info *certs)
{
	int depth;
	BIO *mem;
	BUF_MEM *buf;
	struct ssl_cert_info ssl_certs[MAX_CERTS];
	fetch_msg msg;

	for (depth = 0; depth <= f->cert_depth; depth++) {
		assert(certs[depth].cert != NULL);

		/* get certificate version */
		ssl_certs[depth].version = X509_get_version(certs[depth].cert);

		/* not before date */
		mem = BIO_new(BIO_s_mem());
		ASN1_TIME_print(mem, X509_get_notBefore(certs[depth].cert));
		BIO_get_mem_ptr(mem, &buf);
		(void) BIO_set_close(mem, BIO_NOCLOSE);
		BIO_free(mem);
		memcpy(ssl_certs[depth].not_before,
		       buf->data,
		       min(sizeof(ssl_certs[depth].not_before) - 1,
			   (unsigned)buf->length));
		ssl_certs[depth].not_before[min(sizeof(ssl_certs[depth].not_before) - 1,
					    (unsigned)buf->length)] = 0;
		BUF_MEM_free(buf);

		/* not after date */
		mem = BIO_new(BIO_s_mem());
		ASN1_TIME_print(mem,
				X509_get_notAfter(certs[depth].cert));
		BIO_get_mem_ptr(mem, &buf);
		(void) BIO_set_close(mem, BIO_NOCLOSE);
		BIO_free(mem);
		memcpy(ssl_certs[depth].not_after,
		       buf->data,
		       min(sizeof(ssl_certs[depth].not_after) - 1,
			   (unsigned)buf->length));
		ssl_certs[depth].not_after[min(sizeof(ssl_certs[depth].not_after) - 1,
					   (unsigned)buf->length)] = 0;
		BUF_MEM_free(buf);

		/* signature type */
		ssl_certs[depth].sig_type =
			X509_get_signature_type(certs[depth].cert);

		/* serial number */
		ssl_certs[depth].serial =
			ASN1_INTEGER_get(
				X509_get_serialNumber(certs[depth].cert));

		/* issuer name */
		mem = BIO_new(BIO_s_mem());
		X509_NAME_print_ex(mem,
				   X509_get_issuer_name(certs[depth].cert),
				   0, XN_FLAG_SEP_CPLUS_SPC |
				   XN_FLAG_DN_REV | XN_FLAG_FN_NONE);
		BIO_get_mem_ptr(mem, &buf);
		(void) BIO_set_close(mem, BIO_NOCLOSE);
		BIO_free(mem);
		memcpy(ssl_certs[depth].issuer,
		       buf->data,
		       min(sizeof(ssl_certs[depth].issuer) - 1,
			   (unsigned) buf->length));
		ssl_certs[depth].issuer[min(sizeof(ssl_certs[depth].issuer) - 1,
					(unsigned) buf->length)] = 0;
		BUF_MEM_free(buf);

		/* subject */
		mem = BIO_new(BIO_s_mem());
		X509_NAME_print_ex(mem,
				   X509_get_subject_name(certs[depth].cert),
				   0,
				   XN_FLAG_SEP_CPLUS_SPC |
				   XN_FLAG_DN_REV |
				   XN_FLAG_FN_NONE);
		BIO_get_mem_ptr(mem, &buf);
		(void) BIO_set_close(mem, BIO_NOCLOSE);
		BIO_free(mem);
		memcpy(ssl_certs[depth].subject,
		       buf->data,
		       min(sizeof(ssl_certs[depth].subject) - 1,
			   (unsigned)buf->length));
		ssl_certs[depth].subject[min(sizeof(ssl_certs[depth].subject) - 1,
					 (unsigned) buf->length)] = 0;
		BUF_MEM_free(buf);

		/* type of certificate */
		ssl_certs[depth].cert_type =
			X509_certificate_type(certs[depth].cert,
					      X509_get_pubkey(certs[depth].cert));

		/* and clean up */
		certs[depth].cert->references--;
		if (certs[depth].cert->references == 0) {
			X509_free(certs[depth].cert);
		}
	}

	msg.type = FETCH_CERT_ERR;
	msg.data.cert_err.certs = ssl_certs;
	msg.data.cert_err.num_certs = depth;
	fetch_send_callback(&msg, f->fetch_handle);
}

/**
 * Handle a completed fetch (CURLMSG_DONE from curl_multi_info_read()).
 *
 * \param curl_handle curl easy handle of fetch
 * \param result The result code of the completed fetch.
 */
static void fetch_curl_done(CURL *curl_handle, CURLcode result)
{
	bool finished = false;
	bool error = false;
	bool cert = false;
	bool abort_fetch;
	struct curl_fetch_info *f;
	char **_hideous_hack = (char **) (void *) &f;
	CURLcode code;
	struct cert_info certs[MAX_CERTS];

	/* find the structure associated with this fetch */
	/* For some reason, cURL thinks CURLINFO_PRIVATE should be a string?! */
	code = curl_easy_getinfo(curl_handle, CURLINFO_PRIVATE, _hideous_hack);
	assert(code == CURLE_OK);

	abort_fetch = f->abort;
	LOG("done %s", nsurl_access(f->url));

	if ((abort_fetch == false) &&
	    (result == CURLE_OK ||
	     ((result == CURLE_WRITE_ERROR) && (f->stopped == false)))) {
		/* handle incoming gopher data */
		if (f->gopher)
			gopher_fetch_data(f->gopher, NULL, 0);
		/* fetch completed normally or the server fed us a junk gzip
		 * stream (usually in the form of garbage at the end of the
		 * stream). Curl will have fed us all but the last chunk of
		 * decoded data, which is sad as, if we'd received the last
		 * chunk, too, we'd be able to render the whole object.
		 * As is, we'll just have to accept that the end of the
		 * object will be truncated in this case and leave it to
		 * the content handlers to cope.
		 */
		if (f->stopped ||
		    (!f->had_headers &&	fetch_curl_process_headers(f))) {
			; /* redirect with no body or similar */
		} else {
			finished = true;
		}
	} else if (result == CURLE_PARTIAL_FILE) {
		/* CURLE_PARTIAL_FILE occurs if the received body of a
		 * response is smaller than that specified in the
		 * Content-Length header.
		 */
		if (!f->had_headers && fetch_curl_process_headers(f))
			; /* redirect with partial body, or similar */
		else {
			finished = true;
		}
	} else if (result == CURLE_WRITE_ERROR && f->stopped) {
		/* CURLE_WRITE_ERROR occurs when fetch_curl_data
		 * returns 0, which we use to abort intentionally
		 */
		;
	} else if (result == CURLE_SSL_PEER_CERTIFICATE ||
			result == CURLE_SSL_CACERT) {
		/* CURLE_SSL_PEER_CERTIFICATE renamed to
		 * CURLE_PEER_FAILED_VERIFICATION
		 */
		memset(certs, 0, sizeof(certs));
		memcpy(certs, f->cert_data, sizeof(certs));
		memset(f->cert_data, 0, sizeof(f->cert_data));
		cert = true;
	} else {
		LOG("Unknown cURL response code %d", result);
		error = true;
	}

	fetch_curl_stop(f);

	if (abort_fetch) {
		; /* fetch was aborted: no callback */
	} else if (finished) {
		fetch_msg msg;
		msg.type = FETCH_FINISHED;
		fetch_send_callback(&msg, f->fetch_handle);
	} else if (cert) {
		/* user needs to validate certificate with issue */
		curl_start_cert_validate(f, certs);
	} else if (error) {
		fetch_msg msg;
		switch (result) {
		case CURLE_SSL_CONNECT_ERROR:
			msg.type = FETCH_SSL_ERR;
			break;

		case CURLE_OPERATION_TIMEDOUT:
			msg.type = FETCH_TIMEDOUT;
			msg.data.error = curl_easy_strerror(result);
			break;

		default:
			msg.type = FETCH_ERROR;
			msg.data.error = curl_easy_strerror(result);
		}

		fetch_send_callback(&msg, f->fetch_handle);
	}

	fetch_free(f->fetch_handle);
}


/**
 * Do some work on current fetches.
 *
 * Must be called regularly to make progress on fetches.
 */
static void fetch_curl_poll(lwc_string *scheme_ignored)
{
	int running, queue;
	CURLMcode codem;
	CURLMsg *curl_msg;

	if (nsoption_bool(suppress_curl_debug) == false) {
		fd_set read_fd_set, write_fd_set, exc_fd_set;
		int max_fd = -1;
		int i;

		FD_ZERO(&read_fd_set);
		FD_ZERO(&write_fd_set);
		FD_ZERO(&exc_fd_set);

		codem = curl_multi_fdset(fetch_curl_multi,
				&read_fd_set, &write_fd_set,
				&exc_fd_set, &max_fd);
		assert(codem == CURLM_OK);

		LOG("Curl file descriptor states (maxfd=%i):", max_fd);
		for (i = 0; i <= max_fd; i++) {
			bool read = false;
			bool write = false;
			bool error = false;

			if (FD_ISSET(i, &read_fd_set)) {
				read = true;
			}
			if (FD_ISSET(i, &write_fd_set)) {
				write = true;
			}
			if (FD_ISSET(i, &exc_fd_set)) {
				error = true;
			}
			if (read || write || error) {
				LOG("  fd %i: %s %s %s", i,
						read  ? "read" : "    ",
						write ? "write" : "     ",
						error ? "error" : "     ");
			}
		}
	}

	/* do any possible work on the current fetches */
	do {
		codem = curl_multi_perform(fetch_curl_multi, &running);
		if (codem != CURLM_OK && codem != CURLM_CALL_MULTI_PERFORM) {
			LOG("curl_multi_perform: %i %s", codem, curl_multi_strerror(codem));
			guit->misc->warning("MiscError", curl_multi_strerror(codem));
			return;
		}
	} while (codem == CURLM_CALL_MULTI_PERFORM);

	/* process curl results */
	curl_msg = curl_multi_info_read(fetch_curl_multi, &queue);
	while (curl_msg) {
		switch (curl_msg->msg) {
			case CURLMSG_DONE:
				fetch_curl_done(curl_msg->easy_handle,
						curl_msg->data.result);
				break;
			default:
				break;
		}
		curl_msg = curl_multi_info_read(fetch_curl_multi, &queue);
	}
}




/**
 * Callback function for fetch progress.
 */
static int
fetch_curl_progress(void *clientp,
		    double dltotal,
		    double dlnow,
		    double ultotal,
		    double ulnow)
{
	static char fetch_progress_buffer[256]; /**< Progress buffer for cURL */
	struct curl_fetch_info *f = (struct curl_fetch_info *) clientp;
	uint64_t time_now_ms;
	fetch_msg msg;

	if (f->abort) {
		return 0;
        }

	msg.type = FETCH_PROGRESS;
	msg.data.progress = fetch_progress_buffer;

	/* Rate limit each fetch's progress notifications */
        nsu_getmonotonic_ms(&time_now_ms);
#define UPDATE_DELAY_MS (1000 / UPDATES_PER_SECOND)
	if (time_now_ms - f->last_progress_update < UPDATE_DELAY_MS) {
		return 0;
        }
#undef UPDATE_DELAY_MS
	f->last_progress_update = time_now_ms;

	if (dltotal > 0) {
		snprintf(fetch_progress_buffer, 255,
				messages_get("Progress"),
				human_friendly_bytesize(dlnow),
				human_friendly_bytesize(dltotal));
		fetch_send_callback(&msg, f->fetch_handle);
	} else {
		snprintf(fetch_progress_buffer, 255,
				messages_get("ProgressU"),
				human_friendly_bytesize(dlnow));
		fetch_send_callback(&msg, f->fetch_handle);
	}

	return 0;
}


/**
 * Ignore everything given to it.
 *
 * Used to ignore cURL debug.
 */
static int fetch_curl_ignore_debug(CURL *handle,
			    curl_infotype type,
			    char *data,
			    size_t size,
			    void *userptr)
{
	return 0;
}


/**
 * Callback function for cURL.
 */
static size_t fetch_curl_data(char *data, size_t size, size_t nmemb, void *_f)
{
	struct curl_fetch_info *f = _f;
	CURLcode code;
	fetch_msg msg;

	/* gopher data receives special treatment */
	if (f->gopher && gopher_need_generate(f->gopher->type)) {
		/* We didn't receive anything yet, check for error.
		 * type 3 items report an error
		 */
		if (!f->http_code) {
			if (data[0] == GOPHER_TYPE_ERROR) {
				/* TODO: try to guess better from the string ?
				 * like "3 '/bcd' doesn't exist!"
				 * TODO: what about other file types ?
				 */
				f->http_code = 404;
			} else {
				f->http_code = 200;
			}
			fetch_set_http_code(f->fetch_handle, f->http_code);
		}
	}

	/* ensure we only have to get this information once */
	if (!f->http_code) {
		code = curl_easy_getinfo(f->curl_handle, CURLINFO_HTTP_CODE,
					 &f->http_code);
		fetch_set_http_code(f->fetch_handle, f->http_code);
		assert(code == CURLE_OK);
	}

	/* ignore body if this is a 401 reply by skipping it and reset
	 * the HTTP response code to enable follow up fetches.
	 */
	if (f->http_code == 401) {
		f->http_code = 0;
		return size * nmemb;
	}

	if (f->abort || (!f->had_headers && fetch_curl_process_headers(f))) {
		f->stopped = true;
		return 0;
	}

	/* gopher data receives special treatment */
	/* TODO: strip the last line of text file when single dot. */
	if (f->gopher && gopher_need_generate(f->gopher->type)) {
		return gopher_fetch_data(f->gopher, data, size * nmemb);
	}

	/* send data to the caller */
	msg.type = FETCH_DATA;
	msg.data.header_or_data.buf = (const uint8_t *) data;
	msg.data.header_or_data.len = size * nmemb;
	fetch_send_callback(&msg, f->fetch_handle);

	if (f->abort) {
		f->stopped = true;
		return 0;
	}

	return size * nmemb;
}


/**
 * Callback function for headers.
 *
 * See RFC 2616 4.2.
 */
static size_t
fetch_curl_header(char *data, size_t size, size_t nmemb, void *_f)
{
	struct curl_fetch_info *f = _f;
	int i;
	fetch_msg msg;
	size *= nmemb;

	if (f->abort) {
		f->stopped = true;
		return 0;
	}

	msg.type = FETCH_HEADER;
	msg.data.header_or_data.buf = (const uint8_t *) data;
	msg.data.header_or_data.len = size;
	fetch_send_callback(&msg, f->fetch_handle);

#define SKIP_ST(o) for (i = (o); i < (int) size && (data[i] == ' ' || data[i] == '\t'); i++)

	if (12 < size && strncasecmp(data, "Location:", 9) == 0) {
		/* extract Location header */
		free(f->location);
		f->location = malloc(size);
		if (!f->location) {
			LOG("malloc failed");
			return size;
		}
		SKIP_ST(9);
		strncpy(f->location, data + i, size - i);
		f->location[size - i] = '\0';
		for (i = size - i - 1; i >= 0 &&
				(f->location[i] == ' ' ||
				f->location[i] == '\t' ||
				f->location[i] == '\r' ||
				f->location[i] == '\n'); i--)
			f->location[i] = '\0';
	} else if (15 < size && strncasecmp(data, "Content-Length:", 15) == 0) {
		/* extract Content-Length header */
		SKIP_ST(15);
		if (i < (int)size && '0' <= data[i] && data[i] <= '9')
			f->content_length = atol(data + i);
	} else if (17 < size && strncasecmp(data, "WWW-Authenticate:", 17) == 0) {
		/* extract the first Realm from WWW-Authenticate header */
		SKIP_ST(17);

		while (i < (int) size - 5 &&
				strncasecmp(data + i, "realm", 5))
			i++;
		while (i < (int) size - 1 && data[++i] != '"')
			/* */;
		i++;

		if (i < (int) size) {
			size_t end = i;

			while (end < size && data[end] != '"')
				++end;

			if (end < size) {
				free(f->realm);
				f->realm = malloc(end - i + 1);
				if (f->realm != NULL) {
					strncpy(f->realm, data + i, end - i);
					f->realm[end - i] = '\0';
				}
			}
		}
	} else if (11 < size && strncasecmp(data, "Set-Cookie:", 11) == 0) {
		/* extract Set-Cookie header */
		SKIP_ST(11);

		fetch_set_cookie(f->fetch_handle, &data[i]);
	}

	return size;
#undef SKIP_ST
}

static int fetch_curl_fdset(lwc_string *scheme, fd_set *read_set,
			    fd_set *write_set, fd_set *error_set)
{
	CURLMcode code;
	int maxfd = -1;

	code = curl_multi_fdset(fetch_curl_multi,
				read_set,
				write_set,
				error_set,
				&maxfd);
	assert(code == CURLM_OK);

	return maxfd;
}



/* exported function documented in content/fetchers/curl.h */
nserror fetch_curl_register(void)
{
	CURLcode code;
	curl_version_info_data *data;
	int i;
	lwc_string *scheme;
	/*const*/ struct fetcher_operation_table fetcher_ops = {
		.initialise = fetch_curl_initialise,
		.acceptable = fetch_curl_can_fetch,
		.setup = fetch_curl_setup,
		.start = fetch_curl_start,
		.abort = fetch_curl_abort,
		.free = fetch_curl_free,
		.poll = fetch_curl_poll,
		.fdset = fetch_curl_fdset,
		.finalise = fetch_curl_finalise
	};

	LOG("curl_version %s", curl_version());

	code = curl_global_init(CURL_GLOBAL_ALL);
	if (code != CURLE_OK) {
		LOG("curl_global_init failed.");
		return NSERROR_INIT_FAILED;
	}

	fetch_curl_multi = curl_multi_init();
	if (!fetch_curl_multi) {
		LOG("curl_multi_init failed.");
		return NSERROR_INIT_FAILED;
	}

#if LIBCURL_VERSION_NUM >= 0x071e00
	/* built against 7.30.0 or later: configure caching */
	{
		CURLMcode mcode;
		int maxconnects = nsoption_int(max_fetchers) +
				nsoption_int(max_cached_fetch_handles);

#undef SETOPT
#define SETOPT(option, value) \
	mcode = curl_multi_setopt(fetch_curl_multi, option, value);	\
	if (mcode != CURLM_OK)						\
		goto curl_multi_setopt_failed;

		SETOPT(CURLMOPT_MAXCONNECTS, maxconnects);
		SETOPT(CURLMOPT_MAX_TOTAL_CONNECTIONS, maxconnects);
		SETOPT(CURLMOPT_MAX_HOST_CONNECTIONS, nsoption_int(max_fetchers_per_host));
	}
#endif

	/* Create a curl easy handle with the options that are common to all
	 *  fetches.
	 */
	fetch_blank_curl = curl_easy_init();
	if (!fetch_blank_curl) {
		LOG("curl_easy_init failed");
		return NSERROR_INIT_FAILED;
	}

#undef SETOPT
#define SETOPT(option, value) \
	code = curl_easy_setopt(fetch_blank_curl, option, value);	\
	if (code != CURLE_OK)						\
		goto curl_easy_setopt_failed;

	if (verbose_log) {
	    SETOPT(CURLOPT_VERBOSE, 1);
	} else {
	    SETOPT(CURLOPT_VERBOSE, 0);
	}
	SETOPT(CURLOPT_ERRORBUFFER, fetch_error_buffer);
	if (nsoption_bool(suppress_curl_debug)) {
		SETOPT(CURLOPT_DEBUGFUNCTION, fetch_curl_ignore_debug);
	}
	SETOPT(CURLOPT_WRITEFUNCTION, fetch_curl_data);
	SETOPT(CURLOPT_HEADERFUNCTION, fetch_curl_header);
	SETOPT(CURLOPT_PROGRESSFUNCTION, fetch_curl_progress);
	SETOPT(CURLOPT_NOPROGRESS, 0);
	SETOPT(CURLOPT_USERAGENT, user_agent_string());
	SETOPT(CURLOPT_ENCODING, "gzip");
	SETOPT(CURLOPT_LOW_SPEED_LIMIT, 1L);
	SETOPT(CURLOPT_LOW_SPEED_TIME, 180L);
	SETOPT(CURLOPT_NOSIGNAL, 1L);
	SETOPT(CURLOPT_CONNECTTIMEOUT, nsoption_uint(curl_fetch_timeout));

	if (nsoption_charp(ca_bundle) &&
	    strcmp(nsoption_charp(ca_bundle), "")) {
		LOG("ca_bundle: '%s'", nsoption_charp(ca_bundle));
		SETOPT(CURLOPT_CAINFO, nsoption_charp(ca_bundle));
	}
	if (nsoption_charp(ca_path) && strcmp(nsoption_charp(ca_path), "")) {
		LOG("ca_path: '%s'", nsoption_charp(ca_path));
		SETOPT(CURLOPT_CAPATH, nsoption_charp(ca_path));
	}

	/* Detect whether the SSL CTX function API works */
	curl_with_openssl = true;
	code = curl_easy_setopt(fetch_blank_curl,
			CURLOPT_SSL_CTX_FUNCTION, NULL);
	if (code != CURLE_OK) {
		curl_with_openssl = false;
	}

	LOG("cURL %slinked against openssl", curl_with_openssl ? "" : "not ");

	/* cURL initialised okay, register the fetchers */

	data = curl_version_info(CURLVERSION_NOW);

	for (i = 0; data->protocols[i]; i++) {
		if (strcmp(data->protocols[i], "http") == 0) {
			scheme = lwc_string_ref(corestring_lwc_http);

		} else if (strcmp(data->protocols[i], "https") == 0) {
			scheme = lwc_string_ref(corestring_lwc_https);

		} else if (strcmp(data->protocols[i], "gopher") == 0) {
			scheme = lwc_string_ref(corestring_lwc_gopher);
			/* We use a different setup hook */
			fetcher_ops.setup = fetch_curl_setup_gopher;

		} else {
			/* Ignore non-http(s) protocols */
			continue;
		}

		if (fetcher_add(scheme, &fetcher_ops) != NSERROR_OK) {
			LOG("Unable to register cURL fetcher for %s", data->protocols[i]);
		}
	}

	return NSERROR_OK;

curl_easy_setopt_failed:
	LOG("curl_easy_setopt failed.");
	return NSERROR_INIT_FAILED;

#if LIBCURL_VERSION_NUM >= 0x071e00
curl_multi_setopt_failed:
	LOG("curl_multi_setopt failed.");
	return NSERROR_INIT_FAILED;
#endif
}
