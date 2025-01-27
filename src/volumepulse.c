/*============================================================================
Copyright (c) 2020-2025 Raspberry Pi Holdings Ltd.
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
============================================================================*/

#include <locale.h>
#include <glib/gi18n.h>
#include <pulse/pulseaudio.h>

#ifdef LXPLUG
#include "plugin.h"
#else
#include "lxutils.h"
#endif

#include "volumepulse.h"
#include "commongui.h"
#include "pulse.h"
#include "bluetooth.h"

/*----------------------------------------------------------------------------*/
/* Typedefs and macros                                                        */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* Global data                                                                */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* Prototypes                                                                 */
/*----------------------------------------------------------------------------*/

static int get_value (const char *fmt, ...);
static void hdmi_init (VolumePulsePlugin *vol);
static gboolean button_release (GtkWidget *, GdkEventButton *event, VolumePulsePlugin *vol, gboolean input);
#ifdef LXPLUG
static gboolean volumepulse_button_press_event (GtkWidget *widget, GdkEventButton *event, VolumePulsePlugin *vol);
static gboolean micpulse_button_press_event (GtkWidget *widget, GdkEventButton *event, VolumePulsePlugin *vol);
#else
static gboolean volmic_button_press (GtkWidget *, GdkEventButton *, VolumePulsePlugin *vol);
static gboolean volumepulse_button_release (GtkWidget *widget, GdkEventButton *event, VolumePulsePlugin *vol);
static gboolean micpulse_button_release (GtkWidget *widget, GdkEventButton *event, VolumePulsePlugin *vol);
static void vol_gesture_end (GtkGestureLongPress *, GdkEventSequence *, gpointer data);
static void mic_gesture_end (GtkGestureLongPress *, GdkEventSequence *, gpointer data);
#endif

/*----------------------------------------------------------------------------*/
/* Function definitions                                                       */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* Generic helper functions                                                   */
/*----------------------------------------------------------------------------*/

/* Call the supplied system command and parse the result for an integer value */

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

/* Find number of HDMI devices and device names */

static void hdmi_init (VolumePulsePlugin *vol)
{
    int i, m;

#ifdef LXPLUG
    /* check xrandr for connected monitors */
    m = get_value ("xrandr -q | grep -c connected");
#else
    /* check wlr-randr for connected monitors */
    m = get_value ("wlr-randr | grep -c ^[^[:space:]]");
#endif
    if (m < 0) m = 1; /* couldn't read, so assume 1... */
    if (m > 2) m = 2;

    for (i = 0; i < 2; i++)
    {
        if (vol->hdmi_names[i]) g_free (vol->hdmi_names[i]);
        vol->hdmi_names[i] = NULL;
    }

    /* get the names */
    if (m == 2)
    {
#ifdef LXPLUG
        for (i = 0; i < 2; i++)
        {
            vol->hdmi_names[i] = get_string ("xrandr --listmonitors | grep %d: | cut -d ' ' -f 6", i);
        }
#else
        vol->hdmi_names[0] = get_string ("wlr-randr | grep  ^[^[:space:]] | sort | head -n 1 | cut -d ' ' -f 1");
        vol->hdmi_names[1] = get_string ("wlr-randr | grep  ^[^[:space:]] | sort | tail -n 1 | cut -d ' ' -f 1");
#endif

        /* check both devices are HDMI */
        if (vol->hdmi_names[0] && !strncmp (vol->hdmi_names[0], "HDMI", 4)
            && vol->hdmi_names[1] && !strncmp (vol->hdmi_names[1], "HDMI", 4))
                return;
    }

    /* only one device, just name it "HDMI" */
    for (i = 0; i < 2; i++)
    {
        if (vol->hdmi_names[i]) g_free (vol->hdmi_names[i]);
        vol->hdmi_names[i] = g_strdup (_("HDMI"));
    }
}

/*----------------------------------------------------------------------------*/
/* wf-panel plugin functions                                                  */
/*----------------------------------------------------------------------------*/

/* Handler for button click */
static gboolean button_release (GtkWidget *, GdkEventButton *event, VolumePulsePlugin *vol, gboolean input)
{
#ifndef LXPLUG
    if (pressed == PRESS_LONG) return FALSE;
#endif

    switch (event->button)
    {
        case 1: /* left-click - show volume popup */
                if (!vol->popup_shown) popup_window_show (vol, input);
                update_display (vol, input);
                return FALSE;

        case 2: /* middle-click - toggle mute */
                pulse_set_mute (vol, pulse_get_mute (vol, input) ? 0 : 1, input);
                break;

        case 3: /* right-click - show device list */
                menu_show (vol, input);
                wrap_show_menu (vol->plugin[input ? 1 : 0], vol->menu_devices[input ? 1 : 0]);
                break;
    }

    update_display (vol, input);
    return TRUE;
}

