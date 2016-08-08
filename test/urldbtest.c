/*
 * Copyright 2015 Vincent Sanders <vince@netsurf-browser.org>
 * Copyright 2011 John Mark Bell <jmb@netsurf-browser.org>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * Test nsurl operations.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <check.h>

#include <libwapcaplet/libwapcaplet.h>

#include "utils/corestrings.h"
#include "utils/log.h"
#include "utils/corestrings.h"
#include "utils/nsurl.h"
#include "netsurf/url_db.h"
#include "content/urldb.h"
#include "desktop/cookie_manager.h"

const char *test_urldb_path = "test/data/urldb";

struct netsurf_table *guit = NULL;

/*************** original test helpers ************/

bool cookie_manager_add(const struct cookie_data *data)
{
	return true;
}

void cookie_manager_remove(const struct cookie_data *data)
{
}

static nsurl *make_url(const char *url)
{
	nsurl *nsurl;
	if (nsurl_create(url, &nsurl) != NSERROR_OK) {
		LOG("failed creating nsurl");
		exit(1);
	}
	return nsurl;
}

static char *make_path_query(nsurl *url)
{
	size_t len;
	char *path_query;
	if (nsurl_get(url, NSURL_PATH | NSURL_QUERY, &path_query, &len) !=
			NSERROR_OK) {
		LOG("failed creating path_query");
		exit(1);
	}
	return path_query;
}

static lwc_string *make_lwc(const char *str)
{
	lwc_string *lwc;
	if (lwc_intern_string(str, strlen(str), &lwc) != lwc_error_ok) {
		LOG("failed creating lwc_string");
		exit(1);
	}
	return lwc;
}

static bool test_urldb_set_cookie(const char *header, const char *url,
		const char *referer)
{
	nsurl *r = NULL;
	nsurl *nsurl = make_url(url);
	bool ret;

	if (referer != NULL)
		r = make_url(referer);

	ret = urldb_set_cookie(header, nsurl, r);

	if (referer != NULL)
		nsurl_unref(r);
	nsurl_unref(nsurl);

	return ret;
}

static char *test_urldb_get_cookie(const char *url)
{
	nsurl *nsurl = make_url(url);
	char *ret;

	ret = urldb_get_cookie(nsurl, true);
	nsurl_unref(nsurl);

	return ret;
}


/*************************************************/

/** urldb create fixture */
static void urldb_create(void)
{
	nserror res;
	res = corestrings_init();
	ck_assert_int_eq(res, NSERROR_OK);
}

static void urldb_lwc_iterator(lwc_string *str, void *pw)
{
	int *scount = pw;

	LOG("[%3u] %.*s", str->refcnt,
	    (int)lwc_string_length(str),
	    lwc_string_data(str));
	(*scount)++;
}

/** urldb teardown fixture */
static void urldb_teardown(void)
{
	int scount = 0;

	corestrings_fini();

	LOG("Remaining lwc strings:");
	lwc_iterate_strings(urldb_lwc_iterator, &scount);
	ck_assert_int_eq(scount, 0);
}

