/**
 * Copyright (c) 2008-2014 LxDE Developers, see the file AUTHORS for details.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <alsa/asoundlib.h>
#include <poll.h>
#include <libfm/fm-gtk.h>

#include <gst/gst.h>
#include <gst/audio/mixerutils.h>
#include <gst/interfaces/mixer.h>

#include "plugin.h"

#define ICONS_VOLUME_HIGH   PACKAGE_DATA_DIR "/images/volume-high.png"
#define ICONS_VOLUME_MEDIUM PACKAGE_DATA_DIR "/images/volume-medium.png"
#define ICONS_VOLUME_LOW    PACKAGE_DATA_DIR "/images/volume-low.png"
#define ICONS_MUTE          PACKAGE_DATA_DIR "/images/mute.png"
#define ICONS_TICK          PACKAGE_DATA_DIR "/images/dialog-ok-apply.png"

#define ICON_BUTTON_TRIM 4

#define CUSTOM_MENU

typedef struct {

    /* Graphics. */
    GtkWidget * plugin;             /* Back pointer to the widget */
    LXPanel * panel;                /* Back pointer to panel */
    config_setting_t * settings;        /* Plugin settings */
    GtkWidget * tray_icon;          /* Displayed image */
    GtkWidget * popup_window;           /* Top level window for popup */
    GtkWidget * volume_scale;           /* Scale for volume */
    GtkWidget * mute_check;         /* Checkbox for mute state */
    GtkWidget * menu_popup;
    gboolean show_popup;            /* Toggle to show and hide the popup on left click */
    guint volume_scale_handler;         /* Handler for vscale widget */
    guint mute_check_handler;           /* Handler for mute_check widget */

    /* ALSA interface. */
    snd_mixer_t * mixer;            /* The mixer */
    snd_mixer_selem_id_t * sid;         /* The element ID */
    snd_mixer_elem_t * master_element;      /* The Master element */
    guint mixer_evt_idle;           /* Timer to handle restarting poll */
    guint restart_idle;

    /* unloading and error handling */
    GIOChannel **channels;                      /* Channels that we listen to */
    guint *watches;                             /* Watcher IDs for channels */
    guint num_channels;                         /* Number of channels */

    /* Icons */
    const char* icon;
    const char* icon_panel;
    const char* icon_fallback;

    GDBusObjectManager *objmanager;         /* needed for Bluetooth audio devices */
    const char *bt_conname;                 /* BlueZ name of device - just used during connection */
    gboolean bt_used;                       /* flag to show if the current device is Bluetooth */
    int sink;                               /* sink number of current Bluetooth device */
    GDBusConnection *con;                   /* DBus P2P connection to PulseAudio server */
} VolumeALSAPlugin;

static void send_message (void);
static gboolean asound_restart(gpointer vol_gpointer);
static gboolean asound_initialize(VolumeALSAPlugin * vol);
static void asound_deinitialize(VolumeALSAPlugin * vol);
static void volumealsa_update_display(VolumeALSAPlugin * vol);
static void volumealsa_destructor(gpointer user_data);
static void volumealsa_build_popup_window(GtkWidget *p);
static guint xfce_mixer_is_default_card (GstElement *card);
static const gchar *xfce_mixer_get_card_display_name (GstElement *card);
static gboolean xfce_mixer_is_bcm_device (GstElement *card);
static const gchar * xfce_mixer_get_bcm_device_id (void);
static GtkWidget *volumealsa_configure(LXPanel *panel, GtkWidget *p);


static guint32 get_bt_vol (VolumeALSAPlugin *vol)
{
    char buffer[32];
    sprintf (buffer, "/org/pulseaudio/core1/sink%d", vol->sink);
    GVariant *var = g_dbus_connection_call_sync (vol->con, NULL, buffer, "org.freedesktop.DBus.Properties", "Get", g_variant_new ("(ss)", "org.PulseAudio.Core1.Device", "Volume"), NULL, 0, -1, NULL, NULL);
    return g_variant_get_uint32 (g_variant_get_child_value (g_variant_get_child_value (g_variant_get_child_value (var, 0), 0), 0));
}

static gboolean get_bt_mute (VolumeALSAPlugin *vol)
{
    char buffer[32];
    sprintf (buffer, "/org/pulseaudio/core1/sink%d", vol->sink);
    GVariant *var = g_dbus_connection_call_sync (vol->con, NULL, buffer, "org.freedesktop.DBus.Properties", "Get", g_variant_new ("(ss)", "org.PulseAudio.Core1.Device", "Mute"), NULL, 0, -1, NULL, NULL);
    return g_variant_get_boolean (g_variant_get_child_value (g_variant_get_child_value (var, 0), 0));
}

static void set_bt_vol (VolumeALSAPlugin *vol, guint32 volume)
{
    GVariantBuilder *builder;
    GVariant *value, *var;
    char buffer[32];

    builder = g_variant_builder_new (G_VARIANT_TYPE ("au"));
    g_variant_builder_add (builder, "u", volume);
    value = g_variant_new ("au", builder);
    var = g_variant_new ("(ssv)", "org.PulseAudio.Core1.Device", "Volume", value);
    sprintf (buffer, "/org/pulseaudio/core1/sink%d", vol->sink);
    g_dbus_connection_call_sync (vol->con, NULL, buffer, "org.freedesktop.DBus.Properties", "Set", var, NULL, 0, -1, NULL, NULL);
}

static void set_bt_mute (VolumeALSAPlugin *vol, gboolean mute)
{
    GVariant *value, *var;
    char buffer[32];

    value = g_variant_new ("b", mute);
    var = g_variant_new ("(ssv)", "org.PulseAudio.Core1.Device", "Mute", value);
    sprintf (buffer, "/org/pulseaudio/core1/sink%d", vol->sink);
    g_dbus_connection_call_sync (vol->con, NULL, buffer, "org.freedesktop.DBus.Properties", "Set", var, NULL, 0, -1, NULL, NULL);
}

static gboolean get_bt_default_dev (VolumeALSAPlugin *vol, char **dev)
{
    GError *error;
    GVariant *var1, *var2;

    error = NULL;
    var1 = g_dbus_connection_call_sync (vol->con, NULL, "/org/pulseaudio/core1", "org.freedesktop.DBus.Properties", "Get", g_variant_new ("(ss)", "org.PulseAudio.Core1", "FallbackSink"), NULL, 0, -1, NULL, &error);
    if (error)
    {
        printf ("Cannot get default device - %s\n", error->message);
        return FALSE;
    }
    if (!var1)
    {
        printf ("No default sink list\n");
        return FALSE;
    }
    error = NULL;
    var2 = g_dbus_connection_call_sync (vol->con, NULL, g_variant_get_string (g_variant_get_child_value (g_variant_get_child_value (var1, 0), 0), NULL), "org.freedesktop.DBus.Properties", "Get", g_variant_new ("(ss)", "org.PulseAudio.Core1.Device", "Name"), NULL, 0, -1, NULL, &error);
    if (error)
    {
        printf ("Cannot get default device - %s\n", error->message);
        return FALSE;
    }
    if (!var2)
    {
        printf ("No name found\n");
        return FALSE;
    }
    printf ("Default device = %s\n", g_variant_get_string (g_variant_get_child_value (g_variant_get_child_value (var2, 0), 0), NULL));
    *dev = g_variant_dup_string (g_variant_get_child_value (g_variant_get_child_value (var2, 0), 0), NULL);
    return TRUE;
}

int get_pa_sink (VolumeALSAPlugin *vol, const char *dev)
{
    GError *error;
    GVariant *var, *var2;
    GVariantIter iter;
    gchar *key;
    const gchar *res;
    int sink;

    error = NULL;
    var = g_dbus_connection_call_sync (vol->con, NULL, "/org/pulseaudio/core1", "org.freedesktop.DBus.Properties", "Get", g_variant_new ("(ss)", "org.PulseAudio.Core1", "Sinks"), NULL, 0, -1, NULL, &error);
    if (error)
    {
        printf ("Could not read sink list - %s\n", error->message);
        return -1;
    }

    g_variant_iter_init (&iter, g_variant_get_child_value (g_variant_get_child_value (var, 0), 0));
    while (g_variant_iter_next (&iter, "o", &key, NULL))
    {
        error = NULL;
        var2 = g_dbus_connection_call_sync (vol->con, NULL, key, "org.freedesktop.DBus.Properties", "Get", g_variant_new ("(ss)", "org.PulseAudio.Core1.Device", "Name"), NULL, 0, -1, NULL, &error);
        if (error)
        {
            printf ("Could not get name - %s\n", error->message);
            return -1;
        }
        res = g_variant_get_string (g_variant_get_child_value (g_variant_get_child_value (var2, 0), 0), NULL);
        if (!g_strcmp0 (dev + 20, res + 11))
        {
            return (key[26] - '0');
        }
    }
    return -1;
}

