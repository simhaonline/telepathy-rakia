/*
 * sip-media-session.c - Source for SIPMediaSession
 * Copyright (C) 2005 Collabora Ltd.
 * Copyright (C) 2005,2006 Nokia Corporation
 *   @author Kai Vehmanen <first.surname@nokia.com>
 *
 * Based on telepathy-gabble implementation (gabble-media-session).
 *   @author Ole Andre Vadla Ravnaas <ole.andre.ravnaas@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <dbus/dbus-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#include <sofia-sip/sdp.h>
#include <sofia-sip/sip_status.h>

#include <telepathy-glib/interfaces.h>
#include <telepathy-glib/dbus.h>
#include <telepathy-glib/errors.h>
#include <telepathy-glib/svc-media-interfaces.h>

#include "sip-media-channel.h"
#include "sip-connection.h"
#include "sip-connection-helpers.h"
#include "sip-media-session.h"
#include "sip-media-stream.h"

#define DEBUG_FLAG SIP_DEBUG_MEDIA
#include "debug.h"

static void session_handler_iface_init (gpointer, gpointer);

G_DEFINE_TYPE_WITH_CODE(SIPMediaSession,
    sip_media_session,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_MEDIA_SESSION_HANDLER,
      session_handler_iface_init)
    )

#define DEFAULT_SESSION_TIMEOUT 50000

/* signal enum */
enum
{
  SIG_STREAM_ADDED,
  SIG_TERMINATED,
  SIG_LAST_SIGNAL
};

/* properties */
enum
{
  PROP_MEDIA_CHANNEL = 1,
  PROP_OBJECT_PATH,
  PROP_SESSION_ID,
  PROP_INITIATOR,
  PROP_PEER,
  PROP_STATE,
  LAST_PROPERTY
};

static guint signals[SIG_LAST_SIGNAL] = {0};

typedef struct {
    gchar *name;
    gchar *attributes;
} SessionStateDescription;

/**
 * StreamEngine session states:
 * - pending-created, objects created, local cand/codec query ongoing
 * - pending-initiated, 'Ready' signal received
 * - active, remote codecs delivered to StreamEngine (note,
 *   SteamEngine might still fail to verify connectivity and report
 *   an error)
 * - ended, session has ended
 */
static const SessionStateDescription session_states[] =
{
    { "JS_STATE_PENDING_CREATED",   ANSI_BOLD_ON ANSI_FG_BLACK ANSI_BG_WHITE   },
    { "JS_STATE_PENDING_INITIATED", ANSI_BOLD_ON               ANSI_BG_MAGENTA },
    { "JS_STATE_ACTIVE",            ANSI_BOLD_ON               ANSI_BG_BLUE    },
    { "JS_STATE_ENDED",                                        ANSI_BG_RED     }
};

/* private structure */
typedef struct _SIPMediaSessionPrivate SIPMediaSessionPrivate;

struct _SIPMediaSessionPrivate
{
  SIPConnection *conn;
  SIPMediaChannel *channel;             /** see gobj. prop. 'media-channel' */
  gchar *object_path;                   /** see gobj. prop. 'object-path' */
  gchar *id;                            /** see gobj. prop. 'session-id' */
  TpHandle initiator;                   /** see gobj. prop. 'initator' */
  TpHandle peer;                        /** see gobj. prop. 'peer' */
  JingleSessionState state;             /** see gobj. prop. 'state' */
  guint timer_id;
  gboolean accepted;                    /**< session has been locally accepted for use */
  gboolean oa_pending;                  /**< offer/answer waiting to be sent */
  gboolean se_ready;                    /**< connection established with stream-engine */
  gboolean dispose_has_run;
  GPtrArray *streams;
};

#define SIP_MEDIA_SESSION_GET_PRIVATE(o)     (G_TYPE_INSTANCE_GET_PRIVATE ((o), SIP_TYPE_MEDIA_SESSION, SIPMediaSessionPrivate))

static void sip_media_session_get_property (GObject    *object,
					    guint       property_id,
					    GValue     *value,
					    GParamSpec *pspec);
static void sip_media_session_set_property (GObject      *object,
					    guint         property_id,
					    const GValue *value,
					    GParamSpec   *pspec);