#ifndef LXPLUG
static gboolean volmic_button_press (GtkWidget *, GdkEventButton *, VolumePulsePlugin *vol)
{
    pressed = PRESS_NONE;
    if (vol->popup_window[0] || vol->popup_window[1]) vol->popup_shown = TRUE;
    else vol->popup_shown = FALSE;
    return FALSE;
}

static gboolean volumepulse_button_release (GtkWidget *widget, GdkEventButton *event, VolumePulsePlugin *vol)
{
    return button_release (widget, event, vol, FALSE);
}

static gboolean micpulse_button_release (GtkWidget *widget, GdkEventButton *event, VolumePulsePlugin *vol)
{
    return button_release (widget, event, vol, TRUE);
}

/* Handler for long-press gesture */
static void vol_gesture_end (GtkGestureLongPress *, GdkEventSequence *, gpointer data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) data;
    if (pressed == PRESS_LONG)
    {
        menu_show (vol, FALSE);
        wrap_show_menu (vol->plugin[0], vol->menu_devices[0]);
    }
}

static void mic_gesture_end (GtkGestureLongPress *, GdkEventSequence *, gpointer data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) data;
    if (pressed == PRESS_LONG)
    {
        menu_show (vol, TRUE);
        wrap_show_menu (vol->plugin[1], vol->menu_devices[1]);
    }
}
#endif

/* Handler for system config changed message from panel */
void volumepulse_update_display (VolumePulsePlugin *vol)
{
    update_display (vol, FALSE);
    update_display (vol, TRUE);
}

/* Handler for control message */
gboolean volumepulse_control_msg (VolumePulsePlugin *vol, const char *cmd)
{
    if (!gtk_widget_is_visible (vol->plugin[0])) return TRUE;

    if (!strncmp (cmd, "mute", 4))
    {
        pulse_set_mute (vol, pulse_get_mute (vol, FALSE) ? 0 : 1, FALSE);
        update_display (vol, FALSE);
        return TRUE;
    }

    if (!strncmp (cmd, "volu", 4))
    {
        if (pulse_get_mute (vol, FALSE)) pulse_set_mute (vol, 0, FALSE);
        else
        {
            int volume = pulse_get_volume (vol, FALSE);
            if (volume < 100)
            {
                volume += 9;  // some hardware rounds volumes, so make sure we are going as far as possible up before we round....
                volume /= 5;
                volume *= 5;
            }
            pulse_set_volume (vol, volume, FALSE);
        }
        update_display (vol, FALSE);
        return TRUE;
    }

    if (!strncmp (cmd, "vold", 4))
    {
        if (pulse_get_mute (vol, FALSE)) pulse_set_mute (vol, 0, FALSE);
        else
        {
            int volume = pulse_get_volume (vol, FALSE);
            if (volume > 0)
            {
                volume -= 4; // ... and the same for going down
                volume /= 5;
                volume *= 5;
            }
            pulse_set_volume (vol, volume, FALSE);
        }
        update_display (vol, FALSE);
        return TRUE;
    }

    if (!strncmp (cmd, "stop", 5))
    {
        pulse_terminate (vol);
    }

    if (!strncmp (cmd, "start", 5))
    {
        hdmi_init (vol);
        pulse_init (vol);
    }

    return FALSE;
}

void volumepulse_init (VolumePulsePlugin *vol)
{
    setlocale (LC_ALL, "");
    bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

    if (!g_strcmp0 (getenv ("USER"), "rpi-first-boot-wizard")) vol->wizard = TRUE;
    else vol->wizard = FALSE;

    /* Allocate icon as a child of top level */
    vol->tray_icon[0] = gtk_image_new ();
    gtk_container_add (GTK_CONTAINER (vol->plugin[0]), vol->tray_icon[0]);
    vol->tray_icon[1] = gtk_image_new ();
    gtk_container_add (GTK_CONTAINER (vol->plugin[1]), vol->tray_icon[1]);

    /* Set up button */
    gtk_button_set_relief (GTK_BUTTON (vol->plugin[0]), GTK_RELIEF_NONE);
    g_signal_connect (vol->plugin[0], "scroll-event", G_CALLBACK (volumepulse_mouse_scrolled), vol);
    gtk_widget_add_events (vol->plugin[0], GDK_SCROLL_MASK);

    gtk_button_set_relief (GTK_BUTTON (vol->plugin[1]), GTK_RELIEF_NONE);
    g_signal_connect (vol->plugin[1], "scroll-event", G_CALLBACK (micpulse_mouse_scrolled), vol);
    gtk_widget_add_events (vol->plugin[1], GDK_SCROLL_MASK);

#ifdef LXPLUG
    g_signal_connect (vol->plugin[0], "button-press-event", G_CALLBACK (volumepulse_button_press_event), vol);
    g_signal_connect (vol->plugin[1], "button-press-event", G_CALLBACK (micpulse_button_press_event), vol);
#else
    g_signal_connect (vol->plugin[0], "button-press-event", G_CALLBACK (volmic_button_press), vol);
    g_signal_connect (vol->plugin[1], "button-press-event", G_CALLBACK (volmic_button_press), vol);

    g_signal_connect (vol->plugin[0], "button-release-event", G_CALLBACK (volumepulse_button_release), vol);
    g_signal_connect (vol->plugin[1], "button-release-event", G_CALLBACK (micpulse_button_release), vol);

    vol->gesture[0] = add_long_press (vol->plugin[0], G_CALLBACK (vol_gesture_end), vol);
    vol->gesture[1] = add_long_press (vol->plugin[1], G_CALLBACK (mic_gesture_end), vol);
#endif

    /* Set up variables */
    vol->menu_devices[0] = NULL;
    vol->menu_devices[1] = NULL;
    vol->popup_window[0] = NULL;
    vol->popup_window[1] = NULL;
    vol->profiles_dialog = NULL;
    vol->conn_dialog = NULL;
    vol->hdmi_names[0] = NULL;
    vol->hdmi_names[1] = NULL;

    vol->pipewire = !system ("ps ax | grep pipewire-pulse | grep -qv grep");
    if (vol->pipewire)
    {
        DEBUG ("using pipewire");
    }
    else
    {
        DEBUG ("using pulseaudio");
    }

    /* Delete any old ALSA config */
    vsystem ("rm -f ~/.asoundrc");

    /* Find HDMIs */
    hdmi_init (vol);

    /* Set up PulseAudio */
    pulse_init (vol);

    /* Set up Bluez D-Bus interface */
    bluetooth_init (vol);

    /* Show the widget and return */
    gtk_widget_show_all (vol->plugin[0]);
    gtk_widget_show_all (vol->plugin[1]);

    volumepulse_update_display (vol);
}

