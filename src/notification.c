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
#include <gio/gio.h>

#include "notification.h"
#include "request.h"
#include "permissions.h"
#include "xdp-dbus.h"
#include "xdp-dbus.h"
#include "xdp-utils.h"

#define TABLE_NAME "notifications"

typedef struct _Notification Notification;
typedef struct _NotificationClass NotificationClass;

struct _Notification
{
  XdpNotificationSkeleton parent_instance;
};

struct _NotificationClass
{
  XdpNotificationSkeletonClass parent_class;
};

static XdpImplNotification *impl;
static Notification *notification;
G_LOCK_DEFINE (active);
static GHashTable *active;

typedef struct {
  char *app_id;
  char *id;
} Pair;

static guint
pair_hash (gconstpointer v)
{
  const Pair *p = v;

  return g_str_hash (p->app_id) + g_str_hash (p->id);
}

static gboolean
pair_equal (gconstpointer v1,
            gconstpointer v2)
{
  const Pair *p1 = v1;
  const Pair *p2 = v2;

  return g_str_equal (p1->app_id, p2->app_id) && g_str_equal (p1->id, p2->id);
}

static void
pair_free (gpointer v)
{
  Pair *p = v;

  g_free (p->app_id);
  g_free (p->id);
  g_free (p);
}

static Pair *
pair_copy (Pair *o)
{
  Pair *p;

  p = g_new (Pair, 1);
  p->app_id = g_strdup (o->app_id);
  p->id = g_strdup (o->id);

  return p;
}

GType notification_get_type (void) G_GNUC_CONST;
static void notification_iface_init (XdpNotificationIface *iface);

G_DEFINE_TYPE_WITH_CODE (Notification, notification, XDP_TYPE_NOTIFICATION_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_NOTIFICATION, notification_iface_init));

static void
add_done (GObject *source,
          GAsyncResult *result,
          gpointer data)
{
  g_autoptr(Request) request = data;
  g_autoptr(GError) error = NULL;

  if (!xdp_impl_notification_call_add_notification_finish (impl, result, &error))
    {
      g_warning ("Backend call failed: %s", error->message);
    }
  else
    {
      Pair p;

      p.app_id = request->app_id;
      p.id = (char *)g_object_get_data (G_OBJECT (request), "id");

      G_LOCK (active);
      g_hash_table_insert (active, pair_copy (&p), g_strdup (request->sender));
      G_UNLOCK (active);
    }
}

static gboolean
get_notification_allowed (const char *app_id)
{
  g_autoptr(GVariant) out_perms = NULL;
  g_autoptr(GVariant) out_data = NULL;
  g_autoptr(GError) error = NULL;

  if (!xdp_impl_permission_store_call_lookup_sync (get_permission_store (),
                                                   TABLE_NAME,
                                                   "notification",
                                                   &out_perms,
                                                   &out_data,
                                                   NULL,
                                                   &error))
    {
      g_warning ("Error getting permissions: %s", error->message);
      return TRUE;
    }

  if (out_perms != NULL)
    {
      const char **perms;
      if (g_variant_lookup (out_perms, app_id, "^a&s", &perms))
        return !g_strv_contains (perms, "no");
    }

  return TRUE;
}


static void
handle_add_in_thread_func (GTask *task,
                           gpointer source_object,
                           gpointer task_data,
                           GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  const char *id;
  GVariant *notification;

  REQUEST_AUTOLOCK (request);

  if (strcmp (request->app_id, "") != 0 &&
      !get_notification_allowed (request->app_id))
    return;

  id = (const char *)g_object_get_data (G_OBJECT (request), "id");
  notification = (GVariant *)g_object_get_data (G_OBJECT (request), "notification");

  xdp_impl_notification_call_add_notification (impl,
                                               request->app_id,
                                               id,
                                               notification,
                                               NULL,
                                               add_done,
                                               g_object_ref (request));
}

static gboolean
check_value_type (const char *key,
                  GVariant *value,
                  const GVariantType *type,
                  GError **error)
{
  if (g_variant_is_of_type (value, type))
    return TRUE;

  g_set_error (error,
               XDG_DESKTOP_PORTAL_ERROR,
               XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
               "expected type for key %s is %s, found %s",
               key, (const char *)type, (const char *)g_variant_get_type (value));

  return FALSE;
}

static gboolean
check_priority (GVariant *value,
                GError **error)
{
  const char *priorities[] = { "low", "normal", "high", "urgent", NULL };

  if (!check_value_type ("priority", value, G_VARIANT_TYPE_STRING, error))
    return FALSE;

  if (!g_strv_contains (priorities, g_variant_get_string (value, NULL)))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "%s not a priority", g_variant_get_string (value, NULL));
      return FALSE;
    }

  return TRUE;
}

