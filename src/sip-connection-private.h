/* 
 * Copyright (C) 2005-2007 Collabora Ltd. and Nokia Corporation
 *
 * sip-connection-private.h- Private structues for SIPConnection
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the 
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA. 
 */

#ifndef __SIP_CONNECTION_PRIVATE_H__
#define __SIP_CONNECTION_PRIVATE_H__

#include <telepathy-glib/channel-factory-iface.h>

#include "sip-connection-sofia.h"

struct _SIPConnectionPrivate
{
  gchar *requested_address;

  SIPConnectionSofia *sofia;
  nua_t  *sofia_nua;
  su_home_t *sofia_home;
  nua_handle_t *register_op;

  gchar *registrar_realm;
  gchar *last_auth;

  /* channels */
  TpChannelFactoryIface *text_factory;
  TpChannelFactoryIface *media_factory;

  gchar *address;
  gchar *password;
  gchar *proxy;
  gchar *registrar;
  SIPConnectionKeepaliveMechanism keepalive_mechanism;
  gint keepalive_interval;
  gchar *http_proxy;
  gchar *stun_server;
  guint stun_port;
  gchar *extra_auth_user;
  gchar *extra_auth_password;
  gboolean discover_binding;

  gboolean dispose_has_run : 1;
  gboolean register_succeeded : 1;
};

#define SIP_PROTOCOL_STRING               "sip"

#define SIP_CONNECTION_GET_PRIVATE(o)     (G_TYPE_INSTANCE_GET_PRIVATE ((o), SIP_TYPE_CONNECTION, SIPConnectionPrivate))

#endif /*__SIP_CONNECTION_PRIVATE_H__*/
