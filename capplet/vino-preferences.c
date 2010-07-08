/*
 * Copyright (C) 2003 Sun Microsystems, Inc.
 * Copyright (C) 2006 Jonh Wendell <wendell@bani.com.br> 
 * Copyright © 2010 Codethink Limited
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Authors:
 *      Mark McLoughlin <mark@skynet.ie>
 *      Jonh Wendell <wendell@bani.com.br>
 *      Ryan Lortie <desrt@desrt.ca>
 */

#include "config.h"

#include "vino-connectivity-info.h"
#include "vino-radio-button.h"
#include "vino-message-box.h"
#include "vino-keyring.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

typedef struct
{
  GtkApplication        parent_instance;

  VinoConnectivityInfo *info;
} VinoPreferences;

typedef GtkApplicationClass VinoPreferencesClass;

static GType vino_preferences_get_type (void);
G_DEFINE_TYPE (VinoPreferences, vino_preferences, GTK_TYPE_APPLICATION)


/* We define three GSettings mappings here:
 *
 * First, a relatively boring boolean inversion mapping.
 */
static gboolean
get_inverted (GValue   *value,
              GVariant *variant,
              gpointer  user_data)
{
  g_value_set_boolean (value, !g_variant_get_boolean (variant));
  return TRUE;
}

static GVariant *
set_inverted (const GValue       *value,
              const GVariantType *type,
              gpointer            user_data)
{
  return g_variant_new_boolean (!g_value_get_boolean (value));
}

/* Next, one that maps between the array-of-strings list of
 * authentication mechanisms and a boolean that is FALSE if the 'none'
 * and TRUE otherwise (ie: for 'vnc' in the list).
 */
static gboolean
get_authtype (GValue   *value,
              GVariant *variant,
              gpointer  user_data)
{
  GVariantIter iter;
  const gchar *type;

  g_variant_iter_init (&iter, variant);
  g_value_set_boolean (value, TRUE);

  while (g_variant_iter_next (&iter, "s", &type))
    if (strcmp (type, "none") == 0)
      g_value_set_boolean (value, FALSE);

  return TRUE;
}

static GVariant *
set_authtype (const GValue       *value,
              const GVariantType *type,
              gpointer            user_data)
{
  const gchar *authtype;

  if (g_value_get_boolean (value))
    authtype = "vnc";
  else
    authtype = "none";

  return g_variant_new_strv (&authtype, 1);
}


/* Finally, a somewhat evil mapping for the password setting:
 *
 * If the setting is 'keyring' then we load the password from the
 * keyring.  Else, it is assumed to be a base64-encoded string which is
 * the actual password.
 *
 * On setting the password, we always first try to use the keyring.  If
 * that is successful we write 'keyring' to the settings.  If it fails
 * then we base64-encode the password and write it to the settings.
 *
 * Doing it this way ensures that there is no ambiguity about what the
 * password is in the event that gnome-keyring becomes available then
 * unavailable again.
 */
static gboolean
get_password (GValue   *value,
              GVariant *variant,
              gpointer  user_data)
{
  const gchar *setting;

  setting = g_variant_get_string (variant, NULL);

  if (strcmp (setting, "keyring") == 0)
    {
      g_value_take_string (value, vino_keyring_get_password ());
      return TRUE;
    }
  else
    {
      gchar *decoded;
      gsize length;
      gboolean ok;

      decoded = (gchar *) g_base64_decode (setting, &length);

      if ((ok = g_utf8_validate (decoded, length, NULL)))
        g_value_take_string (value, g_strndup (decoded, length));

      return ok;
    }
}

static GVariant *
set_password (const GValue       *value,
              const GVariantType *type,
              gpointer            user_data)
{
  const gchar *string;
  gchar *base64;

  string = g_value_get_string (value);

  /* first, try to put it in the keyring */
  if (vino_keyring_set_password (string))
    return g_variant_new_string ("keyring");

  /* if that failed, store it in GSettings, base64 */
  base64 = g_base64_encode ((guchar *) string, strlen (string));
  return g_variant_new_from_data (G_VARIANT_TYPE_STRING,
                                  base64, strlen (base64) + 1,
                                  TRUE, g_free, base64);
}

