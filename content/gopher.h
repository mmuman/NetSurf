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

/** Type of Gopher items */
typedef enum {
	GOPHER_TYPE_NONE	= 0,	/**< none set */
	GOPHER_TYPE_ENDOFPAGE	= '.',	/**< a dot alone on a line */
	/* these come from http://tools.ietf.org/html/rfc1436 */
	GOPHER_TYPE_TEXTPLAIN	= '0',	/**< text/plain */
	GOPHER_TYPE_DIRECTORY	= '1',	/**< gopher directory */
	GOPHER_TYPE_CSO_SEARCH	= '2',	/**< CSO search */
	GOPHER_TYPE_ERROR	= '3',	/**< error message */
	GOPHER_TYPE_BINHEX	= '4',	/**< binhex encoded text */
	GOPHER_TYPE_BINARCHIVE	= '5',	/**< binary archive file */
	GOPHER_TYPE_UUENCODED	= '6',	/**< uuencoded text */
	GOPHER_TYPE_QUERY	= '7',	/**< gopher search query */
	GOPHER_TYPE_TELNET	= '8',	/**< telnet link */
	GOPHER_TYPE_BINARY	= '9',	/**< generic binary */
	GOPHER_TYPE_DUPSERV	= '+',	/**< duplicated server */
	GOPHER_TYPE_GIF		= 'g',	/**< GIF image */
	GOPHER_TYPE_IMAGE	= 'I',	/**< image (depends, usually jpeg) */
	GOPHER_TYPE_TN3270	= 'T',	/**< tn3270 session */
	/* not standardized but widely used,
	 * cf. http://en.wikipedia.org/wiki/Gopher_%28protocol%29#Gopher_item_types
	 */
	GOPHER_TYPE_HTML	= 'h',	/**< HTML file or URL */
	GOPHER_TYPE_INFO	= 'i',	/**< information text */
	GOPHER_TYPE_AUDIO	= 's',	/**< audio (wav?) */
	/* not standardized, some servers use them */
	GOPHER_TYPE_PDF_ALT	= 'd',	/**< seems to be only for PDF files */
	GOPHER_TYPE_PNG		= 'p',	/**< PNG image */
		/* cf. gopher://namcub.accelera-labs.com/1/pics */
	GOPHER_TYPE_MIME	= 'M',	/**< multipart/mixed MIME data */
		/* cf. http://www.pms.ifi.lmu.de/mitarbeiter/ohlbach/multimedia/IT/IBMtutorial/3376c61.html */
	/* cf. http://nofixedpoint.motd.org/2011/02/22/an-introduction-to-the-gopher-protocol/ */
	GOPHER_TYPE_PDF		= 'P',	/**< PDF file */
	GOPHER_TYPE_BITMAP	= ':',	/**< Bitmap image (Gopher+) */
	GOPHER_TYPE_MOVIE	= ';',	/**< Movie (Gopher+) */
	GOPHER_TYPE_SOUND	= ';',	/**< Sound (Gopher+) */
	GOPHER_TYPE_CALENDAR	= 'c',	/**< Calendar */
	GOPHER_TYPE_EVENT	= 'e',	/**< Event */
	GOPHER_TYPE_MBOX	= 'm',	/**< mbox file */
} gopher_item_type;

/** gopher-specific page state */
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

const char *gopher_type_to_mime(gopher_item_type type);
bool gopher_need_generate(gopher_item_type type);


#endif
