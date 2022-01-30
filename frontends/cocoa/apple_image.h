/*
 * Copyright 2011 Sven Weidauer <sven.weidauer@gmail.com>
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


#ifndef _NETSURF_COCOA_APPLE_IMAGE_H_
#define _NETSURF_COCOA_APPLE_IMAGE_H_

#include "utils/config.h"
#include "utils/errors.h"

#ifdef WITH_APPLE_IMAGE

/**
 * Initialise apple image handlers instead of generic core ones.
 */
nserror apple_image_init(void);

#else

#define apple_image_init() NSERROR_OK

#endif /* WITH_APPLE_IMAGE */

#endif
