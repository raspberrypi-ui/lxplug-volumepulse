/*
Copyright (c) 2022 Raspberry Pi (Trading) Ltd.
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

#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include "plugin.h"
#include "volumepulse.h"
#include "pulse.h"
#include "bluetooth.h"

/*----------------------------------------------------------------------------*/
/* Static function prototypes                                                 */
/*----------------------------------------------------------------------------*/

/* Helpers */
static int get_value (const char *fmt, ...);

/* Volume popup */
static void popup_window_show (GtkWidget *p);
static void popup_window_scale_changed (GtkRange *range, VolumePulsePlugin *vol);
static void popup_window_mute_toggled (GtkWidget *widget, VolumePulsePlugin *vol);
static gboolean popup_mapped (GtkWidget *widget, GdkEvent *event, VolumePulsePlugin *vol);
static gboolean popup_button_press (GtkWidget *widget, GdkEventButton *event, VolumePulsePlugin *vol);

/* Menu popup */
static void menu_show (VolumePulsePlugin *vol);
static void menu_mark_default (GtkWidget *widget, gpointer data);
static void menu_set_alsa_input (GtkWidget *widget, VolumePulsePlugin *vol);
static void menu_set_bluetooth_input (GtkWidget *widget, VolumePulsePlugin *vol);

/* Handlers and graphics */
static gboolean micpulse_button_press_event (GtkWidget *widget, GdkEventButton *event, VolumePulsePlugin *vol);
static void micpulse_menu_set_position (GtkWidget *menu, gint *px, gint *py, gboolean *push_in, VolumePulsePlugin *vol);
static void micpulse_mouse_scrolled (GtkScale *scale, GdkEventScroll *evt, VolumePulsePlugin *vol);
static void micpulse_theme_change (GtkWidget *widget, VolumePulsePlugin *vol);

/* Plugin */
static void micpulse_panel_configuration_changed (LXPanel *panel, GtkWidget *plugin);
static gboolean micpulse_control_msg (GtkWidget *plugin, const char *cmd);
static GtkWidget *micpulse_constructor (LXPanel *panel, config_setting_t *settings);
static void micpulse_destructor (gpointer user_data);

/*----------------------------------------------------------------------------*/
/* Profiles dialog                                                            */
/*----------------------------------------------------------------------------*/

/* Global called by other files - needs to exist, but does nothing on a plugin without a profile dialog */

void profiles_dialog_add_combo (VolumePulsePlugin *vol, GtkListStore *ls, GtkWidget *dest, int sel, const char *label, const char *name)
{
}

/*----------------------------------------------------------------------------*/
/* Generic helper functions                                                   */
/*----------------------------------------------------------------------------*/

/* System command accepting variable arguments */

int vsystem (const char *fmt, ...)
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

/* Call the supplied system command and return a new string with the first word of the result */

char *get_string (const char *fmt, ...)
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

/* Destroy a widget and null its pointer */

void close_widget (GtkWidget **wid)
{
    if (*wid)
    {
        gtk_widget_destroy (*wid);
        *wid = NULL;
    }
}

/*----------------------------------------------------------------------------*/
/* Volume scale popup window                                                  */
/*----------------------------------------------------------------------------*/

/* Create the pop-up volume window */

