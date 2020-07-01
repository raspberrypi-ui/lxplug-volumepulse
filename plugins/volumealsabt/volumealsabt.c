/*
Copyright (c) 2018 Raspberry Pi (Trading) Ltd.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
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

#define _ISOC99_SOURCE /* lrint() */
#define _GNU_SOURCE /* exp10() */

#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <alsa/asoundlib.h>
#include <poll.h>
#include <libfm/fm-gtk.h>
#include <pulse/pulseaudio.h>

#include "plugin.h"

#define DEBUG_ON
#ifdef DEBUG_ON
#define DEBUG(fmt,args...) if(getenv("DEBUG_VA"))g_message("va: " fmt,##args)
#else
#define DEBUG
#endif

typedef struct {
    snd_mixer_t *mixer;                 /* The mixer */
    guint num_channels;                 /* Number of channels */
    GIOChannel **channels;              /* Channels that we listen to */
    guint *watches;                     /* Watcher IDs for channels */
} mixer_info_t;

typedef struct {
    /* plugin */
    GtkWidget *plugin;                  /* Back pointer to widget */
    LXPanel *panel;                     /* Back pointer to panel */
    config_setting_t *settings;         /* Plugin settings */

    /* graphics */
    GtkWidget *tray_icon;               /* Displayed icon */
    GtkWidget *popup_window;            /* Top level window for popup */
    GtkWidget *volume_scale;            /* Scale for volume */
    GtkWidget *mute_check;              /* Checkbox for mute state */
    GtkWidget *menu_popup;              /* Right-click menu */
    GtkWidget *options_dlg;             /* Device options dialog */
    GtkWidget *options_play;            /* Playback options table */
    GtkWidget *options_capt;            /* Capture options table */
    GtkWidget *options_set;             /* General settings box */
    GtkWidget *outputs;                 /* Output select menu */
    GtkWidget *inputs;                  /* Input select menu */
    gboolean show_popup;                /* Toggle to show and hide the popup on left click */
    guint volume_scale_handler;         /* Handler for vscale widget */
    guint mute_check_handler;           /* Handler for mute_check widget */
    char *odev_name;
    char *idev_name;

    /* ALSA interface. */
    mixer_info_t mixers[2];             /* mixers[0] = output; mixers[1] = input */
    snd_mixer_elem_t *master_element;   /* Master element on output mixer - main volume control */
    guint mixer_evt_idle;               /* Timer to handle mixer reset */
    guint restart_idle;                 /* Timer to handle restarting */
    gboolean stopped;                   /* Flag to indicate that ALSA is restarting */

    /* Bluetooth interface */
    GDBusObjectManager *objmanager;     /* BlueZ object manager */
    char *bt_conname;                   /* BlueZ name of device - just used during connection */
    char *bt_reconname;                 /* BlueZ name of second device - used during reconnection */
    gboolean bt_input;                  /* Is the device being connected as an input or an output? */
    GtkWidget *conn_dialog;             /* Connection dialog box */
    GtkWidget *conn_label;              /* Dialog box text field */
    GtkWidget *conn_ok;                 /* Dialog box button */
    GDBusProxy *baproxy;                /* Proxy for BlueALSA */
    gulong basignal;                    /* ID of g-signal handler on BlueALSA */

    /* HDMI devices */
    guint hdmis;                        /* Number of HDMI devices */
    char *mon_names[2];                 /* Names of HDMI devices */

    /* PulseAudio interface */
    pa_threaded_mainloop *pa_mainloop;
    pa_context *pa_context;
    pa_context_state_t pa_state;
    char *pa_default_sink;              /* current default sink name */
    char *pa_default_source;            /* current default source name */
    int pa_channels;                    /* number of channels on default sink */
    int pa_volume;                      /* volume setting on default sink */
    int pa_mute;                        /* mute setting on default sink */
} VolumeALSAPlugin;

typedef enum {
    OUTPUT_MIXER = 0,
    INPUT_MIXER = 1
} MixerIO;

#define BLUEALSA_DEV (-99)

#define BT_SERV_AUDIO_SOURCE    "0000110A"
#define BT_SERV_AUDIO_SINK      "0000110B"
#define BT_SERV_HSP             "00001108"
#define BT_SERV_HFP             "0000111E"

/* Helpers */
static char *get_string (const char *fmt, ...);
static int get_value (const char *fmt, ...);
static int vsystem (const char *fmt, ...);
static gboolean find_in_section (char *file, char *sec, char *seek);
static int hdmi_monitors (VolumeALSAPlugin *vol);

/* Bluetooth */
static void bt_cb_object_added (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data);
static void bt_cb_object_removed (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data);
static void bt_cb_name_owned (GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer user_data);
static void bt_cb_name_unowned (GDBusConnection *connection, const gchar *name, gpointer user_data);
static void bt_cb_ba_name_owned (GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer user_data);
static void bt_cb_ba_name_unowned (GDBusConnection *connection, const gchar *name, gpointer user_data);
static void bt_cb_ba_signal (GDBusProxy *prox, gchar *sender, gchar *signal, GVariant *params, gpointer user_data);
static void bt_connect_device (VolumeALSAPlugin *vol);
static void bt_cb_connected (GObject *source, GAsyncResult *res, gpointer user_data);
static void bt_cb_trusted (GObject *source, GAsyncResult *res, gpointer user_data);
static void bt_reconnect_devices (VolumeALSAPlugin *vol);
static void bt_cb_reconnected (GObject *source, GAsyncResult *res, gpointer user_data);
static void bt_disconnect_device (VolumeALSAPlugin *vol, char *device);
static void bt_cb_disconnected (GObject *source, GAsyncResult *res, gpointer user_data);
static gboolean bt_has_service (VolumeALSAPlugin *vol, const gchar *path, const gchar *service);
static gboolean bt_is_connected (VolumeALSAPlugin *vol, const gchar *path);

/* Volume and mute */
static long lrint_dir (double x, int dir);
static int get_normalized_volume (snd_mixer_elem_t *elem, gboolean capture);
static int set_normalized_volume (snd_mixer_elem_t *elem, int volume, int dir, gboolean capture);
static gboolean asound_has_mute (VolumeALSAPlugin *vol);
static gboolean asound_is_muted (VolumeALSAPlugin *vol);
static void asound_set_mute (VolumeALSAPlugin *vol, gboolean mute);
static int asound_get_volume (VolumeALSAPlugin *vol);
static void asound_set_volume (VolumeALSAPlugin *vol, int volume);

/* ALSA */
static gboolean asound_initialize (VolumeALSAPlugin *vol);
static void asound_deinitialize (VolumeALSAPlugin *vol);
static gboolean asound_find_master_elem (VolumeALSAPlugin *vol);
static gboolean asound_mixer_initialize (VolumeALSAPlugin *vol, MixerIO io);
static void asound_mixer_deinitialize (VolumeALSAPlugin *vol, MixerIO io);
static int asound_find_valid_device (void);
static gboolean asound_current_dev_check (VolumeALSAPlugin *vol);
static gboolean asound_has_volume_control (int dev);
static gboolean asound_has_input (int dev);
static gboolean asound_restart (gpointer user_data);
static gboolean asound_reset_mixer_evt_idle (gpointer user_data);
static gboolean asound_mixer_event (GIOChannel *channel, GIOCondition cond, gpointer user_data);

/* .asoundrc */
static int asound_get_default_card (void);
static int asound_get_default_input (void);
static void asound_set_default_card (int num);
static void asound_set_default_input (int num);
static char *asound_get_bt_device (void);
static char *asound_get_bt_input (void);
static void asound_set_bt_device (char *devname);
static void asound_set_bt_input (char *devname);
static gboolean asound_is_current_bt_dev (const char *obj, gboolean is_input);
static int asound_get_bcm_device_num (void);
static int asound_is_bcm_device (int num);
static char *asound_default_device_name (void);
static char *asound_default_input_name (void);

/* Handlers and graphics */
static void volumealsa_update_display (VolumeALSAPlugin *vol);
static void volumealsa_theme_change (GtkWidget *widget, VolumeALSAPlugin *vol);
static void volumealsa_open_config_dialog (GtkWidget *widget, VolumeALSAPlugin *vol);
static void volumealsa_show_connect_dialog (VolumeALSAPlugin *vol, gboolean failed, const gchar *param);
static void volumealsa_close_connect_dialog (GtkButton *button, gpointer user_data);
static gint volumealsa_delete_connect_dialog (GtkWidget *widget, GdkEvent *event, gpointer user_data);
static gboolean volumealsa_button_press_event (GtkWidget *widget, GdkEventButton *event, LXPanel *panel);

/* Menu popup */
static GtkWidget *volumealsa_menu_item_add (VolumeALSAPlugin *vol, GtkWidget *menu, const char *label, const char *name, gboolean selected, gboolean input, GCallback cb);
static void volumealsa_menu_show_default (GtkWidget *widget, gpointer data);
static void volumealsa_build_device_menu (VolumeALSAPlugin *vol);
static void volumealsa_set_external_output (GtkWidget *widget, VolumeALSAPlugin *vol);
static void volumealsa_set_external_input (GtkWidget *widget, VolumeALSAPlugin *vol);
static void volumealsa_set_internal_output (GtkWidget *widget, VolumeALSAPlugin *vol);
static void volumealsa_set_bluetooth_output (GtkWidget *widget, VolumeALSAPlugin *vol);
static void volumealsa_set_bluetooth_input (GtkWidget *widget, VolumeALSAPlugin *vol);

/* Volume popup */
static void volumealsa_build_popup_window (GtkWidget *p);
static void volumealsa_popup_scale_changed (GtkRange *range, VolumeALSAPlugin *vol);
static void volumealsa_popup_scale_scrolled (GtkScale *scale, GdkEventScroll *evt, VolumeALSAPlugin *vol);
static void volumealsa_popup_mute_toggled (GtkWidget *widget, VolumeALSAPlugin *vol);
static void volumealsa_popup_set_position (GtkWidget *menu, gint *px, gint *py, gboolean *push_in, gpointer data);
static gboolean volumealsa_mouse_out (GtkWidget *widget, GdkEventButton *event, VolumeALSAPlugin *vol);

/* Options dialog */
static void show_options (VolumeALSAPlugin *vol, snd_mixer_t *mixer, gboolean input, char *devname);
static void show_input_options (VolumeALSAPlugin *vol);
static void show_output_options (VolumeALSAPlugin *vol);
static void update_options (VolumeALSAPlugin *vol);
static void close_options (VolumeALSAPlugin *vol);
static void options_ok_handler (GtkButton *button, gpointer *user_data);
static gboolean options_wd_close_handler (GtkWidget *wid, GdkEvent *event, gpointer user_data);
static void playback_range_change_event (GtkRange *range, gpointer user_data);
static void capture_range_change_event (GtkRange *range, gpointer user_data);
static void playback_switch_toggled_event (GtkToggleButton *togglebutton, gpointer user_data);
static void capture_switch_toggled_event (GtkToggleButton *togglebutton, gpointer user_data);
static void enum_changed_event (GtkComboBox *combo, gpointer *user_data);
static GtkWidget *find_box_child (GtkWidget *container, gint type, const char *name);

/* PulseAudio */
static void pulse_init (VolumeALSAPlugin *vol);
static void pulse_disconnect (VolumeALSAPlugin *vol);
static void pulse_close (VolumeALSAPlugin *vol);
static void pa_error_handler (VolumeALSAPlugin *vol, char *name);
static int pulse_get_defaults (VolumeALSAPlugin *vol);
static int pulse_update_alsa_names (VolumeALSAPlugin *vol);
static int pulse_set_default_sink (VolumeALSAPlugin *vol, const char *sinkname);
static int pulse_move_streams_to_default_sink (VolumeALSAPlugin *vol);
static void pulse_change_sink (VolumeALSAPlugin *vol, const char *sinkname);
static int pulse_get_default_sink_info (VolumeALSAPlugin *vol);
static int pulse_set_volume (VolumeALSAPlugin *vol, int volume);
static int pulse_get_volume (VolumeALSAPlugin *vol);
static int pulse_set_mute (VolumeALSAPlugin *vol, int mute);
static int pulse_get_mute (VolumeALSAPlugin *vol);
static char *bluez_to_pa_sink_name (char *bluez_name);
static char *bluez_to_pa_source_name (char *bluez_name);
static char *pa_sink_to_bluez_name (char *pa_name);
static char *pa_source_to_bluez_name (char *pa_name);

/* Plugin */
static GtkWidget *volumealsa_configure (LXPanel *panel, GtkWidget *plugin);
static void volumealsa_panel_configuration_changed (LXPanel *panel, GtkWidget *plugin);
static gboolean volumealsa_control_msg (GtkWidget *plugin, const char *cmd);
static GtkWidget *volumealsa_constructor (LXPanel *panel, config_setting_t *settings);
static void volumealsa_destructor (gpointer user_data);

/*----------------------------------------------------------------------------*/
/* Generic helper functions                                                   */
/*----------------------------------------------------------------------------*/

static char *get_string (const char *fmt, ...)
{
    char *cmdline, *line = NULL, *res = NULL;
    size_t len = 0;

    va_list arg;
    va_start (arg, fmt);
    g_vasprintf (&cmdline, fmt, arg);
    va_end (arg);

    FILE *fp = popen (cmdline, "r");
    if (fp)
    {
        if (getline (&line, &len, fp) > 0)
        {
            res = line;
            while (*res++) if (g_ascii_isspace (*res)) *res = 0;
            res = g_strdup (line);
        }
        pclose (fp);
        g_free (line);
    }
    g_free (cmdline);
    return res ? res : g_strdup ("");
}

static int get_value (const char *fmt, ...)
{
    char *res;
    int n, m;

    res = get_string (fmt);
    n = sscanf (res, "%d", &m);
    g_free (res);

    if (n != 1) return -1;
    else return m;
}

static int vsystem (const char *fmt, ...)
{
    char *cmdline;
    int res;

    va_list arg;
    va_start (arg, fmt);
    g_vasprintf (&cmdline, fmt, arg);
    va_end (arg);
    res = system (cmdline);
    g_free (cmdline);
    return res;
}

static gboolean find_in_section (char *file, char *sec, char *seek)
{
    char *cmd = g_strdup_printf ("sed -n '/%s/,/}/p' %s 2>/dev/null | grep -q %s", sec, file, seek);
    int res = system (cmd);
    g_free (cmd);
    if (res == 0) return TRUE;
    else return FALSE;
}

/* Multiple HDMI support */

static int hdmi_monitors (VolumeALSAPlugin *vol)
{
    int i, m;

    /* check xrandr for connected monitors */
    m = get_value ("xrandr -q | grep -c connected");
    if (m < 0) m = 1; /* couldn't read, so assume 1... */
    if (m > 2) m = 2;

    /* get the names */
    if (m == 2)
    {
        for (i = 0; i < m; i++)
        {
            vol->mon_names[i] = get_string ("xrandr --listmonitors | grep %d: | cut -d ' ' -f 6", i);
        }

        /* check both devices are HDMI */
        if ((vol->mon_names[0] && strncmp (vol->mon_names[0], "HDMI", 4) != 0)
            || (vol->mon_names[1] && strncmp (vol->mon_names[1], "HDMI", 4) != 0))
                m = 1;
    }

    return m;
}


/*----------------------------------------------------------------------------*/
/* Bluetooth D-Bus interface                                                  */
/*----------------------------------------------------------------------------*/

static void bt_cb_object_added (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    const char *obj = g_dbus_object_get_object_path (object);
    if (asound_is_current_bt_dev (obj, FALSE) || asound_is_current_bt_dev (obj, TRUE))
    {
        DEBUG ("Selected Bluetooth audio device has connected");
        asound_initialize (vol);
        volumealsa_update_display (vol);
    }
}

static void bt_cb_object_removed (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    const char *obj = g_dbus_object_get_object_path (object);
    if (asound_is_current_bt_dev (obj, FALSE) || asound_is_current_bt_dev (obj, TRUE))
    {
        DEBUG ("Selected Bluetooth audio device has disconnected");
        asound_initialize (vol);
        volumealsa_update_display (vol);
    }
}

static void bt_cb_name_owned (GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    DEBUG ("Name %s owned on DBus", name);

    /* BlueZ exists - get an object manager for it */
    GError *error = NULL;
    vol->objmanager = g_dbus_object_manager_client_new_for_bus_sync (G_BUS_TYPE_SYSTEM, 0, "org.bluez", "/", NULL, NULL, NULL, NULL, &error);
    if (error)
    {
        DEBUG ("Error getting object manager - %s", error->message);
        vol->objmanager = NULL;
        g_error_free (error);
    }
    else
    {
        /* register callbacks for devices being added or removed */
        // now taken care of by monitoring PCMAdded / PCMRemoved signals on org.bluealsa
        // g_signal_connect (vol->objmanager, "object-added", G_CALLBACK (bt_cb_object_added), vol);
        // g_signal_connect (vol->objmanager, "object-removed", G_CALLBACK (bt_cb_object_removed), vol);

        /* Check whether a Bluetooth audio device is the current default output or input - connect to one or both if so */
        char *device = asound_get_bt_device ();
        char *idevice = asound_get_bt_input ();
        if (device || idevice)
        {
            /* Reconnect the current Bluetooth audio device */
            if (vol->bt_conname) g_free (vol->bt_conname);
            if (vol->bt_reconname) g_free (vol->bt_reconname);
            if (device) vol->bt_conname = device;
            else if (idevice) vol->bt_conname = idevice;

            if (device && idevice && g_strcmp0 (device, idevice)) vol->bt_reconname = idevice;
            else vol->bt_reconname = NULL;

            DEBUG ("Reconnecting devices");
            bt_reconnect_devices (vol);
        }
    }
}

