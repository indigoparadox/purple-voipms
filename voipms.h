
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

#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <glib.h>
#include <curl/curl.h>
#include <json-glib/json-glib.h>

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
#define VOIPMS_PLUGIN_NAME "VOIP.ms SMS Protocol"
#define VOIPMS_PLUGIN_DEFAULT_API_URL "https://voip.ms/api/v1/rest.php"

#define VOIPMS_STATUS_ONLINE   "online"

#define VOIPMS_ERROR_SIZE CURL_ERROR_SIZE + 255
#define VOIPMS_DATE_BUFFER_SIZE 20
#define VOIPMS_MAX_AGE_DAYS 91
#define VOIPMS_DAY_SECONDS (60 * 60 * 24)
#define VOIPMS_POLL_SECONDS 1

typedef enum {
   VOIPMS_METHOD_SENDSMS,
   VOIPMS_METHOD_GETSMS,
   VOIPMS_METHOD_DELETESMS
} VOIPMS_METHOD;

typedef void (*GcFunc)(
   PurpleConnection *from,
   PurpleConnection *to,
   gpointer userdata
);

struct RequestMemoryStruct {
   char* memory;
   size_t size;
};

struct VoipMsAccount {
   guint timer; 
   CURLM* multi_handle;
   int still_running;
   gboolean requests_in_progress;
};

struct VoipMsMessage {
   gchar* id;
   gchar* contact;
   gchar* message;
   struct tm timeinfo;
   PurpleAccount* account;
};

struct VoipMsRequestData {
   VOIPMS_METHOD method;
   char* error_buffer;
   struct RequestMemoryStruct chunk;
   void* attachment;
};

struct VoipMsSendImData {
   const char* from_username;
   const char* to_username;
   const char* message;
   PurpleMessageFlags receive_flags;
   gboolean processed;
   gboolean success; /* TRUE for send success, set by request monitor. */
   gchar* error_buffer;
};

struct GcFuncData {
   GcFunc fn;
   PurpleConnection *from;
   gpointer userdata;
};

struct GcFuncDataMessageList {
   GList* messages;
   PurpleAccount* account;
};

#endif /* VOIPMS_H */

