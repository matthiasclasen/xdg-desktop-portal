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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <string.h>

#include <glib/gi18n.h>

#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>

#include "geolocation.h"
#include "request.h"
#include "permissions.h"
#include "xdp-dbus.h"
#include "xdp-utils.h"
#include <geoclue.h>

#define PERMISSIONS_TABLE "portals"
#define PERMISSIONS_ID "geolocation"

typedef struct _Geolocation Geolocation;
typedef struct _GeolocationClass GeolocationClass;

struct _Geolocation
{
  XdpGeolocationSkeleton parent_instance;
};

struct _GeolocationClass
{
  XdpGeolocationSkeletonClass parent_class;
};

static Geolocation *geolocation;
static XdpImplAccess *access_impl;
static GClueSimple *simple;
static GHashTable *requests;

static void
emit_location_updated (Request *request,
                       GClueLocation *location)
{
  GList *connections, *l;
  GVariant *signal_variant;

  connections = g_dbus_interface_skeleton_get_connections (G_DBUS_INTERFACE_SKELETON (request));

  signal_variant = g_variant_new ("(dddddds)",
                                  gclue_location_get_latitude (location),
                                  gclue_location_get_longitude (location),
                                  gclue_location_get_altitude (location),
                                  gclue_location_get_accuracy (location),
                                  gclue_location_get_speed (location),
                                  gclue_location_get_heading (location),
                                  gclue_location_get_description (location));
  g_variant_ref_sink (signal_variant);

  for (l = connections; l != NULL; l = l->next)
    {
      GDBusConnection *connection = l->data;
      g_dbus_connection_emit_signal (connection,
                                     request->sender,
                                     request->id,
                                     "org.freedesktop.portal.Geolocation",
                                     "LocationUpdated",
                                     signal_variant,
                                     NULL);
    }

  g_variant_unref (signal_variant);
  g_list_free_full (connections, g_object_unref);
}

static void
purge_dead_requests (void)
{
  Request *request;
  GHashTableIter iter;

  g_hash_table_iter_init (&iter, requests);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *)&request))
    {
      if (!request->exported)
        g_hash_table_iter_remove (&iter);
    }
}

static void
notify_cb (GObject *obj,
           GParamSpec *pspec,
           gpointer data)
{
  GClueLocation *location = gclue_simple_get_location (simple);
  Request *request;
  GHashTableIter iter;

  g_hash_table_iter_init (&iter, requests);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *)&request))
    {
      if (request->exported)
        emit_location_updated (request, location);
      else
        g_hash_table_iter_remove (&iter);
    }
}

static void
ensure_geoclue (void)
{
  g_autoptr(GError) error = NULL;

  if (simple != NULL)
    return;

  simple = gclue_simple_new_sync ("xdg-desktop-portal",
                                  GCLUE_ACCURACY_LEVEL_EXACT,
                                  NULL,
                                  &error);
  if (simple == NULL)
    {
      g_warning ("Failed to get geoclue: %s", error->message);
      return;
    }

  g_signal_connect (simple, "notify::location", G_CALLBACK (notify_cb), NULL);
}

GType geolocation_get_type (void) G_GNUC_CONST;
static void geolocation_iface_init (XdpGeolocationIface *iface);

G_DEFINE_TYPE_WITH_CODE (Geolocation, geolocation, XDP_TYPE_GEOLOCATION_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_GEOLOCATION, geolocation_iface_init))

static gboolean
get_geolocation_permissions (const char *app_id,
                             gboolean *allowed,
                             gint64 *last_used)
{
  g_autoptr(GVariant) out_perms = NULL;
  g_autoptr(GVariant) out_data = NULL;
  g_autoptr(GError) error = NULL;
  const char **perms;

  if (!xdp_impl_permission_store_call_lookup_sync (get_permission_store (),
                                                   PERMISSIONS_TABLE,
                                                   PERMISSIONS_ID,
                                                   &out_perms,
                                                   &out_data,
                                                   NULL,
                                                   &error))
    {
      g_warning ("Error getting permissions: %s", error->message);
      return FALSE;
    }

  if (out_perms == NULL)
    return FALSE;

  if (!g_variant_lookup (out_perms, app_id, "^a&s", &perms))
    return FALSE;

  if (g_strv_length ((char **)perms) < 2)
    {
      g_warning ("Wrong permission format");
      return FALSE;
    }

  *allowed = !g_str_equal (perms[0], "NONE");
  *last_used = g_ascii_strtoll (perms[1], NULL, 10);

  return TRUE;
}

static void
set_geolocation_permissions (const char *app_id,
                             gboolean allowed,
                             gint64 timestamp)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *date = NULL;
  const char *permissions[3];

  date = g_strdup_printf ("%li", timestamp);
  permissions[0] = allowed ? "EXACT" : "NONE";
  permissions[1] = (const char *)date;
  permissions[2] = NULL;

  if (!xdp_impl_permission_store_call_set_permission_sync (get_permission_store (),
                                                           PERMISSIONS_TABLE,
                                                           TRUE,
                                                           PERMISSIONS_ID,
                                                           app_id,
                                                           permissions,
                                                           NULL,
                                                           &error))
    {
      g_warning ("Error setting permissions: %s", error->message);
    }
}

