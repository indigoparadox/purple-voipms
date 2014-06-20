
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

static PurpleConnection* get_voipms_gc( const char* username ) {
   PurpleAccount* acct = purple_accounts_find( username, VOIPMS_PLUGIN_ID );
   if( acct && purple_account_is_connected( acct ) ) {
      return acct->gc;
   } else {
      return NULL;
   }
}

static void call_if_voipms( gpointer data, gpointer userdata ) {
   PurpleConnection* gc = (PurpleConnection*)(data);
   struct GcFuncData* gcfdata = (struct GcFuncData*)userdata;

   if( !strcmp( gc->account->protocol_id, VOIPMS_PLUGIN_ID ) ) {
      gcfdata->fn( gcfdata->from, gc, gcfdata->userdata );
   }
}

static void foreach_voipms_gc(
   GcFunc fn, PurpleConnection* from, gpointer userdata
) {
   struct GcFuncData gcfdata = { fn, from, userdata };
   g_list_foreach(
      purple_connections_get_all(), call_if_voipms, &gcfdata
   );
}

static void discover_status(
   PurpleConnection* from, PurpleConnection* to, gpointer userdata
) {
   const char* from_username = from->account->username;
   const char* to_username = to->account->username;
   const char* status_id;
   const char* message;

   if( purple_find_buddy( from->account, to_username ) ) {
      PurpleStatus *status = purple_account_get_active_status( to->account );
      status_id = purple_status_get_id( status );
      message = purple_status_get_attr_string( status, "message" );

      if( !strcmp( status_id, VOIPMS_STATUS_ONLINE ) ) {
         purple_debug_info(
            "voipms",
            "%s sees that %s is %s: %s\n",
            from_username, to_username, status_id, message
         );
         purple_prpl_got_user_status(
            from->account, to_username, status_id,
            (message) ? "message" : NULL, message, NULL
         );
      } else {
         purple_debug_error(
            "voipms",
            "%s's buddy %s has an unknown status: %s, %s",
            from_username, to_username, status_id, message
         );
      }
   }
}

static void report_status_change(
   PurpleConnection* from, PurpleConnection* to, gpointer userdata
) {
   purple_debug_info(
      "voipms",
      "Notifying %s that %s changed status...\n",
      to->account->username,
      from->account->username
   );
   discover_status( to, from, NULL );
}

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

JsonParser* voipms_api_request(
   GSList** args, PurpleAccount* account, char* error_buffer
) {
   CURL* curl = NULL;
   CURLcode res;
   struct RequestMemoryStruct chunk;
   GSList* arg_iter;
   char* api_url = NULL;
   size_t new_length = 1,
      old_length = 0;
   JsonParser* parser = NULL;

   /* Setup some buffers and stuff. */
   chunk.memory = malloc( 1 );
   chunk.size = 0;

   /* Add the credentials to the request. */
   *args = g_slist_append( *args, g_strdup_printf( "api_username=%s",
      purple_url_encode( account->username )
   ) );
   *args = g_slist_append( *args, g_strdup_printf( "api_password=%s",
      purple_url_encode( account->password )
   ) );

   /* Build the query string. */
   arg_iter = *args;
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

   printf( "%s\n", api_url );

   /* Setup the request. */
   curl = curl_easy_init();
   curl_easy_setopt( curl, CURLOPT_URL, api_url );
   curl_easy_setopt(
      curl, CURLOPT_WRITEFUNCTION, voipms_api_request_write_body_callback
   );

   /* Only write to the buffers provided. */
   curl_easy_setopt( curl, CURLOPT_WRITEDATA, (void*)(&chunk) );

   if( NULL != error_buffer ) {
      curl_easy_setopt( curl, CURLOPT_ERRORBUFFER, error_buffer );
   }

   /* Perform the request and return the result. */
   res = curl_easy_perform( curl );
   if( CURLE_OK != res ) {
      /* TODO: Handle me. */
   }

   #if 0
   purple_debug_info( "voipms", "Response from server: %s\n", chunk.memory );
   #endif

   parser = json_parser_new();
   if( !json_parser_load_from_data( parser, chunk.memory, chunk.size, NULL ) ) {
      purple_debug_error(
         "voipms", "Error parsing response: %s\n", chunk.memory
      );
      g_object_unref( parser );
      parser = NULL;
   }

api_request_cleanup:

   curl_easy_cleanup( curl );

   free( chunk.memory );
   
   if( NULL != api_url ) {
      free( api_url );
   }

   return parser;
}