static gboolean
check_button (GVariant *button,
              GError **error)
{
  int i;
  gboolean has_label = FALSE;
  gboolean has_action = FALSE;

  for (i = 0; i < g_variant_n_children (button); i++)
    {
      const char *key;
      g_autoptr(GVariant) value = NULL;

      g_variant_get_child (button, i, "{&sv}", &key, &value);
      if (strcmp (key, "label") == 0)
        has_label = TRUE;
      else if (strcmp (key, "action") == 0)
        has_action = TRUE;
      else if (strcmp (key, "target") == 0)
        ;
      else
        {
          g_set_error (error,
                       XDG_DESKTOP_PORTAL_ERROR,
                       XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                       "%s not valid key", key);
          return FALSE;
        }
    }

  if (!has_label)
    {
      g_set_error_literal (error,
                           XDG_DESKTOP_PORTAL_ERROR,
                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                           "label key is missing");
      return FALSE;
    }

  if (!has_action)
    {
      g_set_error_literal (error,
                           XDG_DESKTOP_PORTAL_ERROR,
                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                           "action key is missing");
      return FALSE;
    }

  return TRUE;
}

static gboolean
check_buttons (GVariant *value,
               GError **error)
{
  int i;

  if (!check_value_type ("buttons", value, G_VARIANT_TYPE ("aa{sv}"), error))
    return FALSE;

  for (i = 0; i < g_variant_n_children (value); i++)
    {
      g_autoptr(GVariant) button = g_variant_get_child_value (value, i);

      if (!check_button (button, error))
        {
          g_prefix_error (error, "invalid button: ");
          return FALSE;
        }
    }
  return TRUE;
}

static gboolean
check_serialized_icon (GVariant *value,
                       GError **error)
{
  g_autoptr(GIcon) icon = NULL;

  if (g_variant_is_of_type (value, G_VARIANT_TYPE_STRING) ||
      g_variant_is_of_type (value, G_VARIANT_TYPE("(sv)")))
    icon = g_icon_deserialize (value);

  if (!icon)
    {
      g_set_error_literal (error,
                           XDG_DESKTOP_PORTAL_ERROR,
                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                           "invalid icon");
      return FALSE;
    }

  return TRUE;
}

static gboolean
check_notification (GVariant *notification,
                    GError **error)
{
  int i;

  if (!check_value_type ("notification", notification, G_VARIANT_TYPE_VARDICT, error))
    return FALSE;

  for (i = 0; i < g_variant_n_children (notification); i++)
    {
      const char *key;
      g_autoptr(GVariant) value = NULL;

      g_variant_get_child (notification, i, "{&sv}", &key, &value);
      if (strcmp (key, "title") == 0 ||
          strcmp (key, "body") == 0)
        {
          if (!check_value_type (key, value, G_VARIANT_TYPE_STRING, error))
            return FALSE;
        }
      else if (strcmp (key, "icon") == 0)
        {
          if (!check_serialized_icon (value, error))
            return FALSE;
        }
      else if (strcmp (key, "priority") == 0)
        {
          if (!check_priority (value, error))
            return FALSE;
        }
      else if (strcmp (key, "default-action") == 0)
        {
          if (!check_value_type (key, value, G_VARIANT_TYPE_STRING, error))
            return FALSE;
        }
      else if (strcmp (key, "default-action-target") == 0)
        ;
      else if (strcmp (key, "buttons") == 0)
        {
          if (!check_buttons (value, error))
            return FALSE;
        }
      else
        {
          g_set_error (error,
                       XDG_DESKTOP_PORTAL_ERROR,
                       XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                       "%s not valid key", key);
          return FALSE;
        }
    }

  return TRUE;
}

static gboolean
notification_handle_add_notification (XdpNotification *object,
                                      GDBusMethodInvocation *invocation,
                                      const char *arg_id,
                                      GVariant *notification)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GTask) task = NULL;
  g_autoptr(GError) error = NULL;

  g_object_set_data_full (G_OBJECT (request), "id", g_strdup (arg_id), g_free);
  g_object_set_data_full (G_OBJECT (request), "notification", g_variant_ref (notification), (GDestroyNotify)g_variant_unref);

  if (!check_notification (notification, &error))
    {
      g_prefix_error (&error, "invalid notification: ");
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_add_in_thread_func);

  xdp_notification_complete_add_notification (object, invocation);

  return TRUE;
}