START_TEST(urldb_original_test)
{
	struct host_part *h;
	struct path_data *p;
	const struct url_data *u;
	lwc_string *scheme;
	lwc_string *fragment;
	nsurl *url;
	nsurl *urlr;
	char *path_query;

	h = urldb_add_host("127.0.0.1");
	if (!h) {
		LOG("failed adding host");
		return 1;
	}

	h = urldb_add_host("intranet");
	if (!h) {
		LOG("failed adding host");
		return 1;
	}

	url = make_url("http://intranet/");
	scheme = nsurl_get_component(url, NSURL_SCHEME);
	p = urldb_add_path(scheme, 0, h, strdup("/"), NULL, url);
	if (!p) {
		LOG("failed adding path");
		return 1;
	}
	lwc_string_unref(scheme);

	urldb_set_url_title(url, "foo");

	u = urldb_get_url_data(url);
	assert(u && strcmp(u->title, "foo") == 0);
	nsurl_unref(url);

	/* Get host entry */
	h = urldb_add_host("netsurf.strcprstskrzkrk.co.uk");
	if (!h) {
		LOG("failed adding host");
		return 1;
	}

	/* Get path entry */
	url = make_url("http://netsurf.strcprstskrzkrk.co.uk/path/to/resource.htm?a=b");
	scheme = nsurl_get_component(url, NSURL_SCHEME);
	path_query = make_path_query(url);
	fragment = make_lwc("zz");
	p = urldb_add_path(scheme, 0, h, strdup(path_query), fragment, url);
	if (!p) {
		LOG("failed adding path");
		return 1;
	}
	lwc_string_unref(fragment);

	fragment = make_lwc("aa");
	p = urldb_add_path(scheme, 0, h, strdup(path_query), fragment, url);
	if (!p) {
		LOG("failed adding path");
		return 1;
	}
	lwc_string_unref(fragment);

	fragment = make_lwc("yy");
	p = urldb_add_path(scheme, 0, h, strdup(path_query), fragment, url);
	if (!p) {
		LOG("failed adding path");
		return 1;
	}
	free(path_query);
	lwc_string_unref(fragment);
	lwc_string_unref(scheme);
	nsurl_unref(url);

	url = make_url("file:///home/");
	urldb_add_url(url);
	nsurl_unref(url);

	url = make_url("http://www.minimarcos.org.uk/cgi-bin/forum/Blah.pl?,v=login,p=2");
	urldb_set_cookie("mmblah=foo; path=/; expires=Thur, 31-Dec-2099 00:00:00 GMT\r\n", url, NULL);
	nsurl_unref(url);

	url = make_url("http://www.minimarcos.org.uk/cgi-bin/forum/Blah.pl?,v=login,p=2");
	urldb_set_cookie("BlahPW=bar; path=/; expires=Thur, 31-Dec-2099 00:00:00 GMT\r\n", url, NULL);
	nsurl_unref(url);

	url = make_url("http://ccdb.cropcircleresearch.com/");
	urldb_set_cookie("details=foo|bar|Sun, 03-Jun-2007;expires=Mon, 24-Jul-2006 09:53:45 GMT\r\n", url, NULL);
	nsurl_unref(url);

	url = make_url("http://www.google.com/");
	urldb_set_cookie("PREF=ID=a:TM=b:LM=c:S=d; path=/; domain=.google.com\r\n", url, NULL);
	nsurl_unref(url);

	url = make_url("http://www.bbc.co.uk/");
	urldb_set_cookie("test=foo, bar, baz; path=/, quux=blah; path=/", url, NULL);
	nsurl_unref(url);

//	urldb_set_cookie("a=b; path=/; domain=.a.com", "http://a.com/", NULL);

	url = make_url("https://www.foo.com/blah/moose");
	urlr = make_url("https://www.foo.com/blah/moose");
	urldb_set_cookie("foo=bar;Path=/blah;Secure\r\n", url, urlr);
	nsurl_unref(url);
	nsurl_unref(urlr);

	url = make_url("https://www.foo.com/blah/wxyzabc");
	urldb_get_cookie(url, true);
	nsurl_unref(url);

	/* Mantis bug #993 */
	url = make_url("http:moodle.org");
	ck_assert(urldb_add_url(url) == true);
	ck_assert(urldb_get_url(url) != NULL);
	nsurl_unref(url);

	/* Mantis bug #993 */
	url = make_url("http://a_a/");
	ck_assert(urldb_add_url(url));
	ck_assert(urldb_get_url(url));
	nsurl_unref(url);

	/* Mantis bug #996 */
	url = make_url("http://foo@moose.com/");
	if (urldb_add_url(url)) {
		LOG("added %s", nsurl_access(url));
		ck_assert(urldb_get_url(url) != NULL);
	}
	nsurl_unref(url);

	/* Mantis bug #913 */
	url = make_url("http://www2.2checkout.com/");
	ck_assert(urldb_add_url(url));
	ck_assert(urldb_get_url(url));
	nsurl_unref(url);

	/* Numeric subdomains */
	url = make_url("http://2.bp.blogspot.com/_448y6kVhntg/TSekubcLJ7I/AAAAAAAAHJE/yZTsV5xT5t4/s1600/covers.jpg");
	ck_assert(urldb_add_url(url));
	ck_assert(urldb_get_url(url));
	nsurl_unref(url);

	/* Valid path */
	ck_assert(test_urldb_set_cookie("name=value;Path=/\r\n", "http://www.google.com/", NULL));

	/* Valid path (non-root directory) */
	ck_assert(test_urldb_set_cookie("name=value;Path=/foo/bar/\r\n", "http://www.example.org/foo/bar/", NULL));

	/* Defaulted path */
	ck_assert(test_urldb_set_cookie("name=value\r\n", "http://www.example.org/foo/bar/baz/bat.html", NULL));
	ck_assert(test_urldb_get_cookie("http://www.example.org/foo/bar/baz/quux.htm"));

	/* Defaulted path with no non-leaf path segments */
	ck_assert(test_urldb_set_cookie("name=value\r\n", "http://no-non-leaf.example.org/index.html", NULL));
	ck_assert(test_urldb_get_cookie("http://no-non-leaf.example.org/page2.html"));
	ck_assert(test_urldb_get_cookie("http://no-non-leaf.example.org/"));

	/* Valid path (includes leafname) */
	ck_assert(test_urldb_set_cookie("name=value;Version=1;Path=/index.cgi\r\n", "http://example.org/index.cgi", NULL));
	ck_assert(test_urldb_get_cookie("http://example.org/index.cgi"));

	/* Valid path (includes leafname in non-root directory) */
	ck_assert(test_urldb_set_cookie("name=value;Path=/foo/index.html\r\n", "http://www.example.org/foo/index.html", NULL));
	/* Should _not_ match the above, as the leafnames differ */
	ck_assert(test_urldb_get_cookie("http://www.example.org/foo/bar.html") == NULL);

	/* Invalid path (contains different leafname) */
	ck_assert(test_urldb_set_cookie("name=value;Path=/index.html\r\n", "http://example.org/index.htm", NULL) == false);

	/* Invalid path (contains leafname in different directory) */
	ck_assert(test_urldb_set_cookie("name=value;Path=/foo/index.html\r\n", "http://www.example.org/bar/index.html", NULL) == false);

	/* Test partial domain match with IP address failing */
	ck_assert(test_urldb_set_cookie("name=value;Domain=.foo.org\r\n", "http://192.168.0.1/", NULL) == false);

	/* Test handling of non-domain cookie sent by server (domain part should
	 * be ignored) */
	ck_assert(test_urldb_set_cookie("foo=value;Domain=blah.com\r\n", "http://www.example.com/", NULL));
	ck_assert(strcmp(test_urldb_get_cookie("http://www.example.com/"), "foo=value") == 0);

	/* Test handling of domain cookie from wrong host (strictly invalid but
	 * required to support the real world) */
	ck_assert(test_urldb_set_cookie("name=value;Domain=.example.com\r\n", "http://foo.bar.example.com/", NULL));
	ck_assert(strcmp(test_urldb_get_cookie("http://www.example.com/"), "foo=value; name=value") == 0);

	/* Test presence of separators in cookie value */
	ck_assert(test_urldb_set_cookie("name=\"value=foo\\\\bar\\\\\\\";\\\\baz=quux\";Version=1\r\n", "http://www.example.org/", NULL));
	ck_assert(strcmp(test_urldb_get_cookie("http://www.example.org/"), "$Version=1; name=\"value=foo\\\\bar\\\\\\\";\\\\baz=quux\"") == 0);

	/* Test cookie with blank value */
	ck_assert(test_urldb_set_cookie("a=\r\n", "http://www.example.net/", NULL));
	ck_assert(strcmp(test_urldb_get_cookie("http://www.example.net/"), "a=") == 0);

	/* Test specification of multiple cookies in one header */
	ck_assert(test_urldb_set_cookie("a=b, foo=bar; Path=/\r\n", "http://www.example.net/", NULL));
	ck_assert(strcmp(test_urldb_get_cookie("http://www.example.net/"), "a=b; foo=bar") == 0);

	/* Test use of separators in unquoted cookie value */
	ck_assert(test_urldb_set_cookie("foo=moo@foo:blah?moar\\ text\r\n", "http://example.com/", NULL));
	ck_assert(strcmp(test_urldb_get_cookie("http://example.com/"), "foo=moo@foo:blah?moar\\ text; name=value") == 0);

	/* Test use of unnecessary quotes */
	ck_assert(test_urldb_set_cookie("foo=\"hello\";Version=1,bar=bat\r\n", "http://example.com/", NULL));
	ck_assert(strcmp(test_urldb_get_cookie("http://example.com/"), "foo=\"hello\"; bar=bat; name=value") == 0);

	/* Test domain matching in unverifiable transactions */
	ck_assert(test_urldb_set_cookie("foo=bar; domain=.example.tld\r\n", "http://www.foo.example.tld/", "http://bar.example.tld/"));
	ck_assert(strcmp(test_urldb_get_cookie("http://www.foo.example.tld/"), "foo=bar") == 0);

	/* Test expiry */
	ck_assert(test_urldb_set_cookie("foo=bar", "http://expires.com/", NULL));
	ck_assert(strcmp(test_urldb_get_cookie("http://expires.com/"), "foo=bar") == 0);
	ck_assert(test_urldb_set_cookie("foo=bar; expires=Thu, 01-Jan-1970 00:00:01 GMT\r\n", "http://expires.com/", NULL));
	ck_assert(test_urldb_get_cookie("http://expires.com/") == NULL);

	urldb_dump();
	urldb_destroy();
}
END_TEST