static gboolean priv_timeout_session (gpointer data);
static SIPMediaStream* priv_create_media_stream (SIPMediaSession *session, guint media_type);

static nua_handle_t *priv_get_nua_handle_for_session (SIPMediaSession *session);
static void priv_offer_answer_step (SIPMediaSession *session);

static void sip_media_session_init (SIPMediaSession *obj)
{
  SIPMediaSessionPrivate *priv = SIP_MEDIA_SESSION_GET_PRIVATE (obj);

  g_debug ("%s called", G_STRFUNC);

  /* allocate any data required by the object here */

  priv->streams = g_ptr_array_sized_new (0);
}

static GObject *
sip_media_session_constructor (GType type, guint n_props,
			       GObjectConstructParam *props)
{
  GObject *obj;
  SIPMediaSessionPrivate *priv;
  DBusGConnection *bus;

  obj = G_OBJECT_CLASS (sip_media_session_parent_class)->
           constructor (type, n_props, props);
  priv = SIP_MEDIA_SESSION_GET_PRIVATE (SIP_MEDIA_SESSION (obj));

  g_object_get (priv->channel, "connection", &priv->conn, NULL);

  priv->state = JS_STATE_PENDING_CREATED;

  /* note: session is always created to either create a new outbound
   *       request for a media channel, or to respond to an incoming 
   *       request ... thus oa_pending is TRUE at start */
  priv->oa_pending = TRUE;

  bus = tp_get_bus ();
  dbus_g_connection_register_g_object (bus, priv->object_path, obj);

  return obj;
}

