
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

#include "voipms.h"

static void voipms_init( PurplePlugin* );
static void voipms_destroy( PurplePlugin* );

static PurplePlugin* _voipms_protocol = NULL;

static PurpleConnection* get_voipms_gc( const char* );
static void messages_foreach_process( JsonArray*, guint, JsonNode*, gpointer );
static void messages_foreach_serve( gpointer, gpointer );
static void messages_foreach_free( gpointer );

/* Requests */

static size_t voipms_api_request_write_body_callback(
   void* contents, size_t size, size_t nmemb, void* userp
) {
   size_t realsize = size * nmemb;
   struct RequestMemoryStruct* mem = (struct RequestMemoryStruct*)userp;

   mem->memory = realloc( mem->memory, mem->size + realsize + 1 );
   if( NULL == mem->memory ) {
      /* TODO: Alert to memory problems, somehow. */
      return 0;
   }

   memcpy( &(mem->memory[mem->size]), contents, realsize );
   mem->size += realsize;
   mem->memory[mem->size] = 0;

   return realsize;
}

static void voipms_api_request(
   VOIPMS_METHOD method, GSList* args, PurpleAccount* account, void* attachment
) {
   CURL* curl = NULL;
   GSList* arg_iter;
   char* api_url = NULL;
   size_t new_length = 1,
      old_length = 0;
   struct VoipMsAccount* proto_data = account->gc->proto_data;
   struct VoipMsRequestData* request_data;

   proto_data->requests_in_progress++;

   /* Setup some buffers and stuff. */
   request_data = calloc( 1, sizeof( struct VoipMsRequestData ) );
   request_data->method = method;
   request_data->attachment = attachment;
   request_data->error_buffer = calloc( CURL_ERROR_SIZE, sizeof( char ) );
   request_data->chunk.memory = calloc( 1, sizeof( char ) );
   request_data->chunk.size = 0;

   /* Add the credentials to the request. */
   args = g_slist_append( args, g_strdup_printf( "api_username=%s",
      purple_url_encode( account->username )
   ) );
   args = g_slist_append( args, g_strdup_printf( "api_password=%s",
      purple_url_encode( account->password )
   ) );

   /* Add the method to the request. */
   switch( method ) {
      case VOIPMS_METHOD_GETSMS:
         args = g_slist_append( args, g_strdup( "method=getSMS" ) );
         break;

      case VOIPMS_METHOD_SENDSMS:
         args = g_slist_append( args, g_strdup( "method=sendSMS" ) );
         break;

      case VOIPMS_METHOD_DELETESMS:
         args = g_slist_append( args, g_strdup( "method=deleteSMS" ) );
         break;
   }

   /* Build the query string. */
   arg_iter = args;
   api_url = g_strdup_printf( "%s?", purple_account_get_string(
      account, "api_url", VOIPMS_PLUGIN_DEFAULT_API_URL
   ) );
   new_length = strlen( api_url ) + 1;
   old_length = strlen( api_url );
   while( NULL != arg_iter ) {
      /* Append each arg to the query string. */
      /* TODO: Use glib functions for this? */
      new_length += strlen( arg_iter->data );
      api_url = realloc( api_url, new_length );
      memcpy(
         &(api_url[old_length]), arg_iter->data, strlen( arg_iter->data )
      );
      api_url[new_length - 1] = 0;
      old_length = new_length - 1;

      /* If NULL != g_slist_next() then append & to the query string. */
      if( NULL != g_slist_next( arg_iter ) ) {
         new_length += 1;
         old_length += 1;
         api_url = realloc( api_url, new_length );
         api_url[new_length - 2] = '&';
         api_url[new_length - 1] = 0;
      }

      arg_iter = g_slist_next( arg_iter );
   }

   /* Setup the request. */
   curl = curl_easy_init();
   curl_easy_setopt( curl, CURLOPT_URL, api_url );
   curl_easy_setopt(
      curl, CURLOPT_WRITEFUNCTION, voipms_api_request_write_body_callback
   );
   curl_easy_setopt( curl, CURLOPT_PRIVATE, request_data );
   curl_easy_setopt( curl, CURLOPT_WRITEDATA, &(request_data->chunk) );
   curl_easy_setopt( curl, CURLOPT_ERRORBUFFER, request_data->error_buffer );
   curl_easy_setopt( curl, CURLOPT_FAILONERROR, 1 );

   curl_multi_add_handle( proto_data->multi_handle, curl );

   if( NULL != api_url ) {
      free( api_url );
   }

   g_slist_free_full( args, g_free );

}