static void popup_window_show (GtkWidget *p)
{
    VolumePulsePlugin *vol = lxpanel_plugin_get_data (p);
    gint x, y;

    /* Create a new window. */
    vol->popup_window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name (vol->popup_window, "panelpopup");
    gtk_window_set_decorated (GTK_WINDOW (vol->popup_window), FALSE);

    gtk_container_set_border_width (GTK_CONTAINER (vol->popup_window), 5);
    gtk_window_set_skip_taskbar_hint (GTK_WINDOW (vol->popup_window), TRUE);
    gtk_window_set_skip_pager_hint (GTK_WINDOW (vol->popup_window), TRUE);
    gtk_window_set_type_hint (GTK_WINDOW (vol->popup_window), GDK_WINDOW_TYPE_HINT_DROPDOWN_MENU);

    /* Create a scrolled window as the child of the top level window. */
    GtkWidget *scrolledwindow = gtk_scrolled_window_new (NULL, NULL);
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
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add (GTK_CONTAINER (viewport), box);

    /* Create a vertical scale as the child of the vertical box. */
    vol->popup_volume_scale = gtk_scale_new (GTK_ORIENTATION_VERTICAL, GTK_ADJUSTMENT (gtk_adjustment_new (100, 0, 100, 0, 0, 0)));
    g_object_set (vol->popup_volume_scale, "height-request", 120, NULL);
    gtk_scale_set_draw_value (GTK_SCALE (vol->popup_volume_scale), FALSE);
    gtk_range_set_inverted (GTK_RANGE (vol->popup_volume_scale), TRUE);
    gtk_box_pack_start (GTK_BOX (box), vol->popup_volume_scale, TRUE, TRUE, 0);
    gtk_widget_set_can_focus (vol->popup_volume_scale, FALSE);

    /* Value-changed and scroll-event signals. */
    vol->volume_scale_handler = g_signal_connect (vol->popup_volume_scale, "value-changed", G_CALLBACK (popup_window_scale_changed), vol);
    g_signal_connect (vol->popup_volume_scale, "scroll-event", G_CALLBACK (micpulse_mouse_scrolled), vol);

    /* Create a check button as the child of the vertical box. */
    vol->popup_mute_check = gtk_check_button_new_with_label (_("Mute"));
    gtk_box_pack_end (GTK_BOX (box), vol->popup_mute_check, FALSE, FALSE, 0);
    vol->mute_check_handler = g_signal_connect (vol->popup_mute_check, "toggled", G_CALLBACK (popup_window_mute_toggled), vol);
    gtk_widget_set_can_focus (vol->popup_mute_check, FALSE);

    /* Show the window - need to draw the window in order to allow the plugin position helper to get its size */
    gtk_window_set_position (GTK_WINDOW (vol->popup_window), GTK_WIN_POS_MOUSE);
    gtk_widget_show_all (vol->popup_window);
    gtk_widget_hide (vol->popup_window);
    lxpanel_plugin_popup_set_position_helper (vol->panel, vol->plugin, vol->popup_window, &x, &y);
    gdk_window_move (gtk_widget_get_window (vol->popup_window), x, y);
    gtk_window_present (GTK_WINDOW (vol->popup_window));

    /* Connect the function which hides the window when the mouse is clicked outside it */
    g_signal_connect (G_OBJECT (vol->popup_window), "map-event", G_CALLBACK (popup_mapped), vol);
    g_signal_connect (G_OBJECT (vol->popup_window), "button-press-event", G_CALLBACK (popup_button_press), vol);
}

/* Handler for "value_changed" signal on popup window vertical scale */

static void popup_window_scale_changed (GtkRange *range, VolumePulsePlugin *vol)
{
    if (pulse_get_mute (vol, TRUE)) return;

    /* Update the PulseAudio volume */
    pulse_set_volume (vol, gtk_range_get_value (range), TRUE);

    volumepulse_update_display (vol);
}

/* Handler for "toggled" signal on popup window mute checkbox */

static void popup_window_mute_toggled (GtkWidget *widget, VolumePulsePlugin *vol)
{
    /* Toggle the PulseAudio mute */
    pulse_set_mute (vol, gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget)), TRUE);

    volumepulse_update_display (vol);
}

/* Handler for "focus-out" signal on popup window */

static gboolean popup_mapped (GtkWidget *widget, GdkEvent *event, VolumePulsePlugin *vol)
{
    gdk_seat_grab (gdk_display_get_default_seat (gdk_display_get_default ()), gtk_widget_get_window (widget), GDK_SEAT_CAPABILITY_ALL_POINTING, TRUE, NULL, NULL, NULL, NULL);
    return FALSE;
}

static gboolean popup_button_press (GtkWidget *widget, GdkEventButton *event, VolumePulsePlugin *vol)
{
    int x, y;
    gtk_window_get_size (GTK_WINDOW (widget), &x, &y);
    if (event->x < 0 || event->y < 0 || event->x > x || event->y > y)
    {
        close_widget (&vol->popup_window);
        gdk_seat_ungrab (gdk_display_get_default_seat (gdk_display_get_default ()));
    }
    return FALSE;
}

/*----------------------------------------------------------------------------*/
/* Device select menu                                                         */
/*----------------------------------------------------------------------------*/

/* Create the device select menu */

