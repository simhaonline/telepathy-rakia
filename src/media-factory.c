/*
 * media-factory.c - Media channel factory for SIP connection manager
 * Copyright (C) 2007 Collabora Ltd.
 * Copyright (C) 2007-2008 Nokia Corporation
 *
 * This work is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This work is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this work; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <telepathy-glib/svc-connection.h>
#include <telepathy-glib/interfaces.h>
#include <string.h>
#include "media-factory.h"
#include "sip-connection.h"

#define DEBUG_FLAG TPSIP_DEBUG_CONNECTION
#include "debug.h"

static void factory_iface_init (gpointer, gpointer);

G_DEFINE_TYPE_WITH_CODE (TpsipMediaFactory, tpsip_media_factory,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (TP_TYPE_CHANNEL_FACTORY_IFACE,
      factory_iface_init))

enum
{
  PROP_CONNECTION = 1,
  PROP_STUN_SERVER,
  PROP_STUN_PORT,
  LAST_PROPERTY
};

typedef struct _TpsipMediaFactoryPrivate TpsipMediaFactoryPrivate;
struct _TpsipMediaFactoryPrivate
{
  /* unreferenced (since it owns this factory) */
  TpsipConnection *conn;
  /* array of referenced (TpsipMediaChannel *) */
  GPtrArray *channels;
  /* for unique channel object paths, currently always increments */
  guint channel_index;

  gchar *stun_server;
  guint16 stun_port;

  gboolean dispose_has_run;
};

#define TPSIP_MEDIA_FACTORY_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), TPSIP_TYPE_MEDIA_FACTORY, TpsipMediaFactoryPrivate))

static void
tpsip_media_factory_init (TpsipMediaFactory *fac)
{
  TpsipMediaFactoryPrivate *priv = TPSIP_MEDIA_FACTORY_GET_PRIVATE (fac);

  priv->conn = NULL;
  priv->channels = g_ptr_array_sized_new (1);
  priv->channel_index = 0;
  priv->dispose_has_run = FALSE;
}

static void
tpsip_media_factory_dispose (GObject *object)
{
  TpsipMediaFactory *fac = TPSIP_MEDIA_FACTORY (object);
  TpsipMediaFactoryPrivate *priv = TPSIP_MEDIA_FACTORY_GET_PRIVATE (fac);

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  tp_channel_factory_iface_close_all (TP_CHANNEL_FACTORY_IFACE (object));

  g_assert (priv->channels == NULL);

  g_free (priv->stun_server);

  if (G_OBJECT_CLASS (tpsip_media_factory_parent_class)->dispose)
    G_OBJECT_CLASS (tpsip_media_factory_parent_class)->dispose (object);
}