static void bt_cb_name_unowned (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    DEBUG ("Name %s unowned on DBus", name);

    if (vol->objmanager) g_object_unref (vol->objmanager);
    if (vol->bt_conname) g_free (vol->bt_conname);
    if (vol->bt_reconname) g_free (vol->bt_reconname);
    vol->objmanager = NULL;
    vol->bt_conname = NULL;
    vol->bt_reconname = NULL;
}

static void bt_cb_ba_name_owned (GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    DEBUG ("Name %s owned on DBus", name);

    GError *error = NULL;
    vol->baproxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM, 0, NULL, "org.bluealsa", "/org/bluealsa", "org.bluealsa.Manager1", NULL, &error);

    if (error)
    {
        DEBUG ("Error getting proxy - %s", error->message);
        g_error_free (error);
    }
    else vol->basignal = g_signal_connect (vol->baproxy, "g-signal",  G_CALLBACK (bt_cb_ba_signal), vol);
}

static void bt_cb_ba_name_unowned (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    DEBUG ("Name %s unowned on DBus", name);
    g_signal_handler_disconnect (vol->baproxy, vol->basignal);
    g_object_unref (vol->baproxy);
}

static void bt_cb_ba_signal (GDBusProxy *prox, gchar *sender, gchar *signal, GVariant *params, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    if (!g_strcmp0 (signal, "PCMAdded") || !g_strcmp0 (signal, "PCMRemoved"))
    {
        DEBUG ("PCMs changed - %s", signal);
        if (asound_get_default_card () == BLUEALSA_DEV)
        {
            asound_initialize (vol);
            volumealsa_update_display (vol);
        }
    }
}

static void bt_connect_device (VolumeALSAPlugin *vol)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (vol->objmanager, vol->bt_conname, "org.bluez.Device1");
    DEBUG ("Connecting device %s...", vol->bt_conname);
    if (interface)
    {
        // trust and connect
        g_dbus_proxy_call (G_DBUS_PROXY (interface), "org.freedesktop.DBus.Properties.Set", 
            g_variant_new ("(ssv)", g_dbus_proxy_get_interface_name (G_DBUS_PROXY (interface)), "Trusted", g_variant_new_boolean (TRUE)),
            G_DBUS_CALL_FLAGS_NONE, -1, NULL, bt_cb_trusted, vol);
        g_dbus_proxy_call (G_DBUS_PROXY (interface), "Connect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, bt_cb_connected, vol);
        g_object_unref (interface);
    }
    else
    {
        DEBUG ("Couldn't get device interface from object manager");
        if (vol->conn_dialog) volumealsa_show_connect_dialog (vol, TRUE, _("Could not get BlueZ interface"));
        if (vol->bt_conname) g_free (vol->bt_conname);
        vol->bt_conname = NULL;
    }
}

static void bt_cb_connected (GObject *source, GAsyncResult *res, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    GError *error = NULL;
    char *paname;

    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
    if (var) g_variant_unref (var);

    if (error)
    {
        DEBUG ("Connect error %s", error->message);

        // update dialog to show a warning
        if (vol->conn_dialog) volumealsa_show_connect_dialog (vol, TRUE, error->message);
        g_error_free (error);
    }
    else
    {
        DEBUG ("Connected OK");

        // set connected device as PulseAudio default
        if (vol->bt_input)
        {
            paname = bluez_to_pa_source_name (vol->bt_conname);
            pa_context_set_default_source (vol->pa_context, paname, NULL, NULL);
        }
        else
        {
            paname = bluez_to_pa_sink_name (vol->bt_conname);
            pulse_change_sink (vol, paname);
        }
        g_free (paname);

        // close the connection dialog
        volumealsa_close_connect_dialog (NULL, vol);
    }

    // delete the connection information
    g_free (vol->bt_conname);
    vol->bt_conname = NULL;

    // reinit alsa to configure mixer
    //asound_initialize (vol);
    volumealsa_update_display (vol);
}

static void bt_cb_trusted (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
    if (var) g_variant_unref (var);

    if (error)
    {
        DEBUG ("Trusting error %s", error->message);
        g_error_free (error);
    }
    else DEBUG ("Trusted OK");
}

static void bt_reconnect_devices (VolumeALSAPlugin *vol)
{
    while (vol->bt_conname)
    {
        GDBusInterface *interface = g_dbus_object_manager_get_interface (vol->objmanager, vol->bt_conname, "org.bluez.Device1");
        DEBUG ("Reconnecting %s...", vol->bt_conname);
        if (interface)
        {
            // trust and connect
            g_dbus_proxy_call (G_DBUS_PROXY (interface), "org.freedesktop.DBus.Properties.Set",
                g_variant_new ("(ssv)", g_dbus_proxy_get_interface_name (G_DBUS_PROXY (interface)), "Trusted", g_variant_new_boolean (TRUE)),
                G_DBUS_CALL_FLAGS_NONE, -1, NULL, bt_cb_trusted, vol);
            g_dbus_proxy_call (G_DBUS_PROXY (interface), "Connect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, bt_cb_reconnected, vol);
            g_object_unref (interface);
            break;
        }

        DEBUG ("Couldn't get device interface from object manager - device not available to reconnect");
        g_free (vol->bt_conname);

        if (vol->bt_reconname)
        {
            vol->bt_conname = vol->bt_reconname;
            vol->bt_reconname = NULL;
        }
        else vol->bt_conname = NULL;
    }
}

static void bt_cb_reconnected (GObject *source, GAsyncResult *res, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    GError *error = NULL;

    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
    if (var) g_variant_unref (var);

    if (error) DEBUG ("Connect error %s", error->message);
    else DEBUG ("Connected OK");

    // delete the connection information
    g_free (vol->bt_conname);
    vol->bt_conname = NULL;

    // connect to second device if there is one...
    if (vol->bt_reconname)
    {
        vol->bt_conname = vol->bt_reconname;
        vol->bt_reconname = NULL;
        DEBUG ("Connecting to second device %s...", vol->bt_conname);
        bt_reconnect_devices (vol);
    }
    else
    {
        // reinit alsa to configure mixer
        asound_initialize (vol);
        volumealsa_update_display (vol);
    }
}

static void bt_disconnect_device (VolumeALSAPlugin *vol, char *device)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (vol->objmanager, device, "org.bluez.Device1");
    DEBUG ("Disconnecting device %s...", device);
    if (interface)
    {
        // call the disconnect method on BlueZ
        g_dbus_proxy_call (G_DBUS_PROXY (interface), "Disconnect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, bt_cb_disconnected, vol);
        g_object_unref (interface);
    }
    else
    {
        DEBUG ("Couldn't get device interface from object manager - device probably already disconnected");
        if (vol->bt_conname)
        {
            DEBUG ("Connecting to %s...", vol->bt_conname);
            bt_connect_device (vol);
        }
    }
}

static void bt_cb_disconnected (GObject *source, GAsyncResult *res, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
    if (var) g_variant_unref (var);

    if (error)
    {
        DEBUG ("Disconnect error %s", error->message);
        g_error_free (error);
    }
    else DEBUG ("Disconnected OK");

    // call BlueZ over DBus to connect to the device
    if (vol->bt_conname)
    {
        if (vol->bt_input == FALSE)
        {
            // mixer and element handles will now be invalid
            vol->mixers[OUTPUT_MIXER].mixer = NULL;
            vol->master_element = NULL;
        }

        DEBUG ("Connecting to %s...", vol->bt_conname);
        bt_connect_device (vol);
    }
}

static gboolean bt_has_service (VolumeALSAPlugin *vol, const gchar *path, const gchar *service)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (vol->objmanager, path, "org.bluez.Device1");
    GVariant *elem, *var = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "UUIDs");
    GVariantIter iter;
    g_variant_iter_init (&iter, var);
    while ((elem = g_variant_iter_next_value (&iter)))
    {
        const char *uuid = g_variant_get_string (elem, NULL);
        if (!strncasecmp (uuid, service, 8)) return TRUE;
        g_variant_unref (elem);
    }
    g_variant_unref (var);
    g_object_unref (interface);
    return FALSE;
}

static gboolean bt_is_connected (VolumeALSAPlugin *vol, const gchar *path)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (vol->objmanager, path, "org.bluez.Device1");
    GVariant *var = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Connected");
    gboolean res = g_variant_get_boolean (var);
    g_variant_unref (var);
    g_object_unref (interface);
    return res;
}


/*----------------------------------------------------------------------------*/
/* Volume and mute control                                                    */
/*----------------------------------------------------------------------------*/

#ifdef __UCLIBC__
#define exp10(x) (exp((x) * log(10)))
#endif

static long lrint_dir (double x, int dir)
{
    if (dir > 0) return lrint (ceil(x));
    else if (dir < 0) return lrint (floor(x));
    else return lrint (x);
}

static int get_normalized_volume (snd_mixer_elem_t *elem, gboolean capture)
{
    long min, max, lvalue, rvalue;
    double normalized, min_norm;
    int err;

    err = capture ? snd_mixer_selem_get_capture_dB_range (elem, &min, &max) : snd_mixer_selem_get_playback_dB_range (elem, &min, &max);
    if (err < 0 || min >= max)
    {
        err = capture ? snd_mixer_selem_get_capture_volume_range (elem, &min, &max) : snd_mixer_selem_get_playback_volume_range (elem, &min, &max);
        if (err < 0 || min == max) return 0;

        err = capture ? snd_mixer_selem_get_capture_volume (elem, SND_MIXER_SCHN_FRONT_LEFT, &lvalue) : snd_mixer_selem_get_playback_volume (elem, SND_MIXER_SCHN_FRONT_LEFT, &lvalue);
        if (err < 0) return 0;

        err = capture ? snd_mixer_selem_get_capture_volume (elem, SND_MIXER_SCHN_FRONT_RIGHT, &rvalue) : snd_mixer_selem_get_playback_volume (elem, SND_MIXER_SCHN_FRONT_RIGHT, &rvalue);
        if (err < 0) return 0;

        lvalue += rvalue;
        lvalue >>= 1;
        lvalue -= min;
        lvalue *= 100;
        lvalue /= (max - min);
        return (int) lvalue;
    }

    err = capture ? snd_mixer_selem_get_capture_dB (elem, SND_MIXER_SCHN_FRONT_LEFT, &lvalue) : snd_mixer_selem_get_playback_dB (elem, SND_MIXER_SCHN_FRONT_LEFT, &lvalue);
    if (err < 0) return 0;

    err = capture ? snd_mixer_selem_get_capture_dB (elem, SND_MIXER_SCHN_FRONT_RIGHT, &rvalue) : snd_mixer_selem_get_playback_dB (elem, SND_MIXER_SCHN_FRONT_RIGHT, &rvalue);
    if (err < 0) return 0;

    lvalue += rvalue;
    lvalue >>= 1;

    if (max - min <= 2400)
    {
        lvalue -= min;
        lvalue *= 100;
        lvalue /= (max - min);
        return (int) lvalue;
    }

    normalized = exp10 ((lvalue - max) / 6000.0);
    if (min != SND_CTL_TLV_DB_GAIN_MUTE)
    {
        min_norm = exp10 ((min - max) / 6000.0);
        normalized = (normalized - min_norm) / (1 - min_norm);
    }

    return (int) round (normalized * 100);
}

static int set_normalized_volume (snd_mixer_elem_t *elem, int volume, int dir, gboolean capture)
{
    long min, max, value;
    double min_norm;
    int err;
    double vol_perc = (double) volume / 100;

    err = capture ? snd_mixer_selem_get_capture_dB_range (elem, &min, &max) : snd_mixer_selem_get_playback_dB_range (elem, &min, &max);
    if (err < 0 || min >= max)
    {
        err = capture ? snd_mixer_selem_get_capture_volume_range (elem, &min, &max) : snd_mixer_selem_get_playback_volume_range (elem, &min, &max);
        if (err < 0) return err;

        value = lrint_dir (vol_perc * (max - min), dir) + min;
        return capture ? snd_mixer_selem_set_capture_volume_all (elem, value) : snd_mixer_selem_set_playback_volume_all (elem, value);
    }

    if (max - min <= 2400)
    {
        value = lrint_dir (vol_perc * (max - min), dir) + min;
        if (dir == 0) dir = 1;  // dir = 0 seems to round down...
        return capture ? snd_mixer_selem_set_capture_dB_all (elem, value, dir) : snd_mixer_selem_set_playback_dB_all (elem, value, dir);
    }

    if (min != SND_CTL_TLV_DB_GAIN_MUTE)
    {
        min_norm = exp10 ((min - max) / 6000.0);
        vol_perc = vol_perc * (1 - min_norm) + min_norm;
    }
    value = lrint_dir (6000.0 * log10 (vol_perc), dir) + max;
    return capture ? snd_mixer_selem_set_capture_dB_all (elem, value, dir) : snd_mixer_selem_set_playback_dB_all (elem, value, dir);
}

/* Get the presence of the mute control from the sound system. */
static gboolean asound_has_mute (VolumeALSAPlugin *vol)
{
    if (vol->master_element == NULL || snd_mixer_elem_get_type (vol->master_element) != SND_MIXER_ELEM_SIMPLE) return FALSE;

    return snd_mixer_selem_has_playback_switch (vol->master_element);
}

/* Get the condition of the mute control from the sound system. */
static gboolean asound_is_muted (VolumeALSAPlugin *vol)
{
    if (vol->master_element == NULL || snd_mixer_elem_get_type (vol->master_element) != SND_MIXER_ELEM_SIMPLE) return FALSE;
    if (!snd_mixer_selem_has_playback_channel (vol->master_element, SND_MIXER_SCHN_FRONT_LEFT)) return FALSE;
    if (!snd_mixer_selem_has_playback_switch (vol->master_element)) return FALSE;

    /* The switch is on if sound is not muted, and off if the sound is muted.
     * Initialize so that the sound appears unmuted if the control does not exist. */
    int value = 1;
    snd_mixer_selem_get_playback_switch (vol->master_element, SND_MIXER_SCHN_FRONT_LEFT, &value);
    return (value == 0);
}

static void asound_set_mute (VolumeALSAPlugin *vol, gboolean mute)
{
    if (vol->master_element == NULL || snd_mixer_elem_get_type (vol->master_element) != SND_MIXER_ELEM_SIMPLE) return;
    if (!snd_mixer_selem_has_playback_switch (vol->master_element)) return;

    snd_mixer_selem_set_playback_switch_all (vol->master_element, mute ? 0 : 1);
}

/* Get the volume from the sound system.
 * This implementation returns the average of the Front Left and Front Right channels. */
static int asound_get_volume (VolumeALSAPlugin *vol)
{
    if (vol->master_element == NULL || snd_mixer_elem_get_type (vol->master_element) != SND_MIXER_ELEM_SIMPLE) return 0;
    if (!snd_mixer_selem_has_playback_channel (vol->master_element, SND_MIXER_SCHN_FRONT_LEFT)) return 0;
    if (!snd_mixer_selem_has_playback_volume (vol->master_element)) return 0;

    return get_normalized_volume (vol->master_element, FALSE);
}

/* Set the volume to the sound system.
 * This implementation sets the Front Left and Front Right channels to the specified value. */
static void asound_set_volume (VolumeALSAPlugin *vol, int volume)
{
    if (vol->master_element == NULL || snd_mixer_elem_get_type (vol->master_element) != SND_MIXER_ELEM_SIMPLE) return;
    if (!snd_mixer_selem_has_playback_channel (vol->master_element, SND_MIXER_SCHN_FRONT_LEFT)) return;
    if (!snd_mixer_selem_has_playback_volume (vol->master_element)) return;

    set_normalized_volume (vol->master_element, volume, volume - asound_get_volume (vol), FALSE);
}


/*----------------------------------------------------------------------------*/
/* ALSA interface                                                             */
/*----------------------------------------------------------------------------*/

/* Initialize the ALSA interface */

static gboolean asound_initialize (VolumeALSAPlugin *vol)
{
    /* make sure existing watches are removed by calling deinitialize */
    asound_deinitialize (vol);

    DEBUG ("Initializing...");

    /* if the default device is a Bluetooth device, check it is actually connected... */
    if (asound_get_default_card () == BLUEALSA_DEV)
    {
        char *btdev = asound_get_bt_device ();
        gboolean res = bt_is_connected (vol, btdev);
        g_free (btdev);
        if (!res)
        {
            g_warning ("volumealsa: Default Bluetooth output device not connected - cannot attach mixer");
            return TRUE;
        }
    }

    if (!asound_mixer_initialize (vol, OUTPUT_MIXER))
    {
        g_warning ("volumealsa: Device invalid - cannot attach mixer");
        return TRUE;
    }

    if (!asound_find_master_elem (vol))
    {
        g_warning ("volumealsa: Cannot find suitable master element");
        return TRUE;
    }

    if (!asound_current_dev_check (vol)) return FALSE;

    return TRUE;
}

