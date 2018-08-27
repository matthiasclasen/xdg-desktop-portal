/*
 * Copyright © 2018 Red Hat, Inc
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

#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>

#include "actions.h"
#include "request.h"
#include "permissions.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

typedef struct _Actions Actions;
typedef struct _ActionsClass ActionsClass;

struct _Actions
{
  XdpActionsSkeleton parent_instance;
};

struct _ActionsClass
{
  XdpActionsSkeletonClass parent_class;
};

static XdpImplAccess *impl;
static Actions *actions;

GType actions_get_type (void) G_GNUC_CONST;
static void actions_iface_init (XdpActionsIface *iface);

G_DEFINE_TYPE_WITH_CODE (Actions, actions, XDP_TYPE_ACTIONS_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_ACTIONS, actions_iface_init));

#define TABLE_NAME "actions"

typedef enum { UNSET, NO, YES, ASK } Permission;

static Permission
get_permission (const char *app_id,
                const char *application,
                const char *action)
{
  g_autofree char *obj = NULL;
  g_autoptr(GVariant) out_perms = NULL;
  g_autoptr(GVariant) out_data = NULL;
  g_autoptr(GError) error = NULL;
  const char **permissions;

  obj = g_strconcat (application, "::", action, NULL);

  if (!xdp_impl_permission_store_call_lookup_sync (get_permission_store (),
                                                   TABLE_NAME,
                                                   obj,
                                                   &out_perms,
                                                   &out_data,
                                                   NULL,
                                                   &error))
    {
      g_warning ("Error reading from permission store: %s", error->message);
      return UNSET;
    }

 if (!g_variant_lookup (out_perms, app_id, "^a&s", &permissions))
    {
      g_debug ("No permissions stored for: action %s, app %s", obj, app_id);

      return UNSET;
    }
  else if (g_strv_length ((char **)permissions) != 1)
    {
      g_autofree char *a = g_strjoinv (" ", (char **)permissions);
      g_warning ("Wrong permission format, ignoring (%s)", a);
      return UNSET;
    }

  g_debug ("permission store: action %s, app %s -> %s", obj, app_id, permissions[0]);

  if (strcmp (permissions[0], "yes") == 0)
    return YES;
  else if (strcmp (permissions[0], "no") == 0)
    return NO;
  else if (strcmp (permissions[0], "ask") == 0)
    return ASK;
  else
    {
      g_autofree char *a = g_strjoinv (" ", (char **)permissions);
      g_warning ("Wrong permission format, ignoring (%s)", a);
    }

  return UNSET;
}

static void
set_permission (const char *app_id,
                const char *application,
                const char *action,
                Permission permission)
{
  g_autofree char *obj = g_strconcat (application, "::", action, NULL);  

  g_autoptr(GError) error = NULL;
  const char *permissions[2];

  if (permission == ASK)
    permissions[0] = "ask";
  else if (permission == YES)
    permissions[0] = "yes";
  else if (permission == NO)
    permissions[0] = "no";
  else
    {
      g_warning ("Wrong permission format, ignoring");
      return;
    }
  permissions[1] = NULL;

  if (!xdp_impl_permission_store_call_set_permission_sync (get_permission_store (),
                                                           TABLE_NAME,
                                                           TRUE,
                                                           obj,
                                                           app_id,
                                                           (const char * const*)permissions,
                                                           NULL,
                                                           &error))
    {
      g_warning ("Error updating permission store: %s", error->message);
    }
}

static char *
application_path_from_appid (const char *appid)
{
  char *appid_path, *iter;

  appid_path = g_strconcat ("/", appid, NULL);
  for (iter = appid_path; *iter; iter++)
    {
      if (*iter == '.')
        *iter = '/';

      if (*iter == '-')
        *iter = '_';
    }

  return appid_path;
}

static void
handle_activate_action_in_thread (GTask *task,
                                  gpointer source_object,
                                  gpointer task_data,
                                  GCancellable *cancellable)
{
  Request *request = task_data;
  const char *app_id;
  const char *window;
  const char *application;
  const char *action;
  GVariant *parameters;
  GVariant *platform_data;
  Permission permission;
  gboolean allowed;
  guint32 response = 2;

  REQUEST_AUTOLOCK (request);

  app_id = xdp_app_info_get_id (request->app_info);
  window = (const char *)g_object_get_data (G_OBJECT (request), "window");
  application = (const char *)g_object_get_data (G_OBJECT (request), "application");
  action = (const char *)g_object_get_data (G_OBJECT (request), "action");
  parameters = (GVariant*)g_object_get_data (G_OBJECT (request), "parameters");
  platform_data = (GVariant*)g_object_get_data (G_OBJECT (request), "platform-data");

  permission = get_permission (app_id, application, action);
  if (permission == ASK || permission == UNSET)
    {
      GVariantBuilder opt_builder;
      g_autofree char *title = NULL;
      g_autofree char *subtitle = NULL;
      g_autofree char *body = NULL;
      g_autoptr(GVariant) results = NULL;
      g_autoptr(GError) error = NULL;
      guint32 access_response = 2;
      g_autoptr(GAppInfo) info = NULL;
      g_autoptr(GAppInfo) info2 = NULL;
      const char *app_name = NULL;
      const char *app_name2 = NULL;

      if (app_id[0] != 0)
        {
          g_autofree char *desktop_id;

          desktop_id = g_strconcat (app_id, ".desktop", NULL);
          info = (GAppInfo*)g_desktop_app_info_new (desktop_id);
          app_name = g_app_info_get_display_name (info);
        }

      if (application != NULL)
        {
          g_autofree char *desktop_id;

          desktop_id = g_strconcat (application, ".desktop", NULL);
          info2 = (GAppInfo*)g_desktop_app_info_new (desktop_id);
          app_name2 = g_app_info_get_display_name (info2);
        }

      if (app_name2 == NULL)
        app_name2 = application;

      g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

      title = g_strdup_printf (_("Activate the '%s' action in %s?"), action, app_name2);

      if (app_name == NULL)
        subtitle = g_strdup_printf (_("An application wants to activate %s"), app_name2);
      else
        subtitle = g_strdup_printf (_("%s wants to activate %s"), app_name, app_name2);

      body = g_strdup (_("Access to application actions can be changed "
                         "from the privacy settings at any time."));

      g_variant_builder_add (&opt_builder, "{sv}", "icon", g_variant_new_string ("emblem-system-symbolic"));

      g_debug ("Calling backend for access to action %s::%s", application, action);

      if (!xdp_impl_access_call_access_dialog_sync (impl,
                                                    request->id,
                                                    app_id,
                                                    window,
                                                    title,
                                                    subtitle,
                                                    body,
                                                    g_variant_builder_end (&opt_builder),
                                                    &access_response,
                                                    &results,
                                                    NULL,
                                                    &error))
        {
          g_warning ("A backend call failed: %s", error->message);
        }

      allowed = access_response == 0;

      if (permission == UNSET)
        set_permission (app_id, application, action, allowed ? YES : NO);
    }
  else
    allowed = permission == YES ? TRUE : FALSE;

  if (allowed)
    {
      GDBusConnection *bus;
      g_autofree char *path = application_path_from_appid (application);
      g_autoptr(GVariant) ret = NULL;
      g_autoptr(GError) error = NULL;

      if (platform_data == NULL)
        {
          GVariantBuilder builder;
          g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
          platform_data = g_variant_ref_sink (g_variant_builder_end (&builder));
        }

      bus = g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (actions));
      ret = g_dbus_connection_call_sync (bus,
                                         application,
                                         path,
                                         "org.freedesktop.Application",
                                         "ActivateAction",
                                         g_variant_new ("(s@av@a{sv})", action, parameters, platform_data),
                                         G_VARIANT_TYPE ("()"),
                                         G_DBUS_CALL_FLAGS_NONE,
                                         -1,
                                         NULL,
                                         &error);
      if (!ret)
        {
          g_warning ("Failed to activate action %s::%s: %s", application, action, error->message);
          response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
        }
      else
        response = XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS;
    }
  else
    response = XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED;

  if (request->exported)
    {
      GVariantBuilder results;

      g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
      xdp_request_emit_response (XDP_REQUEST (request),
                                 response,
                                 g_variant_builder_end (&results));
      request_unexport (request);
    }
}

static gboolean
handle_activate_action (XdpActions *object,
                        GDBusMethodInvocation *invocation,
                        const char *arg_parent_window,
                        const char *arg_application,
                        const char *arg_action,
                        GVariant *arg_parameters,
                        GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  g_autoptr(GVariant) platform_data = NULL;
  g_autoptr(GTask) task = NULL;

  REQUEST_AUTOLOCK (request);

  g_object_set_data_full (G_OBJECT (request), "window", g_strdup (arg_parent_window), g_free);
  g_object_set_data_full (G_OBJECT (request), "application", g_strdup (arg_application), g_free);
  g_object_set_data_full (G_OBJECT (request), "action", g_strdup (arg_action), g_free);
  g_object_set_data_full (G_OBJECT (request), "parameters", g_variant_ref (arg_parameters), (GDestroyNotify)g_variant_unref);

  if (g_variant_lookup (arg_options, "@a{sv}", "platform-data", &platform_data))
    g_object_set_data_full (G_OBJECT (request), "platform-data", g_variant_ref (platform_data), (GDestroyNotify)g_variant_unref);

  impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                                  request->id,
                                                  NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_actions_complete_activate_action (object, invocation, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_activate_action_in_thread);

  return TRUE;
}

static void
actions_iface_init (XdpActionsIface *iface)
{
  iface->handle_activate_action = handle_activate_action;
}

static void
actions_init (Actions *actions)
{
  xdp_actions_set_version (XDP_ACTIONS (actions), 1);
}

static void
actions_class_init (ActionsClass *klass)
{
}

GDBusInterfaceSkeleton *
actions_create (GDBusConnection *connection,
                const char      *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_access_proxy_new_sync (connection,
                                         G_DBUS_PROXY_FLAGS_NONE,
                                         dbus_name,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         NULL,
                                         &error);

  if (impl == NULL)
    {
      g_warning ("Failed to create access proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  actions = g_object_new (actions_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (actions);
}