static void
vino_preferences_dialog_response (GtkWidget *widget,
                                  gint       response,
                                  gpointer   user_data)
{
  GError *error = NULL;
  GdkScreen *screen;

  switch (response)
    {
    case GTK_RESPONSE_HELP:
      screen = gtk_widget_get_screen (widget);

      if (!gtk_show_uri (screen, "ghelp:user-guide?goscustdesk-90",
                         GDK_CURRENT_TIME, &error))
        {
          GtkWidget *message_dialog;

          message_dialog =
            gtk_message_dialog_new (GTK_WINDOW (widget),
                                    GTK_DIALOG_DESTROY_WITH_PARENT,
                                    GTK_MESSAGE_ERROR,
                                    GTK_BUTTONS_CLOSE,
                                    _("There was an error displaying help:\n%s"),
                                    error->message);
          gtk_window_set_resizable (GTK_WINDOW (message_dialog), FALSE);

          g_signal_connect (message_dialog, "response",
                            G_CALLBACK (gtk_widget_destroy),
                            NULL);

          gtk_widget_show (message_dialog);

          g_error_free (error);
        }
      break;

    default:
      gtk_widget_destroy (widget);
    }
}

static void
vino_preferences_info_changed (VinoConnectivityInfo *info,
                               gpointer              user_data)
{
  VinoMessageBox *message_box = VINO_MESSAGE_BOX (user_data);
  gchar *internal_host, *external_host, *avahi_host;
  guint16 internal_port, external_port;

  if (!vino_connectivity_info_get (info,
                                   &internal_host, &internal_port,
                                   &external_host, &external_port,
                                   &avahi_host))
    {
      vino_message_box_set_label (message_box,
                                  _("Checking the connectivity of this machine..."));
      vino_message_box_show_image (message_box);

      return;
    }

  if (external_host || internal_host)
    {
      const gchar *host;
      GString *message;
      guint16 port;
      GString *url;

      message = g_string_new (NULL);
      url = g_string_new (NULL);
      host = external_host;
      port = external_port;

      if (host == NULL)
        {
          g_string_append (message, _("Your desktop is only reachable over "
                                      "the local network."));
          g_string_append_c (message, ' ');

          host = internal_host;
          port = internal_port;
        }

      g_string_append_printf (url, "<a href=\"vnc://%s::%d\">%s</a>",
                              host, (guint) port, host);

      if (avahi_host)
        {
          g_string_append (url, _(" or "));
          g_string_append_printf (url, "<a href=\"vnc://%s::%d\">%s</a>",
                                  avahi_host, internal_port, avahi_host);
        } 
      g_string_append_printf (message, _("Others can access your computer "
                                         "using the address %s."), url->str);

      vino_message_box_set_label (message_box, message->str);
      g_string_free (message, TRUE);
      g_string_free (url, TRUE);
    }
  else
    vino_message_box_set_label (message_box,
                                _("Nobody can access your desktop."));

  vino_message_box_hide_image (message_box);

  g_free (internal_host);
  g_free (external_host);
  g_free (avahi_host);
}