static void asound_deinitialize (VolumeALSAPlugin *vol)
{
    guint i;

    DEBUG ("Deinitializing...");
    if (vol->mixer_evt_idle != 0)
    {
        g_source_remove (vol->mixer_evt_idle);
        vol->mixer_evt_idle = 0;
    }
    vol->master_element = NULL;
    asound_mixer_deinitialize (vol, OUTPUT_MIXER);
}

/* An ALSA mixer exposes a variety of simple controls, which are identified only
 * by name. There is no standard for the name of the "master" control, and there
 * are dozens of names used for it on the devices I have seen.
 * A lot of devices seem to put the master as the first control in the list, but
 * the IQaudio devices sort their controls alphabetically, with 'Analogue' first,
 * when the master is actually 'Digital'. On devices like the IQaudio, there is
 * only one control which has both a volume control and a switch, and that is the
 * master.
 * So the way we try to find the master is to search the list of controls from the
 * start for the first control with both volume and switch; if we find one, we
 * assume it is the master. If there are no controls with both volume and switch,
 * we search again for the first volume control.
 * This isn't perfect, but it is as close as I have been able to get so far...
 */

static gboolean asound_find_master_elem (VolumeALSAPlugin *vol)
{
    snd_mixer_elem_t *elem;

    if (!vol->mixers[OUTPUT_MIXER].mixer) return FALSE;

    for (elem = snd_mixer_first_elem (vol->mixers[OUTPUT_MIXER].mixer); elem != NULL; elem = snd_mixer_elem_next (elem))
    {
        if (snd_mixer_selem_is_active (elem) && snd_mixer_selem_has_playback_volume (elem) && snd_mixer_selem_has_playback_switch (elem))
        {
            DEBUG ("Device (vol and switch) attached successfully");
            vol->master_element = elem;
            return TRUE;
        }
    }

    for (elem = snd_mixer_first_elem (vol->mixers[OUTPUT_MIXER].mixer); elem != NULL; elem = snd_mixer_elem_next (elem))
    {
        if (snd_mixer_selem_is_active (elem) && snd_mixer_selem_has_playback_volume (elem))
        {
            DEBUG ("Device (vol only) attached successfully");
            vol->master_element = elem;
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean asound_mixer_initialize (VolumeALSAPlugin *vol, MixerIO io)
{
    char *device = io ? asound_default_input_name () : asound_default_device_name ();
    snd_mixer_t *mixer;
    struct pollfd *fds;
    int nchans, i;
    gboolean res = FALSE;

    DEBUG ("Attaching mixer to %s device %s...", io ? "input" : "output", device);

    vol->mixers[io].mixer = NULL;

    // create and attach the mixer
    if (snd_mixer_open (&mixer, 0)) goto end;

    if (snd_mixer_attach (mixer, device))
    {
        snd_mixer_close (mixer);
        goto end;
    }

    if (snd_mixer_selem_register (mixer, NULL, NULL) || snd_mixer_load (mixer))
    {
        snd_mixer_detach (mixer, device);
        snd_mixer_close (mixer);
        goto end;
    }

    vol->mixers[io].mixer = mixer;

    /* listen for ALSA events on the mixer */
    nchans = snd_mixer_poll_descriptors_count (mixer);
    vol->mixers[io].num_channels = nchans;
    vol->mixers[io].channels = g_new0 (GIOChannel *, nchans);
    vol->mixers[io].watches = g_new0 (guint, nchans);

    fds = g_new0 (struct pollfd, nchans);
    snd_mixer_poll_descriptors (mixer, fds, nchans);
    for (i = 0; i < nchans; ++i)
    {
        vol->mixers[io].channels[i] = g_io_channel_unix_new (fds[i].fd);
        vol->mixers[io].watches[i] = g_io_add_watch (vol->mixers[io].channels[i], G_IO_IN | G_IO_HUP, asound_mixer_event, vol);
    }
    g_free (fds);
    res = TRUE;

end:
    g_free (device);
    return res;
}

static void asound_mixer_deinitialize (VolumeALSAPlugin *vol, MixerIO io)
{
    char *device = io ? asound_default_input_name () : asound_default_device_name ();
    int i;

    DEBUG ("Detaching mixer from %s device %s...", io ? "input" : "output", device);

    for (i = 0; i < vol->mixers[io].num_channels; i++)
    {
        g_source_remove (vol->mixers[io].watches[i]);
        g_io_channel_shutdown (vol->mixers[io].channels[i], FALSE, NULL);
        g_io_channel_unref (vol->mixers[io].channels[i]);
    }

    g_free (vol->mixers[io].channels);
    g_free (vol->mixers[io].watches);
    vol->mixers[io].channels = NULL;
    vol->mixers[io].watches = NULL;
    vol->mixers[io].num_channels = 0;

    if (vol->mixers[io].mixer)
    {
        snd_mixer_detach (vol->mixers[io].mixer, device);
        snd_mixer_close (vol->mixers[io].mixer);
    }
    vol->mixers[io].mixer = NULL;

    g_free (device);
}

static int asound_find_valid_device (void)
{
    // call this if the current ALSA device is invalid - it tries to find an alternative
    g_warning ("volumealsa: Default ALSA device not valid - resetting to internal");

    int num = asound_get_bcm_device_num ();
    if (num != -1)
    {
        g_warning ("volumealsa: Setting to internal device hw:%d", num);
        asound_set_default_card (num);
        return num;
    }
    else
    {
        g_warning ("volumealsa: Internal device not available - looking for first valid ALSA device...");
        while (1)
        {
            if (snd_card_next (&num) < 0)
            {
                g_warning ("volumealsa: Cannot enumerate devices");
                break;
            }
            if (num == -1) break;

            g_warning ("volumealsa: Valid ALSA device hw:%d found", num);
            asound_set_default_card (num);
            return num;
        }
        g_warning ("volumealsa: No ALSA devices found");
        asound_set_default_card (-1);
    }
    return -1;
}

static gboolean asound_current_dev_check (VolumeALSAPlugin *vol)
{
    if (!vsystem ("amixer info 2>/dev/null | grep -q .")) return TRUE;
    else
    {
        asound_deinitialize (vol);
        asound_set_default_card (-1);
        return FALSE;
    }
}

static gboolean asound_has_volume_control (int dev)
{
    if (dev == -1)
        return vsystem ("amixer scontents 2>/dev/null | grep -q pvolume") ? FALSE : TRUE;
    else
        return vsystem ("amixer -c %d scontents 2>/dev/null | grep -q pvolume", dev) ? FALSE : TRUE;
}

static gboolean asound_has_input (int dev)
{
    return vsystem ("amixer -c %d scontents 2>/dev/null | grep -q cvolume", dev) ? FALSE : TRUE;
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

static gboolean asound_restart (gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;

    if (!g_main_current_source ()) return TRUE;
    if (g_source_is_destroyed (g_main_current_source ())) return FALSE;

    if (!asound_initialize (vol))
    {
        g_warning ("volumealsa: Re-initialization failed.");
        return TRUE; // try again in a second
    }

    g_warning ("volumealsa: Restarted ALSA interface...");
    volumealsa_update_display (vol);

    vol->restart_idle = 0;
    return FALSE;
}

static gboolean asound_reset_mixer_evt_idle (gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;

    if (!g_source_is_destroyed (g_main_current_source ()))
        vol->mixer_evt_idle = 0;
    return FALSE;
}

/* Handler for I/O event on ALSA channel. */
static gboolean asound_mixer_event (GIOChannel *channel, GIOCondition cond, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    int i, res = 0;
    snd_mixer_t *mixer = NULL;

    if (g_source_is_destroyed (g_main_current_source ())) return FALSE;

    if (vol->mixer_evt_idle == 0)
    {
        if (vol->mixers[OUTPUT_MIXER].mixer)
        {
            for (i = 0; i < vol->mixers[OUTPUT_MIXER].num_channels; i++)
            {
                if (channel == vol->mixers[OUTPUT_MIXER].channels[i])
                {
                    mixer = vol->mixers[OUTPUT_MIXER].mixer;
                    DEBUG ("Output mixer event");
                }
            }
        }
        if (mixer == NULL && vol->mixers[INPUT_MIXER].mixer)
        {
            for (i = 0; i < vol->mixers[INPUT_MIXER].num_channels; i++)
            {
                if (channel == vol->mixers[INPUT_MIXER].channels[i])
                {
                    mixer = vol->mixers[INPUT_MIXER].mixer;
                    DEBUG ("Input mixer event");
                }
            }
        }
        if (mixer)
        {
            vol->mixer_evt_idle = g_idle_add_full (G_PRIORITY_DEFAULT, (GSourceFunc) asound_reset_mixer_evt_idle, vol, NULL);
            res = snd_mixer_handle_events (mixer);
        }
        else return TRUE;
    }

    /* the status of mixer is changed. update of display is needed. */
    if (cond & G_IO_IN && res >= 0) volumealsa_update_display (vol);

    if ((cond & G_IO_HUP) || (res < 0))
    {
        /* This means there're some problems with alsa. */
        g_warning ("volumealsa: ALSA (or pulseaudio) had a problem: "
                "volumealsa: snd_mixer_handle_events() = %d,"
                " cond 0x%x (IN: 0x%x, HUP: 0x%x).", res, cond,
                G_IO_IN, G_IO_HUP);
        gtk_widget_set_tooltip_text (vol->plugin, "ALSA (or pulseaudio) had a problem."
                " Please check the lxpanel logs.");

        if (vol->restart_idle == 0) vol->restart_idle = g_timeout_add_seconds (1, asound_restart, vol);

        return FALSE;
    }

    return TRUE;
}


/*----------------------------------------------------------------------------*/
/* .asoundrc manipulation                                                     */
/*----------------------------------------------------------------------------*/

static int asound_get_default_card (void)
{
    char *user_config_file = g_build_filename (g_get_home_dir (), "/.asoundrc", NULL);
    char *res;
    int val;

    /* does .asoundrc exist? if not, default device is 0 */
    if (!g_file_test (user_config_file, G_FILE_TEST_IS_REGULAR))
    {
        g_free (user_config_file);
        return 0;
    }

    /* does .asoundrc use type asym? */
    if (find_in_section (user_config_file, "pcm.!default", "asym"))
    {
        /* look in pcm.output section for bluealsa */
        if (find_in_section (user_config_file, "pcm.output", "bluealsa"))
        {
            g_free (user_config_file);
            return BLUEALSA_DEV;
        }

        /* otherwise parse pcm.output section for card number */
        res = get_string ("sed -n '/pcm.output/,/}/{/card/p}' %s 2>/dev/null | grep -o 'card\\s[0-9]' | cut -d ' ' -f 2", user_config_file);
        if (sscanf (res, "%d", &val) != 1) val = -1;
    }
    else
    {
        /* first check to see if Bluetooth is in use */
        if (find_in_section (user_config_file, "pcm.!default", "bluealsa"))
        {
            g_free (user_config_file);
            return BLUEALSA_DEV;
        }

        /* if not, check for new format file */
        res = get_string ("sed -n '/pcm.!default/,/}/{/slave.pcm/p}' %s 2>/dev/null | cut -d '\"' -f 2 | cut -d : -f 2", user_config_file);
        if (sscanf (res, "%d", &val) == 1) goto DONE;
        g_free (res);

        /* if not, check for old format file */
        res = get_string ("sed -n '/pcm.!default/,/}/{/card/p}' %s 2>/dev/null | grep -o 'card\\s[0-9]' | cut -d ' ' -f 2", user_config_file);
        if (sscanf (res, "%d", &val) == 1) goto DONE;

        /* nothing valid found, default device is 0 */
        val = 0;
    }

    DONE: g_free (res);
    g_free (user_config_file);
    return val;
}

static int asound_get_default_input (void)
{
    char *user_config_file = g_build_filename (g_get_home_dir (), "/.asoundrc", NULL);
    char *res;
    int val;

    /* does .asoundrc exist? if not, default device is 0 */
    if (!g_file_test (user_config_file, G_FILE_TEST_IS_REGULAR))
    {
        g_free (user_config_file);
        return 0;
    }

    /* does .asoundrc use type asym? */
    if (find_in_section (user_config_file, "pcm.!default", "asym"))
    {
        /* look in pcm.input section for bluealsa */
        if (find_in_section (user_config_file, "pcm.input", "bluealsa"))
        {
            g_free (user_config_file);
            return BLUEALSA_DEV;
        }

        /* parse pcm.input section for card number */
        res = get_string ("sed -n '/pcm.input/,/}/{/card/p}' %s 2>/dev/null | grep -o 'card\\s[0-9]' | cut -d ' ' -f 2", user_config_file);
        if (sscanf (res, "%d", &val) != 1) val = -1;
    }
    else
    {
        /* default input device is same as output pcm device */
        /* check for new format file */
        res = get_string ("sed -n '/pcm.!default/,/}/{/slave.pcm/p}' %s 2>/dev/null | cut -d '\"' -f 2 | cut -d : -f 2", user_config_file);
        if (sscanf (res, "%d", &val) == 1) goto DONE;
        g_free (res);

        /* if not, check for old format file */
        res = get_string ("sed -n '/pcm.!default/,/}/{/card/p}' %s 2>/dev/null | grep -o 'card\\s[0-9]' | cut -d ' ' -f 2", user_config_file);
        if (sscanf (res, "%d", &val) == 1) goto DONE;

        /* nothing valid found, default device is 0 */
        val = 0;
    }

    DONE: g_free (res);
    g_free (user_config_file);
    return val;
}

/* Standard text blocks used in .asoundrc for ALSA (_A) and Bluetooth (_B) devices */
#define PREFIX      "pcm.!default {\n\ttype asym\n\tplayback.pcm {\n\t\ttype plug\n\t\tslave.pcm \"output\"\n\t}\n\tcapture.pcm {\n\t\ttype plug\n\t\tslave.pcm \"input\"\n\t}\n}"
#define OUTPUT_A    "\npcm.output {\n\ttype hw\n\tcard %d\n}"
#define INPUT_A     "\npcm.input {\n\ttype hw\n\tcard %d\n}"
#define CTL_A       "\nctl.!default {\n\ttype hw\n\tcard %d\n}"
#define OUTPUT_B    "\npcm.output {\n\ttype bluealsa\n\tdevice \"%02X:%02X:%02X:%02X:%02X:%02X\"\n\tprofile \"a2dp\"\n}"
#define INPUT_B     "\npcm.input {\n\ttype bluealsa\n\tdevice \"%02X:%02X:%02X:%02X:%02X:%02X\"\n\tprofile \"sco\"\n}"
#define CTL_B       "\nctl.!default {\n\ttype bluealsa\n}"

static void asound_set_default_card (int num)
{
    char *user_config_file = g_build_filename (g_get_home_dir (), "/.asoundrc", NULL);

    /* does .asoundrc exist? if not, write default contents and exit */
    if (!g_file_test (user_config_file, G_FILE_TEST_IS_REGULAR))
    {
        vsystem ("echo '" PREFIX "\n" OUTPUT_A "\n" CTL_A "' >> %s", num, num, user_config_file);
        goto DONE;
    }

    /* does .asoundrc use type asym? if not, replace file with default contents and exit */
    if (!find_in_section (user_config_file, "pcm.!default", "asym"))
    {
        vsystem ("echo '" PREFIX "\n" OUTPUT_A "\n" CTL_A "' > %s", num, num, user_config_file);
        goto DONE;
    }

    /* is there a pcm.output section? update it if so; if not, append one */
    if (!find_in_section (user_config_file, "pcm.output", "type"))
        vsystem ("echo '" OUTPUT_A "' >> %s", num, user_config_file);
    else
        vsystem ("sed -i '/pcm.output/,/}/c pcm.output {\\n\\ttype hw\\n\\tcard %d\\n}' %s", num, user_config_file);

    /* is there a ctl.!default section? update it if so; if not, append one */
    if (!find_in_section (user_config_file, "ctl.!default", "type"))
        vsystem ("echo '" CTL_A "' >> %s", num, user_config_file);
    else
        vsystem ("sed -i '/ctl.!default/,/}/c ctl.!default {\\n\\ttype hw\\n\\tcard %d\\n}' %s", num, user_config_file);

    DONE: g_free (user_config_file);
}

static void asound_set_default_input (int num)
{
    char *user_config_file = g_build_filename (g_get_home_dir (), "/.asoundrc", NULL);

    /* does .asoundrc exist? if not, write default contents and exit */
    if (!g_file_test (user_config_file, G_FILE_TEST_IS_REGULAR))
    {
        vsystem ("echo '" PREFIX "\n" OUTPUT_A "\n" INPUT_A "\n" CTL_A "' >> %s", 0, num, 0, user_config_file);
        goto DONE;
    }

    /* does .asoundrc use type asym? if not, replace file with default contents and current output, and exit */
    if (!find_in_section (user_config_file, "pcm.!default", "asym"))
    {
        int dev = asound_get_default_card ();
        if (dev != BLUEALSA_DEV)
            vsystem ("echo '" PREFIX "\n" OUTPUT_A "\n" INPUT_A "\n" CTL_A "' > %s", dev, num, dev, user_config_file);
        else
        {
            unsigned int b1, b2, b3, b4, b5, b6;
            char *btdev = asound_get_bt_device ();
            if (btdev && sscanf (btdev, "/org/bluez/hci0/dev_%x_%x_%x_%x_%x_%x", &b1, &b2, &b3, &b4, &b5, &b6) == 6)
                vsystem ("echo '" PREFIX "\n" OUTPUT_B "\n" INPUT_A "\n" CTL_B "' > %s", b1, b2, b3, b4, b5, b6, num, user_config_file);
            else
                vsystem ("echo '" PREFIX "\n" OUTPUT_A "\n" INPUT_A "\n" CTL_A "' > %s", 0, num, 0, user_config_file);
            if (btdev) g_free (btdev);
        }
        goto DONE;
    }

    /* is there a pcm.input section? update it if so; if not, append one */
    if (!find_in_section (user_config_file, "pcm.input", "type"))
        vsystem ("echo '" INPUT_A "' >> %s", num, user_config_file);
    else
        vsystem ("sed -i '/pcm.input/,/}/c pcm.input {\\n\\ttype hw\\n\\tcard %d\\n}' %s", num, user_config_file);

    DONE: g_free (user_config_file);
}

static char *asound_get_bt_device (void)
{
    char *user_config_file = g_build_filename (g_get_home_dir (), "/.asoundrc", NULL);
    char *res, *ret = NULL;

    /* first check the pcm.output section */
    res = get_string ("sed -n '/pcm.output/,/}/{/device/p}' %s 2>/dev/null | cut -d '\"' -f 2 | tr : _", user_config_file);
    if (strlen (res) == 17) goto DONE;
    else g_free (res);

    /* if nothing there, check the default block */
    res = get_string ("sed -n '/pcm.!default/,/}/{/device/p}' %s 2>/dev/null | cut -d '\"' -f 2 | tr : _", user_config_file);
    if (strlen (res) == 17) goto DONE;
    else g_free (res);

    res = NULL;
    DONE: g_free (user_config_file);
    if (res)
    {
        ret = g_strdup_printf ("/org/bluez/hci0/dev_%s", res);
        g_free (res);
    }
    return ret;
}

static char *asound_get_bt_input (void)
{
    char *user_config_file = g_build_filename (g_get_home_dir (), "/.asoundrc", NULL);
    char *res, *ret = NULL;

    /* check the pcm.input section */
    res = get_string ("sed -n '/pcm.input/,/}/{/device/p}' %s 2>/dev/null | cut -d '\"' -f 2 | tr : _", user_config_file);
    if (strlen (res) == 17) goto DONE;
    else g_free (res);

    res = NULL;
    DONE: g_free (user_config_file);
    if (res)
    {
        ret = g_strdup_printf ("/org/bluez/hci0/dev_%s", res);
        g_free (res);
    }
    return ret;
}

static void asound_set_bt_device (char *devname)
{
    char *user_config_file = g_build_filename (g_get_home_dir (), "/.asoundrc", NULL);
    unsigned int b1, b2, b3, b4, b5, b6;

    /* parse the device name to make sure it is valid */
    if (sscanf (devname, "/org/bluez/hci0/dev_%x_%x_%x_%x_%x_%x", &b1, &b2, &b3, &b4, &b5, &b6) != 6)
    {
        DEBUG ("Failed to set device - name %s invalid", devname);
        goto DONE;
    }

    /* does .asoundrc exist? if not, write default contents and exit */
    if (!g_file_test (user_config_file, G_FILE_TEST_IS_REGULAR))
    {
        vsystem ("echo '" PREFIX "\n" OUTPUT_B "\n" CTL_B "' >> %s", b1, b2, b3, b4, b5, b6, user_config_file);
        goto DONE;
    }

    /* does .asoundrc use type asym? if not, replace file with default contents and exit */
    if (!find_in_section (user_config_file, "pcm.!default", "asym"))
    {
        vsystem ("echo '" PREFIX "\n" OUTPUT_B "\n" CTL_B "' > %s", b1, b2, b3, b4, b5, b6, user_config_file);
        goto DONE;
    }

    /* is there a pcm.output section? update it if so; if not, append one */
    if (!find_in_section (user_config_file, "pcm.output", "type"))
        vsystem ("echo '" OUTPUT_B "' >> %s", b1, b2, b3, b4, b5, b6, user_config_file);
    else
        vsystem ("sed -i '/pcm.output/,/}/c pcm.output {\\n\\ttype bluealsa\\n\\tdevice \"%02X:%02X:%02X:%02X:%02X:%02X\"\\n\\tprofile \"a2dp\"\\n}' %s", b1, b2, b3, b4, b5, b6, user_config_file);

    /* is there a ctl.!default section? update it if so; if not, append one */
    if (!find_in_section (user_config_file, "ctl.!default", "type"))
        vsystem ("echo '" CTL_B "' >> %s", user_config_file);
    else
        vsystem ("sed -i '/ctl.!default/,/}/c ctl.!default {\\n\\ttype bluealsa\\n}' %s", user_config_file);

    DONE: g_free (user_config_file);
}

static void asound_set_bt_input (char *devname)
{
    char *user_config_file = g_build_filename (g_get_home_dir (), "/.asoundrc", NULL);
    unsigned int b1, b2, b3, b4, b5, b6;

    /* parse the device name to make sure it is valid */
    if (sscanf (devname, "/org/bluez/hci0/dev_%x_%x_%x_%x_%x_%x", &b1, &b2, &b3, &b4, &b5, &b6) != 6)
    {
        DEBUG ("Failed to set device - name %s invalid", devname);
        goto DONE;
    }

    /* does .asoundrc exist? if not, write default contents and exit */
    if (!g_file_test (user_config_file, G_FILE_TEST_IS_REGULAR))
    {
        vsystem ("echo '" PREFIX "\n" OUTPUT_A "\n" INPUT_B "\n" CTL_A "' >> %s", 0, b1, b2, b3, b4, b5, b6, 0, user_config_file);
        goto DONE;
    }

    /* does .asoundrc use type asym? if not, replace file with default contents and current output, and exit */
    if (!find_in_section (user_config_file, "pcm.!default", "asym"))
    {
        int dev = asound_get_default_card ();
        if (dev != BLUEALSA_DEV)
            vsystem ("echo '" PREFIX "\n" OUTPUT_A "\n" INPUT_B "\n" CTL_A "' > %s", dev, b1, b2, b3, b4, b5, b6, dev, user_config_file);
        else
        {
            unsigned int c1, c2, c3, c4, c5, c6;
            char *btdev = asound_get_bt_device ();
            if (btdev && sscanf (btdev, "/org/bluez/hci0/dev_%x_%x_%x_%x_%x_%x", &c1, &c2, &c3, &c4, &c5, &c6) == 6)
                vsystem ("echo '" PREFIX "\n" OUTPUT_B "\n" INPUT_B "\n" CTL_B "' > %s", c1, c2, c3, c4, c5, c6, b1, b2, b3, b4, b5, b6, user_config_file);
            else
                vsystem ("echo '" PREFIX "\n" OUTPUT_A "\n" INPUT_B "\n" CTL_A "' > %s", 0, b1, b2, b3, b4, b5, b6, 0, user_config_file);
            if (btdev) g_free (btdev);
        }
        goto DONE;
    }

    /* is there a pcm.input section? update it if so; if not, append one */
    if (!find_in_section (user_config_file, "pcm.input", "type"))
        vsystem ("echo '" INPUT_B "' >> %s", b1, b2, b3, b4, b5, b6, user_config_file);
    else
        vsystem ("sed -i '/pcm.input/,/}/c pcm.input {\\n\\ttype bluealsa\\n\\tdevice \"%02X:%02X:%02X:%02X:%02X:%02X\"\\n\\tprofile \"sco\"\\n}' %s", b1, b2, b3, b4, b5, b6, user_config_file);

    DONE: g_free (user_config_file);
}

static gboolean asound_is_current_bt_dev (const char *obj, gboolean is_input)
{
    gboolean res = FALSE;
    char *device = is_input ? asound_get_bt_input () : asound_get_bt_device ();
    if (device)
    {
        if (strstr (obj, device)) res = TRUE;
        g_free (device);
    }
    return res;
}

static int asound_get_bcm_device_num (void)
{
    int num = -1;

    while (1)
    {
        if (snd_card_next (&num) < 0)
        {
            g_warning ("volumealsa: Cannot enumerate devices");
            break;
        }
        if (num == -1) break;

        if (asound_is_bcm_device (num)) return num;
    }
    return -1;
}

static int asound_is_bcm_device (int num)
{
    char *name;
    if (snd_card_get_name (num, &name)) return FALSE;
    int res = strncmp (name, "bcm2835", 7);
    if (!strncmp (name, "bcm2835", 7))
    {
        if (!g_strcmp0 (name, "bcm2835 ALSA")) res = 1;
        else res = 2;
    }
    else res = 0;
    g_free (name);

    return res;
}

static char *asound_default_device_name (void)
{
    int num = asound_get_default_card ();
    if (num == BLUEALSA_DEV) return g_strdup_printf ("bluealsa");
    else return g_strdup_printf ("hw:%d", num);
}

static char *asound_default_input_name (void)
{
    int num = asound_get_default_input ();
    if (num == BLUEALSA_DEV) return g_strdup_printf ("bluealsa");
    else return g_strdup_printf ("hw:%d", num);
}


/*----------------------------------------------------------------------------*/
/* Plugin handlers and graphics                                               */
/*----------------------------------------------------------------------------*/

/* Do a full redraw of the display. */
static void volumealsa_update_display (VolumeALSAPlugin *vol)
{
    gboolean mute;
    int level;
#ifdef ENABLE_NLS
    // need to rebind here for tooltip update
    textdomain (GETTEXT_PACKAGE);
#endif

    if (vol->options_dlg) update_options (vol);

    /* check that the mixer is still valid */
    if (vol->master_element == NULL || snd_mixer_elem_get_type (vol->master_element) != SND_MIXER_ELEM_SIMPLE)
    {
        DEBUG ("Master element not valid");
        mute = TRUE;
        level = 0;
    }
    else
    {
        /* read current mute and volume status */
        mute = pulse_get_mute (vol);
        level = pulse_get_volume (vol);
        if (mute) level = 0;
    }

    /* update icon */
    const char *icon = "audio-volume-muted";
    if (!mute)
    {
        if (level >= 66) icon = "audio-volume-high";
        else if (level >= 33) icon = "audio-volume-medium";
        else if (level > 0) icon = "audio-volume-low";
    }
    lxpanel_plugin_set_taskbar_icon (vol->panel, vol->tray_icon, icon);

    /* update popup window controls */
    if (vol->mute_check)
    {
        g_signal_handler_block (vol->mute_check, vol->mute_check_handler);
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (vol->mute_check), mute);
        gtk_widget_set_sensitive (vol->mute_check, asound_has_mute (vol) ? TRUE : FALSE);
        g_signal_handler_unblock (vol->mute_check, vol->mute_check_handler);
    }

    if (vol->volume_scale)
    {
        g_signal_handler_block (vol->volume_scale, vol->volume_scale_handler);
        gtk_range_set_value (GTK_RANGE (vol->volume_scale), level);
        g_signal_handler_unblock (vol->volume_scale, vol->volume_scale_handler);
        gtk_widget_set_sensitive (vol->volume_scale, (vol->master_element && snd_mixer_elem_get_type (vol->master_element) == SND_MIXER_ELEM_SIMPLE));
    }

    /* update tooltip */
    char *tooltip;
    if (vol->master_element && snd_mixer_elem_get_type (vol->master_element) == SND_MIXER_ELEM_SIMPLE)
        tooltip = g_strdup_printf ("%s %d", _("Volume control"), level);
    else
        tooltip = g_strdup_printf (_("No volume control on this device"));
    gtk_widget_set_tooltip_text (vol->plugin, tooltip);
    g_free (tooltip);
}

static void volumealsa_theme_change (GtkWidget *widget, VolumeALSAPlugin *vol)
{
    volumealsa_update_display (vol);
}

static void volumealsa_open_config_dialog (GtkWidget *widget, VolumeALSAPlugin *vol)
{
    gtk_menu_popdown (GTK_MENU (vol->menu_popup));
    show_output_options (vol);
}

static void volumealsa_open_input_config_dialog (GtkWidget *widget, VolumeALSAPlugin *vol)
{
    gtk_menu_popdown (GTK_MENU (vol->menu_popup));
    show_input_options (vol);
}

static void volumealsa_show_connect_dialog (VolumeALSAPlugin *vol, gboolean failed, const gchar *param)
{
    char buffer[256];

    if (!failed)
    {
        vol->conn_dialog = gtk_dialog_new_with_buttons (_("Connecting Audio Device"), NULL, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, NULL);
        gtk_window_set_icon_name (GTK_WINDOW (vol->conn_dialog), "preferences-system-bluetooth");
        gtk_window_set_position (GTK_WINDOW (vol->conn_dialog), GTK_WIN_POS_CENTER);
        gtk_container_set_border_width (GTK_CONTAINER (vol->conn_dialog), 10);
        sprintf (buffer, _("Connecting to Bluetooth audio device '%s'..."), param);
        vol->conn_label = gtk_label_new (buffer);
        gtk_label_set_line_wrap (GTK_LABEL (vol->conn_label), TRUE);
        gtk_label_set_justify (GTK_LABEL (vol->conn_label), GTK_JUSTIFY_LEFT);
        gtk_misc_set_alignment (GTK_MISC (vol->conn_label), 0.0, 0.0);
        gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (vol->conn_dialog))), vol->conn_label, TRUE, TRUE, 0);
        g_signal_connect (GTK_OBJECT (vol->conn_dialog), "delete_event", G_CALLBACK (volumealsa_delete_connect_dialog), vol);
        gtk_widget_show_all (vol->conn_dialog);
    }
    else
    {
        sprintf (buffer, _("Failed to connect to device - %s. Try to connect again."), param);
        gtk_label_set_text (GTK_LABEL (vol->conn_label), buffer);
        vol->conn_ok = gtk_dialog_add_button (GTK_DIALOG (vol->conn_dialog), _("_OK"), 1);
        g_signal_connect (vol->conn_ok, "clicked", G_CALLBACK (volumealsa_close_connect_dialog), vol);
        gtk_widget_show (vol->conn_ok);
    }
}