TCase *urldb_original_case_create(void)
{
	TCase *tc;
	tc = tcase_create("Original urldb tests");

	/* ensure corestrings are initialised and finalised for every test */
	tcase_add_checked_fixture(tc,
				  urldb_create,
				  urldb_teardown);

	tcase_add_test(tc, urldb_original_test);

	return tc;
}

START_TEST(urldb_session_test)
{
	nserror res;

	res = urldb_load(test_urldb_path);
	ck_assert_int_eq(res, NSERROR_OK);

	urldb_destroy();
}
END_TEST

TCase *urldb_session_case_create(void)
{
	TCase *tc;
	tc = tcase_create("Full session");

	/* ensure corestrings are initialised and finalised for every test */
	tcase_add_checked_fixture(tc,
				  urldb_create,
				  urldb_teardown);

	tcase_add_test(tc, urldb_session_test);

	return tc;
}

/**
 * Test urldb_add_host asserting on NULL.
 */
START_TEST(urldb_api_add_host_assert_test)
{
	struct host_part *res;
	res = urldb_add_host(NULL);
	ck_assert(res == NULL);
}
END_TEST


TCase *urldb_api_case_create(void)
{
	TCase *tc;
	tc = tcase_create("API checks");

	tcase_add_test_raise_signal(tc,
				    urldb_api_add_host_assert_test,
				    6);

	return tc;
}


Suite *urldb_suite_create(void)
{
	Suite *s;
	s = suite_create("URLDB");

	suite_add_tcase(s, urldb_api_case_create());
	suite_add_tcase(s, urldb_session_case_create());
	suite_add_tcase(s, urldb_original_case_create());

	return s;
}

int main(int argc, char **argv)
{
	int number_failed;
	SRunner *sr;

	sr = srunner_create(urldb_suite_create());

	srunner_run_all(sr, CK_ENV);

	number_failed = srunner_ntests_failed(sr);
	srunner_free(sr);

	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
