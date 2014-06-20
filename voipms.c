
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

static void call_if_voipms( gpointer data, gpointer userdata ) {
   PurpleConnection* gc = (PurpleConnection*)(data);
   GcFuncData* gcfdata = (GcFuncData*)userdata;

   if( !strcmp( gc->account->protocol_id, VOIPMS_PLUGIN_ID ) ) {
      gcfdata->fn( gcfdata->from, gc, gcfdata->userdata );
   }
}

static void foreach_voipms_gc(
   GcFunc fn, PurpleConnection* from, gpointer userdata
) {
   GcFuncData gcfdata = { fn, from, userdata };
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

static void voipms_login( PurpleAccount* acct ) {
   PurpleConnection* gc = purple_account_get_connection( acct );
 
   purple_debug_info( "voipms", "Logging in %s...\n", acct->username );
 
   purple_connection_update_progress(
      gc,
      _( "Connecting" ),
      0, /* Which connection step this is. */
      2  /* Total number of steps */
   );
 
   #if 0
   purple_connection_update_progress(gc, _("Connected"),
                                     1,   /* which connection step this is */
                                     2);  /* total number of steps */
   #endif

   purple_connection_set_state( gc, PURPLE_CONNECTED );
 
   /* Tell purple about everyone on our buddy list who's connected. */
   foreach_voipms_gc( discover_status, gc, NULL );

   /* TODO: Check for offline messages. */
 
   /* Notify other VOIP.ms accounts. */
   foreach_voipms_gc( report_status_change, gc, NULL );
}

static void voipms_close( PurpleConnection* gc ) {
   /* Motify other VOIP.ms accounts. */
   foreach_voipms_gc( report_status_change, gc, NULL );
}

static int voipms_send_im(
   PurpleConnection* gc, const char* who, const char* message,
   PurpleMessageFlags flags
) {
   const char *from_username = gc->account->username;
   PurpleMessageFlags receive_flags = 
      ((flags & ~PURPLE_MESSAGE_SEND) | PURPLE_MESSAGE_RECV);
   PurpleAccount* to_acct = purple_accounts_find( who, VOIPMS_PLUGIN_ID );
   PurpleConnection* to;
   char* msg;

   purple_debug_info(
      "voipms",
      "Sending message from %s to %s: %s\n",
      from_username, who, message
   );

   /* Is the sender blocked by the recipient's privacy settings? */
   if( to_acct && !purple_privacy_check( to_acct, gc->account->username ) ) {
      msg = g_strdup_printf(
         _( "Your message was blocked by %s's privacy settings." ), who
      );
      purple_debug_info(
         "voipms",
         "Discarding; %s is blocked by %s's privacy settings.\n",
         from_username,
         who
      );
      purple_conv_present_error( who, gc->account, msg );
      g_free( msg );
      return 0;
  }

   serv_got_im( to, from_username, message, receive_flags, time( NULL ) );

   return 1;
}

static GList* voipms_status_types( PurpleAccount* acct ) {
   GList* types = NULL;
   PurpleStatusType* type;

   type = purple_status_type_new_with_attrs(
      PURPLE_STATUS_AVAILABLE,
      VOIPMS_STATUS_ONLINE, NULL, TRUE, TRUE, FALSE,
      "message", _( "Message" ), purple_value_new( PURPLE_TYPE_STRING ),
      NULL
   );
   types = g_list_prepend( types, type );

   return g_list_reverse( types );
}

static void voipms_destroy( PurplePlugin *plugin ) {
   purple_debug_info( "voipms", "Shutting down.\n" );
}

static PurplePluginProtocolInfo prpl_info = {
   OPT_PROTO_NO_PASSWORD | OPT_PROTO_CHAT_TOPIC,  /* options */
   NULL,               /* user_splits, initialized in voipms_init() */
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
   NULL,                               /* list_icon */
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
   "VOIP.ms SMS Plugin",                                    /* name */
   VOIPMS_PLUGIN_ID,                                        /* version */
   "VOIP.ms SMS Plugin",                               /* summary */
   "VOIP.ms SMS Plugin",                               /* description */
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

PURPLE_INIT_PLUGIN( null, voipms_init, info );