static void volumealsa_close_connect_dialog (GtkButton *button, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    if (vol->conn_dialog)
    {
        gtk_widget_destroy (vol->conn_dialog);
        vol->conn_dialog = NULL;
    }
}

static gint volumealsa_delete_connect_dialog (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    if (vol->conn_dialog)
    {
        gtk_widget_destroy (vol->conn_dialog);
        vol->conn_dialog = NULL;
    }
    return TRUE;
}

/* Handler for "button-press-event" signal on main widget. */

static gboolean volumealsa_button_press_event (GtkWidget *widget, GdkEventButton *event, LXPanel *panel)
{
    VolumeALSAPlugin *vol = lxpanel_plugin_get_data (widget);

#ifdef ENABLE_NLS
    textdomain (GETTEXT_PACKAGE);
#endif
    if (vol->stopped) return TRUE;

    if (!asound_current_dev_check (vol)) volumealsa_update_display (vol);

#if 0
    if (vol->master_element == NULL && asound_get_default_card () == BLUEALSA_DEV && vol->objmanager)
    {
        /* the mixer is unattached, and there is a default Bluetooth output device - try connecting it... */
        DEBUG ("No mixer with Bluetooth device - try to reconnect");
        char *dev = asound_get_bt_device ();
        if (dev)
        {
            if (!bt_is_connected (vol, dev))
            {
                vol->bt_conname = dev;
                bt_connect_device (vol);
            }
            else g_free (dev);
        }
    }
#endif

    if (event->button == 1)
    {
        /* left-click - show or hide volume popup */
        if (!asound_has_volume_control (-1))
        {
            GtkWidget *mi;
            vol->menu_popup = gtk_menu_new ();
            mi = gtk_menu_item_new_with_label (_("No volume control on this device"));
            gtk_widget_set_sensitive (mi, FALSE);
            gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_popup), mi);
            gtk_widget_show_all (vol->menu_popup);
            gtk_menu_popup (GTK_MENU (vol->menu_popup), NULL, NULL, (GtkMenuPositionFunc) volumealsa_popup_set_position, (gpointer) vol,
                event->button, event->time);
            return TRUE;
        }

        if (vol->show_popup)
        {
            gtk_widget_hide (vol->popup_window);
            vol->show_popup = FALSE;
        }
        else
        {
            volumealsa_build_popup_window (vol->plugin);
            volumealsa_update_display (vol);

            gint x, y;
            gtk_window_set_position (GTK_WINDOW (vol->popup_window), GTK_WIN_POS_MOUSE);
            // need to draw the window in order to allow the plugin position helper to get its size
            gtk_widget_show_all (vol->popup_window);
            gtk_widget_hide (vol->popup_window);
            lxpanel_plugin_popup_set_position_helper (panel, widget, vol->popup_window, &x, &y);
            gdk_window_move (gtk_widget_get_window (vol->popup_window), x, y);
            gtk_window_present (GTK_WINDOW (vol->popup_window));
            gdk_pointer_grab (gtk_widget_get_window (vol->popup_window), TRUE, GDK_BUTTON_PRESS_MASK, NULL, NULL, GDK_CURRENT_TIME);
            g_signal_connect (G_OBJECT (vol->popup_window), "focus-out-event", G_CALLBACK (volumealsa_mouse_out), vol);
            vol->show_popup = TRUE;
        }
    }
    else if (event->button == 2)
    {
        /* middle-click - toggle mute */
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (vol->mute_check), ! gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (vol->mute_check)));
    }
    else if (event->button == 3)
    {
        /* right-click - show device list */
        volumealsa_build_device_menu (vol);
        gtk_widget_show_all (vol->menu_popup);
        gtk_menu_popup (GTK_MENU (vol->menu_popup), NULL, NULL, (GtkMenuPositionFunc) volumealsa_popup_set_position, (gpointer) vol,
            event->button, event->time);
    }
    return TRUE;
}