static void
remove_done (GObject *source,
             GAsyncResult *result,
             gpointer data)
{
  g_autoptr(Request) request = data;
  g_autoptr(GError) error = NULL;

  if (!xdp_impl_notification_call_add_notification_finish (impl, result, &error))
    {
      g_warning ("Backend call failed: %s", error->message);
    }
  else
    {
      Pair p;

      p.app_id = request->app_id;
      p.id = (char *)g_object_get_data (G_OBJECT (request), "id");

      G_LOCK (active);
      g_hash_table_remove (active, &p);
      G_UNLOCK (active);
    }
}

static gboolean
notification_handle_remove_notification (XdpNotification *object,
                                         GDBusMethodInvocation *invocation,
                                         const char *arg_id)
{
  Request *request = request_from_invocation (invocation);

  g_object_set_data_full (G_OBJECT (request), "id", g_strdup (arg_id), g_free);

  xdp_impl_notification_call_remove_notification (impl,
                                                  request->app_id,
                                                  arg_id,
                                                  NULL,
                                                  remove_done, g_object_ref (request));

  xdp_notification_complete_remove_notification (object, invocation);

  return TRUE;
}

static void
action_invoked (GDBusConnection *connection,
                const gchar     *sender_name,
                const gchar     *object_path,
                const gchar     *interface_name,
                const gchar     *signal_name,
                GVariant        *parameters,
                gpointer         user_data)
{
   Pair p;
   const char *action;
   GVariant *param;
   const char *sender;

   g_variant_get (parameters, "(&s&s&s@av)", &p.app_id, &p.id, &action, &param);

   sender = g_hash_table_lookup (active, &p);
   if (sender == NULL)
     return;

   g_dbus_connection_emit_signal (connection,
                                  sender,
                                  "/org/freedesktop/portal/desktop",
                                  "org.freedesktop.portal.Notification",
                                  "ActionInvoked",
                                  g_variant_new ("(ss@av)",
                                                 p.id, action,
                                                 param),
                                  NULL);

}

static void
name_owner_changed (GDBusConnection *connection,
                    const gchar     *sender_name,
                    const gchar     *object_path,
                    const gchar     *interface_name,
                    const gchar     *signal_name,
                    GVariant        *parameters,
                    gpointer         user_data)
{
  const char *name, *from, *to;

  g_variant_get (parameters, "(sss)", &name, &from, &to);

  if (name[0] == ':' &&
      strcmp (name, from) == 0 &&
      strcmp (to, "") == 0)
    {
      GHashTableIter iter;
      Pair *p;

      G_LOCK (active);

      g_hash_table_iter_init (&iter, active);
      while (g_hash_table_iter_next (&iter, (gpointer *)&p, NULL))
        {
          if (g_strcmp0 (p->app_id, name) == 0)
            g_hash_table_iter_remove (&iter);
        }

      G_UNLOCK (active);
    }
}

static void
notification_iface_init (XdpNotificationIface *iface)
{
  iface->handle_add_notification = notification_handle_add_notification;
  iface->handle_remove_notification = notification_handle_remove_notification;
}

enum {
  PROP_0,
  PROP_VERSION
};

static void
notification_set_property (GObject *object,
                           guint prop_id,
                           const GValue *value,
                           GParamSpec *pspec)
{
  G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
notification_get_property (GObject *object,
                           guint prop_id,
                           GValue *value,
                           GParamSpec *pspec)
{
  switch (prop_id)
    {
    case PROP_VERSION:
      g_value_set_uint (value, 1);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
notification_init (Notification *resolver)
{
}

static void
notification_class_init (NotificationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = notification_set_property;
  object_class->get_property = notification_get_property;

  xdp_notification_override_properties (object_class, PROP_VERSION);
}

GDBusInterfaceSkeleton *
notification_create (GDBusConnection *connection,
                     const char *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_notification_proxy_new_sync (connection,
                                               G_DBUS_PROXY_FLAGS_NONE,
                                               dbus_name,
                                               DESKTOP_PORTAL_OBJECT_PATH,
                                               NULL, &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create notification proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  notification = g_object_new (notification_get_type (), NULL);
  active = g_hash_table_new_full (pair_hash, pair_equal, pair_free, g_free);

  g_dbus_connection_signal_subscribe (connection,
                                      dbus_name,
                                      "org.freedesktop.portal.impl.Notification",
                                      "ActionInvoked",
                                      DESKTOP_PORTAL_OBJECT_PATH,
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      action_invoked,
                                      NULL, NULL);

  g_dbus_connection_signal_subscribe (connection,
                                      "org.freedesktop.DBus",
                                      "org.freedesktop.DBus",
                                      "NameOwnerChanged",
                                      "/org/freedesktop/DBus",
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      name_owner_changed,
                                      NULL, NULL);

  return G_DBUS_INTERFACE_SKELETON (notification);
}
