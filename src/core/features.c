/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi
 *
 * Header parsing routines.
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

#include "common.h"

RCSID("$Id$");

#include "features.h"

#include "lib/walloc.h"
#include "lib/header.h"
#include "lib/misc.h"
#include "lib/glib-missing.h"

#include "if/gnet_property_priv.h"

#include "lib/override.h"		/* Must be the last header included */

struct header_x_feature {
	gchar *name;
	int major;
	int minor;
};

xfeatures_t xfeatures;

/***
 *** X-Features header parsing utilities
 ***/

/*
 * features_close
 */
void features_close()
{
	header_features_cleanup(&xfeatures.uploads);
	header_features_cleanup(&xfeatures.downloads);
	header_features_cleanup(&xfeatures.connections);
}

/*
 * header_features_add
 *
 * Add support for feature_name with the specified version to the X-Features
 * header.
 */
void header_features_add(struct xfeature_t *xfeatures,
	gchar *feature_name,
	int feature_version_major,
	int feature_version_minor)
{
	struct header_x_feature *feature = walloc(sizeof(*feature));

	feature->name = g_strdup(feature_name);
	feature->major = feature_version_major;
	feature->minor = feature_version_minor;

	xfeatures->features = g_list_append(xfeatures->features, feature);
}

/*
 * header_features_cleanup
 *
 * Removes all memory used by the header_features_add.
 */
void header_features_cleanup(struct xfeature_t *xfeatures)
{
	GList *cur;
	for(cur = g_list_first(xfeatures->features);
		cur != g_list_last(xfeatures->features);
		cur = g_list_next(cur)) {

		struct header_x_feature *feature =
			(struct header_x_feature *) cur->data;

		G_FREE_NULL(feature->name);
		wfree(feature, sizeof(*feature));
	}
}

/*
 * header_features_generate
 *
 * Adds the X-Features header to a HTTP request.
 * buf should point to the beginning of the header, *rw should contain the
 * number of bytes that were allready written. type should be the type of which
 * we should include in the X-Features header.
 * *rw is changed too *rw + bytes written
 */
void header_features_generate(struct xfeature_t *xfeatures,
	gchar *buf, size_t len, size_t *rw)
{
	static const char hdr[] = "X-Features";
	GList *cur;
	gpointer fmt;

	g_assert(len <= INT_MAX);
	g_assert(*rw <= INT_MAX);

	if (len - *rw < (sizeof(hdr) + sizeof(": \r\n") - 1))
		return;

	if (g_list_first(xfeatures->features) == NULL)
		return;

	fmt = header_fmt_make(hdr, ", ", len - *rw);

	for (
		cur = g_list_first(xfeatures->features);
		cur != NULL;
		cur = g_list_next(cur)
	) {
		gchar feature_version[50];
		struct header_x_feature *feature =
			(struct header_x_feature *) cur->data;

		gm_snprintf(feature_version, sizeof(feature_version), "%s/%d.%d",
			feature->name, feature->major, feature->minor);

		header_fmt_append_value(fmt, feature_version);
	}

	header_fmt_end(fmt);

	if ((size_t) header_fmt_length(fmt) < len - *rw) {
		*rw += gm_snprintf(&buf[*rw], len - *rw, "%s", header_fmt_string(fmt));
	}

	header_fmt_free(fmt);
}

/*
 * header_get_feature
 *
 * Retrieves the major and minor version from a feature in the X-Features
 * header, if no support was found both major and minor are 0.
 */
void header_get_feature(const gchar *feature_name, const header_t *header,
	int *feature_version_major, int *feature_version_minor)
{
	gchar *buf = NULL;
	gchar *start, *ep;
	gint error;
	gulong val;

	*feature_version_major = 0;
	*feature_version_minor = 0;

	buf = header_get(header, (const gchar *) "X-Features");

	/*
	 * We could also try to scan for the header: feature_name, so this would
     * make this function even more generic. But I would suggest another
     * function for this though.
     */

	if (buf == NULL) {
		/*
		 * Actually the 'specs' say we should assume it is supported if the
		 * X-Features header is not there. But I wouldn't count on it, and
		 * it was only for "legacy" attributes in the HTTP file exchange.
		 * Better safe than sorry.
		 */

		return;
	}

	/*
	 * We must locate the feature_name exactly, and not a subpart of another
	 * feature.  If we look for "bar", then we must not match on "foobar".
	 */

	for (start = buf;;) {
		gint pc;			/* Previous char */

		buf = strcasestr(buf, feature_name);

		if (buf == NULL)
			return;
		if (buf == start)
			break;

		pc = (gint) *(guchar *) (buf - 1);
		if (is_ascii_space(pc) || pc == ',' || pc == ';')
			break;			/* Found it! */

		/*
		 * Since we're looking for whole words separated by a space or the
		 * regular header punctuation, the next match can't occur before
		 * the end of the current string we matched...
		 */

		buf += strlen(feature_name);
	}

	buf += strlen(feature_name);		/* Should now be on the "/" sep */

	if (*buf != '/') {
		g_warning("[header] Malformed X-Features header, ignoring");
		if (dbg > 2)
			header_dump(header, stderr);

		return;
	}

	buf++;

	if (*buf == '\0')
		return;

	val = gm_atoul(buf, &ep, &error);
	if (error || val > INT_MAX)
		return;
	*feature_version_major = (gint) val;

	if (*ep != '.')
		return;

	buf = ++ep;
	val = gm_atoul(buf, &ep, &error);
	if (error || val > INT_MAX)
		return;
	*feature_version_minor = (gint) val;
}

/* vi: set ts=4: */