/*----------------------------------------------------------------------------*/
/* Device select menu                                                         */
/*----------------------------------------------------------------------------*/

static GtkWidget *volumealsa_menu_item_add (VolumeALSAPlugin *vol, GtkWidget *menu, const char *label, const char *name, gboolean selected, gboolean input, GCallback cb)
{
    GtkWidget *mi = gtk_image_menu_item_new_with_label (label);
    gtk_image_menu_item_set_always_show_image (GTK_IMAGE_MENU_ITEM (mi), TRUE);
    if (0) // selected
    {
        GtkWidget *image = gtk_image_new ();
        lxpanel_plugin_set_menu_icon (vol->panel, image, "dialog-ok-apply");
        gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM(mi), image);
        if (input)
        {
            if (vol->idev_name) g_free (vol->idev_name);
            vol->idev_name = g_strdup (label);
        }
        else
        {
            if (vol->odev_name) g_free (vol->odev_name);
            vol->odev_name = g_strdup (label);
        }
    }
    gtk_widget_set_name (mi, name);
    g_signal_connect (mi, "activate", cb, (gpointer) vol);

    // count the list first - we need indices...
    int count = 0;
    GList *l = g_list_first (gtk_container_get_children (GTK_CONTAINER (menu)));
    while (l)
    {
        count++;
        l = l->next;
    }

    // find the start point of the last section - either a separator or the beginning of the list
    l = g_list_last (gtk_container_get_children (GTK_CONTAINER (menu)));
    while (l)
    {
        if (G_OBJECT_TYPE (l->data) == GTK_TYPE_SEPARATOR_MENU_ITEM) break;
        count--;
        l = l->prev;
    }

    // if l is NULL, init to element after start; if l is non-NULL, init to element after separator
    if (!l) l = gtk_container_get_children (GTK_CONTAINER (menu));
    else l = l->next;

    // loop forward from the first element, comparing against the new label
    while (l)
    {
        if (g_strcmp0 (label, gtk_menu_item_get_label (GTK_MENU_ITEM (l->data))) < 0) break;
        count++;
        l = l->next;
    }

    gtk_menu_shell_insert (GTK_MENU_SHELL (menu), mi, count);
    return mi;
}

static void volumealsa_menu_show_default (GtkWidget *widget, gpointer data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) data;
    char *bt_sink = pa_sink_to_bluez_name (vol->pa_default_sink);

    if (!g_strcmp0 (gtk_widget_get_name (widget), vol->pa_default_sink) || !g_strcmp0 (gtk_widget_get_name (widget), bt_sink))
    {
        GtkWidget *image = gtk_image_new ();
        lxpanel_plugin_set_menu_icon (vol->panel, image, "dialog-ok-apply");
        gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (widget), image);
    }
    g_free (bt_sink);
}

static void volumealsa_build_device_menu (VolumeALSAPlugin *vol)
{
    GtkWidget *mi;
    gint devices = 0, inputs = 0, card_num, def_card, def_inp;
    gboolean ext_dev = FALSE, bt_dev = FALSE, osel = FALSE, isel = FALSE;

    def_card = asound_get_default_card ();
    def_inp = asound_get_default_input ();

    vol->menu_popup = gtk_menu_new ();

    // create input selector...
    if (vol->objmanager)
    {
        // iterate all the objects the manager knows about
        GList *objects = g_dbus_object_manager_get_objects (vol->objmanager);
        while (objects != NULL)
        {
            GDBusObject *object = (GDBusObject *) objects->data;
            const char *objpath = g_dbus_object_get_object_path (object);
            GList *interfaces = g_dbus_object_get_interfaces (object);
            while (interfaces != NULL)
            {
                // if an object has a Device1 interface, it is a Bluetooth device - add it to the list
                GDBusInterface *interface = G_DBUS_INTERFACE (interfaces->data);
                if (g_strcmp0 (g_dbus_proxy_get_interface_name (G_DBUS_PROXY (interface)), "org.bluez.Device1") == 0)
                {
                    if (bt_has_service (vol, g_dbus_proxy_get_object_path (G_DBUS_PROXY (interface)), BT_SERV_HSP))
                    {
                        GVariant *name = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Alias");
                        GVariant *icon = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Icon");
                        GVariant *paired = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Paired");
                        GVariant *trusted = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Trusted");
                        if (name && icon && paired && trusted && g_variant_get_boolean (paired) && g_variant_get_boolean (trusted))
                        {
                            // create a menu if there isn't one already
                            if (!inputs) vol->inputs = gtk_menu_new ();
                            volumealsa_menu_item_add (vol, vol->inputs, g_variant_get_string (name, NULL), objpath, asound_is_current_bt_dev (objpath, TRUE), TRUE, G_CALLBACK (volumealsa_set_bluetooth_input));
                            if (asound_is_current_bt_dev (objpath, TRUE)) isel = TRUE;
                            inputs++;
                        }
                        g_variant_unref (name);
                        g_variant_unref (icon);
                        g_variant_unref (paired);
                        g_variant_unref (trusted);
                    }
                    break;
                }
                interfaces = interfaces->next;
            }
            objects = objects->next;
        }
    }

    card_num = -1;
    while (1)
    {
        if (snd_card_next (&card_num) < 0)
        {
            g_warning ("volumealsa: Cannot enumerate devices");
            break;
        }
        if (card_num == -1) break;

        if (asound_has_input (card_num))
        {
            char *nam, *dev;
            snd_card_get_name (card_num, &nam);
            dev = g_strdup_printf ("%d", card_num);

            // either create a menu, or add a separator if there already is one
            if (!inputs) vol->inputs = gtk_menu_new ();
            else
            {
                mi = gtk_separator_menu_item_new ();
                gtk_menu_shell_append (GTK_MENU_SHELL (vol->inputs), mi);
            }
            volumealsa_menu_item_add (vol, vol->inputs, nam, dev, card_num == def_inp, TRUE, G_CALLBACK (volumealsa_set_external_input));
            if (card_num == def_inp) isel = TRUE;
            inputs++;
        }
    }

    if (inputs)
    {
        // add the input options menu item to the input menu
        mi = gtk_separator_menu_item_new ();
        gtk_menu_shell_append (GTK_MENU_SHELL (vol->inputs), mi);

        mi = gtk_image_menu_item_new_with_label (_("Input Device Settings..."));
        g_signal_connect (mi, "activate", G_CALLBACK (volumealsa_open_input_config_dialog), (gpointer) vol);
        gtk_menu_shell_append (GTK_MENU_SHELL (vol->inputs), mi);
        gtk_widget_set_sensitive (mi, isel);
    }

    // create a submenu for the outputs if there is an input submenu
    if (inputs) vol->outputs = gtk_menu_new ();
    else vol->outputs = vol->menu_popup;

    /* add internal device... */
    card_num = -1;
    while (1)
    {
        if (snd_card_next (&card_num) < 0)
        {
            g_warning ("volumealsa: Cannot enumerate devices");
            break;
        }
        if (card_num == -1) break;

        int res = asound_is_bcm_device (card_num);
        if (res == 2)
        {
            /* separate devices for each internal input */
            char *nam;
            snd_card_get_name (card_num, &nam);

            if (!g_strcmp0 (nam, "bcm2835 HDMI 1"))
                mi = volumealsa_menu_item_add (vol, vol->outputs, vol->hdmis == 1 ? _("HDMI") : vol->mon_names[0], nam, card_num == def_card, FALSE, G_CALLBACK (volumealsa_set_external_output));
            else if (!g_strcmp0 (nam, "bcm2835 HDMI 2"))
                mi = volumealsa_menu_item_add (vol, vol->outputs, vol->hdmis == 1 ? _("HDMI") : vol->mon_names[1], nam, card_num == def_card, FALSE, G_CALLBACK (volumealsa_set_external_output));
            else
                mi = volumealsa_menu_item_add (vol, vol->outputs, _("Analog"), nam, card_num == def_card, FALSE, G_CALLBACK (volumealsa_set_external_output));

            g_free (nam);
            devices++;
        }
    }

    // add Bluetooth devices...
    if (vol->objmanager)
    {
        // iterate all the objects the manager knows about
        GList *objects = g_dbus_object_manager_get_objects (vol->objmanager);
        while (objects != NULL)
        {
            GDBusObject *object = (GDBusObject *) objects->data;
            const char *objpath = g_dbus_object_get_object_path (object);
            GList *interfaces = g_dbus_object_get_interfaces (object);
            while (interfaces != NULL)
            {
                // if an object has a Device1 interface, it is a Bluetooth device - add it to the list
                GDBusInterface *interface = G_DBUS_INTERFACE (interfaces->data);
                if (g_strcmp0 (g_dbus_proxy_get_interface_name (G_DBUS_PROXY (interface)), "org.bluez.Device1") == 0)
                {
                    if (bt_has_service (vol, g_dbus_proxy_get_object_path (G_DBUS_PROXY (interface)), BT_SERV_AUDIO_SINK))
                    {
                        GVariant *name = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Alias");
                        GVariant *icon = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Icon");
                        GVariant *paired = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Paired");
                        GVariant *trusted = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Trusted");
                        if (name && icon && paired && trusted && g_variant_get_boolean (paired) && g_variant_get_boolean (trusted))
                        {
                            if (!bt_dev && devices)
                            {
                                mi = gtk_separator_menu_item_new ();
                                gtk_menu_shell_append (GTK_MENU_SHELL (vol->outputs), mi);
                            }

                            volumealsa_menu_item_add (vol, vol->outputs, g_variant_get_string (name, NULL), objpath, asound_is_current_bt_dev (objpath, FALSE), FALSE, G_CALLBACK (volumealsa_set_bluetooth_output));
                            if (asound_is_current_bt_dev (objpath, FALSE)) osel = TRUE;
                            bt_dev = TRUE;
                            devices++;
                        }
                        g_variant_unref (name);
                        g_variant_unref (icon);
                        g_variant_unref (paired);
                        g_variant_unref (trusted);
                    }
                    break;
                }
                interfaces = interfaces->next;
            }
            objects = objects->next;
        }
    }

    // add external devices...
    card_num = -1;
    while (1)
    {
        if (snd_card_next (&card_num) < 0)
        {
            g_warning ("volumealsa: Cannot enumerate devices");
            break;
        }
        if (card_num == -1) break;

        if (!asound_is_bcm_device (card_num))
        {
            char *nam;
            snd_card_get_name (card_num, &nam);

            if (!ext_dev && devices)
            {
                mi = gtk_separator_menu_item_new ();
                gtk_menu_shell_append (GTK_MENU_SHELL (vol->outputs), mi);
            }

            mi = volumealsa_menu_item_add (vol, vol->outputs, nam, nam, card_num == def_card, FALSE, G_CALLBACK (volumealsa_set_external_output));
            if (card_num == def_card) osel = TRUE;
            if (!asound_has_volume_control (card_num))
            {
                char *lab = g_strdup_printf ("<i>%s</i>", nam);
                gtk_label_set_markup (GTK_LABEL (gtk_bin_get_child (GTK_BIN (mi))), lab);
                gtk_widget_set_tooltip_text (mi, _("No volume control on this device"));
                g_free (lab);
            }

            g_free (nam);
            ext_dev = TRUE;
            devices++;
        }
    }

    /* update the menu item names, which are currently ALSA device names, to PulseAudio device names */
    pulse_update_alsa_names (vol);

    /* update the fallback sink and source */
    pulse_get_defaults (vol);

    /* select the fallbacks in the menu */
    gtk_container_foreach (GTK_CONTAINER (vol->outputs), volumealsa_menu_show_default, vol);

    if (bt_dev || ext_dev)
    {
        // add the output options menu item to the output menu
        mi = gtk_separator_menu_item_new ();
        gtk_menu_shell_append (GTK_MENU_SHELL (vol->outputs), mi);

        mi = gtk_image_menu_item_new_with_label (_("Output Device Settings..."));
        g_signal_connect (mi, "activate", G_CALLBACK (volumealsa_open_config_dialog), (gpointer) vol);
        gtk_menu_shell_append (GTK_MENU_SHELL (vol->outputs), mi);
        gtk_widget_set_sensitive (mi, osel);
    }

    if (inputs)
    {
        // insert submenus
        mi = gtk_menu_item_new_with_label (_("Audio Outputs"));
        gtk_menu_item_set_submenu (GTK_MENU_ITEM (mi), vol->outputs);
        gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_popup), mi);

        mi = gtk_separator_menu_item_new ();
        gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_popup), mi);

        mi = gtk_menu_item_new_with_label (_("Audio Inputs"));
        gtk_menu_item_set_submenu (GTK_MENU_ITEM (mi), vol->inputs);
        gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_popup), mi);
    }

    if (!devices)
    {
        mi = gtk_image_menu_item_new_with_label (_("No audio devices found"));
        gtk_widget_set_sensitive (GTK_WIDGET (mi), FALSE);
        gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_popup), mi);
    }

    // lock menu if a dialog is open
    if (vol->conn_dialog || vol->options_dlg)
    {
        GList *items = gtk_container_get_children (GTK_CONTAINER (vol->menu_popup));
        while (items)
        {
            gtk_widget_set_sensitive (GTK_WIDGET (items->data), FALSE);
            items = items->next;
        }
        g_list_free (items);
    }
}

static void volumealsa_set_external_output (GtkWidget *widget, VolumeALSAPlugin *vol)
{
    if (strstr (vol->pa_default_sink, "bluez") && g_strcmp0 (vol->pa_default_sink, vol->pa_default_source))
    {
        // if the current default sink is Bluetooth and not also the default source, disconnect it
        char *bt_name = pa_sink_to_bluez_name (vol->pa_default_sink);
        bt_disconnect_device (vol, bt_name);
        g_free (bt_name);
    }

    pulse_change_sink (vol, gtk_widget_get_name (widget));
    //asound_initialize (vol);
    volumealsa_update_display (vol);
}

static void volumealsa_set_external_input (GtkWidget *widget, VolumeALSAPlugin *vol)
{
    int dev;

    if (sscanf (widget->name, "%d", &dev) == 1)
    {
        /* if there is a Bluetooth device in use, get its name so we can disconnect it */
        char *device = asound_get_bt_input ();

        asound_set_default_input (dev);

        /* disconnect old Bluetooth device if it is not also output */
        if (device)
        {
            char *dev2 = asound_get_bt_device ();
            if (g_strcmp0 (device, dev2)) bt_disconnect_device (vol, device);
            if (dev2) g_free (dev2);
            g_free (device);
        }
    }
}

static void volumealsa_set_internal_output (GtkWidget *widget, VolumeALSAPlugin *vol)
{
    /* if there is a Bluetooth device in use, get its name so we can disconnect it */
    char *device = asound_get_bt_device ();

    /* check that the BCM device is default... */
    int dev = asound_get_bcm_device_num ();
    if (dev != asound_get_default_card ()) asound_set_default_card (dev);

    /* set the output channel on the BCM device */
    vsystem ("amixer -q cset numid=3 %s 2>/dev/null", widget->name);

    asound_initialize (vol);
    volumealsa_update_display (vol);

    /* disconnect old Bluetooth device if it is not also input */
    if (device)
    {
        char *dev2 = asound_get_bt_input ();
        if (g_strcmp0 (device, dev2)) bt_disconnect_device (vol, device);
        if (dev2) g_free (dev2);
        g_free (device);
    }
}