static void messages_foreach_process(
   JsonArray* array, guint index_, JsonNode* element_node, gpointer user_data
) {
   PurpleAccount* acct = (PurpleAccount*)user_data;
   JsonObject* message;
   const gchar* id,
      * date;
   time_t message_time;
   struct tm* message_timeinfo;

   #if 0
   /* Parse/translate message metadata. */
   message = json_node_get_object( element_node );
   id = json_object_get_string_member( message, "id" );
   date = json_object_get_string_member( message, "date" );
   message_timeinfo = getdate( date );

   /* Pass the message on to the user. */
   serv_got_im(
      acct->gc,
      json_object_get_string_member( message, "contact" ),
      json_object_get_string_member( message, "message" ),
      PURPLE_MESSAGE_RECV,
      message_time
   );
   #endif
}

static gboolean voipms_messages_timer( PurpleAccount* acct ) {
   time_t to_rawtime,
      from_rawtime;
   struct tm* to_timeinfo,
      * from_timeinfo;
   char curl_error_str[VOIPMS_ERROR_SIZE],
      from_filter_date[VOIPMS_DATE_BUFFER_SIZE] = { 0 },
      to_filter_date[VOIPMS_DATE_BUFFER_SIZE] = { 0 };
   GSList* api_args = NULL;
   JsonParser* parser = NULL;
   JsonNode* root = NULL;
   JsonObject* response = NULL;
   JsonArray* messages = NULL;
   const gchar* status;

   /* Calculate as wide a range as the API will allow us. */
   time( &from_rawtime );
   from_rawtime -= VOIPMS_DAY_SECONDS * VOIPMS_MAX_AGE_DAYS;
   from_timeinfo = localtime( &from_rawtime );
   strftime( from_filter_date, VOIPMS_DATE_BUFFER_SIZE, "%F", from_timeinfo );

   time( &to_rawtime );
   to_timeinfo = localtime( &to_rawtime );
   strftime( to_filter_date, VOIPMS_DATE_BUFFER_SIZE, "%F", to_timeinfo );

   /* Build and send the API request. */
   api_args = g_slist_append( api_args, g_strdup( "method=getSMS" ) );
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

   parser = voipms_api_request(
      &api_args,
      acct,
      curl_error_str
   );

   if( NULL == parser ) {
      goto messages_timer_cleanup;
   }

   root = json_parser_get_root( parser );
   response = json_node_get_object( root );
   status = json_object_get_string_member( response, "status" );
   if( strcmp( status, "success" ) ) {
      purple_debug_error( "voipms", "Request status: %s", status );
      goto messages_timer_cleanup;
   }
      
   messages = json_object_get_array_member( response, "sms" );  
   json_array_foreach_element(
      messages, messages_foreach_process, acct
   );

messages_timer_cleanup:

   g_object_unref( parser );

   g_slist_free_full( api_args, g_free );

   #if 0
   free( timeinfo );
   #endif

   return TRUE;
}

static void voipms_login( PurpleAccount* acct ) {
   PurpleConnection* gc = purple_account_get_connection( acct );
   struct VoipMsAccount* vmsa;

   vmsa = calloc( 1, sizeof( struct VoipMsAccount ) );
   acct->gc->proto_data = vmsa;
 
   purple_debug_info( "voipms", "Logging in %s...\n", acct->username );
 
   purple_connection_update_progress(
      gc,
      "Connecting",
      0, /* Which connection step this is. */
      2  /* Total number of steps */
   );
 
   #if 0
   purple_connection_update_progress(gc, "Connected",
                                     1,   /* which connection step this is */
                                     2);  /* total number of steps */
   #endif

   purple_connection_set_state( gc, PURPLE_CONNECTED );
 
   /* Tell purple about everyone on our buddy list who's connected. */
   foreach_voipms_gc( discover_status, gc, NULL );

   /* Start polling for new messages. */
   vmsa->timer = purple_timeout_add_seconds(
      5, (GSourceFunc)voipms_messages_timer, acct
   );
 
   /* Notify other VOIP.ms accounts. */
   foreach_voipms_gc( report_status_change, gc, NULL );
}

