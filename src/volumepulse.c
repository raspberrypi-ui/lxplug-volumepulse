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
/* Typedefs and macros */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* Plug-in global data                                                        */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* Prototypes                                                                 */
/*----------------------------------------------------------------------------*/

/* Helpers */
static int get_value (const char *fmt, ...);
static void hdmi_init (VolumePulsePlugin *vol);

/* Menu popup */
static void menu_open_profile_dialog (GtkWidget *widget, VolumePulsePlugin *vol);

#ifdef LXPLUG
/* Button handlers */
static gboolean volumepulse_button_press_event (GtkWidget *widget, GdkEventButton *event, VolumePulsePlugin *vol);
static gboolean micpulse_button_press_event (GtkWidget *widget, GdkEventButton *event, VolumePulsePlugin *vol);
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
/* Device select menu - vol                                                   */
/*----------------------------------------------------------------------------*/

void vol_menu_show (VolumePulsePlugin *vol)
{
    GtkWidget *mi;

    // create the menu
    if (menu_create (vol, FALSE))
    {
        // add the profiles menu item to the top level menu
        mi = gtk_separator_menu_item_new ();
        gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_devices[0]), mi);

        mi = gtk_menu_item_new_with_label (_("Device Profiles..."));
        g_signal_connect (mi, "activate", G_CALLBACK (menu_open_profile_dialog), (gpointer) vol);
        gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_devices[0]), mi);
    }

    // lock menu if a dialog is open
    if (vol->conn_dialog || vol->profiles_dialog)
        gtk_container_foreach (GTK_CONTAINER (vol->menu_devices[0]), (void *) gtk_widget_set_sensitive, FALSE);

    // show the menu
    gtk_widget_show_all (vol->menu_devices[0]);
}

/* Add a device entry to the menu */

void vol_menu_add_item (VolumePulsePlugin *vol, const char *label, const char *name)
{
    GList *list, *l;
    int count;
    const char *disp_label = device_display_name (vol, label);

    GtkWidget *mi = gtk_check_menu_item_new_with_label (disp_label);
    gtk_widget_set_name (mi, name);
    if (strstr (name, "bluez"))
    {
        g_signal_connect (mi, "activate", G_CALLBACK (menu_set_bluetooth_device_output), (gpointer) vol);
    }
    else
    {
        g_signal_connect (mi, "activate", G_CALLBACK (menu_set_alsa_device_output), (gpointer) vol);
        gtk_widget_set_sensitive (mi, FALSE);
        gtk_widget_set_tooltip_text (mi, _("Output to this device not available in the current profile"));
    }

    // find the start point of the last section - either a separator or the beginning of the list
    list = gtk_container_get_children (GTK_CONTAINER (vol->menu_devices[0]));
    count = g_list_length (list);
    l = g_list_last (list);
    while (l)
    {
        if (G_OBJECT_TYPE (l->data) == GTK_TYPE_SEPARATOR_MENU_ITEM) break;
        count--;
        l = l->prev;
    }

    // if l is NULL, init to element after start; if l is non-NULL, init to element after separator
    if (!l) l = list;
    else l = l->next;

    // loop forward from the first element, comparing against the new label
    while (l)
    {
        if (g_strcmp0 (disp_label, gtk_menu_item_get_label (GTK_MENU_ITEM (l->data))) < 0) break;
        count++;
        l = l->next;
    }

    gtk_menu_shell_insert (GTK_MENU_SHELL (vol->menu_devices[0]), mi, count);
    g_list_free (list);
}

/* Handler for menu click to open the profiles dialog */

static void menu_open_profile_dialog (GtkWidget *, VolumePulsePlugin *vol)
{
    profiles_dialog_show (vol);
}

/*----------------------------------------------------------------------------*/
/* Device select menu - mic                                                   */
/*----------------------------------------------------------------------------*/