static void volumealsa_set_bluetooth_output (GtkWidget *widget, VolumeALSAPlugin *vol)
{
    //asound_deinitialize (vol);
    volumealsa_update_display (vol);

    char *odevice = pa_sink_to_bluez_name (vol->pa_default_sink);

    // is this device already connected and attached - might want to force reconnect here?
    if (!g_strcmp0 (widget->name, odevice))
    {
        DEBUG ("Reconnect device %s", widget->name);
        // store the name of the BlueZ device to connect to
        if (vol->bt_conname) g_free (vol->bt_conname);
        vol->bt_conname = g_strdup (widget->name);
        vol->bt_input = FALSE;

        // show the connection dialog
        volumealsa_show_connect_dialog (vol, FALSE, gtk_menu_item_get_label (GTK_MENU_ITEM (widget)));

        // disconnect the device prior to reconnect
        bt_disconnect_device (vol, odevice);

        g_free (odevice);
        return;
    }

    char *idevice = pa_source_to_bluez_name (vol->pa_default_source);

    // check to see if this device is already connected
    if (!g_strcmp0 (widget->name, idevice))
    {
        DEBUG ("Device %s is already connected", widget->name);
        char *paname = bluez_to_pa_sink_name (vol->bt_conname);
        pulse_change_sink (vol, paname);
        g_free (paname);
        //asound_initialize (vol);
        volumealsa_update_display (vol);

        /* disconnect old Bluetooth output device */
        if (odevice) bt_disconnect_device (vol, odevice);
    }
    else
    {
        DEBUG ("Need to connect device %s", widget->name);
        // store the name of the BlueZ device to connect to
        if (vol->bt_conname) g_free (vol->bt_conname);
        vol->bt_conname = g_strdup (widget->name);
        vol->bt_input = FALSE;

        // show the connection dialog
        volumealsa_show_connect_dialog (vol, FALSE, gtk_menu_item_get_label (GTK_MENU_ITEM (widget)));

        // disconnect the current output device unless it is also the input device; otherwise just connect the new device
        if (odevice && g_strcmp0 (idevice, odevice)) bt_disconnect_device (vol, odevice);
        else bt_connect_device (vol);
    }

    if (idevice) g_free (idevice);
    if (odevice) g_free (odevice);
}

static void volumealsa_set_bluetooth_input (GtkWidget *widget, VolumeALSAPlugin *vol)
{
    char *idevice = asound_get_bt_input ();

    // is this device already connected and attached - might want to force reconnect here?
    if (!g_strcmp0 (widget->name, idevice))
    {
        DEBUG ("Reconnect device %s", widget->name);
        // store the name of the BlueZ device to connect to
        if (vol->bt_conname) g_free (vol->bt_conname);
        vol->bt_conname = g_strdup (widget->name);
        vol->bt_input = TRUE;

        // show the connection dialog
        volumealsa_show_connect_dialog (vol, FALSE, gtk_menu_item_get_label (GTK_MENU_ITEM (widget)));

        // disconnect the current input device unless it is also the output device; otherwise just connect the new device
        bt_disconnect_device (vol, idevice);

        g_free (idevice);
        return;
    }

    char *odevice = asound_get_bt_device ();

    // check to see if this device is already connected
    if (!g_strcmp0 (widget->name, odevice))
    {
        DEBUG ("Device %s is already connected\n", widget->name);
        asound_set_bt_input (widget->name);

        /* disconnect old Bluetooth input device */
        if (idevice) bt_disconnect_device (vol, idevice);
    }
    else
    {
        DEBUG ("Need to connect device %s", widget->name);
        // store the name of the BlueZ device to connect to
        if (vol->bt_conname) g_free (vol->bt_conname);
        vol->bt_conname = g_strdup (widget->name);
        vol->bt_input = TRUE;

        // show the connection dialog
        volumealsa_show_connect_dialog (vol, FALSE, gtk_menu_item_get_label (GTK_MENU_ITEM (widget)));

        // disconnect the current input device unless it is also the output device; otherwise just connect the new device
        if (idevice && g_strcmp0 (idevice, odevice)) bt_disconnect_device (vol, idevice);
        else bt_connect_device (vol);
    }

    if (idevice) g_free (idevice);
    if (odevice) g_free (odevice);
}


/*----------------------------------------------------------------------------*/
/* Volume scale popup window                                                  */
/*----------------------------------------------------------------------------*/

/* Build the window that appears when the top level widget is clicked. */
static void volumealsa_build_popup_window (GtkWidget *p)
{
    VolumeALSAPlugin *vol = lxpanel_plugin_get_data (p);

    if (vol->popup_window) gtk_widget_destroy (vol->popup_window);

    /* Create a new window. */
    vol->popup_window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name (vol->popup_window, "volals");
    gtk_window_set_decorated (GTK_WINDOW (vol->popup_window), FALSE);
    gtk_container_set_border_width (GTK_CONTAINER (vol->popup_window), 5);
    gtk_window_set_skip_taskbar_hint (GTK_WINDOW (vol->popup_window), TRUE);
    gtk_window_set_skip_pager_hint (GTK_WINDOW (vol->popup_window), TRUE);
    gtk_window_set_type_hint (GTK_WINDOW (vol->popup_window), GDK_WINDOW_TYPE_HINT_DIALOG);

    /* Create a scrolled window as the child of the top level window. */
    GtkWidget *scrolledwindow = gtk_scrolled_window_new (NULL, NULL);
    gtk_widget_set_name (scrolledwindow, "whitewd");
    gtk_container_set_border_width (GTK_CONTAINER (scrolledwindow), 0);
    gtk_widget_show (scrolledwindow);
    gtk_container_add (GTK_CONTAINER (vol->popup_window), scrolledwindow);
    gtk_widget_set_can_focus (scrolledwindow, FALSE);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolledwindow), GTK_POLICY_NEVER, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolledwindow), GTK_SHADOW_NONE);

    /* Create a viewport as the child of the scrolled window. */
    GtkWidget *viewport = gtk_viewport_new (NULL, NULL);
    gtk_container_add (GTK_CONTAINER (scrolledwindow), viewport);
    gtk_viewport_set_shadow_type (GTK_VIEWPORT (viewport), GTK_SHADOW_NONE);
    gtk_widget_show (viewport);

    gtk_container_set_border_width (GTK_CONTAINER (vol->popup_window), 0);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolledwindow), GTK_SHADOW_IN);
    /* Create a vertical box as the child of the viewport. */
    GtkWidget *box = gtk_vbox_new (FALSE, 0);
    gtk_container_add (GTK_CONTAINER (viewport), box);

    /* Create a vertical scale as the child of the vertical box. */
    vol->volume_scale = gtk_vscale_new (GTK_ADJUSTMENT (gtk_adjustment_new (100, 0, 100, 0, 0, 0)));
    gtk_widget_set_name (vol->volume_scale, "volscale");
    g_object_set (vol->volume_scale, "height-request", 120, NULL);
    gtk_scale_set_draw_value (GTK_SCALE (vol->volume_scale), FALSE);
    gtk_range_set_inverted (GTK_RANGE (vol->volume_scale), TRUE);
    gtk_box_pack_start (GTK_BOX (box), vol->volume_scale, TRUE, TRUE, 0);
    gtk_widget_set_can_focus (vol->volume_scale, FALSE);

    /* Value-changed and scroll-event signals. */
    vol->volume_scale_handler = g_signal_connect (vol->volume_scale, "value-changed", G_CALLBACK (volumealsa_popup_scale_changed), vol);
    g_signal_connect (vol->volume_scale, "scroll-event", G_CALLBACK (volumealsa_popup_scale_scrolled), vol);

    /* Create a check button as the child of the vertical box. */
    vol->mute_check = gtk_check_button_new_with_label (_("Mute"));
    gtk_box_pack_end (GTK_BOX (box), vol->mute_check, FALSE, FALSE, 0);
    vol->mute_check_handler = g_signal_connect (vol->mute_check, "toggled", G_CALLBACK (volumealsa_popup_mute_toggled), vol);
    gtk_widget_set_can_focus (vol->mute_check, FALSE);
}

/* Handler for "value_changed" signal on popup window vertical scale. */
static void volumealsa_popup_scale_changed (GtkRange *range, VolumeALSAPlugin *vol)
{
    /* Reflect the value of the control to the sound system. */
    if (!pulse_get_mute (vol))
        pulse_set_volume (vol, gtk_range_get_value (range));

    /* Redraw the controls. */
    volumealsa_update_display (vol);
}

/* Handler for "scroll-event" signal on popup window vertical scale. */
static void volumealsa_popup_scale_scrolled (GtkScale *scale, GdkEventScroll *evt, VolumeALSAPlugin *vol)
{
    /* Get the state of the vertical scale. */
    gdouble val = gtk_range_get_value (GTK_RANGE (vol->volume_scale));

    /* Dispatch on scroll direction to update the value. */
    if ((evt->direction == GDK_SCROLL_UP) || (evt->direction == GDK_SCROLL_LEFT))
        val += 2;
    else
        val -= 2;

    /* Reset the state of the vertical scale.  This provokes a "value_changed" event. */
    gtk_range_set_value (GTK_RANGE (vol->volume_scale), CLAMP((int) val, 0, 100));
}

/* Handler for "toggled" signal on popup window mute checkbox. */
static void volumealsa_popup_mute_toggled (GtkWidget *widget, VolumeALSAPlugin *vol)
{
    /* Reflect the mute toggle to the sound system. */
    pulse_set_mute (vol, gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget)));

    /* Redraw the controls. */
    volumealsa_update_display (vol);
}

static void volumealsa_popup_set_position (GtkWidget *menu, gint *px, gint *py, gboolean *push_in, gpointer data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) data;

    /* Determine the coordinates. */
    lxpanel_plugin_popup_set_position_helper (vol->panel, vol->plugin, menu, px, py);
    *push_in = TRUE;
}

/* Handler for "focus-out" signal on popup window. */
static gboolean volumealsa_mouse_out (GtkWidget *widget, GdkEventButton *event, VolumeALSAPlugin *vol)
{
    /* Hide the widget. */
    gtk_widget_hide (vol->popup_window);
    vol->show_popup = FALSE;
    gdk_pointer_ungrab (GDK_CURRENT_TIME);
    return FALSE;
}

/*----------------------------------------------------------------------------*/
/* Options dialog                                                             */
/*----------------------------------------------------------------------------*/

static void show_options (VolumeALSAPlugin *vol, snd_mixer_t *mixer, gboolean input, char *devname)
{
    snd_mixer_elem_t *elem;
    GtkWidget *slid, *box, *btn, *scr, *wid;
    GtkObject *adj;
    guint cols;
    int swval;
    char *lbl;

    vol->options_play = NULL;
    vol->options_capt = NULL;
    vol->options_set = NULL;

    // loop through elements, adding controls to relevant tabs
    for (elem = snd_mixer_first_elem (mixer); elem != NULL; elem = snd_mixer_elem_next (elem))
    {
#if 0
        printf ("Element %s %d %d %d %d %d\n",
            snd_mixer_selem_get_name (elem),
            snd_mixer_selem_is_active (elem),
            snd_mixer_selem_has_playback_volume (elem),
            snd_mixer_selem_has_playback_switch (elem),
            snd_mixer_selem_has_capture_volume (elem),
            snd_mixer_selem_has_capture_switch (elem));
#endif
        if (snd_mixer_selem_has_playback_volume (elem))
        {
            if (!vol->options_play) vol->options_play = gtk_hbox_new (FALSE, 5);
            box = gtk_vbox_new (FALSE, 5);
            gtk_box_pack_start (GTK_BOX (vol->options_play), box, FALSE, FALSE, 5);
            gtk_box_pack_start (GTK_BOX (box), gtk_label_new (snd_mixer_selem_get_name (elem)), FALSE, FALSE, 5);
            if (snd_mixer_selem_has_playback_switch (elem))
            {
                btn = gtk_check_button_new_with_label (_("Enable"));
                gtk_widget_set_name (btn, snd_mixer_selem_get_name (elem));
                snd_mixer_selem_get_playback_switch (elem, SND_MIXER_SCHN_MONO, &swval);
                gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (btn), swval);
                gtk_box_pack_end (GTK_BOX (box), btn, FALSE, FALSE, 5);
                g_signal_connect (btn, "toggled", G_CALLBACK (playback_switch_toggled_event), elem);
            }
            adj = gtk_adjustment_new (50.0, 0.0, 100.0, 1.0, 0.0, 0.0);
            slid = gtk_vscale_new (GTK_ADJUSTMENT (adj));
            gtk_widget_set_name (slid, snd_mixer_selem_get_name (elem));
            gtk_range_set_inverted (GTK_RANGE (slid), TRUE);
            gtk_range_set_update_policy (GTK_RANGE (slid), GTK_UPDATE_DISCONTINUOUS);
            gtk_range_set_value (GTK_RANGE (slid), get_normalized_volume (elem, FALSE));
            gtk_widget_set_size_request (slid, 80, 150);
            gtk_scale_set_draw_value (GTK_SCALE (slid), FALSE);
            gtk_box_pack_start (GTK_BOX (box), slid, FALSE, FALSE, 0);
            g_signal_connect (slid, "value-changed", G_CALLBACK (playback_range_change_event), elem);
        }
        else if (snd_mixer_selem_has_playback_switch (elem))
        {
            if (!vol->options_set) vol->options_set = gtk_vbox_new (FALSE, 5);
            box = gtk_hbox_new (FALSE, 5);
            lbl = g_strdup_printf (_("%s (Playback)"), snd_mixer_selem_get_name (elem));
            gtk_box_pack_start (GTK_BOX (box), gtk_label_new (lbl), FALSE, FALSE, 5);
            g_free (lbl);
            btn = gtk_check_button_new ();
            gtk_box_pack_end (GTK_BOX (box), btn, FALSE, FALSE, 5);
            gtk_widget_set_name (btn, snd_mixer_selem_get_name (elem));
            snd_mixer_selem_get_playback_switch (elem, SND_MIXER_SCHN_MONO, &swval);
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (btn), swval);
            gtk_box_pack_start (GTK_BOX (vol->options_set), box, FALSE, FALSE, 5);
            g_signal_connect (btn, "toggled", G_CALLBACK (playback_switch_toggled_event), elem);
        }

        if (snd_mixer_selem_has_capture_volume (elem))
        {
            if (!vol->options_capt) vol->options_capt = gtk_hbox_new (FALSE, 5);
            box = gtk_vbox_new (FALSE, 5);
            gtk_box_pack_start (GTK_BOX (vol->options_capt), box, FALSE, FALSE, 5);
            gtk_box_pack_start (GTK_BOX (box), gtk_label_new (snd_mixer_selem_get_name (elem)), FALSE, FALSE, 5);
            if (snd_mixer_selem_has_capture_switch (elem))
            {
                btn = gtk_check_button_new_with_label (_("Enable"));
                gtk_widget_set_name (btn, snd_mixer_selem_get_name (elem));
                snd_mixer_selem_get_capture_switch (elem, SND_MIXER_SCHN_MONO, &swval);
                gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (btn), swval);
                gtk_box_pack_end (GTK_BOX (box), btn, FALSE, FALSE, 5);
                g_signal_connect (btn, "toggled", G_CALLBACK (capture_switch_toggled_event), elem);
            }
            adj = gtk_adjustment_new (50.0, 0.0, 100.0, 1.0, 0.0, 0.0);
            slid = gtk_vscale_new (GTK_ADJUSTMENT (adj));
            gtk_widget_set_name (slid, snd_mixer_selem_get_name (elem));
            gtk_range_set_inverted (GTK_RANGE (slid), TRUE);
            gtk_range_set_update_policy (GTK_RANGE (slid), GTK_UPDATE_DISCONTINUOUS);
            gtk_range_set_value (GTK_RANGE (slid), get_normalized_volume (elem, TRUE));
            gtk_widget_set_size_request (slid, 80, 150);
            gtk_scale_set_draw_value (GTK_SCALE (slid), FALSE);
            gtk_box_pack_start (GTK_BOX (box), slid, FALSE, FALSE, 0);
            g_signal_connect (slid, "value-changed", G_CALLBACK (capture_range_change_event), elem);
        }
        else if (snd_mixer_selem_has_capture_switch (elem))
        {
            if (!vol->options_set) vol->options_set = gtk_vbox_new (FALSE, 5);
            box = gtk_hbox_new (FALSE, 5);
            lbl = g_strdup_printf (_("%s (Capture)"), snd_mixer_selem_get_name (elem));
            gtk_box_pack_start (GTK_BOX (box), gtk_label_new (lbl), FALSE, FALSE, 5);
            g_free (lbl);
            btn = gtk_check_button_new ();
            gtk_box_pack_end (GTK_BOX (box), btn, FALSE, FALSE, 5);
            gtk_widget_set_name (btn, snd_mixer_selem_get_name (elem));
            snd_mixer_selem_get_capture_switch (elem, SND_MIXER_SCHN_MONO, &swval);
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (btn), swval);
            gtk_box_pack_start (GTK_BOX (vol->options_set), box, FALSE, FALSE, 5);
            g_signal_connect (btn, "toggled", G_CALLBACK (capture_switch_toggled_event), elem);
        }

        if (snd_mixer_selem_is_enumerated (elem))
        {
            if (!vol->options_set) vol->options_set = gtk_vbox_new (FALSE, 5);
            box = gtk_hbox_new (FALSE, 5);
            if (snd_mixer_selem_is_enum_playback (elem) && !snd_mixer_selem_is_enum_capture (elem))
                lbl = g_strdup_printf (_("%s (Playback)"), snd_mixer_selem_get_name (elem));
            else if (snd_mixer_selem_is_enum_capture (elem) && !snd_mixer_selem_is_enum_playback (elem))
                lbl = g_strdup_printf (_("%s (Capture)"), snd_mixer_selem_get_name (elem));
            else
                lbl = g_strdup_printf ("%s", snd_mixer_selem_get_name (elem));
            gtk_box_pack_start (GTK_BOX (box), gtk_label_new (lbl), FALSE, FALSE, 5);
            g_free (lbl);
            btn = gtk_combo_box_text_new ();
            gtk_box_pack_end (GTK_BOX (box), btn, FALSE, FALSE, 5);
            gtk_widget_set_name (btn, snd_mixer_selem_get_name (elem));
            int items = snd_mixer_selem_get_enum_items (elem);
            for (int i = 0; i < items; i++)
            {
                char buffer[128];
                snd_mixer_selem_get_enum_item_name (elem, i, 128, buffer);
                gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (btn), buffer);
            }
            int sel;
            snd_mixer_selem_get_enum_item (elem, SND_MIXER_SCHN_MONO, &sel);
            gtk_combo_box_set_active (GTK_COMBO_BOX (btn), sel);
            gtk_box_pack_start (GTK_BOX (vol->options_set), box, FALSE, FALSE, 5);
            g_signal_connect (btn, "changed", G_CALLBACK (enum_changed_event), elem);
        }
    }

    // create the window itself
    vol->options_dlg = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title (GTK_WINDOW (vol->options_dlg), input ? _("Input Device Options") : _("Output Device Options"));
    gtk_window_set_position (GTK_WINDOW (vol->options_dlg), GTK_WIN_POS_CENTER);
    gtk_window_set_default_size (GTK_WINDOW (vol->options_dlg), 400, 300);
    gtk_container_set_border_width (GTK_CONTAINER (vol->options_dlg), 10);
    gtk_window_set_icon_name (GTK_WINDOW (vol->options_dlg), "multimedia-volume-control");
    g_signal_connect (vol->options_dlg, "delete-event", G_CALLBACK (options_wd_close_handler), vol);

    box = gtk_vbox_new (FALSE, 5);
    gtk_container_add (GTK_CONTAINER (vol->options_dlg), box);

    char *dev = g_strdup_printf (_("%s Device : %s"), input ? _("Input") : _("Output"), devname);
    wid = gtk_label_new (dev);
    gtk_misc_set_alignment (GTK_MISC (wid), 0.0, 0.5);
    gtk_box_pack_start (GTK_BOX (box), wid, FALSE, FALSE, 5);
    g_free (dev);

    if (!vol->options_play && !vol->options_capt && !vol->options_set)
    {
        gtk_box_pack_start (GTK_BOX (box), gtk_label_new (_("No controls available on this device")), TRUE, TRUE, 0);
    }
    else
    {
        wid = gtk_notebook_new ();
        if (vol->options_play)
        {
            scr = gtk_scrolled_window_new (NULL, NULL);
            gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scr), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
            gtk_scrolled_window_add_with_viewport (GTK_SCROLLED_WINDOW (scr), vol->options_play);
            gtk_notebook_append_page (GTK_NOTEBOOK (wid), scr, gtk_label_new (_("Playback")));
        }
        if (vol->options_capt)
        {
            scr = gtk_scrolled_window_new (NULL, NULL);
            gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scr), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
            gtk_scrolled_window_add_with_viewport (GTK_SCROLLED_WINDOW (scr), vol->options_capt);
            gtk_notebook_append_page (GTK_NOTEBOOK (wid), scr, gtk_label_new (_("Capture")));
        }
        if (vol->options_set)
        {
            scr = gtk_scrolled_window_new (NULL, NULL);
            gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scr), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
            gtk_scrolled_window_add_with_viewport (GTK_SCROLLED_WINDOW (scr), vol->options_set);
            gtk_notebook_append_page (GTK_NOTEBOOK (wid), scr, gtk_label_new (_("Options")));
        }
        gtk_box_pack_start (GTK_BOX (box), wid, FALSE, FALSE, 5);
    }

    wid = gtk_hbutton_box_new ();
    gtk_button_box_set_layout (GTK_BUTTON_BOX (wid), GTK_BUTTONBOX_END);
    gtk_box_pack_start (GTK_BOX (box), wid, FALSE, FALSE, 5);

    btn = gtk_button_new_from_stock (GTK_STOCK_OK);
    g_signal_connect (btn, "clicked", G_CALLBACK (options_ok_handler), vol);
    gtk_box_pack_end (GTK_BOX (wid), btn, FALSE, FALSE, 5);

    gtk_widget_show_all (vol->options_dlg);
}

