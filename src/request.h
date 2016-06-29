/*
 * Copyright © 2016 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 *       Matthias Clasen <mclasen@redhat.com>
 */

#pragma once

#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"

typedef struct _Request Request;
typedef struct _RequestClass RequestClass;

struct _Request
{
  XdpRequestSkeleton parent_instance;

  gboolean exported;
  char *app_id;
  char *id;
  char *sender;
  GMutex mutex;

  XdpImplRequest *impl_request;
};

struct _RequestClass
{
  XdpRequestSkeletonClass parent_class;
};

GType request_get_type (void) G_GNUC_CONST;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Request, g_object_unref)

void set_proxy_use_threads (GDBusProxy *proxy);

void request_init_invocation (GDBusMethodInvocation  *invocation, const char *app_id);
Request *request_from_invocation (GDBusMethodInvocation *invocation);
void request_export (Request *request,
                     GDBusConnection *connection);
void request_unexport (Request *request);
void close_requests_for_sender (const char *sender);

void request_set_impl_request (Request *request,
                               XdpImplRequest *impl_request);

static inline void
auto_unlock_helper (GMutex **mutex)
{
  if (*mutex)
    g_mutex_unlock (*mutex);
}

static inline GMutex *
auto_lock_helper (GMutex *mutex)
{
  if (mutex)
    g_mutex_lock (mutex);
  return mutex;
}

#define REQUEST_AUTOLOCK(request) G_GNUC_UNUSED __attribute__((cleanup (auto_unlock_helper))) GMutex * G_PASTE (request_auto_unlock, __LINE__) = auto_lock_helper (&request->mutex);