static void set_default_sink (VolumeALSAPlugin *vol, int sink)
{
    GError *error;
    GVariant *value, *var;
    char buffer[32];

    vol->sink = sink;
    sprintf (buffer, "/org/pulseaudio/core1/sink%d", vol->sink);
    value = g_variant_new ("o", buffer);
    var = g_variant_new ("(ssv)", "org.PulseAudio.Core1", "FallbackSink", value);

    error = NULL;
    g_dbus_connection_call_sync (vol->con, NULL, "/org/pulseaudio/core1", "org.freedesktop.DBus.Properties", "Set", var, NULL, 0, -1, NULL, &error);
    if (error) printf ("Cannot set sink - %s\n", error->message);
}

static int get_default_sink (VolumeALSAPlugin *vol)
{
    GError *error = NULL;
    GVariant *var = g_dbus_connection_call_sync (vol->con, NULL, "/org/pulseaudio/core1", "org.freedesktop.DBus.Properties", "Get", g_variant_new ("(ss)", "org.PulseAudio.Core1", "FallbackSink"), NULL, 0, -1, NULL, &error);
    if (error)
    {
        printf ("Error reading sink - %s\n", error->message);
        return -1;
    }
    if (!var)
    {
        printf ("No default sink\n");
        return -1;
    }
    const char *res = g_variant_get_string (g_variant_get_child_value (g_variant_get_child_value (var, 0), 0), NULL);
    int sink = res[26] - '0';
    if (sink >= 0 && sink <= 9) return sink;
    else return -1;
}

static void cb_connected (GObject *source, GAsyncResult *res, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);

    if (error)
    {
        //system ("pulseaudio --kill");
        printf ("Connect error %s\n", error->message);
    }
    else
    {
        printf ("Connected OK\n");
        vol->bt_used = TRUE;
        int sink = get_pa_sink (vol, vol->bt_conname);
        set_default_sink (vol, sink);

        volumealsa_update_display (vol);
        gtk_menu_popdown (GTK_MENU(vol->menu_popup));
        send_message ();
    }
}

static void cb_disconnected (GObject *source, GAsyncResult *res, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);

    if (error) printf ("Disconnect error %s\n", error->message);
    else printf ("Disconnected OK\n");

    // call BlueZ over DBus to connect to the device
    printf ("Connecting...\n");
    GDBusInterface *interface = g_dbus_object_manager_get_interface (vol->objmanager, vol->bt_conname, "org.bluez.Device1");
    g_dbus_proxy_call (G_DBUS_PROXY (interface), "Connect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_connected, vol);
}