static void show_output_options (VolumeALSAPlugin *vol)
{
    if (vol->mixers[OUTPUT_MIXER].mixer)
        show_options (vol, vol->mixers[OUTPUT_MIXER].mixer, FALSE, vol->odev_name);
}

static void show_input_options (VolumeALSAPlugin *vol)
{
    if (asound_get_default_input () == asound_get_default_card ())
    {
        DEBUG ("Input and output device the same - use output mixer for dialog");
        if (vol->mixers[OUTPUT_MIXER].mixer)
            show_options (vol, vol->mixers[OUTPUT_MIXER].mixer, TRUE, vol->idev_name);
    }
    else if (asound_mixer_initialize (vol, INPUT_MIXER))
    {
        DEBUG ("Created new mixer for input dialog");
        show_options (vol, vol->mixers[INPUT_MIXER].mixer, TRUE, vol->idev_name);
    }
}

static void update_options (VolumeALSAPlugin *vol)
{
    snd_mixer_elem_t *elem;
    guint pcol = 0, ccol = 0;
    GtkWidget *wid;
    int swval, i;

    i = vol->mixers[INPUT_MIXER].mixer ? 1 : 0;
    for (elem = snd_mixer_first_elem (vol->mixers[i].mixer); elem != NULL; elem = snd_mixer_elem_next (elem))
    {
        if (snd_mixer_selem_has_playback_volume (elem))
        {
            wid = find_box_child (vol->options_play, GTK_TYPE_VSCALE, snd_mixer_selem_get_name (elem));
            if (wid)
            {
                g_signal_handlers_block_by_func (wid, playback_range_change_event, elem);
                gtk_range_set_value (GTK_RANGE (wid), get_normalized_volume (elem, FALSE));
                g_signal_handlers_unblock_by_func (wid, playback_range_change_event, elem);
            }
        }
        if (snd_mixer_selem_has_playback_switch (elem))
        {
            wid = find_box_child (vol->options_play, GTK_TYPE_CHECK_BUTTON, snd_mixer_selem_get_name (elem));
            if (!wid) wid = find_box_child (vol->options_set, GTK_TYPE_CHECK_BUTTON, snd_mixer_selem_get_name (elem));
            if (wid)
            {
                snd_mixer_selem_get_playback_switch (elem, SND_MIXER_SCHN_MONO, &swval);
                g_signal_handlers_block_by_func (wid, playback_switch_toggled_event, elem);
                gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (wid), swval);
                g_signal_handlers_unblock_by_func (wid, playback_switch_toggled_event, elem);
            }
        }
        if (snd_mixer_selem_has_capture_volume (elem))
        {
            wid = find_box_child (vol->options_capt, GTK_TYPE_VSCALE, snd_mixer_selem_get_name (elem));
            {
                g_signal_handlers_block_by_func (wid, capture_range_change_event, elem);
                gtk_range_set_value (GTK_RANGE (wid), get_normalized_volume (elem, TRUE));
                g_signal_handlers_unblock_by_func (wid, capture_range_change_event, elem);
            }
        }
        if (snd_mixer_selem_has_capture_switch (elem))
        {
            wid = find_box_child (vol->options_capt, GTK_TYPE_CHECK_BUTTON, snd_mixer_selem_get_name (elem));
            if (!wid) wid = find_box_child (vol->options_set, GTK_TYPE_CHECK_BUTTON, snd_mixer_selem_get_name (elem));
            if (wid)
            {
                snd_mixer_selem_get_capture_switch (elem, SND_MIXER_SCHN_MONO, &swval);
                g_signal_handlers_block_by_func (wid, capture_switch_toggled_event, elem);
                gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (wid), swval);
                g_signal_handlers_unblock_by_func (wid, capture_switch_toggled_event, elem);
            }
        }
        if (snd_mixer_selem_is_enumerated (elem))
        {
            wid = find_box_child (vol->options_set, GTK_TYPE_COMBO_BOX_TEXT, snd_mixer_selem_get_name (elem));
            if (wid)
            {
                snd_mixer_selem_get_enum_item (elem, SND_MIXER_SCHN_MONO, &swval);
                g_signal_handlers_block_by_func (wid, enum_changed_event, elem);
                gtk_combo_box_set_active (GTK_COMBO_BOX (wid), swval);
                g_signal_handlers_unblock_by_func (wid, enum_changed_event, elem);
            }
        }
    }
}

static void close_options (VolumeALSAPlugin *vol)
{
    if (vol->mixers[INPUT_MIXER].mixer)
    {
        DEBUG ("Deinitializing input mixer");
        asound_mixer_deinitialize (vol, INPUT_MIXER);
    }

    gtk_widget_destroy (vol->options_dlg);
    vol->options_dlg = NULL;
}

static void options_ok_handler (GtkButton *button, gpointer *user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;

    close_options (vol);
}

static gboolean options_wd_close_handler (GtkWidget *wid, GdkEvent *event, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;

    close_options (vol);
    return TRUE;
}

static void playback_range_change_event (GtkRange *range, gpointer user_data)
{
    snd_mixer_elem_t *elem = (snd_mixer_elem_t *) user_data;

    int volume = (int) gtk_range_get_value (range);
    set_normalized_volume (elem, volume, volume - get_normalized_volume (elem, FALSE), FALSE);

    g_signal_handlers_block_by_func (range, playback_range_change_event, elem);
    gtk_range_set_value (range, get_normalized_volume (elem, FALSE));
    g_signal_handlers_unblock_by_func (range, playback_range_change_event, elem);
}

static void capture_range_change_event (GtkRange *range, gpointer user_data)
{
    snd_mixer_elem_t *elem = (snd_mixer_elem_t *) user_data;

    int volume = (int) gtk_range_get_value (range);
    set_normalized_volume (elem, volume, volume - get_normalized_volume (elem, TRUE), TRUE);

    g_signal_handlers_block_by_func (range, capture_range_change_event, elem);
    gtk_range_set_value (range, get_normalized_volume (elem, TRUE));
    g_signal_handlers_unblock_by_func (range, capture_range_change_event, elem);
}

static void playback_switch_toggled_event (GtkToggleButton *togglebutton, gpointer user_data)
{
    snd_mixer_elem_t *elem = (snd_mixer_elem_t *) user_data;

    snd_mixer_selem_set_playback_switch_all (elem, gtk_toggle_button_get_active (togglebutton));
}

static void capture_switch_toggled_event (GtkToggleButton *togglebutton, gpointer user_data)
{
    snd_mixer_elem_t *elem = (snd_mixer_elem_t *) user_data;

    snd_mixer_selem_set_capture_switch_all (elem, gtk_toggle_button_get_active (togglebutton));
}

static void enum_changed_event (GtkComboBox *combo, gpointer *user_data)
{
    snd_mixer_elem_t *elem = (snd_mixer_elem_t *) user_data;

    snd_mixer_selem_set_enum_item (elem, SND_MIXER_SCHN_MONO, gtk_combo_box_get_active (combo));
}

static GtkWidget *find_box_child (GtkWidget *container, gint type, const char *name)
{
    GList *l, *list = gtk_container_get_children (GTK_CONTAINER (container));
    for (l = list; l; l = l->next)
    {
        GList *m, *mist = gtk_container_get_children (GTK_CONTAINER (l->data));
        for (m = mist; m; m = m->next)
        {
            if (G_OBJECT_TYPE (m->data) == type && !g_strcmp0 (name, gtk_widget_get_name (m->data)))
                return m->data;
            if (G_OBJECT_TYPE (m->data) == GTK_TYPE_HBUTTON_BOX)
            {
                GList *n, *nist = gtk_container_get_children (GTK_CONTAINER (m->data));
                for (n = nist; n; n = n->next)
                {
                    if (G_OBJECT_TYPE (n->data) == type && !g_strcmp0 (name, gtk_widget_get_name (n->data)))
                        return n->data;
                }
            }
        }
    }
    return NULL;
}

/*----------------------------------------------------------------------------*/
/* PulseAudio controller                                                      */
/*----------------------------------------------------------------------------*/

/* Initialisation / teardown
 * -------------------------
 * The PulseAudio controller runs asynchronously in a new thread.
 * The initial functions are to set up and tear down the controller,
 * which is subsequently accessed by its context, which is created
 * during the init function and a pointer to which is stored in the
 * plugin global data structure
 */

static void pa_cb_state (pa_context *pacontext, void *userdata)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) userdata;

    if (pacontext == NULL)
    {
        vol->pa_state = PA_CONTEXT_FAILED;
        pa_threaded_mainloop_signal (vol->pa_mainloop, 0);
        return;
    }

    vol->pa_state = pa_context_get_state (pacontext);
    pa_threaded_mainloop_signal (vol->pa_mainloop, 0);
}

static void pulse_init (VolumeALSAPlugin *vol)
{
    pa_proplist *paprop;
    pa_mainloop_api *paapi;

    vol->pa_context = NULL;
    vol->pa_mainloop = pa_threaded_mainloop_new ();
    pa_threaded_mainloop_start (vol->pa_mainloop);

    pa_threaded_mainloop_lock (vol->pa_mainloop);
    paapi = pa_threaded_mainloop_get_api (vol->pa_mainloop);

    paprop = pa_proplist_new ();
    pa_proplist_sets (paprop, PA_PROP_APPLICATION_NAME, "unknown");
    pa_proplist_sets (paprop, PA_PROP_MEDIA_ROLE, "music");
    vol->pa_context = pa_context_new_with_proplist (paapi, "unknown", paprop);
    pa_proplist_free (paprop);

    if (vol->pa_context == NULL)
    {
        pa_threaded_mainloop_unlock (vol->pa_mainloop);
        pa_error_handler (vol, "create context");
        return;
    }

    vol->pa_state = PA_CONTEXT_UNCONNECTED;

    pa_context_set_state_callback (vol->pa_context, &pa_cb_state, vol);
    pa_context_connect (vol->pa_context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL);

    while (vol->pa_state != PA_CONTEXT_READY && vol->pa_state != PA_CONTEXT_FAILED)
    {
        pa_threaded_mainloop_wait (vol->pa_mainloop);
    }

    pa_threaded_mainloop_unlock (vol->pa_mainloop);

    if (vol->pa_state != PA_CONTEXT_READY)
    {
        pa_error_handler (vol, "init context");
        return;
    }

    vol->pa_default_sink = NULL;
    vol->pa_default_source = NULL;
}

static void pulse_disconnect (VolumeALSAPlugin *vol)
{
    if (vol->pa_context != NULL)
    {
        pa_threaded_mainloop_lock (vol->pa_mainloop);
        pa_context_disconnect (vol->pa_context);
        pa_context_unref (vol->pa_context);
        vol->pa_context = NULL;
        pa_threaded_mainloop_unlock (vol->pa_mainloop);
    }
}

static void pulse_close (VolumeALSAPlugin *vol)
{
    if (vol->pa_mainloop != NULL)
    {
        pa_threaded_mainloop_stop (vol->pa_mainloop);
        pa_threaded_mainloop_free (vol->pa_mainloop);
    }
}

static void pa_error_handler (VolumeALSAPlugin *vol, char *name)
{
    int rc;

    if (vol->pa_context != NULL)
    {
        rc = pa_context_errno (vol->pa_context);
        g_warning ("%s: err:%d %s\n", name, rc, pa_strerror (rc));
        pulse_disconnect (vol);
    }
    pulse_close (vol);
}

static void pa_cb_generic_success (pa_context *context, int success, void *userdata)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) userdata;

    pa_threaded_mainloop_signal (vol->pa_mainloop, 0);
}

/* Generic operations
 * ------------------
 *
 * Access to the controller is via asynchronous functions which request
 * information or settings. Because the plugin itself runs synchronously,
 * all controller access functions are wrapped in code which waits for
 * them to complete. Returned values, where appropriate, are written to
 * the plugin global data structure via callbacks from the async functions.
 * The macros below are the boilerplate around each async call.
 */

#define START_PA_OPERATION \
    pa_operation *op; \
    pa_threaded_mainloop_lock (vol->pa_mainloop);

#define END_PA_OPERATION(name) \
    if (!op) \
    { \
        pa_threaded_mainloop_unlock (vol->pa_mainloop); \
        pa_error_handler (vol, name); \
        return 0; \
    } \
    while (pa_operation_get_state (op) == PA_OPERATION_RUNNING) \
    { \
        pa_threaded_mainloop_wait (vol->pa_mainloop); \
    } \
    pa_operation_unref (op); \
    pa_threaded_mainloop_unlock (vol->pa_mainloop); \
    return 1;

/* Get defaults
 * ------------
 *
 * Updates the names of the current default sink and source in the plugin
 * global data structure.
 */

static void pa_cb_get_server_info (pa_context *context, const pa_server_info *i, void *userdata)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) userdata;

    if (vol->pa_default_sink) g_free (vol->pa_default_sink);
    vol->pa_default_sink = g_strdup (i->default_sink_name);

    if (vol->pa_default_source) g_free (vol->pa_default_source);
    vol->pa_default_source = g_strdup (i->default_source_name);

    pa_threaded_mainloop_signal (vol->pa_mainloop, 0);
}