void volumepulse_destructor (gpointer user_data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) user_data;

    close_widget (&vol->profiles_dialog);
    close_widget (&vol->conn_dialog);
    close_widget (&vol->menu_devices[0]);
    close_widget (&vol->menu_devices[1]);
#ifdef LXPLUG
    close_widget (&vol->popup_window[0]);
    close_widget (&vol->popup_window[1]);
#else
    close_popup ();
#endif

    bluetooth_terminate (vol);
    pulse_terminate (vol);

#ifndef LXPLUG
    if (vol->gesture[0]) g_object_unref (vol->gesture[0]);
    if (vol->gesture[1]) g_object_unref (vol->gesture[1]);
#endif

    g_free (vol);
}

/*----------------------------------------------------------------------------*/
/* LXPanel plugin functions                                                   */
/*----------------------------------------------------------------------------*/
#ifdef LXPLUG

/* Constructor */
static GtkWidget *volumepulse_constructor (LXPanel *panel, config_setting_t *settings)
{
    /* Allocate and initialize plugin context */
    VolumePulsePlugin *vol = g_new0 (VolumePulsePlugin, 1);

    /* Allocate top level widget and set into plugin widget pointer */
    vol->panel = panel;
    vol->settings = settings;
    vol->box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    vol->plugin[0] = gtk_button_new ();
    gtk_box_pack_start (GTK_BOX (vol->box), vol->plugin[0], TRUE, TRUE, 0);
    vol->plugin[1] = gtk_button_new ();
    gtk_box_pack_start (GTK_BOX (vol->box), vol->plugin[1], TRUE, TRUE, 0);

    lxpanel_plugin_set_data (vol->box, vol, volumepulse_destructor);

    volumepulse_init (vol);

    return vol->box;
}

/* Handler for button press */
static gboolean volumepulse_button_press_event (GtkWidget *widget, GdkEventButton *event, VolumePulsePlugin *vol)
{
    close_widget (&vol->popup_window[0]);
    close_widget (&vol->popup_window[1]);
    return button_release (widget, event, vol, FALSE);
}

static gboolean micpulse_button_press_event (GtkWidget *widget, GdkEventButton *event, VolumePulsePlugin *vol)
{
    close_widget (&vol->popup_window[0]);
    close_widget (&vol->popup_window[1]);
    return button_release (widget, event, vol, TRUE);
}

/* Handler for system config changed message from panel */
static void volumepulse_configuration_changed (LXPanel *, GtkWidget *plugin)
{
    VolumePulsePlugin *vol = lxpanel_plugin_get_data (plugin);
    volumepulse_update_display (vol);
}

/* Handler for control message */
static gboolean volumepulse_control (GtkWidget *plugin, const char *cmd)
{
    VolumePulsePlugin *vol = lxpanel_plugin_get_data (plugin);
    return volumepulse_control_msg (vol, cmd);
}

FM_DEFINE_MODULE (lxpanel_gtk, volumepulse)

/* Plugin descriptor */
LXPanelPluginInit fm_module_init_lxpanel_gtk =
{
    .name = N_("Volume Control (PulseAudio)"),
    .description = N_("Display and control volume for PulseAudio"),
    .new_instance = volumepulse_constructor,
    .reconfigure = volumepulse_configuration_changed,
    .control = volumepulse_control,
    .gettext_package = GETTEXT_PACKAGE
};
#endif

/* End of file */
/*----------------------------------------------------------------------------*/