static void voipms_api_request_progress( PurpleAccount* account ) {
   struct VoipMsAccount* proto_data = account->gc->proto_data;
   int running_previously = proto_data->still_running,
      queue_count;
   struct CURLMsg* msg = NULL;
   struct VoipMsRequestData* request_data = NULL;
   JsonParser* parser = NULL;
   JsonNode* root = NULL;
   JsonObject* response = NULL;
   JsonArray* messages = NULL;
   const gchar* status = NULL;
   struct GcFuncDataMessageList message_list = { NULL, account };
   struct VoipMsSendImData* send_im_data = NULL;

   curl_multi_perform( proto_data->multi_handle, &(proto_data->still_running) );

   if( running_previously > proto_data->still_running ) {
      /* Nothing to process. */
      goto api_request_progress_cleanup;
   }

   #if 0
   purple_debug_info( "voipms", "Checking requests...\n" );
   #endif

   msg = curl_multi_info_read( proto_data->multi_handle, &queue_count );

   if( NULL == msg || CURLMSG_DONE != msg->msg ) {
      /* Don't know how to handle this. */
      goto api_request_progress_cleanup;
   }

   curl_easy_getinfo(
      msg->easy_handle, CURLINFO_PRIVATE, (char*)(&request_data)
   );

   /* A valid request has finished, at any rate. */
   proto_data->requests_in_progress--;

   if( NULL == request_data ) {
      /* Don't know how to handle this. */
      purple_debug_info(
         "voipms", "NULL request_data returned for request.\n"
      );
      goto api_request_progress_curl_cleanup;
   }

   /* Prepare the kind of attachment we'll be using. */
   switch( request_data->method ) {
      case VOIPMS_METHOD_SENDSMS:
         send_im_data = (struct VoipMsSendImData*)(request_data->attachment);
         send_im_data->processed = TRUE;
         break;

      default:
         break;
   }

   /* Parse the JSON response. */
   parser = json_parser_new();
   if( !json_parser_load_from_data(
      parser,
      request_data->chunk.memory,
      request_data->chunk.size,
      NULL
   ) ) {
      purple_debug_error(
         "voipms",
         "Error parsing response: %s\n",
         request_data->chunk.memory
      );
      goto api_request_progress_curl_cleanup;
   }
   root = json_parser_get_root( parser );

   /* TODO: Make sure the response was successful. */
   if( NULL == root ) {
      if( NULL != send_im_data ) {
         send_im_data->error_buffer = g_strdup_printf(
            "Error parsing response: %s\n", request_data->chunk.memory
         );
      }
      purple_debug_error(
         "voipms",
         "Error parsing response: %s\n",
         request_data->chunk.memory
      );
      goto api_request_progress_curl_cleanup;
   }

   response = json_node_get_object( root );

   /* Get the status of the request. */
   status = json_object_get_string_member( response, "status" );
   if( strcmp( status, "success" ) ) {
      purple_debug_error( "voipms", "Request status: %s\n", status );
      if( NULL != send_im_data ) {
         send_im_data->error_buffer = g_strdup( request_data->error_buffer );
      }
      goto api_request_progress_curl_cleanup;
   }

   switch( request_data->method ) {
      case VOIPMS_METHOD_GETSMS:
         /* Parse the messages and reverse them so that newest come last      *
          * before we serve them.                                             */
         messages = json_object_get_array_member( response, "sms" );  
         json_array_foreach_element(
            messages, messages_foreach_process, &message_list
         );
         message_list.messages = g_list_reverse( message_list.messages );
         g_list_foreach( message_list.messages, messages_foreach_serve, NULL );
         break;

      case VOIPMS_METHOD_SENDSMS:
         send_im_data = (struct VoipMsSendImData*)(request_data->attachment);
         send_im_data->success = TRUE;
         break;

      default:
         break;
   }

api_request_progress_curl_cleanup:

   /* Cleanup the handle we were just working with. */
   curl_multi_remove_handle( proto_data->multi_handle, msg->easy_handle );
   curl_easy_cleanup( msg->easy_handle );

api_request_progress_cleanup:

   if( NULL != parser ) {
      g_object_unref( parser );
   }

   if( NULL != request_data ) {
      free( request_data->chunk.memory );
      if( NULL != request_data->error_buffer ) {
         free( request_data->error_buffer );
      }
      
      /* Don't free the send_im_data attachment here, since we use it to send *
       * back success flag.                                                   */
   }

   g_list_free_full( message_list.messages, messages_foreach_free );

   return;
}