static int pulse_get_defaults (VolumeALSAPlugin *vol)
{
    START_PA_OPERATION
    op = pa_context_get_server_info (vol->pa_context, &pa_cb_get_server_info, vol);
    END_PA_OPERATION ("get_server_info")
}

/* Updating ALSA names in menu
 * ---------------------------
 *
 * The device select menu looks at ALSA and Bluez to find audio devices,
 * which are stored with their ALSA or Bluez names. After discovery, the
 * update_names function is called which invokes the get_sink_info_list
 * operation, which causes a callback for each PulseAudio sink. In this
 * callback, the ALSA name of that sink is looked for in the menu, and is
 * replaced with the PulseAudio name.
 */

static void replace_alsa_on_match (GtkWidget *widget, gpointer data)
{
    pa_sink_info *i = (pa_sink_info *) data;
    const char *alsaname = pa_proplist_gets (i->proplist, "alsa.card_name");

    if (!strcmp (alsaname, gtk_widget_get_name (widget)))
        gtk_widget_set_name (widget, i->name);
}

static void pa_cb_get_sink_info_list (pa_context *context, const pa_sink_info *i, int eol, void *userdata)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) userdata;

    if (!eol)
    {
        const char *api = pa_proplist_gets (i->proplist, "device.api");
        if (!strcmp (api, "alsa"))
            gtk_container_foreach (GTK_CONTAINER (vol->outputs), replace_alsa_on_match, (void *) i);
    }
    pa_threaded_mainloop_signal (vol->pa_mainloop, 0);
}

static int pulse_update_alsa_names (VolumeALSAPlugin *vol)
{
    START_PA_OPERATION
    op = pa_context_get_sink_info_list (vol->pa_context, &pa_cb_get_sink_info_list, vol);
    END_PA_OPERATION ("get_sink_info_list")
}

/* Changing default sink
 * ---------------------
 *
 * The top-level change_sink function first calls the set_default_sink operation.
 * The get_sink_input_info_list operation is then called, which returns a callback
 * for each current sink input stream. The callback then in turn calls the
 * move_sink_input_by_name operation for each input stream to move it to the
 * new default sink.
 */

static int pulse_set_default_sink (VolumeALSAPlugin *vol, const char *sinkname)
{
    START_PA_OPERATION
    op = pa_context_set_default_sink (vol->pa_context, sinkname, &pa_cb_generic_success, vol);
    END_PA_OPERATION ("set_default_sink")
}

static void pa_cb_get_sink_input_info_list (pa_context *context, const pa_sink_input_info *i, int eol, void *userdata)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) userdata;

    if (!eol)
    {
        pa_context_move_sink_input_by_name (context, i->index, vol->pa_default_sink, &pa_cb_generic_success, vol);
    }

    pa_threaded_mainloop_signal (vol->pa_mainloop, 0);
}

static int pulse_move_streams_to_default_sink (VolumeALSAPlugin *vol)
{
    START_PA_OPERATION
    op = pa_context_get_sink_input_info_list (vol->pa_context, &pa_cb_get_sink_input_info_list, vol);
    END_PA_OPERATION ("get_sink_input_info_list")
}

static void pulse_change_sink (VolumeALSAPlugin *vol, const char *sinkname)
{
    if (vol->pa_default_sink) g_free (vol->pa_default_sink);
    vol->pa_default_sink = g_strdup (sinkname);

    pulse_set_default_sink (vol, sinkname);
    pulse_move_streams_to_default_sink (vol);
}

/* Volume and mute control
 * -----------------------
 *
 * For get operations, the generic get_sink_info operation is called on the
 * current default sink; the values are written into the global structure
 * by the callbacks, and the top-level functions return them from there.
 * For set operations, the specific set_sink_xxx operations are called.
 */

#define PA_VOL_SCALE 655    /* GTK volume scale is 0-100; PA scale is 0-65535 */

static void pa_cb_get_sink_info_by_name (pa_context *context, const pa_sink_info *i, int eol, void *userdata)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) userdata;

    if (!eol)
    {
        vol->pa_channels = i->volume.channels;
        vol->pa_volume = i->volume.values[0];
        vol->pa_mute = i->mute;
    }

    pa_threaded_mainloop_signal (vol->pa_mainloop, 0);
}

static int pulse_get_default_sink_info (VolumeALSAPlugin *vol)
{
    START_PA_OPERATION
    op = pa_context_get_sink_info_by_name (vol->pa_context, vol->pa_default_sink, &pa_cb_get_sink_info_by_name, vol);
    END_PA_OPERATION ("get_sink_info_by_name")
}

static int pulse_get_volume (VolumeALSAPlugin *vol)
{
    pulse_get_default_sink_info (vol);
    return vol->pa_volume / PA_VOL_SCALE;
}

static int pulse_get_mute (VolumeALSAPlugin *vol)
{
    pulse_get_default_sink_info (vol);
    return vol->pa_mute;
}

static int pulse_set_volume (VolumeALSAPlugin *vol, int volume)
{
    pa_cvolume cvol;
    cvol.channels = vol->pa_channels;
    cvol.values[0] = volume * PA_VOL_SCALE;
    cvol.values[1] = volume * PA_VOL_SCALE;

    START_PA_OPERATION
    op = pa_context_set_sink_volume_by_name (vol->pa_context, vol->pa_default_sink, &cvol, &pa_cb_generic_success, vol);
    END_PA_OPERATION ("set_sink_volume_by_name")
}

static int pulse_set_mute (VolumeALSAPlugin *vol, int mute)
{
    START_PA_OPERATION
    op = pa_context_set_sink_mute_by_name (vol->pa_context, vol->pa_default_sink, mute, &pa_cb_generic_success, vol);
    END_PA_OPERATION ("set_sink_mute_by_name");
}

/* Bluetooth name remapping
 * ------------------------
 *
 * Helper functions to remap PulseAudio sink and source names to and from
 * Bluez device names.
 */

static char *bluez_to_pa_sink_name (char *bluez_name)
{
    unsigned int b1, b2, b3, b4, b5, b6;

    if (bluez_name == NULL) return NULL;
    if (sscanf (bluez_name, "/org/bluez/hci0/dev_%x_%x_%x_%x_%x_%x", &b1, &b2, &b3, &b4, &b5, &b6) != 6)
    {
        DEBUG ("Bluez name invalid : %s", bluez_name);
        return NULL;
    }
    return g_strdup_printf ("bluez_sink.%02X_%02X_%02X_%02X_%02X_%02X.a2dp_sink", b1, b2, b3, b4, b5, b6);
}

static char *bluez_to_pa_source_name (char *bluez_name)
{
    unsigned int b1, b2, b3, b4, b5, b6;

    if (bluez_name == NULL) return NULL;
    if (sscanf (bluez_name, "/org/bluez/hci0/dev_%x_%x_%x_%x_%x_%x", &b1, &b2, &b3, &b4, &b5, &b6) != 6)
    {
        DEBUG ("Bluez name invalid : %s", bluez_name);
        return NULL;
    }
    return g_strdup_printf ("bluez_source.%02X_%02X_%02X_%02X_%02X_%02X.headset_head_unit", b1, b2, b3, b4, b5, b6);
}

static char *pa_sink_to_bluez_name (char *pa_name)
{
    unsigned int b1, b2, b3, b4, b5, b6;

    if (pa_name == NULL) return NULL;
    if (sscanf (pa_name, "bluez_sink.%x_%x_%x_%x_%x_%x.a2dp_sink", &b1, &b2, &b3, &b4, &b5, &b6) != 6)
    {
        DEBUG ("PulseAudio sink name invalid : %s", pa_name);
        return NULL;
    }
    return g_strdup_printf ("/org/bluez/hci0/dev_%02X_%02X_%02X_%02X_%02X_%02X", b1, b2, b3, b4, b5, b6);
}

static char *pa_source_to_bluez_name (char *pa_name)
{
    unsigned int b1, b2, b3, b4, b5, b6;

    if (pa_name == NULL) return NULL;
    if (sscanf (pa_name, "bluez_source.%x_%x_%x_%x_%x_%x.headset_head_unit", &b1, &b2, &b3, &b4, &b5, &b6) != 6)
    {
        DEBUG ("PulseAudio source name invalid : %s", pa_name);
        return NULL;
    }
    return g_strdup_printf ("/org/bluez/hci0/dev_%02X_%02X_%02X_%02X_%02X_%02X", b1, b2, b3, b4, b5, b6);
}

/*----------------------------------------------------------------------------*/
/* Plugin structure                                                           */
/*----------------------------------------------------------------------------*/

/* Callback when the configuration dialog is to be shown */

static GtkWidget *volumealsa_configure (LXPanel *panel, GtkWidget *plugin)
{
    VolumeALSAPlugin *vol = lxpanel_plugin_get_data (plugin);
    char *path = NULL;
    const gchar *command_line = NULL;
    GAppInfoCreateFlags flags = G_APP_INFO_CREATE_NONE;

#ifdef ENABLE_NLS
    textdomain (GETTEXT_PACKAGE);
#endif

    /* check if command line was configured */
    config_setting_lookup_string (vol->settings, "MixerCommand", &command_line);

    /* if command isn't set in settings then let guess it */
    /* Fallback to alsamixer when PA is not running, or when no PA utility is find */
    if (command_line == NULL)
    {
        if ((path = g_find_program_in_path ("pimixer")))
            command_line = "pimixer";
        else if ((path = g_find_program_in_path ("gnome-alsamixer")))
            command_line = "gnome-alsamixer";
        else if ((path = g_find_program_in_path ("alsamixergui")))
            command_line = "alsamixergui";
        else if ((path = g_find_program_in_path ("xfce4-mixer")))
            command_line = "xfce4-mixer";
        else if ((path = g_find_program_in_path ("alsamixer")))
        {
            command_line = "alsamixer";
            flags = G_APP_INFO_CREATE_NEEDS_TERMINAL;
        }
    }
    g_free (path);

    if (command_line) fm_launch_command_simple (NULL, NULL, flags, command_line, NULL);
    else
    {
        fm_show_error (NULL, NULL,
                      _("Error, you need to install an application to configure"
                        " the sound (pavucontrol, alsamixer ...)"));
    }

    return NULL;
}

/* Callback when panel configuration changes */

static void volumealsa_panel_configuration_changed (LXPanel *panel, GtkWidget *plugin)
{
    VolumeALSAPlugin *vol = lxpanel_plugin_get_data (plugin);

    volumealsa_build_popup_window (vol->plugin);
    volumealsa_update_display (vol);
    if (vol->show_popup) gtk_widget_show_all (vol->popup_window);
}

/* Callback when control message arrives */

static gboolean volumealsa_control_msg (GtkWidget *plugin, const char *cmd)
{
    VolumeALSAPlugin *vol = lxpanel_plugin_get_data (plugin);

    if (!strncmp (cmd, "star", 4))
    {
        asound_initialize (vol);
        volumealsa_update_display (vol);
        g_warning ("volumealsa: Restarted ALSA interface...");
        vol->stopped = FALSE;
        return TRUE;
    }

    if (!strncmp (cmd, "stop", 4))
    {
        asound_deinitialize (vol);
        volumealsa_update_display (vol);
        g_warning ("volumealsa: Stopped ALSA interface...");
        vol->stopped = TRUE;
        return TRUE;
    }

    if (!strncmp (cmd, "mute", 4))
    {
        pulse_set_mute (vol, pulse_get_mute (vol) ? 0 : 1);
        volumealsa_update_display (vol);
        return TRUE;
    }

    if (!strncmp (cmd, "volu", 4))
    {
        if (pulse_get_mute (vol)) pulse_set_mute (vol, 0);
        else
        {
            int volume = pulse_get_volume (vol);
            if (volume < 100)
            {
                volume += 5;
                volume /= 5;
                volume *= 5;
            }
            pulse_set_volume (vol, volume);
        }
        volumealsa_update_display (vol);
        return TRUE;
    }

    if (!strncmp (cmd, "vold", 4))
    {
        if (pulse_get_mute (vol)) pulse_set_mute (vol, 0);
        else
        {
            int volume = pulse_get_volume (vol);
            if (volume > 0)
            {
                volume -= 1; // effectively -5 + 4 for rounding...
                volume /= 5;
                volume *= 5;
            }
            pulse_set_volume (vol, volume);
        }
        volumealsa_update_display (vol);
        return TRUE;
    }

    if (!strncmp (cmd, "hw:", 3))
    {
        int dev;
        if (sscanf (cmd, "hw:%d", &dev) == 1)
        {
            /* if there is a Bluetooth device in use, get its name so we can disconnect it */
            char *device = asound_get_bt_device ();
            char *idevice = asound_get_bt_input ();

            asound_set_default_card (dev);
            asound_set_default_input (dev);
            asound_initialize (vol);
            volumealsa_update_display (vol);

            /* disconnect Bluetooth devices */
            if (device) bt_disconnect_device (vol, device);
            if (idevice && g_strcmp0 (device, idevice)) bt_disconnect_device (vol, idevice);
            if (device) g_free (device);
            if (idevice) g_free (idevice);
        }
        return TRUE;
    }

    return FALSE;
}

/* Plugin constructor */

static GtkWidget *volumealsa_constructor (LXPanel *panel, config_setting_t *settings)
{
    /* Allocate and initialize plugin context and set into Plugin private data pointer. */
    VolumeALSAPlugin *vol = g_new0 (VolumeALSAPlugin, 1);
    GtkWidget *p;

#ifdef ENABLE_NLS
    setlocale (LC_ALL, "");
    bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);
#endif

    vol->bt_conname = NULL;
    vol->bt_reconname = NULL;
    vol->master_element = NULL;
    vol->options_dlg = NULL;
    vol->odev_name = NULL;
    vol->idev_name = NULL;
    vol->mixers[OUTPUT_MIXER].mixer = NULL;
    vol->mixers[INPUT_MIXER].mixer = NULL;

    /* Allocate top level widget and set into Plugin widget pointer. */
    vol->panel = panel;
    vol->plugin = p = gtk_button_new ();
    gtk_button_set_relief (GTK_BUTTON (vol->plugin), GTK_RELIEF_NONE);
    g_signal_connect (vol->plugin, "button-press-event", G_CALLBACK (volumealsa_button_press_event), vol->panel);
    vol->settings = settings;
    lxpanel_plugin_set_data (p, vol, volumealsa_destructor);
    gtk_widget_add_events (p, GDK_BUTTON_PRESS_MASK);
    gtk_widget_set_tooltip_text (p, _("Volume control"));

    /* Allocate icon as a child of top level. */
    vol->tray_icon = gtk_image_new ();
    gtk_container_add (GTK_CONTAINER (p), vol->tray_icon);

    /* Initialize ALSA if default device isn't Bluetooth */
    if (asound_get_default_card () != BLUEALSA_DEV) asound_initialize (vol);

    /* Set up callbacks to see if BlueZ is on DBus */
    g_bus_watch_name (G_BUS_TYPE_SYSTEM, "org.bluez", 0, bt_cb_name_owned, bt_cb_name_unowned, vol, NULL);
    g_bus_watch_name (G_BUS_TYPE_SYSTEM, "org.bluealsa", 0, bt_cb_ba_name_owned, bt_cb_ba_name_unowned, vol, NULL);
    
    /* Initialize volume scale */
    volumealsa_build_popup_window (p);

    /* Connect signals. */
    g_signal_connect (G_OBJECT (p), "scroll-event", G_CALLBACK (volumealsa_popup_scale_scrolled), vol);
    g_signal_connect (panel_get_icon_theme (panel), "changed", G_CALLBACK (volumealsa_theme_change), vol);

    /* Set up for multiple HDMIs */
    vol->hdmis = hdmi_monitors (vol);

    /* set up PulseAudio context */
    pulse_init (vol);
    pulse_get_defaults (vol);

    /* Update the display, show the widget, and return. */
    volumealsa_update_display (vol);
    gtk_widget_show_all (p);

    vol->stopped = FALSE;
    return p;
}

/* Plugin destructor */

static void volumealsa_destructor (gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;

    asound_deinitialize (vol);

    pulse_disconnect (vol);
    pulse_close (vol);

    /* If the dialog box is open, dismiss it. */
    if (vol->popup_window != NULL) gtk_widget_destroy (vol->popup_window);
    if (vol->menu_popup != NULL) gtk_widget_destroy (vol->menu_popup);

    if (vol->restart_idle) g_source_remove (vol->restart_idle);

    if (vol->panel) g_signal_handlers_disconnect_by_func (panel_get_icon_theme (vol->panel), volumealsa_theme_change, vol);

    /* Deallocate all memory. */
    g_free (vol);
}

FM_DEFINE_MODULE (lxpanel_gtk, volumepulse)

/* Plugin descriptor */

LXPanelPluginInit fm_module_init_lxpanel_gtk = 
{
    .name = N_("Volume Control (PulseAudio)"),
    .description = N_("Display and control volume for PulseAudio"),
    .new_instance = volumealsa_constructor,
    .config = volumealsa_configure,
    .reconfigure = volumealsa_panel_configuration_changed,
    .control = volumealsa_control_msg,
    .gettext_package = GETTEXT_PACKAGE
};

/* End of file */
/*----------------------------------------------------------------------------*/
