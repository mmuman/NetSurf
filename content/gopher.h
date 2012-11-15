/*
 * Copyright 2011 François Revol <mmu_man@users.sourceforge.net>
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

/** \file
 * Generate HTML content for displaying gopher directory listings (interface).
 */


#ifndef NETSURF_CONTENT_GOPHER_H
#define NETSURF_CONTENT_GOPHER_H

#include <stddef.h>

typedef enum {
	GOPHER_TYPE_NONE	= 0,	/* none set */
	/* these come from http://tools.ietf.org/html/rfc1436 */
	GOPHER_TYPE_TEXTPLAIN	= '0',
	GOPHER_TYPE_DIRECTORY	= '1',
	GOPHER_TYPE_CSO_SEARCH	= '2',
	GOPHER_TYPE_ERROR	= '3',	/* error message */
	GOPHER_TYPE_BINHEX	= '4',	/* binhex encoded text */
	GOPHER_TYPE_BINARCHIVE	= '5',	/* binary archive file */
	GOPHER_TYPE_UUENCODED	= '6',	/* uuencoded text */
	GOPHER_TYPE_QUERY	= '7',	/* search query */
	GOPHER_TYPE_TELNET	= '8',	/* telnet link */
	GOPHER_TYPE_BINARY	= '9',
	GOPHER_TYPE_GIF		= 'g',	/* GIF image */
	GOPHER_TYPE_HTML	= 'h',	/* HTML file or URL */
	GOPHER_TYPE_INFO	= 'i',	/* information text */
	GOPHER_TYPE_IMAGE	= 'I',	/* image (depends, usually jpeg) */
	GOPHER_TYPE_AUDIO	= 's',	/* audio (wav?) */
	GOPHER_TYPE_TN3270	= 'T',	/* tn3270 session */
	/* those are not standardized */
	GOPHER_TYPE_PDF		= 'd',	/* seems to be only for PDF files */
	GOPHER_TYPE_PNG		= 'p'	/* cf. gopher://namcub.accelera-labs.com/1/pics */
} gopher_item_type;

struct gopher_state {
	char type;	/**< Gopher document type */
	nsurl *url;	/**< the fetched URL */
	struct fetch *fetch_handle;	/**< Copy of the fetch handle */
	bool head_done;	/**< We already sent the <head> part */
	size_t cached;	/**< Amount of cached data in the input buffer */
	char input[2048];	/**< input buffer */
	
};

struct gopher_state *gopher_state_create(nsurl *url, struct fetch *fetch_handle);
void gopher_state_free(struct gopher_state *s);

size_t gopher_fetch_data(struct gopher_state *s, char *data, size_t size);

const char *gopher_type_to_mime(char type);
bool gopher_need_generate(char type);


#endif