/* Helpers */

static gchar* str_replace( gchar* string, const gchar* from, const gchar* to ) {
   gchar* new_string;

   //while( NULL != strstr( string, from ) ) {
      new_string = g_strjoinv( to, g_strsplit( string, from, -1 ) );
   //}

   return new_string;
}

static PurpleConnection* get_voipms_gc( const char* username ) {
   PurpleAccount* acct = purple_accounts_find( username, VOIPMS_PLUGIN_ID );
   if( acct && purple_account_is_connected( acct ) ) {
      return acct->gc;
   } else {
      return NULL;
   }
}

static void messages_foreach_free( gpointer data ) {
   struct VoipMsMessage* message = (struct VoipMsMessage*)data;

   if( NULL == message || NULL == message->message ) {
      return;
   }

   g_free( message->id );
   g_free( message->contact );
   g_free( message->message );
}

static void messages_foreach_serve( gpointer data, gpointer user_data ) {
   struct VoipMsMessage* message = (struct VoipMsMessage*)data;
   GSList* api_args = NULL;
   struct VoipMsAccount* proto_data = message->account->gc->proto_data;
   
   /* Pass the message on to the user. */
   serv_got_im(
      message->account->gc,
      message->contact,
      message->message,
      PURPLE_MESSAGE_RECV,
      mktime( &(message->timeinfo) )
   );

   /* Delete the message from the server. */
   if( !purple_account_get_bool( message->account, "delete", TRUE ) ) {
      goto messages_serve_cleanup;
   }

   /* Build and send the API request. */
   api_args = g_slist_append(
      api_args,
      g_strdup_printf( "id=%s", message->id )
   );

   voipms_api_request(
      VOIPMS_METHOD_DELETESMS, api_args, message->account, NULL
   );

   /* Block until request completes. */
   while( proto_data->still_running ) {
      voipms_api_request_progress( message->account );
      /* TODO: Delay for a second or so. */
   }

messages_serve_cleanup:

   return;
}

static void messages_foreach_process(
   JsonArray* array, guint index_, JsonNode* element_node, gpointer user_data
) {
   struct GcFuncDataMessageList* gcfdata =
      (struct GcFuncDataMessageList*)user_data;
   JsonObject* message_json;
   struct VoipMsMessage* message;
   const gchar* date;

   /* Parse/translate message metadata. */
   message = calloc( 1, sizeof( struct VoipMsMessage ) );
   message_json = json_node_get_object( element_node );
   message->id =
      g_strdup( json_object_get_string_member( message_json, "id" ) );
   date = json_object_get_string_member( message_json, "date" );
   message->contact = 
      g_strdup( json_object_get_string_member( message_json, "contact" ) );
   message->message = 
      g_strdup( json_object_get_string_member( message_json, "message" ) );
   message->account = gcfdata->account;
   strptime( date, "%Y-%m-%d %H:%M:%S", &(message->timeinfo) );

   gcfdata->messages = g_list_append( gcfdata->messages, message );
}