void mic_menu_show (VolumePulsePlugin *vol)
{
    // create the menu
    menu_create (vol, TRUE);

    // lock menu if a dialog is open
    if (vol->conn_dialog || vol->profiles_dialog)
        gtk_container_foreach (GTK_CONTAINER (vol->menu_devices[1]), (void *) gtk_widget_set_sensitive, FALSE);

    // show the menu
    gtk_widget_show_all (vol->menu_devices[1]);
}

/* Add a device entry to the menu */

void mic_menu_add_item (VolumePulsePlugin *vol, const char *label, const char *name)
{
    GList *list, *l;
    int count;

    GtkWidget *mi = gtk_check_menu_item_new_with_label (label);
    gtk_widget_set_name (mi, name);
    if (strstr (name, "bluez"))
    {
        g_signal_connect (mi, "activate", G_CALLBACK (menu_set_bluetooth_device_input), (gpointer) vol);
    }
    else
    {
        g_signal_connect (mi, "activate", G_CALLBACK (menu_set_alsa_device_input), (gpointer) vol);
        gtk_widget_set_sensitive (mi, FALSE);
        gtk_widget_set_tooltip_text (mi, _("Input from this device not available in the current profile"));
    }

    // find the start point of the last section - either a separator or the beginning of the list
    list = gtk_container_get_children (GTK_CONTAINER (vol->menu_devices[1]));
    count = g_list_length (list);
    l = g_list_last (list);
    while (l)
    {
        if (G_OBJECT_TYPE (l->data) == GTK_TYPE_SEPARATOR_MENU_ITEM) break;
        count--;
        l = l->prev;
    }

    // if l is NULL, init to element after start; if l is non-NULL, init to element after separator
    if (!l) l = list;
    else l = l->next;

    // loop forward from the first element, comparing against the new label
    while (l)
    {
        if (g_strcmp0 (label, gtk_menu_item_get_label (GTK_MENU_ITEM (l->data))) < 0) break;
        count++;
        l = l->next;
    }

    gtk_menu_shell_insert (GTK_MENU_SHELL (vol->menu_devices[1]), mi, count);
    g_list_free (list);
}

/*----------------------------------------------------------------------------*/
/* wf-panel plugin functions                                                  */
/*----------------------------------------------------------------------------*/

/* Handler for button click */
gboolean volumepulse_button_release (GtkWidget *, GdkEventButton *event, VolumePulsePlugin *vol)
{
#ifndef LXPLUG
    if (pressed == PRESS_LONG) return FALSE;
#endif

    switch (event->button)
    {
        case 1: /* left-click - show volume popup */
                if (!vol->popup_shown) popup_window_show (vol, FALSE);
                volumepulse_update_display (vol);
                return FALSE;

        case 2: /* middle-click - toggle mute */
                pulse_set_mute (vol, pulse_get_mute (vol, FALSE) ? 0 : 1, FALSE);
                break;

        case 3: /* right-click - show device list */
                vol_menu_show (vol);
                wrap_show_menu (vol->plugin[0], vol->menu_devices[0]);
                break;
    }

    volumepulse_update_display (vol);
    return TRUE;
}

gboolean micpulse_button_release (GtkWidget *, GdkEventButton *event, VolumePulsePlugin *vol)
{
#ifndef LXPLUG
    if (pressed == PRESS_LONG) return FALSE;
#endif

    switch (event->button)
    {
        case 1: /* left-click - show volume popup */
                if (!vol->popup_shown) popup_window_show (vol, TRUE);
                micpulse_update_display (vol);
                return FALSE;

        case 2: /* middle-click - toggle mute */
                pulse_set_mute (vol, pulse_get_mute (vol, TRUE) ? 0 : 1, TRUE);
                break;

        case 3: /* right-click - show device list */
                mic_menu_show (vol);
                wrap_show_menu (vol->plugin[1], vol->menu_devices[1]);
                break;
    }

    micpulse_update_display (vol);
    return TRUE;
}

