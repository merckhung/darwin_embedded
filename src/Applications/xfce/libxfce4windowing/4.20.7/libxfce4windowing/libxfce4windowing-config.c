/*
 * Copyright (c) 2022 Brian Tarricone <brian@tarricone.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

/**
 * SECTION:libxfce4windowing-config
 * @title: Library Configuration
 * @short_description: libxfce4windowing config macros
 * @stability: Stable
 * @include: libxfce4windowing/libxfce4windowing.h
 *
 * Variables and functions to check the libxfce4windowing version.
 **/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "libxfce4windowing-config.h"
#include "libxfce4windowing-visibility.h"

/**
 * libxfce4windowing_major_version:
 *
 * A constat that evaluates to the major version of libxfce4windowing.
 *
 */
const guint libxfce4windowing_major_version = LIBXFCE4WINDOWING_MAJOR_VERSION;

/**
 * libxfce4windowing_minor_version:
 *
 * A constat that evaluates to the minor version of libxfce4windowing.
 *
 */
const guint libxfce4windowing_minor_version = LIBXFCE4WINDOWING_MINOR_VERSION;

/**
 * libxfce4windowing_micro_version:
 *
 * A constat that evaluates to the micro version of libxfce4windowing.
 *
 */
const guint libxfce4windowing_micro_version = LIBXFCE4WINDOWING_MICRO_VERSION;

/**
 * libxfce4windowing_check_version:
 * @required_major: the required major version.
 * @required_minor: the required minor version.
 * @required_micro: the required micro version.
 *
 * Checks that the libxfce4windowing library
 * in use is compatible with the given version. Generally you would pass in
 * the constants #LIBXFCE4WINDOWING_MAJOR_VERSION, #LIBXFCE4WINDOWING_MINOR_VERSION and
 * #LIBXFCE4WINDOWING_MICRO_VERSION as the three arguments to this function; that produces
 * a check that the library in use is compatible with the version of
 * libxfce4windowing the extension was compiled against.
 *
 * |[<!-- language="C" -->
 * const gchar *mismatch;
 * mismatch = libxfce4windowing_check_version(LIBXFCE4WINDOWING_MAJOR_VERSION,
 *                                            LIBXFCE4WINDOWING_MINOR_VERSION,
 *                                            LIBXFCE4WINDOWING_MICRO_VERSION);
 * if (G_UNLIKELY(mismatch != NULL)) {
 *   g_error("Version mismatch: %s", mismatch);
 * }
 * ]|
 *
 * Return value: (nullable) (transfer none): %NULL if the library is compatible
 * with the given version, or a string describing the version mismatch. The
 * returned string is owned by the library and must not be freed or modified by
 * the caller.
 **/
const gchar *
libxfce4windowing_check_version(guint required_major,
                                guint required_minor,
                                guint required_micro) {
    if (required_major > LIBXFCE4WINDOWING_MAJOR_VERSION) {
        return "Libxfce4windowing version too old (major mismatch)";
    } else if (required_major < LIBXFCE4WINDOWING_MAJOR_VERSION) {
        return "Libxfce4windowing version too new (major mismatch)";
    } else if (required_minor > LIBXFCE4WINDOWING_MINOR_VERSION) {
        return "Libxfce4windowing version too old (minor mismatch)";
    } else if (required_minor == LIBXFCE4WINDOWING_MINOR_VERSION && required_micro > LIBXFCE4WINDOWING_MICRO_VERSION) {
        return "Libxfce4windowing version too old (micro mismatch)";
    } else {
        return NULL;
    }
}

#define __LIBXFCE4WINDOWING_CONFIG_C__
#include "libxfce4windowing-visibility.c"