static void disconnect_device (VolumeALSAPlugin *vol)
{
    // get the name of the device with the current sink number
    GError *error = NULL;
    char buffer[32];
    sprintf (buffer, "/org/pulseaudio/core1/sink%d", vol->sink);
    GVariant *var = g_dbus_connection_call_sync (vol->con, NULL, buffer, "org.freedesktop.DBus.Properties", "Get", g_variant_new ("(ss)", "org.PulseAudio.Core1.Device", "Name"), NULL, 0, -1, NULL, &error);
    if (var)
    {
        printf ("Device to disconnect = %s\n", g_variant_print (var, TRUE));
        // convert into a BlueZ device path
        const gchar *res = g_variant_get_string (g_variant_get_child_value (g_variant_get_child_value (var, 0), 0), NULL);
        sprintf (buffer, "/org/bluez/hci0/dev_%s", res + 11);

        // call the disconnect method on BlueZ
        GDBusInterface *interface = g_dbus_object_manager_get_interface (vol->objmanager, buffer, "org.bluez.Device1");
        if (interface)
        {
            printf ("Disconnecting...\n");
            g_dbus_proxy_call (G_DBUS_PROXY (interface), "Disconnect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_disconnected, vol);
            return;
        }
    }

    // if no connection found, just make the new connection
    printf ("Nothing to disconnect - connecting...\n");
    GDBusInterface *interface = g_dbus_object_manager_get_interface (vol->objmanager, vol->bt_conname, "org.bluez.Device1");
    g_dbus_proxy_call (G_DBUS_PROXY (interface), "Connect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_connected, vol);
}

static void set_bt_card_event (GtkWidget * widget, GdkEventButton * event, VolumeALSAPlugin * vol)
{
    // start the PulseAudio server if it isn't running
    system ("pulseaudio --start");
    // ask the PulseAudio server what the PulseAudio P2P address is
    GDBusConnection *con = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
    GDBusProxy *prox = g_dbus_proxy_new_sync (con, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.PulseAudio1", "/org/pulseaudio/server_lookup1", "org.freedesktop.DBus.Properties", NULL, NULL);
    GVariant *var = g_dbus_proxy_get_cached_property (prox, "Address");

    // create a P2P connection to PulseAudio at the returned address
    vol->con = g_dbus_connection_new_for_address_sync (g_variant_get_string (var, NULL), G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT, NULL, NULL, NULL);

    // store the name of the BlueZ device to connect to for use in the callback
    vol->bt_conname = widget->name;
    printf ("Status = %d %s\n", vol->bt_used, vol->bt_conname);
    if (vol->bt_used) disconnect_device (vol);
    else
    {
        // call BlueZ over DBus to connect to the device
        printf ("Connecting...\n");
        GDBusInterface *interface = g_dbus_object_manager_get_interface (vol->objmanager, vol->bt_conname, "org.bluez.Device1");
        g_dbus_proxy_call (G_DBUS_PROXY (interface), "Connect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_connected, vol);
    }
}

/*** ALSA ***/

static gboolean asound_find_elements(VolumeALSAPlugin * vol)
{
    const char *name;
    for (
      vol->master_element = snd_mixer_first_elem(vol->mixer);
      vol->master_element != NULL;
      vol->master_element = snd_mixer_elem_next(vol->master_element))
    {
        snd_mixer_selem_get_id(vol->master_element, vol->sid);
        if ((snd_mixer_selem_is_active(vol->master_element)))
        {
            name = snd_mixer_selem_id_get_name(vol->sid);
            if (!strcmp(name, "Master")) return TRUE;
            if (!strcmp(name, "Front")) return TRUE;
            if (!strcmp(name, "PCM")) return TRUE;
            if (!strcmp(name, "LineOut")) return TRUE;
            if (!strcmp(name, "Digital")) return TRUE;
            if (!strcmp(name, "Headphone")) return TRUE;
            if (!strcmp(name, "Speaker")) return TRUE;
        }
    }
    return FALSE;
}

/* NOTE by PCMan:
 * This is magic! Since ALSA uses its own machanism to handle this part.
 * After polling of mixer fds, it requires that we should call
 * snd_mixer_handle_events to clear all pending mixer events.
 * However, when using the glib IO channels approach, we don't have
 * poll() and snd_mixer_poll_descriptors_revents(). Due to the design of
 * glib, on_mixer_event() will be called for every fd whose status was
 * changed. So, after each poll(), it's called for several times,
 * not just once. Therefore, we cannot call snd_mixer_handle_events()
 * directly in the event handler. Otherwise, it will get called for
 * several times, which might clear unprocessed pending events in the queue.
 * So, here we call it once in the event callback for the first fd.
 * Then, we don't call it for the following fds. After all fds with changed
 * status are handled, we remove this restriction in an idle handler.
 * The next time the event callback is involked for the first fs, we can
 * call snd_mixer_handle_events() again. Racing shouldn't happen here
 * because the idle handler has the same priority as the io channel callback.
 * So, io callbacks for future pending events should be in the next gmain
 * iteration, and won't be affected.
 */

static gboolean asound_reset_mixer_evt_idle(VolumeALSAPlugin * vol)
{
    if (!g_source_is_destroyed(g_main_current_source()))
        vol->mixer_evt_idle = 0;
    return FALSE;
}

/* Handler for I/O event on ALSA channel. */
static gboolean asound_mixer_event(GIOChannel * channel, GIOCondition cond, gpointer vol_gpointer)
{
    VolumeALSAPlugin * vol = (VolumeALSAPlugin *) vol_gpointer;
    int res = 0;

    if (g_source_is_destroyed(g_main_current_source()))
        return FALSE;

    if (vol->mixer_evt_idle == 0)
    {
        vol->mixer_evt_idle = g_idle_add_full(G_PRIORITY_DEFAULT, (GSourceFunc) asound_reset_mixer_evt_idle, vol, NULL);
        res = snd_mixer_handle_events(vol->mixer);
    }

    if (cond & G_IO_IN)
    {
        /* the status of mixer is changed. update of display is needed. */
        volumealsa_update_display(vol);
    }

    if ((cond & G_IO_HUP) || (res < 0))
    {
        /* This means there're some problems with alsa. */
        g_warning("volumealsa: ALSA (or pulseaudio) had a problem: "
                "volumealsa: snd_mixer_handle_events() = %d,"
                " cond 0x%x (IN: 0x%x, HUP: 0x%x).", res, cond,
                G_IO_IN, G_IO_HUP);
        gtk_widget_set_tooltip_text(vol->plugin, "ALSA (or pulseaudio) had a problem."
                " Please check the lxpanel logs.");

        if (vol->restart_idle == 0)
            vol->restart_idle = g_timeout_add_seconds(1, asound_restart, vol);

        return FALSE;
    }

    return TRUE;
}

static gboolean asound_restart(gpointer vol_gpointer)
{
    VolumeALSAPlugin * vol = vol_gpointer;

    if (g_source_is_destroyed(g_main_current_source()))
        return FALSE;

    asound_deinitialize(vol);

    if (!asound_initialize(vol)) {
        g_warning("volumealsa: Re-initialization failed.");
        return TRUE; // try again in a second
    }

    g_warning("volumealsa: Restarted ALSA interface...");

    vol->restart_idle = 0;
    return FALSE;
}

static void asound_get_default_card (char *id)
{
  char tokenbuf[256], type[16], cid[16], state = 0, indef = 0;
  char *bufptr = tokenbuf;
  int inchar, count;
  char *user_config_file = g_build_filename (g_get_home_dir (), "/.asoundrc", NULL);
  FILE *fp = fopen (user_config_file, "rb");
  if (fp)
  {
    type[0] = 0;
    cid[0] = 0;
    count = 0;
    while ((inchar = fgetc (fp)) != EOF)
    {
        if (inchar == ' ' || inchar == '\t' || inchar == '\n' || inchar == '\r')
        {
            if (bufptr != tokenbuf)
            {
                *bufptr = 0;
                switch (state)
                {
                    case 1 :    strcpy (type, tokenbuf);
                                state = 0;
                                break;
                    case 2 :    strcpy (cid, tokenbuf);
                                state = 0;
                                break;
                    default :   if (!strcmp (tokenbuf, "type") && indef) state = 1;
                                else if (!strcmp (tokenbuf, "card") && indef) state = 2;
                                else if (!strcmp (tokenbuf, "pcm.!default")) indef = 1;
                                else if (!strcmp (tokenbuf, "}")) indef = 0;
                                break;
                }
                bufptr = tokenbuf;
                count = 0;
                if (cid[0] && type[0]) break;
            }
            else
            {
                bufptr = tokenbuf;
                count = 0;
            }
        }
        else
        {
            if (count < 255)
            {
                *bufptr++ = inchar;
                count++;
            }
            else tokenbuf[255] = 0;
        }
    }
    fclose (fp);
  }
  if (cid[0] && type[0]) sprintf (id, "%s:%s", type, cid);
  else sprintf (id, "hw:0");
}

static void asound_set_default_card (const char *id)
{
  char cmdbuf[256], idbuf[16], type[16], cid[16], *card, *bufptr = cmdbuf, state = 0, indef = 0;
  int inchar, count;
  char *user_config_file = g_build_filename (g_get_home_dir (), "/.asoundrc", NULL);

  // Break the id string into the type (before the colon) and the card number (after the colon)
  strcpy (idbuf, id);
  card = strchr (idbuf, ':') + 1;
  *(strchr (idbuf, ':')) = 0;

  FILE *fp = fopen (user_config_file, "rb");
  if (!fp)
  {
    // File does not exist - create it from scratch
    fp = fopen (user_config_file, "wb");
    fprintf (fp, "pcm.!default {\n\ttype %s\n\tcard %s\n}\n\nctl.!default {\n\ttype %s\n\tcard %s\n}\n", idbuf, card, idbuf, card);
    fclose (fp);
  }
  else
  {
    // File exists - check to see whether it contains a default card
    type[0] = 0;
    cid[0] = 0;
    count = 0;
    while ((inchar = fgetc (fp)) != EOF)
    {
        if (inchar == ' ' || inchar == '\t' || inchar == '\n' || inchar == '\r')
        {
            if (bufptr != cmdbuf)
            {
                *bufptr = 0;
                switch (state)
                {
                    case 1 :    strcpy (type, cmdbuf);
                                state = 0;
                                break;
                    case 2 :    strcpy (cid, cmdbuf);
                                state = 0;
                                break;
                    default :   if (!strcmp (cmdbuf, "type") && indef) state = 1;
                                else if (!strcmp (cmdbuf, "card") && indef) state = 2;
                                else if (!strcmp (cmdbuf, "pcm.!default")) indef = 1;
                                else if (!strcmp (cmdbuf, "}")) indef = 0;
                                break;
                }
                bufptr = cmdbuf;
                count = 0;
                if (cid[0] && type[0]) break;
            }
            else
            {
                bufptr = cmdbuf;
                count = 0;
            }
        }
        else
        {
            if (count < 255)
            {
                *bufptr++ = inchar;
                count++;
            }
            else cmdbuf[255] = 0;
        }
    }
    fclose (fp);
    if (cid[0] && type[0])
    {
        // This piece of sed is surely self-explanatory...
        sprintf (cmdbuf, "sed -i '/pcm.!default\\|ctl.!default/,/}/ { s/type .*/type %s/g; s/card .*/card %s/g; }' %s", idbuf, card, user_config_file);
        system (cmdbuf);
        // Oh, OK then - it looks for type * and card * within the delimiters pcm.!default or ctl.!default and } and replaces the parameters
    }
    else
    {
        // No default card; append to end of file
        fp = fopen (user_config_file, "ab");
        fprintf (fp, "\n\npcm.!default {\n\ttype %s\n\tcard %s\n}\n\nctl.!default {\n\ttype %s\n\tcard %s\n}\n", idbuf, card, idbuf, card);
        fclose (fp);
    }
  }
  g_free (user_config_file);
}

static void asound_set_bcm_card (void)
{
    const char *bcm = xfce_mixer_get_bcm_device_id ();
    if (bcm) asound_set_default_card (bcm);
}

static int asound_get_bcm_output (void)
{
    int val = -1, tmp;
    char buf[128];
    FILE *res = popen ("amixer cget numid=3", "r");
    while (!feof (res))
    {
        fgets (buf, 128, res);
        if (sscanf (buf, "  : values=%d", &tmp)) val = tmp;
    }
    fclose (res);

    if (val == 0)
    {
        /* set to analog if result is auto */
        system ("amixer cset numid=3 2");
        val = 2;
    }
    return val;
}

/* Initialize the ALSA interface. */
static gboolean asound_initialize(VolumeALSAPlugin * vol)
{
    char device[32];

    asound_get_default_card (device);
    /* Access the "default" device. */
    snd_mixer_selem_id_alloca(&vol->sid);
    snd_mixer_open(&vol->mixer, 0);
    if (snd_mixer_attach(vol->mixer, device))
    {
        g_warning ("volumealsa: Default ALSA device not valid - resetting to internal");
        asound_set_bcm_card ();
        asound_get_default_card (device);
        snd_mixer_attach(vol->mixer, device);
    }
    snd_mixer_selem_register(vol->mixer, NULL, NULL);
    snd_mixer_load(vol->mixer);

    /* Find Master element, or Front element, or PCM element, or LineOut element.
     * If one of these succeeds, master_element is valid. */
    if (!asound_find_elements (vol))
    {
        // this is a belt-and-braces check in case a driver has become corrupt...
        g_warning ("volumealsa: Can't find elements - resetting to internal");
        snd_mixer_detach (vol->mixer, device);
        snd_mixer_free (vol->mixer);
        asound_set_bcm_card ();
        asound_get_default_card (device);
        snd_mixer_open (&vol->mixer, 0);
        snd_mixer_attach (vol->mixer, device);
        snd_mixer_selem_register (vol->mixer, NULL, NULL);
        snd_mixer_load (vol->mixer);
        if (!asound_find_elements (vol)) return FALSE;
   }

    /* Set the playback volume range as we wish it. */
    snd_mixer_selem_set_playback_volume_range(vol->master_element, 0, 100);

    /* Listen to events from ALSA. */
    int n_fds = snd_mixer_poll_descriptors_count(vol->mixer);
    struct pollfd * fds = g_new0(struct pollfd, n_fds);

    vol->channels = g_new0(GIOChannel *, n_fds);
    vol->watches = g_new0(guint, n_fds);
    vol->num_channels = n_fds;

    snd_mixer_poll_descriptors(vol->mixer, fds, n_fds);
    int i;
    for (i = 0; i < n_fds; ++i)
    {
        GIOChannel* channel = g_io_channel_unix_new(fds[i].fd);
        vol->watches[i] = g_io_add_watch(channel, G_IO_IN | G_IO_HUP, asound_mixer_event, vol);
        vol->channels[i] = channel;
    }
    g_free(fds);
    return TRUE;
}

static void asound_deinitialize(VolumeALSAPlugin * vol)
{
    guint i;

    if (vol->mixer_evt_idle != 0) {
        g_source_remove(vol->mixer_evt_idle);
        vol->mixer_evt_idle = 0;
    }

    for (i = 0; i < vol->num_channels; i++) {
        g_source_remove(vol->watches[i]);
        g_io_channel_shutdown(vol->channels[i], FALSE, NULL);
        g_io_channel_unref(vol->channels[i]);
    }
    g_free(vol->channels);
    g_free(vol->watches);
    vol->channels = NULL;
    vol->watches = NULL;
    vol->num_channels = 0;

    snd_mixer_close(vol->mixer);
    vol->master_element = NULL;
    /* FIXME: unalloc vol->sid */
}

/* Get the presence of the mute control from the sound system. */
static gboolean asound_has_mute(VolumeALSAPlugin * vol)
{
    return ((vol->master_element != NULL) ? snd_mixer_selem_has_playback_switch(vol->master_element) : FALSE);
}

/* Get the condition of the mute control from the sound system. */
static gboolean asound_is_muted(VolumeALSAPlugin * vol)
{
    /* The switch is on if sound is not muted, and off if the sound is muted.
     * Initialize so that the sound appears unmuted if the control does not exist. */
    int value = 1;
    if (vol->master_element != NULL)
        snd_mixer_selem_get_playback_switch(vol->master_element, 0, &value);
    return (value == 0);
}

/* Get the volume from the sound system.
 * This implementation returns the average of the Front Left and Front Right channels. */
static int asound_get_volume(VolumeALSAPlugin * vol)
{
    long aleft = 0;
    long aright = 0;
    if (vol->master_element != NULL)
    {
        snd_mixer_selem_get_playback_volume(vol->master_element, SND_MIXER_SCHN_FRONT_LEFT, &aleft);
        snd_mixer_selem_get_playback_volume(vol->master_element, SND_MIXER_SCHN_FRONT_RIGHT, &aright);
    }
    return (aleft + aright) >> 1;
}

/* Set the volume to the sound system.
 * This implementation sets the Front Left and Front Right channels to the specified value. */
static void asound_set_volume(VolumeALSAPlugin * vol, int volume)
{
    if (vol->master_element != NULL)
    {
        snd_mixer_selem_set_playback_volume(vol->master_element, SND_MIXER_SCHN_FRONT_LEFT, volume);
        snd_mixer_selem_set_playback_volume(vol->master_element, SND_MIXER_SCHN_FRONT_RIGHT, volume);
    }
}

/*** Graphics ***/

static void volumealsa_update_current_icon(VolumeALSAPlugin * vol)
{
    /* Mute status. */
    gboolean mute;
    int level;
    /* Mute status. */
    if (!vol->bt_used)
    {
        mute = asound_is_muted(vol);
        level = asound_get_volume(vol);
    }
    else
    {
        mute = get_bt_mute (vol);
        level = get_bt_vol (vol) / 656;
    }

    /* Change icon according to mute / volume */
    const char* icon="audio-volume-muted";
    const char* icon_panel="audio-volume-muted-panel";
    const char* icon_fallback=ICONS_MUTE;
    if (mute)
    {
         icon_panel = "audio-volume-muted-panel";
         icon="audio-volume-muted";
         icon_fallback=ICONS_MUTE;
    }
    else if (level >= 75)
    {
         icon_panel = "audio-volume-high-panel";
         icon="audio-volume-high";
         icon_fallback=ICONS_VOLUME_HIGH;
    }
    else if (level >= 50)
    {
         icon_panel = "audio-volume-medium-panel";
         icon="audio-volume-medium";
         icon_fallback=ICONS_VOLUME_MEDIUM;
    }
    else if (level > 0)
    {
         icon_panel = "audio-volume-low-panel";
         icon="audio-volume-low";
         icon_fallback=ICONS_VOLUME_LOW;
    }

    vol->icon_panel = icon_panel;
    vol->icon = icon;
    vol->icon_fallback= icon_fallback;
}

void image_set_from_file(LXPanel * p, GtkWidget * image, const char * file)
{
    GdkPixbuf * pixbuf = gdk_pixbuf_new_from_file_at_scale(file, panel_get_icon_size (p) - ICON_BUTTON_TRIM, panel_get_icon_size (p) - ICON_BUTTON_TRIM, TRUE, NULL);
    if (pixbuf != NULL)
    {
        gtk_image_set_from_pixbuf(GTK_IMAGE(image), pixbuf);
        g_object_unref(pixbuf);
    }
}

gboolean image_set_icon_theme(LXPanel * p, GtkWidget * image, const gchar * icon)
{
    if (gtk_icon_theme_has_icon(panel_get_icon_theme (p), icon))
    {
        GdkPixbuf * pixbuf = gtk_icon_theme_load_icon(panel_get_icon_theme (p), icon, panel_get_icon_size (p) - ICON_BUTTON_TRIM, 0, NULL);
        gtk_image_set_from_pixbuf(GTK_IMAGE(image), pixbuf);
        g_object_unref(pixbuf);
        return TRUE;
    }
    return FALSE;
}

/* Do a full redraw of the display. */
static void volumealsa_update_display(VolumeALSAPlugin * vol)
{
    gboolean mute;
    int level;
    /* Mute status. */
    if (!vol->bt_used)
    {
        mute = asound_is_muted(vol);
        level = asound_get_volume(vol);
    }
    else
    {
        mute = get_bt_mute (vol);
        level = get_bt_vol (vol) / 656;
    }
    if (mute) level = 0;

    volumealsa_update_current_icon(vol);

    /* Change icon, fallback to default icon if theme doesn't exsit */
    if ( ! image_set_icon_theme(vol->panel, vol->tray_icon, vol->icon_panel))
    {
        if ( ! image_set_icon_theme(vol->panel, vol->tray_icon, vol->icon))
        {
            image_set_from_file(vol->panel, vol->tray_icon, vol->icon_fallback);
        }
    }

    g_signal_handler_block(vol->mute_check, vol->mute_check_handler);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(vol->mute_check), mute);
    if (!vol->bt_used)
        gtk_widget_set_sensitive(vol->mute_check, asound_has_mute(vol));
    else
        gtk_widget_set_sensitive(vol->mute_check, TRUE);
    g_signal_handler_unblock(vol->mute_check, vol->mute_check_handler);

    /* Volume. */
    if (vol->volume_scale != NULL)
    {
        g_signal_handler_block(vol->volume_scale, vol->volume_scale_handler);
        gtk_range_set_value(GTK_RANGE(vol->volume_scale), level);
        g_signal_handler_unblock(vol->volume_scale, vol->volume_scale_handler);
    }

    /* Display current level in tooltip. */
    char * tooltip = g_strdup_printf("%s %d", _("Volume control"), level);
    gtk_widget_set_tooltip_text(vol->plugin, tooltip);
    g_free(tooltip);
}

/*** Gstreamer access functions copied from xfce_mixer ***/

static gboolean
_xfce_mixer_filter_mixer (GstMixer *mixer,
                          gpointer  user_data)
{
  GstElementFactory *factory;
  const gchar       *long_name;
  gchar             *device_name = NULL;
  gchar             *internal_name;
  gchar             *name;
  gchar             *p;
  gint               length;
  gint              *counter = user_data;
  gchar *device;

  /* Get long name of the mixer element */
  factory = gst_element_get_factory (GST_ELEMENT (mixer));
  long_name = gst_element_factory_get_longname (factory);

  /* Get the device name of the mixer element */
  if (g_object_class_find_property (G_OBJECT_GET_CLASS (G_OBJECT (mixer)), "device-name"))
    g_object_get (mixer, "device-name", &device_name, NULL);

  if (g_object_class_find_property (G_OBJECT_GET_CLASS (G_OBJECT (mixer)), "device"))
    g_object_get (mixer, "device", &device, NULL);

  /* Fall back to default name if neccessary */
  if (G_UNLIKELY (device_name == NULL))
    device_name = g_strdup_printf (_("Unknown Volume Control %d"), (*counter)++);

  /* Build display name */
  name = g_strdup_printf ("%s (%s)", device_name, long_name);

  /* Free device name */
  g_free (device_name);

  /* Set name to be used by xfce4-mixer */
  g_object_set_data_full (G_OBJECT (mixer), "xfce-mixer-name", name, (GDestroyNotify) g_free);
  g_object_set_data_full (G_OBJECT (mixer), "xfce-mixer-id", device, (GDestroyNotify) g_free);

  /* Count alpha-numeric characters in the name */
  for (length = 0, p = name; *p != '\0'; ++p)
    if (g_ascii_isalnum (*p))
      ++length;

  /* Generate internal name */
  internal_name = g_new0 (gchar, length+1);
  for (length = 0, p = name; *p != '\0'; ++p)
    if (g_ascii_isalnum (*p))
      internal_name[length++] = *p;
  internal_name[length] = '\0';

  /* Remember name for use by xfce4-mixer */
  g_object_set_data_full (G_OBJECT (mixer), "xfce-mixer-internal-name", internal_name, (GDestroyNotify) g_free);
  /* Keep the mixer (we want all devices to be visible) */
  return TRUE;
}

static const gchar *xfce_mixer_get_card_id (GstElement *card)
{
  g_return_val_if_fail (GST_IS_MIXER (card), NULL);
  return g_object_get_data (G_OBJECT (card), "xfce-mixer-id");
}

static guint xfce_mixer_is_default_card (GstElement *card)
{
  g_return_val_if_fail (GST_IS_MIXER (card), NULL);

  char cid[16];
  asound_get_default_card (cid);
  if (!strcmp (cid, xfce_mixer_get_card_id (card))) return 1;
  return 0;
}

static const gchar *xfce_mixer_get_card_display_name (GstElement *card)
{
  g_return_val_if_fail (GST_IS_MIXER (card), NULL);
  return g_object_get_data (G_OBJECT (card), "xfce-mixer-name");
}

static gboolean xfce_mixer_is_bcm_device (GstElement *card)
{
  g_return_val_if_fail (GST_IS_MIXER (card), FALSE);
  if (strncmp (xfce_mixer_get_card_display_name (card), "bcm2835", 7) == 0) return TRUE;
  return FALSE;
}

static const gchar * xfce_mixer_get_bcm_device_id (void)
{
    gint counter = 0;
    GList *iter, *mixers = gst_audio_default_registry_mixer_filter (_xfce_mixer_filter_mixer, FALSE, &counter);
    for (iter = mixers; iter != NULL; iter = g_list_next (iter))
    {
        if (xfce_mixer_is_bcm_device (iter->data)) return xfce_mixer_get_card_id (iter->data);
    }
    return NULL;
}

#ifdef CUSTOM_MENU

static void volumealsa_popup_set_position(GtkWidget * menu, gint * px, gint * py, gboolean * push_in, gpointer data)
{
    VolumeALSAPlugin * vol= (VolumeALSAPlugin *) data;

    /* Determine the coordinates. */
    lxpanel_plugin_popup_set_position_helper(vol->panel, vol->plugin, menu, px, py);
    *push_in = TRUE;
}

static void send_message (void)
{
  // to message the xfce mixer dialog, a Dbus connection is dropped and then reacquired
  // doing this the other (more logical) way around doesn't work, as Dbus doesn't pass the message fast enough
  static guint id = 0;
  if (id) g_bus_unown_name (id);
  id = g_bus_own_name (G_BUS_TYPE_SESSION, "org.lxde.volumealsa", 0, NULL, NULL, NULL, NULL, NULL);
}

static void set_default_card_event (GtkWidget * widget, GdkEventButton * event, VolumeALSAPlugin * vol)
{
    system ("pulseaudio --kill");
    vol->bt_used = FALSE;
    asound_set_default_card (widget->name);
    asound_restart (vol);
    volumealsa_update_display (vol);
    gtk_menu_popdown (GTK_MENU(vol->menu_popup));
    send_message ();
}

static void set_bcm_output (GtkWidget * widget, GdkEventButton * event, VolumeALSAPlugin * vol)
{
    char cmdbuf[64];
    const char *bcm;

    /* check that the BCM device is default... */
    system ("pulseaudio --kill");
    vol->bt_used = FALSE;
    bcm = xfce_mixer_get_bcm_device_id ();
    asound_get_default_card (cmdbuf);
    if (strcmp (cmdbuf, bcm))
    {
        /* ... and set it to default if not */
        asound_set_default_card (bcm);
        asound_restart (vol);

        /* set the output channel on the BCM device */
        sprintf (cmdbuf, "amixer cset numid=3 %s", widget->name);
        system (cmdbuf);
    }
    else
    {
        /* set the output channel on the BCM device */
        sprintf (cmdbuf, "amixer cset numid=3 %s", widget->name);
        system (cmdbuf);
    }
    volumealsa_update_display (vol);
    gtk_menu_popdown (GTK_MENU(vol->menu_popup));
    send_message ();
}

static void open_config_dialog (GtkWidget * widget, GdkEventButton * event, VolumeALSAPlugin * vol)
{
    volumealsa_configure (vol->panel, vol->plugin);
    gtk_menu_popdown (GTK_MENU(vol->menu_popup));
}

#endif

/* Handler for "focus-out" signal on popup window. */
static gboolean volumealsa_popup_focus_out(GtkWidget * widget, GdkEvent * event, VolumeALSAPlugin * vol)
{
    /* Hide the widget. */
    gtk_widget_hide(vol->popup_window);
    vol->show_popup = FALSE;
    return FALSE;
}

/* Handler for "button-press-event" signal on main widget. */

static gboolean volumealsa_button_press_event(GtkWidget * widget, GdkEventButton * event, LXPanel * panel)
{
    VolumeALSAPlugin * vol = lxpanel_plugin_get_data(widget);

    /* Left-click.  Show or hide the popup window. */
    if (event->button == 1)
    {
        if (vol->show_popup)
        {
            gtk_widget_hide(vol->popup_window);
            vol->show_popup = FALSE;
        }
        else
        {
            volumealsa_build_popup_window (vol->plugin);
            volumealsa_update_display (vol);

            gint x, y;
            gtk_window_iconify (GTK_WINDOW (vol->popup_window));
            gtk_widget_show_all (vol->popup_window);
            lxpanel_plugin_popup_set_position_helper (panel, widget, vol->popup_window, &x, &y);
            gdk_window_move (gtk_widget_get_window (vol->popup_window), x, y);
            gtk_window_present (GTK_WINDOW (vol->popup_window));
            vol->show_popup = TRUE;
        }
    }

    /* Middle-click.  Toggle the mute status. */
    else if (event->button == 2)
    {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(vol->mute_check), ! gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(vol->mute_check)));
    }
#ifdef CUSTOM_MENU
    else if (event->button == 3)
    {
        gint counter = 0, val = -1;
        GList *iter, *mixers = gst_audio_default_registry_mixer_filter (_xfce_mixer_filter_mixer, FALSE, &counter);
        GtkWidget *image, *mi;
        GdkPixbuf *pixbuf = NULL;
        gboolean def_good = FALSE, ext_dev = FALSE;

        if (gtk_icon_theme_has_icon (panel_get_icon_theme (vol->panel), "dialog-ok-apply"))
            pixbuf = gtk_icon_theme_load_icon (panel_get_icon_theme (vol->panel), "dialog-ok-apply", 16, 0, NULL);
        if (pixbuf == NULL)
            pixbuf = gdk_pixbuf_new_from_file_at_scale (ICONS_TICK, 16, 16, TRUE, NULL);

        if (pixbuf != NULL)
        {
            image = gtk_image_new_from_pixbuf(pixbuf);
            g_object_unref(pixbuf);
        }

        vol->menu_popup = gtk_menu_new ();

        counter = 2;
        for (iter = mixers; iter != NULL; iter = g_list_next (iter))
        {
            if (!xfce_mixer_is_bcm_device (iter->data)) counter++;

            if (xfce_mixer_is_default_card (iter->data))
            {
                def_good = TRUE;
                if (xfce_mixer_is_bcm_device (iter->data))
                {
                    /* if the onboard card is default, find currently-set output */
                    val = asound_get_bcm_output ();
                    break;
                }
            }
        }

        if (!def_good)
        {
            /* The default device is not present - fall back... */
            g_warning ("volumealsa: Default ALSA device not valid - resetting to internal");
            asound_set_bcm_card ();
            asound_restart (vol);
            volumealsa_update_display (vol);
            val = asound_get_bcm_output ();
        }

        mi = gtk_image_menu_item_new_with_label (_("Analog"));
        if (val == 1) gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM(mi), image);
        gtk_widget_set_name (mi, "1");
        g_signal_connect (mi, "button-press-event", G_CALLBACK (set_bcm_output), (gpointer) vol);
        g_signal_connect (mi, "button-release-event", G_CALLBACK (set_bcm_output), (gpointer) vol);
        gtk_menu_shell_append (GTK_MENU_SHELL(vol->menu_popup), mi);

        mi = gtk_image_menu_item_new_with_label (_("HDMI"));
        if (val == 2) gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM(mi), image);
        gtk_widget_set_name (mi, "2");
        g_signal_connect (mi, "button-press-event", G_CALLBACK (set_bcm_output), (gpointer) vol);
        g_signal_connect (mi, "button-release-event", G_CALLBACK (set_bcm_output), (gpointer) vol);
        gtk_menu_shell_append (GTK_MENU_SHELL(vol->menu_popup), mi);

        for (iter = mixers; iter != NULL; iter = g_list_next (iter))
        {
            if (!xfce_mixer_is_bcm_device (iter->data))
            {
                if (!ext_dev)
                {
                    mi = gtk_separator_menu_item_new ();
                    gtk_menu_shell_append (GTK_MENU_SHELL(vol->menu_popup), mi);
                }

                char namebuf[128];
                strcpy (namebuf, xfce_mixer_get_card_display_name (iter->data));
                namebuf[strlen(namebuf) - 13] = 0;
                mi = gtk_image_menu_item_new_with_label (namebuf);

                if (xfce_mixer_is_default_card (iter->data)) gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM(mi), image);
                gtk_widget_set_name (mi, xfce_mixer_get_card_id (iter->data));  // use the widget name to store the card id
                g_signal_connect (mi, "button-press-event", G_CALLBACK (set_default_card_event), (gpointer) vol);
                g_signal_connect (mi, "button-release-event", G_CALLBACK (set_default_card_event), (gpointer) vol);
                gtk_menu_shell_append (GTK_MENU_SHELL(vol->menu_popup), mi);
                ext_dev = TRUE;
            }
        }

        // add Bluetooth devices...
        gboolean bt_dev = FALSE;
        char *btdevice = NULL;
        get_bt_default_dev (vol, &btdevice);

        // iterate all the objects the manager knows about
        GList *objects = g_dbus_object_manager_get_objects (vol->objmanager);
        while (objects != NULL)
        {
            GDBusObject *object = (GDBusObject *) objects->data;
            GList *interfaces = g_dbus_object_get_interfaces (object);
            while (interfaces != NULL)
            {
                // if an object has a Device1 interface, it is a Bluetooth device - add it to the list
                GDBusInterface *interface = G_DBUS_INTERFACE (interfaces->data);
                if (g_strcmp0 (g_dbus_proxy_get_interface_name (G_DBUS_PROXY (interface)), "org.bluez.Device1") == 0)
                {
                    GVariant *name = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Alias");
                    GVariant *paired = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Paired");
                    GVariant *trusted = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Trusted");
                    GVariant *icon = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Icon");
                    if (name && icon && paired && trusted && !g_strcmp0 (g_variant_get_string (icon, NULL), "audio-card") && g_variant_get_boolean (paired) && g_variant_get_boolean (trusted))
                    {
                        if (!bt_dev)
                        {
                            mi = gtk_separator_menu_item_new ();
                            gtk_menu_shell_append (GTK_MENU_SHELL(vol->menu_popup), mi);
                            bt_dev = TRUE;
                        }
                        mi = gtk_image_menu_item_new_with_label (g_variant_get_string (name, NULL));

                        const char *devname = g_dbus_object_get_object_path (object);
                        printf ("Names = %s %s\n", devname, btdevice);
                        if (btdevice)
                        {
                            printf ("Compare %s %s\n", btdevice + (strlen (btdevice) - 17), devname + (strlen (devname) - 17));
                            if (!strncmp (btdevice + (strlen (btdevice) - 17), devname + (strlen (devname) - 17), 17))
                                gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM(mi), image);
                        }
                        gtk_widget_set_name (mi, devname);  // use the widget name to store the card id
                        g_signal_connect (mi, "button-press-event", G_CALLBACK (set_bt_card_event), (gpointer) vol);
                        //g_signal_connect (mi, "button-release-event", G_CALLBACK (set_bt_card_event), (gpointer) vol);
                        gtk_menu_shell_append (GTK_MENU_SHELL(vol->menu_popup), mi);
                    }
                    break;
                }
                interfaces = interfaces->next;
            }
            objects = objects->next;
        }

        if (ext_dev)
        {
            //mi = gtk_separator_menu_item_new ();
            //gtk_menu_shell_append (GTK_MENU_SHELL(vol->menu_popup), mi);

            mi = gtk_image_menu_item_new_with_label (_("Device Settings..."));
            g_signal_connect (mi, "button-press-event", G_CALLBACK (open_config_dialog), (gpointer) vol);
            g_signal_connect (mi, "button-release-event", G_CALLBACK (open_config_dialog), (gpointer) vol);
            gtk_menu_shell_append (GTK_MENU_SHELL(vol->menu_popup), mi);
        }

        gtk_widget_show_all (vol->menu_popup);
        gtk_menu_popup (GTK_MENU(vol->menu_popup), NULL, NULL, (GtkMenuPositionFunc) volumealsa_popup_set_position, (gpointer) vol,
            event->button, event->time);
    }
