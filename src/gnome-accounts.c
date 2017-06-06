/*
 * Copyright © 2017 Red Hat, Inc
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

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "email.h"
#include "request.h"
#include "documents.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

typedef struct _GnomeAccounts GnomeAccounts;
typedef struct _GnomeAccountsClass GnomeAccountsClass;

struct _GnomeAccounts
{
  XdpGnomeAccountsSkeleton parent_instance;
};

struct _GnomeAccountsClass
{
  XdpGnomeAccountsSkeletonClass parent_class;
};

static XdpImplGnomeAccounts *impl;
static GnomeAccounts *gnome_accounts;

GType gnome_accounts_get_type (void) G_GNUC_CONST;
static void gnome_accounts_iface_init (XdpGnomeAccountsIface *iface);

G_DEFINE_TYPE_WITH_CODE (GnomeAccounts, gnome_accounts, XDP_TYPE_GNOME_ACCOUNTS_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_GNOME_ACCOUNTS, gnome_accounts_iface_init));

static void
send_response_in_thread_func (GTask        *task,
                              gpointer      source_object,
                              gpointer      task_data,
                              GCancellable *cancellable)
{
  Request *request = task_data;
  guint response;
  GVariantBuilder new_results;

  g_variant_builder_init (&new_results, G_VARIANT_TYPE_VARDICT);

  REQUEST_AUTOLOCK (request);

  response = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "response"));

  if (request->exported)
    {
      xdp_request_emit_response (XDP_REQUEST (request),
                                 response,
                                 g_variant_builder_end (&new_results));
      request_unexport (request);
    }
}

/* handle_get_accounts */

static void
get_accounts_done (GObject *source,
                   GAsyncResult *result,
                   gpointer data)
{
  g_autoptr(Request) request = data;
  guint response = 2;
  g_autoptr(GVariant) results = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;

  if (!xdp_impl_gnome_accounts_call_get_accounts_finish (XDP_IMPL_GNOME_ACCOUNTS (source),
                                                         &response,
                                                         &results,
                                                         result,
                                                         &error))
    {
      g_warning ("Backend call failed: %s", error->message);
    }

  g_object_set_data (G_OBJECT (request), "response", GINT_TO_POINTER (response));
  g_object_set_data_full (G_OBJECT (request), "results", g_variant_ref (results), (GDestroyNotify)g_variant_unref);

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, send_response_in_thread_func);
}

static XdpOptionKey get_accounts_options[] = {
  { "providers", G_VARIANT_TYPE_STRING_ARRAY },
  { "interfaces", G_VARIANT_TYPE_STRING_ARRAY },
};

static gboolean
handle_get_accounts (XdpGnomeAccounts *object,
                     GDBusMethodInvocation *invocation,
                     const gchar *arg_parent_window,
                     GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = request->app_id;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  GVariantBuilder options;

  REQUEST_AUTOLOCK (request);

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

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  xdp_filter_options (arg_options, &options,
                      get_accounts_options, G_N_ELEMENTS (get_accounts_options));

  xdp_impl_gnome_accounts_call_get_accounts (impl,
                                             request->id,
                                             app_id,
                                             arg_parent_window,
                                             g_variant_builder_end (&options),
                                             NULL,
                                             get_accounts_done,
                                             g_object_ref (request));

  xdp_gnome_accounts_complete_get_accounts (object, invocation, request->id);

  return TRUE;
}

/* handle_add_account */

static void
add_account_done (GObject *source,
                  GAsyncResult *result,
                  gpointer data)
{
  g_autoptr(Request) request = data;
  guint response = 2;
  g_autoptr(GVariant) results = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;

  if (!xdp_impl_gnome_accounts_call_add_account_finish (XDP_IMPL_GNOME_ACCOUNTS (source),
                                                        &response,
                                                        &results,
                                                        result,
                                                        &error))
    {
      g_warning ("Backend call failed: %s", error->message);
    }

  g_object_set_data (G_OBJECT (request), "response", GINT_TO_POINTER (response));
  g_object_set_data_full (G_OBJECT (request), "results", g_variant_ref (results), (GDestroyNotify)g_variant_unref);

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, send_response_in_thread_func);
}