static gboolean voipms_messages_timer( PurpleAccount* acct ) {
   time_t to_rawtime,
      from_rawtime;
   struct tm* to_timeinfo,
      * from_timeinfo;
   char from_filter_date[VOIPMS_DATE_BUFFER_SIZE] = { 0 },
      to_filter_date[VOIPMS_DATE_BUFFER_SIZE] = { 0 };
   GSList* api_args = NULL;
   struct VoipMsAccount* proto_data = acct->gc->proto_data;

   /* Calculate as wide a range as the API will allow us. */
   /* TODO: Use glib functions for this? */
   time( &from_rawtime );
   from_rawtime -= VOIPMS_DAY_SECONDS * VOIPMS_MAX_AGE_DAYS;
   from_timeinfo = localtime( &from_rawtime );
   strftime( from_filter_date, VOIPMS_DATE_BUFFER_SIZE, "%F", from_timeinfo );

   time( &to_rawtime );
   to_timeinfo = localtime( &to_rawtime );
   strftime( to_filter_date, VOIPMS_DATE_BUFFER_SIZE, "%F", to_timeinfo );

   /* Build and send the API request. */
   api_args = g_slist_append(
      api_args,
      g_strdup_printf( "from=%s", from_filter_date )
   );
   api_args = g_slist_append( 
      api_args,
      g_strdup_printf(
         "to=%s", to_filter_date
      )
   );
   api_args = g_slist_append( api_args, g_strdup( "type=1" ) );
   api_args = g_slist_append(
      api_args,
      g_strdup_printf(
         "did=%s", purple_account_get_string( acct, "did", "" )
      )
   );

   /* Don't pile on getSMS requests. */
   if( !proto_data->requests_in_progress ) {
      purple_debug_info( "voipms", "Polling the server for messages...\n" );
      voipms_api_request( VOIPMS_METHOD_GETSMS, api_args, acct, NULL );
   }

   /* Check on all the requests so far. */
   voipms_api_request_progress( acct );

   return TRUE;
}

static void voipms_refresh_buddies( PurpleAccount* acct ) {
   PurpleBlistNode* blist_node;
   const char* default_status;

   /* Allow selecting between "online" and "away" status by default. */
   default_status = purple_account_get_string(
      acct, "default_status", VOIPMS_STATUS_ONLINE
   );

   /* Just set everyone online all the time. */
   for(
      blist_node = purple_blist_get_root();
      NULL != blist_node;
      blist_node = purple_blist_node_next( blist_node, FALSE )
   ) {
      if( !PURPLE_BLIST_NODE_IS_BUDDY( blist_node ) ) {
         continue;
      }

      purple_prpl_got_user_status(
         acct,
         PURPLE_BLIST_NODE_NAME( blist_node ),
         default_status,
         NULL
      );
   }
}

static void voipms_login( PurpleAccount* acct ) {
   PurpleConnection* gc = purple_account_get_connection( acct );
   struct VoipMsAccount* vmsa;

   /* Setup the protocol data section. */
   vmsa = calloc( 1, sizeof( struct VoipMsAccount ) );
   acct->gc->proto_data = vmsa;
 
   purple_debug_info( "voipms", "Logging in %s...\n", acct->username );
 
   purple_connection_update_progress(
      gc,
      "Connecting",
      0, /* Which connection step this is. */
      2  /* Total number of steps. */
   );

   /* Setup the CURL multi handle. */
   vmsa->multi_handle = curl_multi_init();
 
   purple_connection_update_progress(
      gc,
      "Connected",
      1, /* Which connection step this is. */
      2  /* Total number of steps. */
   );

   purple_connection_set_state( gc, PURPLE_CONNECTED );
 
   voipms_refresh_buddies( gc->account );

   /* Start polling for new messages. */
   vmsa->timer = purple_timeout_add_seconds(
      VOIPMS_POLL_SECONDS, (GSourceFunc)voipms_messages_timer, acct
   );
}