#endif
    return TRUE;
}

static void volumealsa_theme_change(GtkWidget * widget, VolumeALSAPlugin * vol)
{
    if ( ! image_set_icon_theme(vol->panel, vol->tray_icon, vol->icon_panel))
    {
        if ( ! image_set_icon_theme(vol->panel, vol->tray_icon, vol->icon))
        {
            image_set_from_file(vol->panel, vol->tray_icon, vol->icon_fallback);
        }
    }
}

/* Handler for "value_changed" signal on popup window vertical scale. */
static void volumealsa_popup_scale_changed(GtkRange * range, VolumeALSAPlugin * vol)
{
    /* Reflect the value of the control to the sound system. */
    if (!vol->bt_used)
    {
        if (!asound_is_muted (vol))
            asound_set_volume(vol, gtk_range_get_value(range));
    }
    else
    {
        if (!get_bt_mute (vol))
            set_bt_vol (vol, gtk_range_get_value (range) * 656);
    }

    /* Redraw the controls. */
    volumealsa_update_display(vol);
}

/* Handler for "scroll-event" signal on popup window vertical scale. */
static void volumealsa_popup_scale_scrolled(GtkScale * scale, GdkEventScroll * evt, VolumeALSAPlugin * vol)
{
    /* Get the state of the vertical scale. */
    gdouble val = gtk_range_get_value(GTK_RANGE(vol->volume_scale));

    /* Dispatch on scroll direction to update the value. */
    if ((evt->direction == GDK_SCROLL_UP) || (evt->direction == GDK_SCROLL_LEFT))
        val += 2;
    else
        val -= 2;

    /* Reset the state of the vertical scale.  This provokes a "value_changed" event. */
    gtk_range_set_value(GTK_RANGE(vol->volume_scale), CLAMP((int)val, 0, 100));
}