static void sip_media_session_get_property (GObject    *object,
					    guint       property_id,
					    GValue     *value,
					    GParamSpec *pspec)
{
  SIPMediaSession *session = SIP_MEDIA_SESSION (object);
  SIPMediaSessionPrivate *priv = SIP_MEDIA_SESSION_GET_PRIVATE (session);

  switch (property_id) {
    case PROP_MEDIA_CHANNEL:
      g_value_set_object (value, priv->channel);
      break;
    case PROP_OBJECT_PATH:
      g_value_set_string (value, priv->object_path);
      break;
    case PROP_SESSION_ID:
      g_value_set_string (value, priv->id);
      break;
    case PROP_INITIATOR:
      g_value_set_uint (value, priv->initiator);
      break;
    case PROP_PEER:
      g_value_set_uint (value, priv->peer);
      break;
    case PROP_STATE:
      g_value_set_uint (value, priv->state);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void priv_session_state_changed (SIPMediaSession *session,
					JingleSessionState prev_state,
					JingleSessionState new_state);

static void sip_media_session_set_property (GObject      *object,
					    guint         property_id,
					    const GValue *value,
					    GParamSpec   *pspec)
{
  SIPMediaSession *session = SIP_MEDIA_SESSION (object);
  SIPMediaSessionPrivate *priv = SIP_MEDIA_SESSION_GET_PRIVATE (session);
  JingleSessionState prev_state;

  switch (property_id) {
    case PROP_MEDIA_CHANNEL:
      priv->channel = g_value_get_object (value);
      break;
    case PROP_OBJECT_PATH:
      g_free (priv->object_path);
      priv->object_path = g_value_dup_string (value);
      break;
    case PROP_SESSION_ID:
      g_free (priv->id);
      priv->id = g_value_dup_string (value);
      break;
    case PROP_INITIATOR:
      priv->initiator = g_value_get_uint (value);
      break;
    case PROP_PEER:
      priv->peer = g_value_get_uint (value);
      break;
    case PROP_STATE:
      prev_state = priv->state;
      priv->state = g_value_get_uint (value);

      if (priv->state != prev_state)
        priv_session_state_changed (session, prev_state, priv->state);

      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void sip_media_session_dispose (GObject *object);
static void sip_media_session_finalize (GObject *object);

static void
sip_media_session_class_init (SIPMediaSessionClass *sip_media_session_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (sip_media_session_class);
  GParamSpec *param_spec;

  g_type_class_add_private (sip_media_session_class, sizeof (SIPMediaSessionPrivate));

  object_class->constructor = sip_media_session_constructor;

  object_class->get_property = sip_media_session_get_property;
  object_class->set_property = sip_media_session_set_property;

  object_class->dispose = sip_media_session_dispose;
  object_class->finalize = sip_media_session_finalize;

  param_spec = g_param_spec_object ("media-channel", "SIPMediaChannel object",
                                    "SIP media channel object that owns this "
                                    "media session object.",
                                    SIP_TYPE_MEDIA_CHANNEL,
                                    G_PARAM_CONSTRUCT_ONLY |
                                    G_PARAM_READWRITE |
                                    G_PARAM_STATIC_NICK |
                                    G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_MEDIA_CHANNEL, param_spec);

  param_spec = g_param_spec_string ("object-path", "D-Bus object path",
                                    "The D-Bus object path used for this "
                                    "object on the bus.",
                                    NULL,
                                    G_PARAM_CONSTRUCT_ONLY |
                                    G_PARAM_READWRITE |
                                    G_PARAM_STATIC_NAME |
                                    G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_OBJECT_PATH, param_spec);

  param_spec = g_param_spec_string ("session-id", "Session ID",
                                    "A unique session identifier used "
                                    "throughout all communication.",
				    NULL,
                                    G_PARAM_CONSTRUCT_ONLY |
                                    G_PARAM_READWRITE |
                                    G_PARAM_STATIC_NAME |
                                    G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_SESSION_ID, param_spec);

  param_spec = g_param_spec_uint ("initiator", "Session initiator",
                                  "The TpHandle representing the contact "
                                  "who initiated the session.",
                                  0, G_MAXUINT32, 0,
                                  G_PARAM_CONSTRUCT_ONLY |
                                  G_PARAM_READWRITE |
                                  G_PARAM_STATIC_NAME |
                                  G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_INITIATOR, param_spec);

  param_spec = g_param_spec_uint ("peer", "Session peer",
                                  "The TpHandle representing the contact "
                                  "with whom this session communicates.",
                                  0, G_MAXUINT32, 0,
                                  G_PARAM_CONSTRUCT_ONLY |
                                  G_PARAM_READWRITE |
                                  G_PARAM_STATIC_NAME |
                                  G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_PEER, param_spec);

  param_spec = g_param_spec_uint ("state", "Session state",
                                  "The current state that the session is in.",
                                  0, G_MAXUINT32, 0,
                                  G_PARAM_READWRITE |
                                  G_PARAM_STATIC_NAME |
                                  G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_STATE, param_spec);

  signals[SIG_STREAM_ADDED] =
    g_signal_new ("stream-added",
                  G_OBJECT_CLASS_TYPE (sip_media_session_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__OBJECT,
                  G_TYPE_NONE, 1, G_TYPE_OBJECT);

  signals[SIG_TERMINATED] =
    g_signal_new ("terminated",
                  G_OBJECT_CLASS_TYPE (sip_media_session_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);
}

static void
sip_media_session_dispose (GObject *object)
{
  SIPMediaSession *self = SIP_MEDIA_SESSION (object);
  SIPMediaSessionPrivate *priv = SIP_MEDIA_SESSION_GET_PRIVATE (self);

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  g_signal_emit (self, signals[SIG_TERMINATED], 0);

  if (priv->conn)
    {
      g_object_unref(priv->conn);
      priv->conn = NULL;
    }

  if (priv->timer_id) {
    g_source_remove (priv->timer_id);
    priv->timer_id = 0;
  }

  if (G_OBJECT_CLASS (sip_media_session_parent_class)->dispose)
    G_OBJECT_CLASS (sip_media_session_parent_class)->dispose (object);

  DEBUG ("exit");
}

void
sip_media_session_finalize (GObject *object)
{
  SIPMediaSession *self = SIP_MEDIA_SESSION (object);
  SIPMediaSessionPrivate *priv = SIP_MEDIA_SESSION_GET_PRIVATE (self);
  guint i;

  /* free any data held directly by the object here */

  G_OBJECT_CLASS (sip_media_session_parent_class)->finalize (object);

  for (i = 0; i < priv->streams->len; i++) {
    SIPMediaStream *stream = g_ptr_array_index (priv->streams, i);
    if (stream)
      g_object_unref (stream);
  }

  g_ptr_array_free(priv->streams, FALSE);

  DEBUG ("exit");
}



/**
 * sip_media_session_error
 *
 * Implements DBus method Error
 * on interface org.freedesktop.Telepathy.Media.SessionHandler
 */
static void
sip_media_session_error (TpSvcMediaSessionHandler *iface,
                         guint errno,
                         const gchar *message,
                         DBusGMethodInvocation *context)
{
  SIPMediaSession *obj = SIP_MEDIA_SESSION (iface);

  GMS_DEBUG_INFO (obj, "Media.SessionHandler::Error called (%s) terminating session", message);

  sip_media_session_terminate (obj);

  tp_svc_media_session_handler_return_from_error (context);
}

static void priv_emit_new_stream (SIPMediaSession *self,
				  SIPMediaStream *stream)
{
  gchar *object_path;
  guint id, media_type;

  g_object_get (stream,
                "object-path", &object_path,
                "id", &id,
                "media-type", &media_type,
                NULL);

  /* note: all of the streams are bidirectional from farsight's point of view, it's
   * just in the signalling they change */

  tp_svc_media_session_handler_emit_new_stream_handler (
      (TpSvcMediaSessionHandler *)self, object_path, id, media_type,
      TP_MEDIA_STREAM_DIRECTION_BIDIRECTIONAL);

  g_free (object_path);
}


/**
 * sip_media_session_ready
 *
 * Implements DBus method Ready
 * on interface org.freedesktop.Telepathy.Media.SessionHandler
 */
static void
sip_media_session_ready (TpSvcMediaSessionHandler *iface,
                         DBusGMethodInvocation *context)
{
  SIPMediaSession *obj = SIP_MEDIA_SESSION (iface);
  SIPMediaSessionPrivate *priv;
  guint i;

  DEBUG ("enter");

  g_assert (SIP_IS_MEDIA_SESSION (obj));

  priv = SIP_MEDIA_SESSION_GET_PRIVATE (obj);

  priv->se_ready = TRUE;

#ifdef TP_API_0_12    
  {
    gchar *object_path;
    guint id;

    g_object_get (priv->stream, 
		  "object-path", &object_path, 
		  "id", &id,
		  NULL);

    tp_svc_media_session_handler_emit_new_media_stream_handler (
        (TpSvcMediaSessionHandler *)self, object_path, TP_MEDIA_STREAM_TYPE_AUDIO,
        TP_MEDIA_STREAM_DIRECTION_BIDIRECTIONAL);

    g_free (object_path);
  }
#else
  /* note: streams are generated in priv_create_media_stream() */

  for (i = 0; i < priv->streams->len; i++) {
    SIPMediaStream *stream = g_ptr_array_index (priv->streams, i);
    if (stream)
      priv_emit_new_stream (obj, stream);
  }
  
#endif

  DEBUG ("exit");

  tp_svc_media_session_handler_return_from_ready (context);
}

/***********************************************************************
 * Helper functions follow (not based on generated templates)
 ***********************************************************************/

TpHandle
sip_media_session_get_peer (SIPMediaSession *session)
{
  SIPMediaSessionPrivate *priv = SIP_MEDIA_SESSION_GET_PRIVATE (session);
  return priv->peer;
}

static void priv_session_state_changed (SIPMediaSession *session,
					JingleSessionState prev_state,
					JingleSessionState new_state)
{
  SIPMediaSessionPrivate *priv = SIP_MEDIA_SESSION_GET_PRIVATE (session);

  GMS_DEBUG_EVENT (session, "state changed from %s to %s",
                   session_states[prev_state].name,
                   session_states[new_state].name);

  if (new_state == JS_STATE_PENDING_INITIATED)
    {
      priv->timer_id =
        g_timeout_add (DEFAULT_SESSION_TIMEOUT, priv_timeout_session, session);
    }
  else if (new_state == JS_STATE_ACTIVE)
    {
      if (priv->timer_id) {
	g_source_remove (priv->timer_id);
	priv->timer_id = 0;
      }
    }
}

#if _GMS_DEBUG_LEVEL
void
sip_media_session_debug (SIPMediaSession *session,
			 DebugMessageType type,
			 const gchar *format, ...)
{
  va_list list;
  gchar buf[512];
  SIPMediaSessionPrivate *priv;
  time_t curtime;
  struct tm *loctime;
  gchar stamp[10];
  const gchar *type_str;

  g_assert (SIP_IS_MEDIA_SESSION (session));

  priv = SIP_MEDIA_SESSION_GET_PRIVATE (session);

  curtime = time (NULL);
  loctime = localtime (&curtime);

  strftime (stamp, sizeof (stamp), "%T", loctime);

  va_start (list, format);

  vsnprintf (buf, sizeof (buf), format, list);

  va_end (list);

  switch (type) {
    case DEBUG_MSG_INFO:
      type_str = ANSI_BOLD_ON ANSI_FG_WHITE;
      break;
    case DEBUG_MSG_DUMP:
      type_str = ANSI_BOLD_ON ANSI_FG_GREEN;
      break;
    case DEBUG_MSG_WARNING:
      type_str = ANSI_BOLD_ON ANSI_FG_YELLOW;
      break;
    case DEBUG_MSG_ERROR:
      type_str = ANSI_BOLD_ON ANSI_FG_WHITE ANSI_BG_RED;
      break;
    case DEBUG_MSG_EVENT:
      type_str = ANSI_BOLD_ON ANSI_FG_CYAN;
      break;
    default:
      g_assert_not_reached ();
  }

  printf ("[%s%s%s] %s%-26s%s %s%s%s\n",
      ANSI_BOLD_ON ANSI_FG_WHITE,
      stamp,
      ANSI_RESET,
      session_states[priv->state].attributes,
      session_states[priv->state].name,
      ANSI_RESET,
      type_str,
      buf,
      ANSI_RESET);

  fflush (stdout);
}
#endif /* _GMS_DEBUG_LEVEL */

static gboolean priv_timeout_session (gpointer data)
{
  SIPMediaSession *session = data;

  g_debug ("%s: session timed out", G_STRFUNC);
  if (session)
    sip_media_session_terminate (session);

  return FALSE;
}

void sip_media_session_terminate (SIPMediaSession *session)
{
  SIPMediaSessionPrivate *priv = SIP_MEDIA_SESSION_GET_PRIVATE (session);
  nua_t *nua = sip_conn_sofia_nua(priv->conn);
  
  DEBUG ("enter");

  if (priv->state == JS_STATE_ENDED)
    return;

  if (priv->state == JS_STATE_PENDING_INITIATED ||
      priv->state == JS_STATE_ACTIVE) {
    g_message("%s: sending SIP BYE (%p).", G_STRFUNC, nua);
    nua_handle_t *nh = priv_get_nua_handle_for_session(session);
    if (nh)
      nua_bye (nh, TAG_END());
    else
      g_warning ("Unable to send BYE, channel handle not available.");
  }

  g_object_set (session, "state", JS_STATE_ENDED, NULL);
}

/**
 * Converts a sofia-sip media type enum to Telepathy media type.
 * See <sofia-sip/sdp.h> and <telepathy-constants.h>.
 *
 * @return G_MAXUINT if the media type cannot be mapped
 */
static guint priv_tp_media_type (sdp_media_e sip_mtype)
{
  switch (sip_mtype)
    {
      case sdp_media_audio: return TP_MEDIA_STREAM_TYPE_AUDIO;
      case sdp_media_video: return TP_MEDIA_STREAM_TYPE_VIDEO; 
      default: return G_MAXUINT;
    }

  g_assert_not_reached();
}

int sip_media_session_set_remote_info (SIPMediaSession *session, const char* r_sdp)
{
  SIPMediaSessionPrivate *priv = SIP_MEDIA_SESSION_GET_PRIVATE (session);
  su_home_t temphome[1] = { SU_HOME_INIT(temphome) };
  sdp_parser_t *parser;
  const char *pa_error;
  int res = 0;

  DEBUG ("enter");

  parser = sdp_parse(temphome, r_sdp, strlen(r_sdp), sdp_f_insane);
  pa_error = sdp_parsing_error(parser);
  if (pa_error) {
    g_warning("%s: error parsing SDP: %s\n", __func__, pa_error);
    res = -1;
  }
  else {
    sdp_session_t *parsed_sdp = sdp_session(parser);
    sdp_media_t *media = parsed_sdp->sdp_media;
    guint i, supported_media_cnt = 0;

    g_debug("Succesfully parsed remote SDP.");

    /* note: for each session, we maintain an ordered list of 
     *       streams (SDP m-lines) which are matched 1:1 to 
     *       the streams of the remote SDP */

    for (i = 0; media; i++) {
      SIPMediaStream *stream = NULL;

      if (i >= priv->streams->len)
	stream = priv_create_media_stream (session, priv_tp_media_type (media->m_type));
      else 
	stream = g_ptr_array_index(priv->streams, i);
      
      if (media->m_type == sdp_media_audio ||
	  media->m_type == sdp_media_video)
	++supported_media_cnt;
      
      g_debug ("Setting remote SDP for stream (%u:%p).", i, stream);	

      /* note: it is ok for the stream to be NULL (unsupported media type) */
      if (stream)
	res = sip_media_stream_set_remote_info (stream, media, parsed_sdp);
     
      media = media->m_next;
    }

    if (supported_media_cnt == 0) {
      g_warning ("No supported media in the session, aborting.");
      res = -1;
    }

    g_assert(media == NULL);
    g_assert(i == priv->streams->len);
    
    /* XXX: hmm, this is not the correct place really */
    g_object_set (session, "state", JS_STATE_ACTIVE, NULL);
  }

  sdp_parser_free(parser);
  su_home_deinit(temphome);

  DEBUG ("exit");

  return res;
}

void sip_media_session_stream_state (SIPMediaSession *sess,
                                     guint stream_id,
                                     guint state)
{
  SIPMediaSessionPrivate *priv;
  priv = SIP_MEDIA_SESSION_GET_PRIVATE (sess);
  g_assert (priv);
  sip_media_channel_stream_state (priv->channel, stream_id, state);
}

gboolean sip_media_session_request_streams (SIPMediaSession *session,
					    const GArray *media_types,
					    GPtrArray **ret,
					    GError **error)
{
  guint i;

  DEBUG ("enter");

  *ret = g_ptr_array_sized_new (media_types->len);

  for (i = 0; i < media_types->len; i++) {
    guint media_type = g_array_index (media_types, guint, i);
    SIPMediaStream *stream;

    g_debug("%s: len of %d, i = %d\n", G_STRFUNC, media_types->len, i);

    stream = priv_create_media_stream (session, media_type);

    g_ptr_array_add (*ret, stream);
  }

  return TRUE;
}

/**
 * Returns a list of pointers to SIPMediaStream objects 
 * associated with this session.
 */
/* FIXME: error handling is a bit weird, and might be assuming ret is
 * initially NULL */
gboolean sip_media_session_list_streams (SIPMediaSession *session,
					 GPtrArray **ret)
{
  SIPMediaSessionPrivate *priv = SIP_MEDIA_SESSION_GET_PRIVATE (session);
  guint i;

  if (priv->streams && priv->streams->len > 0) {
    *ret = g_ptr_array_sized_new (priv->streams->len);
    
    for (i = 0; *ret && i < priv->streams->len; i++) {
      SIPMediaStream *stream = g_ptr_array_index(priv->streams, i);
      
      if (stream)
	g_ptr_array_add (*ret, stream);
    }
  }

  return *ret != NULL;
}

void sip_media_session_accept (SIPMediaSession *self, gboolean accept)
{
  SIPMediaSessionPrivate *priv = SIP_MEDIA_SESSION_GET_PRIVATE (self);
  gboolean p = priv->accepted;

  g_debug ("%s: accepting session: %d", G_STRFUNC, accept);

  priv->accepted = accept;

  if (accept != p)
    priv_offer_answer_step (self);
}

/***********************************************************************
 * Helper functions follow (not based on generated templates)
 ***********************************************************************/

static nua_handle_t *priv_get_nua_handle_for_session (SIPMediaSession *session)
{
  SIPMediaSessionPrivate *priv = SIP_MEDIA_SESSION_GET_PRIVATE (session);
  gpointer *tmp = NULL;

  if (priv->channel) 
    g_object_get (priv->channel, "nua-handle", &tmp, NULL);

  return (nua_handle_t*)tmp;
}

static void priv_stream_new_active_candidate_pair_cb (SIPMediaStream *stream,
						      const gchar *native_candidate_id,
						      const gchar *remote_candidate_id,
						      SIPMediaSession *session)
{
  SIPMediaSessionPrivate *priv;

  g_assert (SIP_IS_MEDIA_SESSION (session));

  DEBUG ("enter");

  priv = SIP_MEDIA_SESSION_GET_PRIVATE (session);

  /* g_assert (priv->state < JS_STATE_ACTIVE); */

  GMS_DEBUG_INFO (session, "stream-engine reported a new active candidate pair [\"%s\" - \"%s\"]",
                  native_candidate_id, remote_candidate_id);

  /* XXX: active candidate pair, requires signaling action, 
   *      but currently done in priv_stream_ready_cb() */
}

static void priv_stream_new_native_candidate_cb (SIPMediaStream *stream,
						 const gchar *candidate_id,
						 const GPtrArray *transports,
						 SIPMediaSession *session)
{
}

static void priv_session_media_state (SIPMediaSession *session, gboolean playing)
{
  guint i;
  SIPMediaSessionPrivate *priv = SIP_MEDIA_SESSION_GET_PRIVATE (session);

  for (i = 0; i < priv->streams->len; i++) {
    SIPMediaStream *stream = g_ptr_array_index(priv->streams, i);
    if (stream)
      sip_media_stream_set_playing (stream, playing);
  }
}

/**
 * Sends an outbound offer/answer if all streams of the session
 * are prepared.
 * 
 * Following inputs are considered in decision making:
 *  - status of local streams (set up with stream-engine)
 *  - whether session is locally accepted
 *  - whether we are the initiator or not
 *  - whether an offer/answer step is pending (either initial,
 *    or a requested update to the session state)  
 */
static void priv_offer_answer_step (SIPMediaSession *session)
{
  SIPMediaSessionPrivate *priv = SIP_MEDIA_SESSION_GET_PRIVATE (session);
  TpBaseConnection *conn = (TpBaseConnection *)(priv->conn);
  guint i;
  gint non_ready_streams = 0;

  DEBUG ("enter");

  /* step: check status of streams */
  for (i = 0; i < priv->streams->len; i++) {
    SIPMediaStream *stream = g_ptr_array_index(priv->streams, i);
    if (stream &&
	sip_media_stream_is_ready (stream) != TRUE)
      ++non_ready_streams;
  }

  /* step: if all stream are ready, send an offer/answer */
  if (non_ready_streams == 0 &&
      priv->oa_pending) {
    nua_t *sofia_nua = sip_conn_sofia_nua (priv->conn);
    GString *user_sdp = g_string_sized_new (0);

    for (i = 0; i < priv->streams->len; i++) {
      SIPMediaStream *stream = g_ptr_array_index(priv->streams, i);
      if (stream)
	user_sdp = g_string_append (user_sdp, sip_media_stream_local_sdp(stream));
      else 
	user_sdp = g_string_append (user_sdp, "m=unknown 0 -/-");
    }

    /* send an offer if the session was initiated by us */
    if (priv->initiator != priv->peer) {
      nua_handle_t *nh;
      su_home_t *sofia_home = sip_conn_sofia_home (priv->conn);
      const char *dest_uri;

      dest_uri = tp_handle_inspect (
          conn->handles[TP_HANDLE_TYPE_CONTACT], priv->peer);

      g_debug ("mapped handle %u to uri %s.", priv->peer, dest_uri);

      if (dest_uri) {
	nh = sip_conn_create_request_handle (sofia_nua, sofia_home, dest_uri, 
            priv->peer);

	/* note:  we need to be prepared to receive media right after the
	 *       offer is sent, so we must set state to playing */
	priv_session_media_state (session, 1);
	
	nua_invite (nh,
		    SOATAG_USER_SDP_STR(user_sdp->str),
		    SOATAG_RTP_SORT(SOA_RTP_SORT_REMOTE),
		    SOATAG_RTP_SELECT(SOA_RTP_SELECT_ALL),
		    TAG_END());

	priv->oa_pending = FALSE;

	g_object_set (priv->channel, "nua-handle", (gpointer)nh, NULL);
      }
      else 
	g_warning ("Unable to send offer due to invalid destination SIP URI.");
    }
    else {
      /* note: only send a reply if session is locally accepted */
      if (priv->accepted == TRUE) {
	nua_handle_t *handle = priv_get_nua_handle_for_session(session);
	g_debug("Answering with SDP: <<<%s>>>.", user_sdp->str);
	if (handle) {
	  nua_respond (handle, 200, sip_200_OK,
		       SOATAG_USER_SDP_STR (user_sdp->str),
		       SOATAG_RTP_SORT(SOA_RTP_SORT_REMOTE),
		       SOATAG_RTP_SELECT(SOA_RTP_SELECT_ALL),
		       TAG_END());
	  
	  priv->oa_pending = FALSE;
	  
	  /* note: we have accepted the call, set state to playing */ 
	  priv_session_media_state (session, 1);
	}
	else
	  g_warning ("Unable to answer to the incoming INVITE, channel handle not available.");
      }
    }
  }
}

static void priv_stream_ready_cb (SIPMediaStream *stream,
				  const GPtrArray *codecs,
				  SIPMediaSession *session)
{
  SIPMediaSessionPrivate *priv;

  g_assert (SIP_IS_MEDIA_SESSION (session));
  priv = SIP_MEDIA_SESSION_GET_PRIVATE (session);
 
  DEBUG ("enter");

  if (priv->state < JS_STATE_PENDING_INITIATED)
    g_object_set (session, "state", JS_STATE_PENDING_INITIATED, NULL);

  priv_offer_answer_step (session);
}

static void priv_stream_supported_codecs_cb (SIPMediaStream *stream,
					     const GPtrArray *codecs,
					     SIPMediaSession *session)
{
  SIPMediaSessionPrivate *priv;

  g_assert (SIP_IS_MEDIA_SESSION (session));

  priv = SIP_MEDIA_SESSION_GET_PRIVATE (session);

  if (priv->initiator != priv->peer)
    {
      GMS_DEBUG_INFO (session, "%s: session not initiated by peer so we're "
                      "not preparing an accept message",
                      G_STRFUNC);
      return;
    }
}

static SIPMediaStream* priv_create_media_stream (SIPMediaSession *self, guint media_type)
{
  SIPMediaSessionPrivate *priv;
  gchar *object_path;
  SIPMediaStream *stream = NULL;

  g_assert (SIP_IS_MEDIA_SESSION (self));

  DEBUG ("enter");

  priv = SIP_MEDIA_SESSION_GET_PRIVATE (self);

  if (media_type == TP_MEDIA_STREAM_TYPE_AUDIO ||
      media_type == TP_MEDIA_STREAM_TYPE_VIDEO) {

    object_path = g_strdup_printf ("%s/MediaStream%d", priv->object_path, priv->streams->len);
    
    stream = g_object_new (SIP_TYPE_MEDIA_STREAM,
			   "media-session", self,
			   "media-type", media_type,
			   "object-path", object_path,
			   "id", priv->streams->len,
			   NULL);

    g_free (object_path);
 
    g_signal_connect (stream, "new-active-candidate-pair",
		      (GCallback) priv_stream_new_active_candidate_pair_cb,
		      self);
    g_signal_connect (stream, "new-native-candidate",
		      (GCallback) priv_stream_new_native_candidate_cb,
		      self);
    g_signal_connect (stream, "ready",
		      (GCallback) priv_stream_ready_cb,
		      self);
    g_signal_connect (stream, "supported-codecs",
		      (GCallback) priv_stream_supported_codecs_cb,
		      self);

    if (priv->se_ready == TRUE) {
      priv_emit_new_stream (self, stream);
    }

    g_signal_emit (self, signals[SIG_STREAM_ADDED], 0, stream);
  }

  /* note: we add an entry even for unsupported media types */
  g_ptr_array_add (priv->streams, stream);

  DEBUG ("exit");

  return stream;
}

static void
session_handler_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpSvcMediaSessionHandlerClass *klass = (TpSvcMediaSessionHandlerClass *)g_iface;

#define IMPLEMENT(x) tp_svc_media_session_handler_implement_##x (\
    klass, (tp_svc_media_session_handler_##x##_impl) sip_media_session_##x)
  IMPLEMENT(error);
  IMPLEMENT(ready);
#undef IMPLEMENT
}