static void voipms_close( PurpleConnection* gc ) {
   struct VoipMsAccount* vmsa = gc->proto_data;

   /* Stop polling for new messages. */
   if( vmsa->timer ) {
      purple_timeout_remove( vmsa->timer );
   }

   /* Shut down CURL. */
   curl_multi_cleanup( vmsa->multi_handle );
}

static int voipms_send_im(
   PurpleConnection* gc, const char* who, const char* message,
   PurpleMessageFlags flags
) {
   const char* from_username = gc->account->username;
   PurpleMessageFlags receive_flags = 
      ((flags & ~PURPLE_MESSAGE_SEND) | PURPLE_MESSAGE_RECV);
   PurpleAccount* to_acct = purple_accounts_find( who, VOIPMS_PLUGIN_ID );
   PurpleConnection* to;
   int retval = 1;
   char* msg;
   gchar* api_message = NULL;
   GSList* api_args = NULL;
   struct VoipMsSendImData send_im_data = { 0 };

   purple_debug_info(
      "voipms",
      "Sending message from %s to %s: %s\n",
      from_username, who, message
   );

   /* Is the sender blocked by the recipient's privacy settings? */
   if( to_acct && !purple_privacy_check( to_acct, gc->account->username ) ) {
      msg = g_strdup_printf(
         "Your message was blocked by %s's privacy settings.", who
      );
      purple_debug_info(
         "voipms",
         "Discarding; %s is blocked by %s's privacy settings.\n",
         from_username,
         who
      );
      purple_conv_present_error( who, gc->account, msg );
      g_free( msg );
      retval = 0;
      goto send_im_cleanup;
   }

   /* Strip the whitespace from the ends of the string. Useful in case we     *
    * have something like OTR installed.                                      */
   /* TODO: Encode ~ for URL, somehow. */
   api_message = g_strdup( purple_url_encode( message ) );
   api_message = str_replace( api_message, "~", "-" );
   g_strstrip( api_message );

   purple_debug_info(
      "voipms",
      "Encoded message: %s\n",
      api_message
   );

   /* Build and send the API request. */
   api_args = g_slist_append(
      api_args,
      g_strdup_printf(
         "did=%s", purple_account_get_string( gc->account, "did", "" )
      )
   );
   api_args = g_slist_append(
      api_args,
      g_strdup_printf(
         "dst=%s", purple_url_encode( who )
      )
   );
   api_args = g_slist_append(
      api_args,
      g_strdup_printf(
         "message=%s", api_message
      )
   );

   /* Build the attachment. */

   voipms_api_request(
      VOIPMS_METHOD_SENDSMS, api_args, gc->account, &send_im_data
   );

   /* Block until request completes. */
   while( !send_im_data.processed ) {
      voipms_api_request_progress( gc->account );
      /* TODO: Delay for a second or so. */
   }

   /* Return success or fail based on response. */
   if( !send_im_data.success ) {
      msg = g_strdup_printf(
         "There was a problem contacting the VOIP.ms API: %s",
         send_im_data.error_buffer
      );
      purple_debug_error(
         "voipms",
         "Discarding; there was a problem contacting the VOIP.ms API.\n"
      );
      purple_conv_present_error( who, gc->account, msg );
      g_free( msg );
      retval = 0;
      goto send_im_cleanup;
   }

   to = get_voipms_gc( who );
   if( to ) {
      /* TODO: Fix timezone? */
      serv_got_im( to, from_username, message, receive_flags, time( NULL ) );
   }

send_im_cleanup:
   
   if( NULL != api_message ) {
      g_free( api_message );
   }

   if( NULL != send_im_data.error_buffer ) {
      g_free( send_im_data.error_buffer );
   }

   return retval;
}

static void voipms_add_buddy (
   PurpleConnection *gc, PurpleBuddy *buddy, PurpleGroup *group
) {
   const char *username = gc->account->username;

   purple_debug_info(
      "nullprpl",
      "Adding %s to %s's buddy list.\n",
      buddy->name,
      username
   );

   voipms_refresh_buddies( gc->account );
}