/* Handler for "toggled" signal on popup window mute checkbox. */
static void volumealsa_popup_mute_toggled(GtkWidget * widget, VolumeALSAPlugin * vol)
{
    /* Get the state of the mute toggle. */
    gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

    if (!vol->bt_used)
    {
        /* Reflect the mute toggle to the sound system. */
        if (vol->master_element != NULL)
        {
            int chn;
            for (chn = 0; chn <= SND_MIXER_SCHN_LAST; chn++)
                snd_mixer_selem_set_playback_switch(vol->master_element, chn, ((active) ? 0 : 1));
        }
    }
    else
    {
        set_bt_mute (vol, active);
    }

    /* Redraw the controls. */
    volumealsa_update_display(vol);
}

#ifndef CUSTOM_MENU

static void default_card_toggled (GtkWidget * widget, VolumeALSAPlugin * vol)
{
    if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget)))
    {
        /* set the new default card */
        asound_set_default_card (widget->name);
        asound_restart (vol);

        volumealsa_update_display (vol);
    }
}

static void bcm_output_toggled (GtkWidget *widget, VolumeALSAPlugin * vol)
{
    char cmdbuf[64];
    const char *bcm;

    if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget)))
    {
        /* check that the BCM device is default... */
        bcm = xfce_mixer_get_bcm_device_id ();
        asound_get_default_card (cmdbuf);
        if (strcmp (cmdbuf, bcm))
        {
            /* ... and set it to default if not */
            asound_set_default_card (bcm);
            asound_restart (vol);

            /* set the output channel on the BCM device */
            sprintf (cmdbuf, "amixer cset numid=3 %s", widget->name);
            system (cmdbuf);

            volumealsa_update_display (vol);
        }
        else
        {
            /* set the output channel on the BCM device */
            sprintf (cmdbuf, "amixer cset numid=3 %s", widget->name);
            system (cmdbuf);
        }
    }
}