static GtkWindow *
vino_preferences_connect_ui (VinoPreferences *app,
                             GtkBuilder      *builder)
{
  struct {
    const gchar             *setting;
    const gchar             *name;
    const gchar             *property;
    GSettingsBindFlags       flags;
    GSettingsBindGetMapping  get_mapping;
    GSettingsBindSetMapping  set_mapping;
  } bindings[] = {
    { "enabled",                "allowed_toggle",        "active"                                                           },

    { "enabled",                "control_settings",      "sensitive",       G_SETTINGS_BIND_GET                             },
    { "view-only",              "view_only_toggle",      "active",          0,                   get_inverted, set_inverted },

    { "enabled",                "security_settings",     "sensitive",       G_SETTINGS_BIND_GET                             },
    { "prompt-enabled",         "prompt_enabled_toggle", "active",                                                          },
    { "authentication-methods", "password_toggle",       "active",          0,                   get_authtype, set_authtype },
    { "authentication-methods", "password_box",          "sensitive",       G_SETTINGS_BIND_GET, get_authtype               },
    { "vnc-password",           "password_entry",        "text",            0,                   get_password, set_password },
    { "use-upnp",               "use_upnp_toggle",       "active",                                                          },

    { "enabled",                "notification_settings", "sensitive",       G_SETTINGS_BIND_GET                             },
    { "icon-visibility",        "icon_always_radio",     "settings-active",                                                 }
  };
  GSettings *settings;
  gpointer window;
  gint i;

  settings = g_settings_new ("org.gnome.Vino");

  for (i = 0; i < G_N_ELEMENTS (bindings); i++)
    g_settings_bind_with_mapping (settings, bindings[i].setting,
                                  gtk_builder_get_object (builder,
                                                          bindings[i].name),
                                  bindings[i].property,
                                  bindings[i].flags,
                                  bindings[i].get_mapping,
                                  bindings[i].set_mapping,
                                  NULL, NULL);

  window = gtk_builder_get_object (builder, "vino_dialog");
  g_signal_connect (window, "response",
                    G_CALLBACK (vino_preferences_dialog_response), NULL);

  app->info = vino_connectivity_info_new (gdk_screen_get_number (gtk_window_get_screen (window)));
  g_signal_connect (app->info, "changed",
                    G_CALLBACK (vino_preferences_info_changed),
                    gtk_builder_get_object (builder, "message"));
  vino_preferences_info_changed (app->info,
                                 gtk_builder_get_object (builder, "message"));

  g_object_unref (settings);
  g_object_unref (builder);

  return window;
}

static GtkWindow *
vino_preferences_create_window (GtkApplication *gtk_app)
{
  VinoPreferences *app = (VinoPreferences *) gtk_app;
  GError     *error = NULL;
  GtkBuilder *builder;
  const char *ui_file;

  vino_radio_button_get_type ();
  vino_message_box_get_type ();

#define VINO_UI_FILE "vino-preferences.ui"
  if (g_file_test (VINO_UI_FILE, G_FILE_TEST_EXISTS))
    ui_file = VINO_UI_FILE;
  else
    ui_file = VINO_UIDIR "/" VINO_UI_FILE;
#undef VINO_UI_FILE

  builder = gtk_builder_new ();
  if (!gtk_builder_add_from_file (builder, ui_file, &error))
    {
      g_warning ("Unable to load ui file '%s': %s", ui_file, error->message);
      g_error_free (error);
      return NULL;
    }

  return vino_preferences_connect_ui (app, builder);
}

static void
vino_preferences_finalize (GObject *object)
{
  VinoPreferences *app = (VinoPreferences *) object;

  g_object_unref (app->info);

  G_OBJECT_CLASS (vino_preferences_parent_class)
    ->finalize (object);
}

static void
vino_preferences_init (VinoPreferences *app)
{
}

static void
vino_preferences_class_init (GtkApplicationClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  class->create_window = vino_preferences_create_window;
  object_class->finalize = vino_preferences_finalize;
}

static GtkApplication *
vino_preferences_new (gint argc, char **argv)
{
  GError *error = NULL;
  GtkApplication *app;
  const gchar **args;

  args = (const gchar **) argv;

  app = g_initable_new (vino_preferences_get_type (), NULL, &error,
                        "application-id", "org.gnome.Vino.Preferences",
                        "argv", g_variant_new_bytestring_array (args, argc),
                        NULL);

  if G_UNLIKELY (app == NULL)
    g_error ("%s", error->message);

  return app;
}

int
main (int argc, char **argv)
{
  GtkApplication *app;

  bindtextdomain (GETTEXT_PACKAGE, VINO_LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  gtk_init (&argc, &argv);
  app = vino_preferences_new (argc, argv);
  gtk_application_get_window (app);
  gtk_application_run (app);
  g_object_unref (app);

  return 0;
}