static void menu_show (VolumePulsePlugin *vol)
{
    GtkWidget *mi;
    GList *items, *head;

    // create input selector
    vol->menu_devices = gtk_menu_new ();
    gtk_widget_set_name (vol->menu_devices, "panelmenu");
    vol->menu_inputs = vol->menu_devices;

    // add ALSA inputs
    pulse_add_devices_to_menu (vol, TRUE, FALSE);

    // add Bluetooth inputs
    bluetooth_add_devices_to_menu (vol, TRUE);

    // did we find any input devices? if not, the menu will be empty...
    items = gtk_container_get_children (GTK_CONTAINER (vol->menu_devices));
    if (items == NULL)
    {
        mi = gtk_menu_item_new_with_label (_("No audio devices found"));
        gtk_widget_set_sensitive (GTK_WIDGET (mi), FALSE);
        gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_devices), mi);
    }

    // update the menu item names, which are currently ALSA device names, to PulseAudio sink/source names
    pulse_update_devices_in_menu (vol);

    // show the default sink and source in the menu
    pulse_get_default_sink_source (vol);
    if (vol->menu_devices) gtk_container_foreach (GTK_CONTAINER (vol->menu_devices), menu_mark_default, vol);

    // lock menu if a dialog is open
    if (vol->conn_dialog)
    {
        items = gtk_container_get_children (GTK_CONTAINER (vol->menu_devices));
        head = items;
        while (items)
        {
            gtk_widget_set_sensitive (GTK_WIDGET (items->data), FALSE);
            items = items->next;
        }
        g_list_free (head);
    }

    // show the menu
    gtk_widget_show_all (vol->menu_devices);
}

/* Add a device entry to the menu */

void menu_add_item (VolumePulsePlugin *vol, const char *label, const char *name, gboolean input)
{
    GtkWidget *menu = input ? vol->menu_inputs : vol->menu_outputs;
    GList *list, *l;
    int count;

    GtkWidget *mi = gtk_check_menu_item_new_with_label (label);
    gtk_widget_set_name (mi, name);
    if (strstr (name, "bluez"))
    {
        g_signal_connect (mi, "activate", G_CALLBACK (menu_set_bluetooth_input), (gpointer) vol);
    }
    else
    {
        g_signal_connect (mi, "activate", G_CALLBACK (menu_set_alsa_input), (gpointer) vol);
        gtk_widget_set_sensitive (mi, FALSE);
        gtk_widget_set_tooltip_text (mi, _("Input from this device not available in the current profile"));
    }

    // find the start point of the last section - either a separator or the beginning of the list
    list = gtk_container_get_children (GTK_CONTAINER (menu));
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

    gtk_menu_shell_insert (GTK_MENU_SHELL (menu), mi, count);
    g_list_free (list);
}

/* Add a separator to the menu (but only if there isn't already one there...) */

void menu_add_separator (VolumePulsePlugin *vol, GtkWidget *menu)
{
    GtkWidget *mi;
    GList *list, *l;

    if (menu == NULL) return;
    if (vol->separator == TRUE) return;

    // find the end of the menu
    list = gtk_container_get_children (GTK_CONTAINER (menu));
    l = g_list_last (list);
    if (l && G_OBJECT_TYPE (l->data) != GTK_TYPE_SEPARATOR_MENU_ITEM)
    {
        mi = gtk_separator_menu_item_new ();
        gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
        vol->separator = TRUE;
    }
    g_list_free (list);
}

/* Add a tickmark to the supplied widget if it is the default item in its parent menu */

static void menu_mark_default (GtkWidget *widget, gpointer data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) data;
    const char *def, *wid = gtk_widget_get_name (widget);

    def = vol->pa_default_source;
    if (!def || !wid) return;

    // check to see if either the two names match (for an ALSA device),
    // or if the BlueZ address from the widget is in the default name */
    if (!g_strcmp0 (def, wid) || (strstr (wid, "bluez") && strstr (def, wid + 20) && !strstr (def, "monitor")))
    {
        gulong hid = g_signal_handler_find (widget, G_SIGNAL_MATCH_ID, g_signal_lookup ("activate", GTK_TYPE_CHECK_MENU_ITEM), 0, NULL, NULL, NULL);
        g_signal_handler_block (widget, hid);
        gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (widget), TRUE);
        g_signal_handler_unblock (widget, hid);
    }
}

/* Handler for menu click to set an ALSA device as input */

static void menu_set_alsa_input (GtkWidget *widget, VolumePulsePlugin *vol)
{
    bluetooth_remove_input (vol);
    pulse_unmute_all_streams (vol);
    pulse_change_source (vol, gtk_widget_get_name (widget));
    pulse_move_input_streams (vol);
    volumepulse_update_display (vol);
}

/* Handler for menu click to set a Bluetooth device as input */

static void menu_set_bluetooth_input (GtkWidget *widget, VolumePulsePlugin *vol)
{
    bluetooth_set_input (vol, gtk_widget_get_name (widget), gtk_menu_item_get_label (GTK_MENU_ITEM (widget)));
}

