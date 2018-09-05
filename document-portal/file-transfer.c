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
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "file-transfer.h"
#include "src/xdp-utils.h"
#include "document-portal-dbus.h"
#include "document-enums.h"
#include "document-portal.h"
#include "document-portal-fuse.h"

static XdpDbusFileTransfer *file_transfer;

typedef struct
{
  GMutex mutex;

  GPtrArray *files;
  gboolean writable;
  gboolean autostop;
  char *key;
  char *sender;
  XdpAppInfo *app_info;
} FileTransfer;

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

#define TRANSFER_AUTOLOCK(transfer) \
  G_GNUC_UNUSED __attribute__((cleanup (auto_unlock_helper))) \
  GMutex * G_PASTE (auto_unlock, __LINE__) = \
    auto_lock_helper (&transfer->mutex);

static void
file_transfer_free (gpointer data)
{
  FileTransfer *transfer = (FileTransfer *)data;

  g_mutex_clear (&transfer->mutex);
  xdp_app_info_unref (transfer->app_info);
  g_ptr_array_unref (transfer->files);
  g_free (transfer->key);
  g_free (transfer->sender);
  g_free (transfer);
}

G_LOCK_DEFINE (transfers);
static GHashTable *transfers;

static FileTransfer *
lookup_transfer (const char *key)
{
  FileTransfer *transfer;

  G_LOCK (transfers);
  transfer = (FileTransfer *)g_hash_table_lookup (transfers, key);
  G_UNLOCK (transfers);

  return transfer;
}

static FileTransfer *
file_transfer_start (XdpAppInfo *app_info,
                     const char *sender,
                     gboolean    writable,
                     gboolean    autostop)
{
  FileTransfer *transfer;

  transfer = g_new0 (FileTransfer, 1);

  g_mutex_init (&transfer->mutex);

  transfer->app_info = xdp_app_info_ref (app_info);
  transfer->sender = g_strdup (sender);
  transfer->writable = writable;
  transfer->autostop = autostop;
  transfer->key = g_strdup_printf ("%u", g_random_int ());
  transfer->files = g_ptr_array_new_with_free_func (g_free);

  G_LOCK (transfers);
  g_hash_table_insert (transfers, transfer->key, transfer);
  G_UNLOCK (transfers);

  g_debug ("start file transfer owned by '%s' (%s)",
           xdp_app_info_get_id (transfer->app_info),
           transfer->sender);

  return transfer;
}

static gboolean
stop (gpointer data)
{
  FileTransfer *transfer = data;

  file_transfer_free (transfer);

  return G_SOURCE_REMOVE;
}

static void
file_transfer_stop (FileTransfer *transfer)
{
  g_debug ("stop file transfer owned by '%s' (%s)",
           xdp_app_info_get_id (transfer->app_info),
           transfer->sender);

  G_LOCK (transfers);
  g_hash_table_steal (transfers, transfer->key);
  G_UNLOCK (transfers);

  g_idle_add (stop, transfer);
}

static void
file_transfer_add_files (FileTransfer *transfer,
                         const char **files)
{
  int i;

  for (i = 0; files[i]; i++)
    g_ptr_array_add (transfer->files, g_strdup (files[i]));

  g_debug ("add %d files to file transfer owned by '%s' (%s)", g_strv_length (files),
           xdp_app_info_get_id (transfer->app_info),
           transfer->sender);
}

static char **
file_transfer_execute (FileTransfer *transfer,
                       XdpAppInfo *target_app_info,
                       GError **error)
{
  guint32 flags;
  DocumentPermissionFlags perms;
  const char *target_app_id;
  int n_fds;
  g_autofree int *fds = NULL;
  int i;
  g_auto(GStrv) ids = NULL;
  char **files = NULL;

  g_debug ("retrieve %d files for %s from file transfer owned by '%s' (%s)",
           transfer->files->len,
           xdp_app_info_get_id (target_app_info),
           xdp_app_info_get_id (transfer->app_info),
           transfer->sender);

  if (xdp_app_info_is_host (target_app_info))
    return g_strdupv ((char **)transfer->files->pdata);

  flags = DOCUMENT_ADD_FLAGS_REUSE_EXISTING | DOCUMENT_ADD_FLAGS_AS_NEEDED_BY_APP;
  
  perms = DOCUMENT_PERMISSION_FLAGS_READ;
  if (transfer->writable)
    perms |= DOCUMENT_PERMISSION_FLAGS_WRITE;

  target_app_id = xdp_app_info_get_id (target_app_info);

  n_fds = transfer->files->len;
  fds = g_new (int, n_fds);
  for (i = 0; i < n_fds; i++)
    {
      const char *file;

      file = g_ptr_array_index (transfer->files, i);
      fds[i] = open (file, O_PATH | O_CLOEXEC);
    }

  ids = document_add_full (fds, n_fds, flags, transfer->app_info, target_app_id, perms, error);

  for (i = 0; i < n_fds; i++)
    close (fds[i]);

  if (ids)
    {
      const char *mountpoint = xdp_fuse_get_mountpoint ();
      files = g_new (char *, n_fds + 1);
      for (i = 0; i < n_fds; i++)
        {
          const char *file = g_ptr_array_index (transfer->files, i);

          if (ids[i][0] == '\0')
            files[i] = g_strdup (file);
          else
            {
              g_autofree char *name = g_path_get_basename (file);
              files[i] = g_build_filename (mountpoint, ids[i], name, NULL);
            }
        }
      files[n_fds] = NULL;
    }

  return files;
}