static void
handle_track_in_thread_func (GTask *task,
                             gpointer source_object,
                             gpointer task_data,
                             GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  g_autofree char *handle = NULL;
  g_autoptr(GError) error = NULL;
  guint response = 2;
  GVariantBuilder opt_builder;
  const char *parent_window;
  gboolean allowed;
  gint64 last_used = 0;
  const char *app_id;

  REQUEST_AUTOLOCK (request);

  parent_window = (const char *)g_object_get_data (G_OBJECT (request), "parent-window");

  app_id = request->app_id;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  if (!get_geolocation_permissions (app_id, &allowed, &last_used))
    {
      guint access_response = 2;
      g_autoptr(GVariant) access_results = NULL;
      g_autoptr(XdpImplRequest) impl_request = NULL;
      GVariantBuilder access_opt_builder;
      g_autofree char *title = NULL;
      g_autofree char *subtitle = NULL;
      const char *body;

      impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (access_impl)),
                                                      G_DBUS_PROXY_FLAGS_NONE,
                                                      g_dbus_proxy_get_name (G_DBUS_PROXY (access_impl)),
                                                      request->id,
                                                      NULL, NULL);

      request_set_impl_request (request, impl_request);

      g_variant_builder_init (&access_opt_builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "deny_label", g_variant_new_string (_("Deny Access")));
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "grant_label", g_variant_new_string (_("Grant Access")));
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "icon", g_variant_new_string ("find-location-symbolic"));

      if (g_str_equal (app_id, ""))
        {
          title = g_strdup_printf (_("Grant Access to Your Location?"));
          subtitle = g_strdup_printf (_("An application wants to use your location."));
        }
      else
        {
          g_autofree char *id = NULL;
          g_autoptr(GDesktopAppInfo) info = NULL;
          const char *name;

          id = g_strconcat (app_id, ".desktop", NULL);
          info = g_desktop_app_info_new (id);
          name = g_app_info_get_display_name (G_APP_INFO (info));

          title = g_strdup_printf (_("Give %s Access to Your Location?"), name);
          if (g_desktop_app_info_has_key (info, "X-Geoclue-Reason"))
            subtitle = g_desktop_app_info_get_string (info, "X-Geoclue-Reason");
          else
            subtitle = g_strdup_printf (_("%s wants to use your location."), name);
        }

      body = _("Location access can be changed at any time from the privacy settings.");

      if (!xdp_impl_access_call_access_dialog_sync (access_impl,
                                                    request->id,
                                                    app_id,
                                                    parent_window,
                                                    title,
                                                    subtitle,
                                                    body,
                                                    g_variant_builder_end (&access_opt_builder),
                                                    &access_response,
                                                    &access_results,
                                                    NULL,
                                                    &error))
        {
          g_warning ("Failed to show access dialog: %s", error->message);
          goto out;
        }

      request_set_impl_request (request, NULL);

      allowed = access_response == 0;
    }

  set_geolocation_permissions (app_id, allowed, allowed ? g_get_monotonic_time () : last_used);

  if (!allowed)
    {
      response = 1;
      goto out;
    }

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  ensure_geoclue ();

  g_hash_table_insert (requests, g_strdup (request->id), g_object_ref (request));
  purge_dead_requests ();
  emit_location_updated (request, gclue_simple_get_location (simple));

  response = 0;

out:
  if (request->exported)
     xdp_request_emit_response (XDP_REQUEST (request), response, g_variant_builder_end (&opt_builder));
}

static gboolean
geolocation_handle_track_location (XdpGeolocation *object,
                                   GDBusMethodInvocation *invocation,
                                   const char *arg_parent_window,
                                   GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GTask) task = NULL;

  g_object_set_data_full (G_OBJECT (request), "parent-window", g_strdup (arg_parent_window), g_free);
  g_object_set_data_full (G_OBJECT (request), "options", g_variant_ref (arg_options), (GDestroyNotify)g_variant_unref);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));
  xdp_geolocation_complete_track_location (object, invocation, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_track_in_thread_func);

  return TRUE;
}

static void
geolocation_class_init (GeolocationClass *klass)
{
}

static void
geolocation_iface_init (XdpGeolocationIface *iface)
{
  iface->handle_track_location = geolocation_handle_track_location;
}

static void
geolocation_init (Geolocation *resolver)
{
}

GDBusInterfaceSkeleton *
geolocation_create (GDBusConnection *connection,
                    const char *dbus_name)
{
  g_autoptr(GError) error = NULL;

  access_impl = xdp_impl_access_proxy_new_sync (connection,
                                                G_DBUS_PROXY_FLAGS_NONE,
                                                dbus_name,
                                                DESKTOP_PORTAL_OBJECT_PATH,
                                                NULL, &error);
  if (access_impl == NULL)
    {
      g_warning ("Failed to create access proxy: %s", error->message);
      return NULL;
    }

  geolocation = g_object_new (geolocation_get_type (), NULL);
  requests = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

  return G_DBUS_INTERFACE_SKELETON (geolocation);
}