#endif

/* Build the window that appears when the top level widget is clicked. */
static void volumealsa_build_popup_window(GtkWidget *p)
{
    VolumeALSAPlugin * vol = lxpanel_plugin_get_data(p);

    gboolean def_good = FALSE;
    gint counter = 0, val = -1;
    GList *iter, *mixers = gst_audio_default_registry_mixer_filter (_xfce_mixer_filter_mixer, FALSE, &counter);

    /* Get the current setting - find cards, and which one is default */
    counter = 2;
    for (iter = mixers; iter != NULL; iter = g_list_next (iter))
    {
        if (!xfce_mixer_is_bcm_device (iter->data)) counter++;

        if (xfce_mixer_is_default_card (iter->data))
        {
            def_good = TRUE;
            if (xfce_mixer_is_bcm_device (iter->data))
            {
                /* if the onboard card is default, find currently-set output */
                val = asound_get_bcm_output ();
                break;
            }
            else val = counter;
        }
    }

    /* Reset the default device if it is invalid */
    if (!def_good)
    {
        /* The default device is not present - fall back... */
        g_warning ("volumealsa: Default ALSA device not valid - resetting to internal");
        asound_set_bcm_card ();
        asound_restart (vol);
        volumealsa_update_display (vol);
        val = asound_get_bcm_output ();
    }

    if (vol->popup_window)
    {
        gtk_widget_destroy (vol->popup_window);
        vol->popup_window = NULL;
    }

    /* Create a new window. */
    vol->popup_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name (vol->popup_window, "volals");
    gtk_window_set_decorated(GTK_WINDOW(vol->popup_window), FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(vol->popup_window), 5);
#ifndef CUSTOM_MENU
    gtk_window_set_default_size (GTK_WINDOW(vol->popup_window), 100, 140);
#endif
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(vol->popup_window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(vol->popup_window), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(vol->popup_window), GDK_WINDOW_TYPE_HINT_UTILITY);

    /* Connect signals. */
    g_signal_connect(G_OBJECT(vol->popup_window), "focus-out-event", G_CALLBACK(volumealsa_popup_focus_out), vol);

    /* Create a scrolled window as the child of the top level window. */
    GtkWidget * scrolledwindow = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_name (scrolledwindow, "whitewd");
    gtk_container_set_border_width (GTK_CONTAINER(scrolledwindow), 0);
    gtk_widget_show(scrolledwindow);
    gtk_container_add(GTK_CONTAINER(vol->popup_window), scrolledwindow);
    gtk_widget_set_can_focus(scrolledwindow, FALSE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW (scrolledwindow), GTK_POLICY_NEVER, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolledwindow), GTK_SHADOW_NONE);

    /* Create a viewport as the child of the scrolled window. */
    GtkWidget * viewport = gtk_viewport_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scrolledwindow), viewport);
    gtk_viewport_set_shadow_type(GTK_VIEWPORT(viewport), GTK_SHADOW_NONE);
    gtk_widget_show(viewport);