#ifndef LXPLUG
gboolean volmic_button_press (GtkWidget *, GdkEventButton *, VolumePulsePlugin *vol)
{
    pressed = PRESS_NONE;
    if (vol->popup_window[0] || vol->popup_window[1]) vol->popup_shown = TRUE;
    else vol->popup_shown = FALSE;
    return FALSE;
}

void vol_gesture_end (GtkGestureLongPress *, GdkEventSequence *, gpointer data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) data;
    if (pressed == PRESS_LONG)
    {
        vol_menu_show (vol);
        wrap_show_menu (vol->plugin[0], vol->menu_devices[0]);
    }
}

void mic_gesture_end (GtkGestureLongPress *, GdkEventSequence *, gpointer data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) data;
    if (pressed == PRESS_LONG)
    {
        mic_menu_show (vol);
        wrap_show_menu (vol->plugin[1], vol->menu_devices[1]);
    }
}
#endif

/* Handler for mouse scroll */
void volumepulse_mouse_scrolled (GtkScale *, GdkEventScroll *evt, VolumePulsePlugin *vol)
{
    if (pulse_get_mute (vol, FALSE)) return;

    /* Update the PulseAudio volume by a step */
    int val = pulse_get_volume (vol, FALSE);

    if (evt->direction == GDK_SCROLL_UP || evt->direction == GDK_SCROLL_LEFT
        || (evt->direction == GDK_SCROLL_SMOOTH && (evt->delta_x < 0 || evt->delta_y < 0)))
    {
        if (val < 100) val += 2;
    }
    else if (evt->direction == GDK_SCROLL_DOWN || evt->direction == GDK_SCROLL_RIGHT
        || (evt->direction == GDK_SCROLL_SMOOTH && (evt->delta_x > 0 || evt->delta_y > 0)))
    {
        if (val > 0) val -= 2;
    }
    pulse_set_volume (vol, val, FALSE);

    volumepulse_update_display (vol);
}

void micpulse_mouse_scrolled (GtkScale *, GdkEventScroll *evt, VolumePulsePlugin *vol)
{
    if (pulse_get_mute (vol, TRUE)) return;

    /* Update the PulseAudio volume by a step */
    int val = pulse_get_volume (vol, TRUE);

    if (evt->direction == GDK_SCROLL_UP || evt->direction == GDK_SCROLL_LEFT
        || (evt->direction == GDK_SCROLL_SMOOTH && (evt->delta_x < 0 || evt->delta_y < 0)))
    {
        if (val < 100) val += 2;
    }
    else if (evt->direction == GDK_SCROLL_DOWN || evt->direction == GDK_SCROLL_RIGHT
        || (evt->direction == GDK_SCROLL_SMOOTH && (evt->delta_x > 0 || evt->delta_y > 0)))
    {
        if (val > 0) val -= 2;
    }
    pulse_set_volume (vol, val, TRUE);

    micpulse_update_display (vol);
}

/* Handler for system config changed message from panel */
void volumepulse_update_display (VolumePulsePlugin *vol)
{
    pulse_count_devices (vol, FALSE);
    if (vol->pa_devices + bluetooth_count_devices (vol, FALSE) > 0)
    {
        gtk_widget_show_all (vol->plugin[0]);
        gtk_widget_set_sensitive (vol->plugin[0], TRUE);
    }
    else
    {
        gtk_widget_hide (vol->plugin[0]);
        gtk_widget_set_sensitive (vol->plugin[0], FALSE);
    }

    /* read current mute and volume status */
    gboolean mute = pulse_get_mute (vol, FALSE);
    int level = pulse_get_volume (vol, FALSE);
    if (mute) level = 0;

    /* update icon */
    const char *icon = "audio-volume-muted";
    if (!mute)
    {
        if (level >= 66) icon = "audio-volume-high";
        else if (level >= 33) icon = "audio-volume-medium";
        else if (level > 0) icon = "audio-volume-low";
        else icon = "audio-volume-silent";
    }
    wrap_set_taskbar_icon (vol, vol->tray_icon[0], icon);

    /* update popup window controls */
    if (vol->popup_window[0])
    {
        g_signal_handler_block (vol->popup_mute_check[0], vol->mute_check_handler[0]);
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (vol->popup_mute_check[0]), mute);
        g_signal_handler_unblock (vol->popup_mute_check[0], vol->mute_check_handler[0]);

        g_signal_handler_block (vol->popup_volume_scale[0], vol->volume_scale_handler[0]);
        gtk_range_set_value (GTK_RANGE (vol->popup_volume_scale[0]), level);
        g_signal_handler_unblock (vol->popup_volume_scale[0], vol->volume_scale_handler[0]);

        gtk_widget_set_sensitive (vol->popup_volume_scale[0], !mute);
    }

    /* update tooltip */
    char *tooltip = g_strdup_printf ("%s %d", _("Volume control"), level);
    if (!vol->wizard) gtk_widget_set_tooltip_text (vol->plugin[0], tooltip);
    g_free (tooltip);
}

