/*
 * sip-connection-private.h - Private structures for RakiaConnection
 * Copyright (C) 2005-2007 Collabora Ltd.
 * Copyright (C) 2005-2009 Nokia Corporation
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

#ifndef __RAKIA_CONNECTION_PRIVATE_H__
#define __RAKIA_CONNECTION_PRIVATE_H__

#include "config.h"

#include <rakia/media-manager.h>
#include <rakia/sofia-decls.h>
#include <sofia-sip/sresolv.h>

#include <telepathy-glib/telepathy-glib.h>

#ifdef HAVE_LIBIPHB
#include <iphbd/libiphb.h>
#endif

struct _RakiaConnectionPrivate
{
  nua_t  *sofia_nua;
  su_home_t *sofia_home;
  nua_handle_t *register_op;
  sres_resolver_t *sofia_resolver;
  const url_t *account_url;
  url_t *proxy_url;
  url_t *registrar_url;

#ifdef HAVE_LIBIPHB
  iphb_t    heartbeat;
  su_wait_t heartbeat_wait[1];
  int       heartbeat_wait_id;
#endif

  gchar *registrar_realm;

  RakiaMediaManager *media_manager;
  TpSimplePasswordManager *password_manager;

  gchar *address;
  gchar *auth_user;
  gchar *password;
  gchar *alias;
  gchar *transport;
  RakiaConnectionKeepaliveMechanism keepalive_mechanism;
  guint keepalive_interval;
  gboolean discover_stun;
  gchar *stun_host;
  guint stun_port;
  gchar *local_ip_address;
  guint local_port;
  gchar *extra_auth_user;
  gchar *extra_auth_password;
  gboolean loose_routing;
  gboolean discover_binding;
  gboolean immutable_streams;
  gboolean ignore_tls_errors;

  gboolean keepalive_interval_specified;

  gboolean dispose_has_run;
};

/* #define RAKIA_PROTOCOL_STRING               "sip" */

#define RAKIA_CONNECTION_GET_PRIVATE(o)     (G_TYPE_INSTANCE_GET_PRIVATE ((o), RAKIA_TYPE_CONNECTION, RakiaConnectionPrivate))

#endif /*__RAKIA_CONNECTION_PRIVATE_H__*/