#ifdef CUSTOM_MENU
    gtk_container_set_border_width(GTK_CONTAINER(vol->popup_window), 0);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolledwindow), GTK_SHADOW_IN);
    /* Create a vertical box as the child of the viewport. */
    GtkWidget * box = gtk_vbox_new (FALSE, 0);
    gtk_container_add(GTK_CONTAINER(viewport), box);
#else
    /* Create a vertical box as the child of the viewport. */
    GtkWidget *bvbox = gtk_vbox_new (FALSE, 0);
    gtk_container_add(GTK_CONTAINER(viewport), bvbox);
    gtk_widget_show (bvbox);

    /* Create a frame as the child of the vbox. */
    GtkWidget * frame = gtk_frame_new(_("Volume"));
    //gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);
    gtk_box_pack_start(GTK_BOX(bvbox), frame, TRUE, TRUE, 0);

    /* Create a vertical box as the child of the frame. */
    GtkWidget * box = gtk_vbox_new(FALSE, 0);
    gtk_container_add(GTK_CONTAINER(frame), box);
#endif

    /* Create a vertical scale as the child of the vertical box. */
    vol->volume_scale = gtk_vscale_new(GTK_ADJUSTMENT(gtk_adjustment_new(100, 0, 100, 0, 0, 0)));
    gtk_widget_set_name (vol->volume_scale, "volscale");
    g_object_set (vol->volume_scale, "height-request", 120, NULL);
    gtk_scale_set_draw_value(GTK_SCALE(vol->volume_scale), FALSE);
    gtk_range_set_inverted(GTK_RANGE(vol->volume_scale), TRUE);
    gtk_box_pack_start(GTK_BOX(box), vol->volume_scale, TRUE, TRUE, 0);
    gtk_widget_set_can_focus (vol->volume_scale, FALSE);

    /* Value-changed and scroll-event signals. */
    vol->volume_scale_handler = g_signal_connect(vol->volume_scale, "value-changed", G_CALLBACK(volumealsa_popup_scale_changed), vol);
    g_signal_connect(vol->volume_scale, "scroll-event", G_CALLBACK(volumealsa_popup_scale_scrolled), vol);

    /* Create a check button as the child of the vertical box. */
    vol->mute_check = gtk_check_button_new_with_label(_("Mute"));
    gtk_box_pack_end(GTK_BOX(box), vol->mute_check, FALSE, FALSE, 0);
    vol->mute_check_handler = g_signal_connect(vol->mute_check, "toggled", G_CALLBACK(volumealsa_popup_mute_toggled), vol);
    gtk_widget_set_can_focus (vol->mute_check, FALSE);

#ifndef CUSTOM_MENU
    /* Create a frame as the child of the vbox. */
    GtkWidget * frame2 = gtk_frame_new (_("Output"));
    gtk_box_pack_end (GTK_BOX(bvbox), frame2, FALSE, FALSE, 0);

    /* Create a vertical box as the child of the frame. */
    GtkWidget * box2 = gtk_vbox_new (FALSE, 0);
    gtk_container_add (GTK_CONTAINER(frame2), box2);

    /* Add radio buttons for the BCM device outputs */
    GtkWidget *rb, *last, *sep;
    last = rb = gtk_radio_button_new_with_label (NULL, _("Auto"));
    gtk_radio_button_group (GTK_RADIO_BUTTON (rb));
    gtk_widget_set_name (rb, "0");
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (rb), val == 0);
    g_signal_connect (rb, "toggled", G_CALLBACK (bcm_output_toggled), (gpointer) vol);
    gtk_box_pack_start (GTK_BOX (box2), rb, FALSE, FALSE, 0);

    rb = gtk_radio_button_new_with_label (gtk_radio_button_group (GTK_RADIO_BUTTON (last)), _("Analog"));
    gtk_widget_set_name (rb, "1");
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (rb), val == 1);
    g_signal_connect (rb, "toggled", G_CALLBACK (bcm_output_toggled), (gpointer) vol);
    gtk_box_pack_start (GTK_BOX (box2), rb, FALSE, FALSE, 0);

    rb = gtk_radio_button_new_with_label (gtk_radio_button_group (GTK_RADIO_BUTTON (last)), _("HDMI"));
    gtk_widget_set_name (rb, "2");
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (rb), val == 2);
    g_signal_connect (rb, "toggled", G_CALLBACK (bcm_output_toggled), (gpointer) vol);
    gtk_box_pack_start (GTK_BOX (box2), rb, FALSE, FALSE, 0);

    /* Add separators and radio buttons for other devices */
    counter = 3;
    for (iter = mixers; iter != NULL; iter = g_list_next (iter))
    {
        if (!xfce_mixer_is_bcm_device (iter->data))
        {
            sep = gtk_hseparator_new ();
            gtk_box_pack_start (GTK_BOX (box2), sep, FALSE, FALSE, 0);

            char namebuf[128];
            strcpy (namebuf, xfce_mixer_get_card_display_name (iter->data));
            namebuf[strlen(namebuf) - 13] = 0;
            rb = gtk_radio_button_new_with_label (gtk_radio_button_group (GTK_RADIO_BUTTON (last)), namebuf);
            gtk_widget_set_name (rb, xfce_mixer_get_card_id (iter->data));
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (rb), val == counter);
            g_signal_connect (rb, "toggled", G_CALLBACK (default_card_toggled), (gpointer) vol);
            gtk_box_pack_start (GTK_BOX (box2), rb, FALSE, FALSE, 0);
            counter++;
        }
    }
