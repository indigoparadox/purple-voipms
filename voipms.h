
/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef VOIPMS_H
#define VOIPMS_H

#ifndef G_GNUC_NULL_TERMINATED
#  if __GNUC__ >= 4
#     define G_GNUC_NULL_TERMINATED __attribute__((__sentinel__))
#  else
#     define G_GNUC_NULL_TERMINATED
#  endif /* __GNUC__ >= 4 */
#endif /* G_GNUC_NULL_TERMINATED */

#ifndef PURPLE_PLUGINS
#  define PURPLE_PLUGINS
#endif

#include "accountopt.h"
#include "blist.h"
#include "core.h"
#include "connection.h"
#include "debug.h"
#include "dnsquery.h"
#include "proxy.h"
#include "prpl.h"
#include "request.h"
#include "savedstatuses.h"
#include "sslconn.h"
#include "version.h"

#if GLIB_MAJOR_VERSION >= 2 && GLIB_MINOR_VERSION >= 12
#  define atoll(a) g_ascii_strtoll(a, NULL, 0)
#endif

#define VOIPMS_PLUGIN_ID "prpl-indigoparadox-voipms"
#define VOIPMS_PLUGIN_VERSION "14.6"
#define VOIPMS_PLUGIN_WEBSITE ""

#define VOIPMS_STATUS_ONLINE   "online"

static PurplePlugin *_voipms_protocol = NULL;

typedef void (*GcFunc)(
   PurpleConnection *from,
   PurpleConnection *to,
   gpointer userdata
);

typedef struct {
   GcFunc fn;
   PurpleConnection *from;
   gpointer userdata;
} GcFuncData;

typedef struct {
   char *from;
   char *message;
   time_t mtime;
   PurpleMessageFlags flags;
} GOfflineMessage;

#endif /* VOIPMS_H */