static void voipms_alias_buddy(
   PurpleConnection* gc, const char* who, const char* alias
) {
   purple_debug_info(
      "voipms", "%s sets %s's alias to %s\n", gc->account->username, who, alias
   );
}

#if 0
static void voipms_change_passwd(
   PurpleConnection* gc, const char* old_pass, const char* new_pass
) {
   purple_debug_info(
      "voipms", "%s wants to change their password\n", gc->account->username
   );
}
#endif

static GList* voipms_status_types( PurpleAccount* acct ) {
   GList* types = NULL;
   PurpleStatusType* type;

   type = purple_status_type_new_with_attrs(
      PURPLE_STATUS_AVAILABLE,
      VOIPMS_STATUS_ONLINE, NULL, TRUE, TRUE, FALSE,
      "message", "Message", purple_value_new( PURPLE_TYPE_STRING ),
      NULL
   );
   types = g_list_prepend( types, type );

   type = purple_status_type_new_with_attrs(
      PURPLE_STATUS_AWAY,
      VOIPMS_STATUS_AWAY, NULL, TRUE, TRUE, FALSE,
      "message", "Message", purple_value_new( PURPLE_TYPE_STRING ),
      NULL
   );
   types = g_list_prepend( types, type );

   return g_list_reverse( types );
}

static const char* voipms_list_icon( PurpleAccount* acct, PurpleBuddy* buddy ) {
   return "voipms";
}

static PurplePluginProtocolInfo prpl_info = {
   0,                               /* options */
   NULL,                               /* user_splits */
   NULL,               /* protocol_options, initialized in voipms_init() */
   {   /* icon_spec, a PurpleBuddyIconSpec */
       "png,jpg,gif",                  /* format */
       0,                              /* min_width */
       0,                              /* min_height */
       128,                            /* max_width */
       128,                            /* max_height */
       10000,                          /* max_filesize */
       PURPLE_ICON_SCALE_DISPLAY,      /* scale_rules */
   },
   voipms_list_icon,                   /* list_icon */
   NULL,                               /* list_emblem */
   NULL,                               /* status_text */
   NULL,                               /* tooltip_text */
   voipms_status_types,                /* status_types */
   NULL,                               /* blist_node_menu */
   NULL,                               /* chat_info */
   NULL,                               /* chat_info_defaults */
   voipms_login,                       /* login */
   voipms_close,                       /* close */
   voipms_send_im,                     /* send_im */
   NULL,                               /* set_info */
   NULL,                               /* send_typing */
   NULL,                               /* get_info */
   NULL,                               /* set_status */
   NULL,                               /* set_idle */
   NULL,                               /* change_passwd */
   voipms_add_buddy,                   /* add_buddy */
   NULL,                               /* add_buddies */
   NULL,                               /* remove_buddy */
   NULL,                               /* remove_buddies */
   NULL,                               /* add_permit */
   NULL,                               /* add_deny */
   NULL,                               /* rem_permit */
   NULL,                               /* rem_deny */
   NULL,                               /* set_permit_deny */
   NULL,                               /* join_chat */
   NULL,                               /* reject_chat */
   NULL,                               /* get_chat_name */
   NULL,                               /* chat_invite */
   NULL,                               /* chat_leave */
   NULL,                               /* chat_whisper */
   NULL,                               /* chat_send */
   NULL,                               /* keepalive */
   NULL,                               /* register_user */
   NULL,                               /* get_cb_info */
   NULL,                               /* get_cb_away */
   voipms_alias_buddy,                 /* alias_buddy */
   NULL,                               /* group_buddy */
   NULL,                               /* rename_group */
   NULL,                               /* buddy_free */
   NULL,                               /* convo_closed */
   NULL,                               /* normalize */
   NULL,                               /* set_buddy_icon */
   NULL,                               /* remove_group */
   NULL,                               /* get_cb_real_name */
   NULL,                               /* set_chat_topic */
   NULL,                               /* find_blist_chat */
   NULL,                               /* roomlist_get_list */
   NULL,                               /* roomlist_cancel */
   NULL,                               /* roomlist_expand_category */
   NULL,                               /* can_receive_file */
   NULL,                               /* send_file */
   NULL,                               /* new_xfer */
   NULL,                               /* offline_message */
   NULL,                               /* whiteboard_prpl_ops */
   NULL,                               /* send_raw */
   NULL,                               /* roomlist_room_serialize */
   NULL,                               /* unregister_user */
   NULL,                               /* send_attention */
   NULL,                               /* get_attention_types */
   sizeof( PurplePluginProtocolInfo ), /* struct_size */
   NULL,                               /* get_account_text_table */
   NULL,                               /* initiate_media */
   NULL,                               /* get_media_caps */
   NULL,                               /* get_moods */
   NULL,                               /* set_public_alias */
   NULL,                               /* get_public_alias */
   NULL,                               /* add_buddy_with_invite */
   NULL                                /* add_buddies_with_invite */
};