static void
tpsip_media_factory_get_property (GObject *object,
                               guint property_id,
                               GValue *value,
                               GParamSpec *pspec)
{
  TpsipMediaFactory *fac = TPSIP_MEDIA_FACTORY (object);
  TpsipMediaFactoryPrivate *priv = TPSIP_MEDIA_FACTORY_GET_PRIVATE (fac);

  switch (property_id) {
    case PROP_CONNECTION:
      g_value_set_object (value, priv->conn);
      break;
    case PROP_STUN_SERVER:
      g_value_set_string (value, priv->stun_server);
      break;
    case PROP_STUN_PORT:
      g_value_set_uint (value, priv->stun_port);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
tpsip_media_factory_set_property (GObject *object,
                               guint property_id,
                               const GValue *value,
                               GParamSpec *pspec)
{
  TpsipMediaFactory *fac = TPSIP_MEDIA_FACTORY (object);
  TpsipMediaFactoryPrivate *priv = TPSIP_MEDIA_FACTORY_GET_PRIVATE (fac);

  switch (property_id) {
    case PROP_CONNECTION:
      priv->conn = g_value_get_object (value);
      break;
    case PROP_STUN_SERVER:
      g_free (priv->stun_server);
      priv->stun_server = g_value_dup_string (value);
      break;
    case PROP_STUN_PORT:
      priv->stun_port = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
tpsip_media_factory_class_init (TpsipMediaFactoryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamSpec *param_spec;

  g_type_class_add_private (klass, sizeof (TpsipMediaFactoryPrivate));

  object_class->get_property = tpsip_media_factory_get_property;
  object_class->set_property = tpsip_media_factory_set_property;
  object_class->dispose = tpsip_media_factory_dispose;

  param_spec = g_param_spec_object ("connection", "TpsipConnection object",
      "SIP connection that owns this media channel factory",
      TPSIP_TYPE_CONNECTION,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CONNECTION, param_spec);

  param_spec = g_param_spec_string ("stun-server", "STUN server address",
      "STUN server address",
      NULL,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_STUN_SERVER, param_spec);

  param_spec = g_param_spec_uint ("stun-port", "STUN port",
      "STUN port.",
      0, G_MAXUINT16,
      TPSIP_DEFAULT_STUN_PORT,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_STUN_PORT, param_spec);
}

static void
tpsip_media_factory_close_one (gpointer data, gpointer user_data)
{
  tpsip_media_channel_close (TPSIP_MEDIA_CHANNEL(data));
  g_object_unref (data);
}

static void
tpsip_media_factory_close_all (TpChannelFactoryIface *iface)
{
  TpsipMediaFactory *fac = TPSIP_MEDIA_FACTORY (iface);
  TpsipMediaFactoryPrivate *priv = TPSIP_MEDIA_FACTORY_GET_PRIVATE (fac);
  GPtrArray *channels;

  channels = priv->channels;
  priv->channels = NULL;
  if (channels)
    {
      g_ptr_array_foreach (channels, tpsip_media_factory_close_one, NULL);
      g_ptr_array_free (channels, TRUE);
    }
}

static void
tpsip_media_factory_connecting (TpChannelFactoryIface *iface)
{
}

static void
tpsip_media_factory_connected (TpChannelFactoryIface *iface)
{
}

static void
tpsip_media_factory_disconnected (TpChannelFactoryIface *iface)
{
}

struct _ForeachData
{
  TpChannelFunc foreach;
  gpointer user_data;
};

static void
_foreach_slave (gpointer channel, gpointer user_data)
{
  struct _ForeachData *data = (struct _ForeachData *)user_data;
  TpChannelIface *chan = TP_CHANNEL_IFACE (channel);

  data->foreach (chan, data->user_data);
}

static void
tpsip_media_factory_foreach (TpChannelFactoryIface *iface,
                          TpChannelFunc foreach,
                          gpointer user_data)
{
  TpsipMediaFactory *fac = TPSIP_MEDIA_FACTORY (iface);
  TpsipMediaFactoryPrivate *priv = TPSIP_MEDIA_FACTORY_GET_PRIVATE (fac);

  struct _ForeachData data = { foreach, user_data };

  g_ptr_array_foreach (priv->channels, _foreach_slave, &data);
}

/**
 * channel_closed:
 *
 * Signal callback for when a media channel is closed. Removes the references
 * that #TpsipMediaFactory holds to them.
 */
static void
channel_closed (TpsipMediaChannel *chan, gpointer user_data)
{
  TpsipMediaFactory *fac = TPSIP_MEDIA_FACTORY (user_data);
  TpsipMediaFactoryPrivate *priv = TPSIP_MEDIA_FACTORY_GET_PRIVATE (fac);

  if (priv->channels)
    {
      g_ptr_array_remove_fast (priv->channels, chan);
      g_object_unref (chan);
    }
}

/**
 * new_media_channel
 *
 * Creates a new empty TpsipMediaChannel.
 */
TpsipMediaChannel *
tpsip_media_factory_new_channel (TpsipMediaFactory *fac,
                                 gpointer request,
                                 TpHandleType handle_type,
                                 TpHandle handle,
                                 TpHandle creator,
                                 GError **error)
{
  TpsipMediaFactoryPrivate *priv;
  TpsipMediaChannel *chan;
  TpBaseConnection *conn;
  gchar *object_path;
  const gchar *nat_traversal = "none";

  g_assert (TPSIP_IS_MEDIA_FACTORY (fac));
  g_assert (creator != 0);

  priv = TPSIP_MEDIA_FACTORY_GET_PRIVATE (fac);
  conn = (TpBaseConnection *)priv->conn;

  object_path = g_strdup_printf ("%s/MediaChannel%u", conn->object_path,
      priv->channel_index++);

  DEBUG("channel object path %s", object_path);

  if (priv->stun_server != NULL)
    {
      nat_traversal = "stun";
    }

  chan = g_object_new (TPSIP_TYPE_MEDIA_CHANNEL,
                       "connection", priv->conn,
                       "object-path", object_path,
                       "creator", creator,
                       "nat-traversal", nat_traversal,
                       NULL);

  g_free (object_path);

  if (priv->stun_server != NULL)
    {
      g_object_set ((GObject *) chan, "stun-server", priv->stun_server, NULL);
      if (priv->stun_port != 0)
        g_object_set ((GObject *) chan, "stun-port", priv->stun_port, NULL);
    }

  if (handle_type == TP_HANDLE_TYPE_CONTACT && handle != creator)
    {
      g_assert (creator == conn->self_handle);

      if (!_tpsip_media_channel_add_member ((GObject *) chan,
               handle, "", error))
        goto err;
    }

  g_signal_connect (chan, "closed", (GCallback) channel_closed, fac);

  g_ptr_array_add (priv->channels, chan);

  tp_channel_factory_iface_emit_new_channel (fac, (TpChannelIface *)chan,
      request);

  return chan;

err:
  g_object_unref (chan);
  return NULL;
}

static TpChannelFactoryRequestStatus
tpsip_media_factory_request (TpChannelFactoryIface *iface,
                             const gchar *chan_type,
                             TpHandleType handle_type,
                             TpHandle handle,
                             gpointer request,
                             TpChannelIface **ret,
                             GError **error_ret)
{
  TpsipMediaFactory *fac = TPSIP_MEDIA_FACTORY (iface);
  TpsipMediaFactoryPrivate *priv;
  TpChannelIface *chan;
  TpChannelFactoryRequestStatus status = TP_CHANNEL_FACTORY_REQUEST_STATUS_ERROR;
  TpBaseConnection *base_conn;
  GError *error = NULL;

  if (strcmp (chan_type, TP_IFACE_CHANNEL_TYPE_STREAMED_MEDIA))
    {
      return TP_CHANNEL_FACTORY_REQUEST_STATUS_NOT_IMPLEMENTED;
    }

  priv = TPSIP_MEDIA_FACTORY_GET_PRIVATE (fac);
  base_conn = (TpBaseConnection *)priv->conn;

  chan = (TpChannelIface *) tpsip_media_factory_new_channel (fac,
                                                             request,
                                                             handle_type,
                                                             handle,
                                                             base_conn->self_handle,
                                                             &error);
  if (chan != NULL)
    {
      *ret = chan;
      status = TP_CHANNEL_FACTORY_REQUEST_STATUS_CREATED;
    }
  else
    {
      g_assert (error != NULL);
      switch (error->code)
        {
        case TP_ERROR_INVALID_HANDLE:
        /* case TP_ERROR_INVALID_ARGUMENT: */
          status = TP_CHANNEL_FACTORY_REQUEST_STATUS_INVALID_HANDLE;
          break;
        default:
          status = TP_CHANNEL_FACTORY_REQUEST_STATUS_ERROR;
        }
      if (error_ret != NULL)
        *error_ret = error;
      else
        g_error_free (error);
    }
  return status;
}

static void
factory_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpChannelFactoryIfaceClass *klass = (TpChannelFactoryIfaceClass *) g_iface;

#define IMPLEMENT(x) klass->x = tpsip_media_factory_##x
  IMPLEMENT(close_all);
  IMPLEMENT(foreach);
  IMPLEMENT(request);
  IMPLEMENT(connecting);
  IMPLEMENT(connected);
  IMPLEMENT(disconnected);
#undef IMPLEMENT
}