#endif
    /* Set background to default. */
    //gtk_widget_set_style(viewport, panel_get_defstyle(vol->panel));
}

/* Plugin constructor. */
static GtkWidget *volumealsa_constructor(LXPanel *panel, config_setting_t *settings)
{
    /* Allocate and initialize plugin context and set into Plugin private data pointer. */
    VolumeALSAPlugin * vol = g_new0(VolumeALSAPlugin, 1);
    GtkWidget *p;

    /* Initialise Gstreamer */
    gst_init (NULL, NULL);

    /* Initialize ALSA.  If that fails, present nothing. */
    if ( ! asound_initialize(vol))
    {
        volumealsa_destructor(vol);
        return NULL;
    }

    /* Check the default device is valid and reset if not */
    gint counter = 0;
    GList *iter, *mixers = gst_audio_default_registry_mixer_filter (_xfce_mixer_filter_mixer, FALSE, &counter);

    /* Get the current setting - find cards, and which one is default */
    gboolean def_good = FALSE;
    for (iter = mixers; iter != NULL; iter = g_list_next (iter))
    {
        if (xfce_mixer_is_default_card (iter->data))
        {
            def_good = TRUE;
            break;
        }
    }

    if (!def_good)
    {
        /* The default device is not present - fall back... */
        g_warning ("volumealsa: Default ALSA device not valid - resetting to internal");
        asound_set_bcm_card ();
        asound_restart (vol);
    }

    /* Allocate top level widget and set into Plugin widget pointer. */
    vol->panel = panel;
    vol->plugin = p = gtk_button_new();
    gtk_button_set_relief (GTK_BUTTON (vol->plugin), GTK_RELIEF_NONE);
    //vol->plugin = p = gtk_event_box_new();
#ifdef CUSTOM_MENU
    g_signal_connect(vol->plugin, "button-press-event", G_CALLBACK(volumealsa_button_press_event), vol->panel);
#endif
    vol->settings = settings;
    lxpanel_plugin_set_data(p, vol, volumealsa_destructor);
    gtk_widget_add_events(p, GDK_BUTTON_PRESS_MASK);
    gtk_widget_set_tooltip_text(p, _("Volume control"));

    /* Allocate icon as a child of top level. */
    vol->tray_icon = gtk_image_new();
    gtk_container_add(GTK_CONTAINER(p), vol->tray_icon);

    /* Initialize window to appear when icon clicked. */
    volumealsa_build_popup_window(p);

    /* Connect signals. */
    g_signal_connect(G_OBJECT(p), "scroll-event", G_CALLBACK(volumealsa_popup_scale_scrolled), vol );
    g_signal_connect(panel_get_icon_theme(panel), "changed", G_CALLBACK(volumealsa_theme_change), vol );

    // get an object manager for BlueZ
    vol->objmanager = g_dbus_object_manager_client_new_for_bus_sync (G_BUS_TYPE_SYSTEM, 0, "org.bluez", "/", NULL, NULL, NULL, NULL, NULL);

    // try to create a connection to PulseAudio
    GDBusConnection *con = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
    GDBusProxy *prox = g_dbus_proxy_new_sync (con, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.PulseAudio1", "/org/pulseaudio/server_lookup1", "org.freedesktop.DBus.Properties", NULL, NULL);
    GVariant *var = g_dbus_proxy_get_cached_property (prox, "Address");
    GError *error = NULL;
    vol->con = g_dbus_connection_new_for_address_sync (g_variant_get_string (var, NULL), G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT, NULL, NULL, &error);
    if (!error && vol->con)
    {
        vol->bt_used = TRUE;
        vol->sink = get_default_sink (vol);
        printf ("Bluetooth audio active %d %d\n", vol->bt_used, vol->sink);
    }

    /* Update the display, show the widget, and return. */
    volumealsa_update_display(vol);
    gtk_widget_show_all(p);
    return p;
}

/* Plugin destructor. */
static void volumealsa_destructor(gpointer user_data)
{
    VolumeALSAPlugin * vol = (VolumeALSAPlugin *) user_data;

    asound_deinitialize(vol);

    /* If the dialog box is open, dismiss it. */
    if (vol->popup_window != NULL)
        gtk_widget_destroy(vol->popup_window);

    if (vol->restart_idle)
        g_source_remove(vol->restart_idle);

   if (vol->panel) /* SF bug #683: crash if constructor failed */
    g_signal_handlers_disconnect_by_func(panel_get_icon_theme(vol->panel),
                                         volumealsa_theme_change, vol);

    /* Deallocate all memory. */
    g_free(vol);
}

/* Callback when the configuration dialog is to be shown. */

static GtkWidget *volumealsa_configure(LXPanel *panel, GtkWidget *p)
{
    VolumeALSAPlugin * vol = lxpanel_plugin_get_data(p);
    char *path = NULL;
    const gchar *command_line = NULL;
    GAppInfoCreateFlags flags = G_APP_INFO_CREATE_NONE;

    /* FIXME: configure settings! */
    /* check if command line was configured */
    config_setting_lookup_string(vol->settings, "MixerCommand", &command_line);
    /* FIXME: support "needs terminal" for MixerCommand */
    /* FIXME: selection for master channel! */
    /* FIXME: configure buttons for each action (toggle volume/mixer/mute)! */

    /* if command isn't set in settings then let guess it */
    if (command_line == NULL && (path = g_find_program_in_path("pulseaudio")))
    {
        g_free(path);
     /* Assume that when pulseaudio is installed, it's launching every time */
        if ((path = g_find_program_in_path("gnome-sound-applet")))
        {
            command_line = "gnome-sound-applet";
        }
        else if ((path = g_find_program_in_path("pavucontrol")))
        {
            command_line = "pavucontrol";
        }
    }

    /* Fallback to alsamixer when PA is not running, or when no PA utility is find */
    if (command_line == NULL)
    {
        if ((path = g_find_program_in_path("gnome-alsamixer")))
        {
            command_line = "gnome-alsamixer";
        }
        else if ((path = g_find_program_in_path("alsamixergui")))
        {
            command_line = "alsamixergui";
        }
        else if ((path = g_find_program_in_path("pimixer")))
        {
            command_line = "pimixer";
        }
        else if ((path = g_find_program_in_path("xfce4-mixer")))
        {
            command_line = "xfce4-mixer";
        }
       else if ((path = g_find_program_in_path("alsamixer")))
        {
            command_line = "alsamixer";
            flags = G_APP_INFO_CREATE_NEEDS_TERMINAL;
        }
    }
    g_free(path);

    if (command_line)
    {
        fm_launch_command_simple(NULL, NULL, flags, command_line, NULL);
    }
    else
    {
        fm_show_error(NULL, NULL,
                      _("Error, you need to install an application to configure"
                        " the sound (pavucontrol, alsamixer ...)"));
    }

    return NULL;
}

/* Callback when panel configuration changes. */
static void volumealsa_panel_configuration_changed(LXPanel *panel, GtkWidget *p)
{
    VolumeALSAPlugin * vol = lxpanel_plugin_get_data(p);

    asound_restart(vol);
    volumealsa_build_popup_window (vol->plugin);
    /* Do a full redraw. */
    volumealsa_update_display(vol);
    if (vol->show_popup) gtk_widget_show_all (vol->popup_window);

}

FM_DEFINE_MODULE(lxpanel_gtk, volumealsa)

/* Plugin descriptor. */
LXPanelPluginInit fm_module_init_lxpanel_gtk = {
    .name = N_("Volume Control (ALSA)"),
    .description = N_("Display and control volume for ALSA"),

    .new_instance = volumealsa_constructor,
    .config = volumealsa_configure,
    .reconfigure = volumealsa_panel_configuration_changed
#ifndef CUSTOM_MENU
    ,
    .button_press_event = volumealsa_button_press_event
#endif
};

/* vim: set sw=4 et sts=4 : */