void micpulse_update_display (VolumePulsePlugin *vol)
{
    pulse_count_devices (vol, TRUE);
    if (!vol->wizard && (vol->pa_devices + bluetooth_count_devices (vol, TRUE) > 0))
    {
        gtk_widget_show_all (vol->plugin[1]);
        gtk_widget_set_sensitive (vol->plugin[1], TRUE);
    }
    else
    {
        gtk_widget_hide (vol->plugin[1]);
        gtk_widget_set_sensitive (vol->plugin[1], FALSE);
    }

    /* read current mute and volume status */
    gboolean mute = pulse_get_mute (vol, TRUE);
    int level = pulse_get_volume (vol, TRUE);
    if (mute) level = 0;

    /* update icon */
    wrap_set_taskbar_icon (vol, vol->tray_icon[1], mute ? "audio-input-mic-muted" : "audio-input-microphone");

    /* update popup window controls */
    if (vol->popup_window[1])
    {
        g_signal_handler_block (vol->popup_mute_check[1], vol->mute_check_handler[1]);
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (vol->popup_mute_check[1]), mute);
        g_signal_handler_unblock (vol->popup_mute_check[1], vol->mute_check_handler[1]);

        g_signal_handler_block (vol->popup_volume_scale[1], vol->volume_scale_handler[1]);
        gtk_range_set_value (GTK_RANGE (vol->popup_volume_scale[1]), level);
        g_signal_handler_unblock (vol->popup_volume_scale[1], vol->volume_scale_handler[1]);

        gtk_widget_set_sensitive (vol->popup_volume_scale[1], !mute);
    }

    /* update tooltip */
    char *tooltip = g_strdup_printf ("%s %d", _("Mic volume"), level);
    if (!vol->wizard) gtk_widget_set_tooltip_text (vol->plugin[1], tooltip);
    g_free (tooltip);
}

/* Handler for control message */
gboolean volumepulse_control_msg (VolumePulsePlugin *vol, const char *cmd)
{
    if (!gtk_widget_is_visible (vol->plugin[0])) return TRUE;

    if (!strncmp (cmd, "mute", 4))
    {
        pulse_set_mute (vol, pulse_get_mute (vol, FALSE) ? 0 : 1, FALSE);
        volumepulse_update_display (vol);
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
        volumepulse_update_display (vol);
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
        volumepulse_update_display (vol);
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
    micpulse_update_display (vol);
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

    setlocale (LC_ALL, "");
    bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

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
    return volumepulse_button_release (widget, event, vol);
}

static gboolean micpulse_button_press_event (GtkWidget *widget, GdkEventButton *event, VolumePulsePlugin *vol)
{
    close_widget (&vol->popup_window[0]);
    close_widget (&vol->popup_window[1]);
    return micpulse_button_release (widget, event, vol);
}

/* Handler for system config changed message from panel */
static void volumepulse_configuration_changed (LXPanel *, GtkWidget *plugin)
{
    VolumePulsePlugin *vol = lxpanel_plugin_get_data (plugin);
    volumepulse_update_display (vol);
    micpulse_update_display (vol);
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