/*----------------------------------------------------------------------------*/
/* Plugin handlers and graphics                                               */
/*----------------------------------------------------------------------------*/

/* Handler for "button-press-event" signal on main widget. */

static gboolean micpulse_button_press_event (GtkWidget *widget, GdkEventButton *event, VolumePulsePlugin *vol)
{
#ifdef ENABLE_NLS
    textdomain (GETTEXT_PACKAGE);
#endif

    switch (event->button)
    {
        case 1: /* left-click - show or hide volume popup */
                if (vol->popup_window) close_widget (&vol->popup_window);
                else popup_window_show (vol->plugin);
                break;

        case 2: /* middle-click - toggle mute */
                pulse_set_mute (vol, pulse_get_mute (vol, TRUE) ? 0 : 1, TRUE);
                break;

        case 3: /* right-click - show device list */
                close_widget (&vol->popup_window);
                menu_show (vol);
                gtk_menu_popup_at_widget (GTK_MENU (vol->menu_devices), vol->plugin, GDK_GRAVITY_NORTH_WEST, GDK_GRAVITY_NORTH_WEST, (GdkEvent *) event);
                break;
    }

    volumepulse_update_display (vol);
    return TRUE;
}

/* Determine popup position for menu */

static void micpulse_menu_set_position (GtkWidget *menu, gint *px, gint *py, gboolean *push_in, VolumePulsePlugin *vol)
{
    /* Determine the coordinates. */
    lxpanel_plugin_popup_set_position_helper (vol->panel, vol->plugin, menu, px, py);
    *push_in = TRUE;
}

/* Handler for "scroll-event" signal */

static void micpulse_mouse_scrolled (GtkScale *scale, GdkEventScroll *evt, VolumePulsePlugin *vol)
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

    volumepulse_update_display (vol);
}

/* Update icon and tooltip */

void volumepulse_update_display (VolumePulsePlugin *vol)
{
#ifdef ENABLE_NLS
    textdomain (GETTEXT_PACKAGE);
#endif

    pulse_count_devices (vol, TRUE);
    if (vol->pa_devices + bluetooth_count_devices (vol, TRUE))
    {
        gtk_widget_show_all (vol->plugin);
        gtk_widget_set_sensitive (vol->plugin, TRUE);
    }
    else
    {
        gtk_widget_hide (vol->plugin);
        gtk_widget_set_sensitive (vol->plugin, FALSE);
    }

    /* read current mute and volume status */
    gboolean mute = pulse_get_mute (vol, TRUE);
    int level = pulse_get_volume (vol, TRUE);
    if (mute) level = 0;

    /* update icon */
    const char *icon = "audio-input-microphone";
    if (mute) icon = "audio-input-mic-muted";
    lxpanel_plugin_set_taskbar_icon (vol->panel, vol->tray_icon, icon);

    /* update popup window controls */
    if (vol->popup_window)
    {
        g_signal_handler_block (vol->popup_mute_check, vol->mute_check_handler);
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (vol->popup_mute_check), mute);
        g_signal_handler_unblock (vol->popup_mute_check, vol->mute_check_handler);

        g_signal_handler_block (vol->popup_volume_scale, vol->volume_scale_handler);
        gtk_range_set_value (GTK_RANGE (vol->popup_volume_scale), level);
        g_signal_handler_unblock (vol->popup_volume_scale, vol->volume_scale_handler);

        gtk_widget_set_sensitive (vol->popup_volume_scale, !mute);
    }

    /* update tooltip */
    char *tooltip = g_strdup_printf ("%s %d", _("Mic volume"), level);
    gtk_widget_set_tooltip_text (vol->plugin, tooltip);
    g_free (tooltip);
}

/* Handler for icon theme change event from panel */

static void micpulse_theme_change (GtkWidget *widget, VolumePulsePlugin *vol)
{
    volumepulse_update_display (vol);
}

/*----------------------------------------------------------------------------*/
/* Plugin structure                                                           */
/*----------------------------------------------------------------------------*/

/* Callback when panel configuration changes */

static void micpulse_panel_configuration_changed (LXPanel *panel, GtkWidget *plugin)
{
    VolumePulsePlugin *vol = lxpanel_plugin_get_data (plugin);

    volumepulse_update_display (vol);
}

/* Callback when control message arrives */