static void voipms_close( PurpleConnection* gc ) {
   struct VoipMsAccount* vmsa = gc->proto_data;

   /* Stop polling for new messages. */
   if( vmsa->timer ) {
      purple_timeout_remove( vmsa->timer );
   }

   /* Notify other VOIP.ms accounts. */
   foreach_voipms_gc( report_status_change, gc, NULL );
}

static int voipms_send_im(
   PurpleConnection* gc, const char* who, const char* message,
   PurpleMessageFlags flags
) {
   const char* from_username = gc->account->username;
   const gchar* status;
   PurpleMessageFlags receive_flags = 
      ((flags & ~PURPLE_MESSAGE_SEND) | PURPLE_MESSAGE_RECV);
   PurpleAccount* to_acct = purple_accounts_find( who, VOIPMS_PLUGIN_ID );
   PurpleConnection* to;
   int retval = 1;
   char* msg,
      curl_error_str[VOIPMS_ERROR_SIZE];
   gchar* api_message = NULL;
   GSList* api_args = NULL;
   JsonParser* parser = NULL;
   JsonNode* root = NULL;
   JsonObject* response = NULL;

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
   api_message = g_strdup( message );
   g_strstrip( api_message );

   /* Build and send the API request. */
   api_args = g_slist_append( api_args, g_strdup( "method=sendSMS" ) );
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
         "message=%s", purple_url_encode( api_message )
      )
   );

   parser = voipms_api_request(
      &api_args,
      gc->account,
      curl_error_str
   );

   /* Return success or fail based on response. */
   if( NULL == parser ) {
      msg = g_strdup_printf(
         "There was a problem contacting the VOIP.ms API: %s", curl_error_str
      );
      purple_debug_info(
         "voipms",
         "Discarding; there was a problem contacting the VOIP.ms API."
      );
      purple_conv_present_error( who, gc->account, msg );
      g_free( msg );
      retval = 0;
      goto send_im_cleanup;
   }

   /* Return success or fail based on the API call success. */
   root = json_parser_get_root( parser );
   response = json_node_get_object( root );
   status = json_object_get_string_member( response, "status" );
   if( strcmp( status, "success" ) ) {
      msg = g_strdup_printf(
         "There was a problem sending the message: %s", status
      );
      purple_debug_error( "voipms", "Request status: %s", status );
      purple_conv_present_error( who, gc->account, msg );
      g_free( msg );
      retval = 0;
      goto send_im_cleanup;
   }

   /* Is the recipient online? */
   to = get_voipms_gc( who );
   if( to ) {
      serv_got_im( to, from_username, message, receive_flags, time( NULL ) );
   }

send_im_cleanup:
   
   g_object_unref( parser );

   g_slist_free_full( api_args, g_free );

   if( NULL != api_message ) {
      g_free( api_message );
   }

   return retval;
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

   return g_list_reverse( types );
}

static const char* voipms_list_icon( PurpleAccount* acct, PurpleBuddy* buddy ) {
   return "null";
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
   //voipms_change_passwd,               /* change_passwd */
   NULL,                               /* change_passwd */
   NULL,                               /* add_buddy */
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
   NULL,                               /* alias_buddy */
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
   //PurpleAccountUserSplit* split;
   PurpleAccountOption* option;
   
   #if 0
   /* See accountopt.h for information about user splits and protocol options.
    */
   split = purple_account_user_split_new(
      "Example user split",  /* text shown to user */
      "default",                /* default value */
      '@');                     /* field separator */
   #endif

   option = purple_account_option_string_new(
      "REST GET API URL",
      "api_url",                
      VOIPMS_PLUGIN_DEFAULT_API_URL
   );
   prpl_info.protocol_options = g_list_append( NULL, option );

   option = purple_account_option_string_new(
      "Account DID",
      "did",                
      ""
   );
   prpl_info.protocol_options = g_list_append( NULL, option );
 
   purple_debug_info( "voipms", "Starting up...\n" );

   //prpl_info.user_splits = g_list_append(NULL, split);

   _voipms_protocol = plugin;
}

static void voipms_destroy( PurplePlugin* plugin ) {
   purple_debug_info( "voipms", "Shutting down.\n" );
}

PURPLE_INIT_PLUGIN( null, voipms_init, info );