static void
start_transfer (GDBusMethodInvocation *invocation,
                GVariant *parameters,
                XdpAppInfo *app_info)
{
  g_autoptr(GVariant) options = NULL;
  gboolean writable;
  gboolean autostop;
  FileTransfer *transfer;
  const char *sender;

  g_variant_get (parameters, "(@a{sv})", &options);
  if (!g_variant_lookup (options, "writable", "b", &writable))
    writable = FALSE;

  if (!g_variant_lookup (options, "autostop", "b", &autostop))
    autostop = TRUE;

  sender = g_dbus_method_invocation_get_sender (invocation);

  transfer = file_transfer_start (app_info, sender, writable, autostop);

  g_dbus_method_invocation_return_value (invocation, g_variant_new ("(s)", transfer->key));
}

static void
add_files (GDBusMethodInvocation *invocation,
           GVariant *parameters,
           XdpAppInfo *app_info)
{
  FileTransfer *transfer;
  const char *key;
  g_autoptr(GVariant) options = NULL;
  GDBusMessage *message;
  GUnixFDList *fd_list;
  g_autoptr(GVariantIter) iter = NULL;
  g_autoptr(GPtrArray) files = NULL;
  GError *error = NULL;
  int fd_id;
  const int *fds;
  int n_fds;

  g_variant_get (parameters, "(&sah@a{sv})", &key, &iter, &options);

  transfer = lookup_transfer (key);
  if (transfer == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid transfer");
      return;
    }

  TRANSFER_AUTOLOCK (transfer);

  if (strcmp (transfer->sender, g_dbus_method_invocation_get_sender (invocation)) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid transfer");
      return;
    }

  message = g_dbus_method_invocation_get_message (invocation);
  fd_list = g_dbus_message_get_unix_fd_list (message);

  files = g_ptr_array_new_with_free_func (g_free);
  fds = g_unix_fd_list_peek_fds (fd_list, &n_fds);

  while (g_variant_iter_next (iter, "h", &fd_id))
    {
      int fd = -1;
      g_autofree char *path = NULL;
      gboolean fd_is_writable;

      if (fd_id < n_fds)
        fd = fds[fd_id];

      if (fd == -1)
        {
          g_dbus_method_invocation_return_gerror (invocation, error);
          return;
        }

      path = xdp_app_info_get_path_for_fd (app_info, fd, 0, NULL, &fd_is_writable);
      if (path == NULL || (transfer->writable && !fd_is_writable))
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 XDG_DESKTOP_PORTAL_ERROR,
                                                 XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                                 "Can't export file");
          return;
        }

      g_ptr_array_add (files, g_steal_pointer (&path));
    }

  g_ptr_array_add (files, NULL);

  file_transfer_add_files (transfer, (const char **)files->pdata);

  g_dbus_method_invocation_return_value (invocation, NULL);
}


static void
retrieve_files (GDBusMethodInvocation *invocation,
                GVariant *parameters,
                XdpAppInfo *app_info)
{
  const char *key;
  FileTransfer *transfer;
  g_auto(GStrv) files = NULL;
  GError *error = NULL;

  g_variant_get (parameters, "(&s@a{sv})", &key, NULL);

  transfer = lookup_transfer (key);
  if (transfer == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid transfer");
      return;
    }

  TRANSFER_AUTOLOCK (transfer);

  files = file_transfer_execute (transfer, app_info, &error);
  if (files == NULL)
    g_dbus_method_invocation_return_gerror (invocation, error);
  else
    g_dbus_method_invocation_return_value (invocation, g_variant_new ("(^as)", files));

  if (transfer->autostop)
    file_transfer_stop (transfer);
}

static void
stop_transfer (GDBusMethodInvocation *invocation,
                GVariant *parameters,
                XdpAppInfo *app_info)
{
  const char *key;
  FileTransfer *transfer;

  g_variant_get (parameters, "(&s)", &key);

  transfer = lookup_transfer (key);
  if (transfer == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid transfer");
      return;
    }

  TRANSFER_AUTOLOCK (transfer);

  file_transfer_stop (transfer);

  g_dbus_method_invocation_return_value (invocation, NULL);
}

typedef void (*PortalMethod) (GDBusMethodInvocation *invocation,
                              GVariant              *parameters,
                              XdpAppInfo            *app_info);

static gboolean
handle_method (GCallback              method_callback,
               GDBusMethodInvocation *invocation)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpAppInfo) app_info = NULL;
  PortalMethod portal_method = (PortalMethod)method_callback;

  app_info = xdp_invocation_lookup_app_info_sync (invocation, NULL, &error);
  if (app_info == NULL)
    g_dbus_method_invocation_return_gerror (invocation, error);
  else
    portal_method (invocation, g_dbus_method_invocation_get_parameters (invocation), app_info);

  return TRUE;
}

GDBusInterfaceSkeleton *
file_transfer_create (void)
{
  file_transfer = xdp_dbus_file_transfer_skeleton_new ();

  g_signal_connect_swapped (file_transfer, "handle-start-transfer", G_CALLBACK (handle_method), start_transfer);
  g_signal_connect_swapped (file_transfer, "handle-add-files", G_CALLBACK (handle_method), add_files);
  g_signal_connect_swapped (file_transfer, "handle-retrieve-files", G_CALLBACK (handle_method), retrieve_files);
  g_signal_connect_swapped (file_transfer, "handle-stop-transfer", G_CALLBACK (handle_method), stop_transfer);

  xdp_dbus_file_transfer_set_version (XDP_DBUS_FILE_TRANSFER (file_transfer), 1);

  transfers = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, file_transfer_free);

  return G_DBUS_INTERFACE_SKELETON (file_transfer);
}