static gboolean micpulse_control_msg (GtkWidget *plugin, const char *cmd)
{
    VolumePulsePlugin *vol = lxpanel_plugin_get_data (plugin);

    if (!strncmp (cmd, "mute", 4))
    {
        pulse_set_mute (vol, pulse_get_mute (vol, TRUE) ? 0 : 1, TRUE);
        volumepulse_update_display (vol);
        return TRUE;
    }

    if (!strncmp (cmd, "volu", 4))
    {
        if (pulse_get_mute (vol, TRUE)) pulse_set_mute (vol, 0, TRUE);
        else
        {
            int volume = pulse_get_volume (vol, TRUE);
            if (volume < 100)
            {
                volume += 5;
                volume /= 5;
                volume *= 5;
            }
            pulse_set_volume (vol, volume, TRUE);
        }
        volumepulse_update_display (vol);
        return TRUE;
    }

    if (!strncmp (cmd, "vold", 4))
    {
        if (pulse_get_mute (vol, TRUE)) pulse_set_mute (vol, 0, TRUE);
        else
        {
            int volume = pulse_get_volume (vol, TRUE);
            if (volume > 0)
            {
                volume -= 1; // effectively -5 + 4 for rounding...
                volume /= 5;
                volume *= 5;
            }
            pulse_set_volume (vol, volume, TRUE);
        }
        volumepulse_update_display (vol);
        return TRUE;
    }

    return FALSE;
}

/* Plugin constructor */

static gboolean init_check (gpointer data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) data;

    volumepulse_update_display (vol);

    return FALSE;
}

static GtkWidget *micpulse_constructor (LXPanel *panel, config_setting_t *settings)
{
    /* Allocate and initialize plugin context and set into plugin private data pointer */
    VolumePulsePlugin *vol = g_new0 (VolumePulsePlugin, 1);

#ifdef ENABLE_NLS
    setlocale (LC_ALL, "");
    bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);
#endif

    vol->menu_devices = NULL;
    vol->popup_window = NULL;
    vol->profiles_dialog = NULL;
    vol->conn_dialog = NULL;

    /* Allocate top level widget and set into plugin widget pointer */
    vol->panel = panel;
    vol->settings = settings;
    vol->plugin = gtk_button_new ();
    lxpanel_plugin_set_data (vol->plugin, vol, micpulse_destructor);

    /* Allocate icon as a child of top level */
    vol->tray_icon = gtk_image_new ();
    gtk_container_add (GTK_CONTAINER (vol->plugin), vol->tray_icon);

    /* Set up button */
    gtk_button_set_relief (GTK_BUTTON (vol->plugin), GTK_RELIEF_NONE);
    gtk_widget_add_events (vol->plugin, GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK);
    gtk_widget_set_tooltip_text (vol->plugin, _("Mic volume"));

    /* Connect signals */
    g_signal_connect (vol->plugin, "button-press-event", G_CALLBACK (micpulse_button_press_event), vol);
    g_signal_connect (vol->plugin, "scroll-event", G_CALLBACK (micpulse_mouse_scrolled), vol);
    g_signal_connect (panel_get_icon_theme (panel), "changed", G_CALLBACK (micpulse_theme_change), vol);

    /* Delete any old ALSA config */
    vsystem ("rm -f ~/.asoundrc");

    /* Set up PulseAudio */
    pulse_init (vol);

    /* Set up Bluez D-Bus interface */
    bluetooth_init (vol, FALSE);

    /* Update the display, show the widget, and return */
    gtk_widget_show_all (vol->plugin);

    g_idle_add (init_check, vol);

    return vol->plugin;
}

/* Plugin destructor */

static void micpulse_destructor (gpointer user_data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) user_data;

    close_widget (&vol->profiles_dialog);
    close_widget (&vol->conn_dialog);
    close_widget (&vol->popup_window);
    close_widget (&vol->menu_devices);

    g_signal_handlers_disconnect_by_func (panel_get_icon_theme (vol->panel), G_CALLBACK (micpulse_theme_change), vol);

    bluetooth_terminate (vol);
    pulse_terminate (vol);

    /* Deallocate all memory. */
    g_free (vol);
}

FM_DEFINE_MODULE (lxpanel_gtk, micpulse)

/* Plugin descriptor */

LXPanelPluginInit fm_module_init_lxpanel_gtk =
{
    .name = N_("Microphone Control (PulseAudio)"),
    .description = N_("Display and control microphones for PulseAudio"),
    .new_instance = micpulse_constructor,
    .reconfigure = micpulse_panel_configuration_changed,
    .control = micpulse_control_msg,
    .gettext_package = GETTEXT_PACKAGE
};

/* End of file */
/*----------------------------------------------------------------------------*/