static PurplePluginInfo info = {
   PURPLE_PLUGIN_MAGIC,                                     /* magic */
   PURPLE_MAJOR_VERSION,                                    /* major_version */
   PURPLE_MINOR_VERSION,                                    /* minor_version */
   PURPLE_PLUGIN_PROTOCOL,                                  /* type */
   NULL,                                                    /* ui_requirement */
   0,                                                       /* flags */
   NULL,                                                    /* dependencies */
   PURPLE_PRIORITY_DEFAULT,                                 /* priority */
   VOIPMS_PLUGIN_ID,                                        /* id */
   VOIPMS_PLUGIN_NAME,                                      /* name */
   VOIPMS_PLUGIN_ID,                                        /* version */
   VOIPMS_PLUGIN_NAME,                                      /* summary */
   VOIPMS_PLUGIN_NAME,                                      /* description */
   NULL,                                                    /* author */
   VOIPMS_PLUGIN_WEBSITE,                                   /* homepage */
   NULL,                                                    /* load */
   NULL,                                                    /* unload */
   voipms_destroy,                                          /* destroy */
   NULL,                                                    /* ui_info */
   &prpl_info,                                              /* extra_info */
   NULL,                                                    /* prefs_info */
   NULL,                                                    /* actions */
   NULL,                                                    /* padding... */
   NULL,
   NULL,
   NULL,
};

static void voipms_init( PurplePlugin* plugin ) {
   PurpleAccountOption* option;
   GList* status_types = NULL;
   PurpleKeyValuePair* status;
   
   option = purple_account_option_string_new(
      "REST GET API URL",
      "api_url",                
      VOIPMS_PLUGIN_DEFAULT_API_URL
   );
   prpl_info.protocol_options =
      g_list_append( prpl_info.protocol_options, option );

   option = purple_account_option_string_new(
      "Account DID",
      "did",                
      ""
   );
   prpl_info.protocol_options =
      g_list_append( prpl_info.protocol_options, option );

   option = purple_account_option_bool_new(
      "Delete Messages on Fetch",
      "delete",
      TRUE
   );
   prpl_info.protocol_options =
      g_list_append( prpl_info.protocol_options, option );

   /* Setup the statuses to choose from. */
   status = calloc( 1, sizeof( PurpleKeyValuePair ) );
   status->key = "Online";
   status->value = VOIPMS_STATUS_ONLINE;
   status_types = g_list_append( status_types, status );

   status = calloc( 1, sizeof( PurpleKeyValuePair ) );
   status->key = "Away";
   status->value = VOIPMS_STATUS_AWAY;
   status_types = g_list_append( status_types, status );

   option = purple_account_option_list_new(
      "Default Contact Status",
      "default_status",
      status_types
   );
   prpl_info.protocol_options =
      g_list_append( prpl_info.protocol_options, option );
 
   purple_debug_info( "voipms", "Starting up...\n" );

   _voipms_protocol = plugin;
}

static void voipms_destroy( PurplePlugin* plugin ) {
   purple_debug_info( "voipms", "Shutting down.\n" );
}

PURPLE_INIT_PLUGIN( voipms, voipms_init, info );