static XdpOptionKey add_account_options[] = {
  { "providers", G_VARIANT_TYPE_STRING_ARRAY },
  { "interfaces", G_VARIANT_TYPE_STRING_ARRAY },
};

static gboolean
handle_add_account (XdpGnomeAccounts *object,
                    GDBusMethodInvocation *invocation,
                    const gchar *arg_parent_window,
                    const gchar *arg_provider,
                    GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = request->app_id;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  GVariantBuilder options;

  REQUEST_AUTOLOCK (request);

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

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  xdp_filter_options (arg_options, &options,
                      add_account_options, G_N_ELEMENTS (add_account_options));

  xdp_impl_gnome_accounts_call_add_account (impl,
                                            request->id,
                                            app_id,
                                            arg_parent_window,
                                            arg_provider,
                                            g_variant_builder_end (&options),
                                            NULL,
                                            add_account_done,
                                            g_object_ref (request));

  xdp_gnome_accounts_complete_add_account (object, invocation, request->id);

  return TRUE;
}

/* handle_ensure_credentials */

static void
ensure_credentials_done (GObject *source,
                         GAsyncResult *result,
                         gpointer data)
{
  g_autoptr(Request) request = data;
  guint response = 2;
  g_autoptr(GVariant) results = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;

  if (!xdp_impl_gnome_accounts_call_ensure_credentials_finish (XDP_IMPL_GNOME_ACCOUNTS (source),
                                                               &response,
                                                               &results,
                                                               result,
                                                               &error))
    {
      g_warning ("Backend call failed: %s", error->message);
    }

  g_object_set_data (G_OBJECT (request), "response", GINT_TO_POINTER (response));
  g_object_set_data_full (G_OBJECT (request), "results", g_variant_ref (results), (GDestroyNotify)g_variant_unref);

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, send_response_in_thread_func);
}

static XdpOptionKey ensure_credentials_options[] = {
};

static gboolean
handle_ensure_credentials (XdpGnomeAccounts *object,
                           GDBusMethodInvocation *invocation,
                           const gchar *arg_parent_window,
                           const gchar *arg_account_id,
                           GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = request->app_id;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  GVariantBuilder options;

  REQUEST_AUTOLOCK (request);

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

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  xdp_filter_options (arg_options, &options,
                      ensure_credentials_options, G_N_ELEMENTS (ensure_credentials_options));

  xdp_impl_gnome_accounts_call_ensure_credentials (impl,
                                                   request->id,
                                                   app_id,
                                                   arg_parent_window,
                                                   arg_account_id,
                                                   g_variant_builder_end (&options),
                                                   NULL,
                                                   ensure_credentials_done,
                                                   g_object_ref (request));

  xdp_gnome_accounts_complete_ensure_credentials (object, invocation, request->id);

  return TRUE;
}

static void
gnome_accounts_iface_init (XdpGnomeAccountsIface *iface)
{
  iface->handle_get_accounts = handle_get_accounts;
  iface->handle_add_account = handle_add_account;
  iface->handle_ensure_credentials = handle_ensure_credentials;
}

static void
gnome_accounts_init (GnomeAccounts *gnome_accounts)
{
  xdp_gnome_accounts_set_version (XDP_GNOME_ACCOUNTS (gnome_accounts), 1);
}

static void
gnome_accounts_class_init (GnomeAccountsClass *klass)
{
}

static void
accounts_changed_cb (XdpImplGnomeAccounts *impl,
                     gpointer data)
{
  xdp_gnome_accounts_emit_accounts_changed (XDP_GNOME_ACCOUNTS (gnome_accounts));
}

GDBusInterfaceSkeleton *
gnome_accounts_create (GDBusConnection *connection,
                       const char      *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_gnome_accounts_proxy_new_sync (connection,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 dbus_name,
                                                 DESKTOP_PORTAL_OBJECT_PATH,
                                                 NULL,
                                                 &error);

  if (impl == NULL)
    {
      g_warning ("Failed to create gnome accounts proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  g_signal_connect (impl, "accounts-changed", G_CALLBACK (accounts_changed_cb), NULL);

  gnome_accounts = g_object_new (gnome_accounts_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (gnome_accounts);
}
