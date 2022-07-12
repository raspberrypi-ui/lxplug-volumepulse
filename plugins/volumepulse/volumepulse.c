/*
Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
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
static void hdmi_init (VolumePulsePlugin *vol);
static const char *device_display_name (VolumePulsePlugin *vol, const char *name);

/* Volume popup */
static void popup_window_show (GtkWidget *p);
static void popup_window_scale_changed (GtkRange *range, VolumePulsePlugin *vol);
static void popup_window_mute_toggled (GtkWidget *widget, VolumePulsePlugin *vol);
static gboolean popup_mapped (GtkWidget *widget, GdkEvent *event, VolumePulsePlugin *vol);
static gboolean popup_button_press (GtkWidget *widget, GdkEventButton *event, VolumePulsePlugin *vol);

/* Menu popup */
static void menu_show (VolumePulsePlugin *vol);
static void menu_mark_default (GtkWidget *widget, gpointer data);
static void menu_set_alsa_output (GtkWidget *widget, VolumePulsePlugin *vol);
static void menu_set_alsa_input (GtkWidget *widget, VolumePulsePlugin *vol);
static void menu_set_bluetooth_output (GtkWidget *widget, VolumePulsePlugin *vol);
static void menu_set_bluetooth_input (GtkWidget *widget, VolumePulsePlugin *vol);
static void menu_open_profile_dialog (GtkWidget *widget, VolumePulsePlugin *vol);

/* Profiles dialog */
static void profiles_dialog_show (VolumePulsePlugin *vol);
static void profiles_dialog_relocate_last_item (GtkWidget *box);
static void profiles_dialog_combo_changed (GtkComboBox *combo, VolumePulsePlugin *vol);
static void profiles_dialog_ok (GtkButton *button, VolumePulsePlugin *vol);
static gboolean profiles_dialog_delete (GtkWidget *wid, GdkEvent *event, VolumePulsePlugin *vol);

/* Handlers and graphics */
static gboolean volumepulse_button_press_event (GtkWidget *widget, GdkEventButton *event, VolumePulsePlugin *vol);
static void volumepulse_menu_set_position (GtkWidget *menu, gint *px, gint *py, gboolean *push_in, VolumePulsePlugin *vol);
static void volumepulse_mouse_scrolled (GtkScale *scale, GdkEventScroll *evt, VolumePulsePlugin *vol);
static void volumepulse_theme_change (GtkWidget *widget, VolumePulsePlugin *vol);

/* Plugin */
static void volumepulse_panel_configuration_changed (LXPanel *panel, GtkWidget *plugin);
static gboolean volumepulse_control_msg (GtkWidget *plugin, const char *cmd);
static GtkWidget *volumepulse_constructor (LXPanel *panel, config_setting_t *settings);
static void volumepulse_destructor (gpointer user_data);

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

/* Find number of HDMI devices and device names */

static void hdmi_init (VolumePulsePlugin *vol)
{
    int i, m;

    /* check xrandr for connected monitors */
    m = get_value ("xrandr -q | grep -c connected");
    if (m < 0) m = 1; /* couldn't read, so assume 1... */
    if (m > 2) m = 2;

    vol->hdmi_names[0] = NULL;
    vol->hdmi_names[1] = NULL;

    /* get the names */
    if (m == 2)
    {
        for (i = 0; i < 2; i++)
        {
            vol->hdmi_names[i] = get_string ("xrandr --listmonitors | grep %d: | cut -d ' ' -f 6", i);
        }

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

/* Remap internal to display names for BCM devices */

static const char *device_display_name (VolumePulsePlugin *vol, const char *name)
{
    if (!g_strcmp0 (name, "bcm2835 HDMI 1")) return vol->hdmi_names[0];
    else if (!g_strcmp0 (name, "vc4-hdmi")) return vol->hdmi_names[0];
    else if (!g_strcmp0 (name, "vc4-hdmi-0")) return vol->hdmi_names[0];
    else if (!g_strcmp0 (name, "bcm2835 HDMI 2")) return vol->hdmi_names[1];
    else if (!g_strcmp0 (name, "vc4-hdmi-1")) return vol->hdmi_names[1];
    else if (!g_strcmp0 (name, "bcm2835 Headphones")) return _("AV Jack");
    else return name;
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
    g_signal_connect (vol->popup_volume_scale, "scroll-event", G_CALLBACK (volumepulse_mouse_scrolled), vol);

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
    if (pulse_get_mute (vol, FALSE)) return;

    /* Update the PulseAudio volume */
    pulse_set_volume (vol, gtk_range_get_value (range), FALSE);

    volumepulse_update_display (vol);
}

/* Handler for "toggled" signal on popup window mute checkbox */

static void popup_window_mute_toggled (GtkWidget *widget, VolumePulsePlugin *vol)
{
    /* Toggle the PulseAudio mute */
    pulse_set_mute (vol, gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget)), FALSE);

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
    vol->menu_inputs = NULL;

    // add ALSA inputs
    //pulse_add_devices_to_menu (vol, TRUE, FALSE);

    // add Bluetooth inputs
    //bluetooth_add_devices_to_menu (vol, TRUE);

    // create a submenu for the outputs if there is an input submenu
    if (vol->menu_inputs)
    {
        vol->menu_outputs = gtk_menu_new ();
        gtk_widget_set_name (vol->menu_devices, "panelmenu");
    }
    else vol->menu_outputs = vol->menu_devices;

    // add internal outputs
    pulse_add_devices_to_menu (vol, FALSE, TRUE);

    // add external outputs
    pulse_add_devices_to_menu (vol, FALSE, FALSE);

    // add Bluetooth devices
    bluetooth_add_devices_to_menu (vol, FALSE);

    // did we find any output devices? if not, the menu will be empty...
    items = gtk_container_get_children (GTK_CONTAINER (vol->menu_outputs));
    if (items != NULL)
    {
        if (vol->menu_inputs)
        {
            // insert submenus
            mi = gtk_menu_item_new_with_label (_("Audio Outputs"));
            gtk_menu_item_set_submenu (GTK_MENU_ITEM (mi), vol->menu_outputs);
            gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_devices), mi);

            mi = gtk_separator_menu_item_new ();
            gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_devices), mi);

            mi = gtk_menu_item_new_with_label (_("Audio Inputs"));
            gtk_menu_item_set_submenu (GTK_MENU_ITEM (mi), vol->menu_inputs);
            gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_devices), mi);
        }

        // add the profiles menu item to the top level menu
        mi = gtk_separator_menu_item_new ();
        gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_devices), mi);

        mi = gtk_menu_item_new_with_label (_("Device Profiles..."));
        g_signal_connect (mi, "activate", G_CALLBACK (menu_open_profile_dialog), (gpointer) vol);
        gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_devices), mi);

        g_list_free (items);
    }
    else
    {
        mi = gtk_menu_item_new_with_label (_("No audio devices found"));
        gtk_widget_set_sensitive (GTK_WIDGET (mi), FALSE);
        gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_devices), mi);
    }

    // update the menu item names, which are currently ALSA device names, to PulseAudio sink/source names
    pulse_update_devices_in_menu (vol);

    // show the default sink and source in the menu
    pulse_get_default_sink_source (vol);
    if (vol->menu_outputs) gtk_container_foreach (GTK_CONTAINER (vol->menu_outputs), menu_mark_default, vol);
    if (vol->menu_inputs) gtk_container_foreach (GTK_CONTAINER (vol->menu_inputs), menu_mark_default, vol);

    // lock menu if a dialog is open
    if (vol->conn_dialog || vol->profiles_dialog)
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
    const char *disp_label = device_display_name (vol, label);

    GtkWidget *mi = gtk_check_menu_item_new_with_label (disp_label);
    gtk_widget_set_name (mi, name);
    if (strstr (name, "bluez"))
    {
        if (input) g_signal_connect (mi, "activate", G_CALLBACK (menu_set_bluetooth_input), (gpointer) vol);
        else g_signal_connect (mi, "activate", G_CALLBACK (menu_set_bluetooth_output), (gpointer) vol);
    }
    else
    {
        if (input) g_signal_connect (mi, "activate", G_CALLBACK (menu_set_alsa_input), (gpointer) vol);
        else g_signal_connect (mi, "activate", G_CALLBACK (menu_set_alsa_output), (gpointer) vol);
        gtk_widget_set_sensitive (mi, FALSE);
        if (input)
            gtk_widget_set_tooltip_text (mi, _("Input from this device not available in the current profile"));
        else
            gtk_widget_set_tooltip_text (mi, _("Output to this device not available in the current profile"));
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
        if (g_strcmp0 (disp_label, gtk_menu_item_get_label (GTK_MENU_ITEM (l->data))) < 0) break;
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

    if (gtk_widget_get_parent (widget) == vol->menu_outputs) def = vol->pa_default_sink;
    else if (gtk_widget_get_parent (widget) == vol->menu_inputs) def = vol->pa_default_source;
    else return;
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

/* Handler for menu click to set an ALSA device as output */

static void menu_set_alsa_output (GtkWidget *widget, VolumePulsePlugin *vol)
{
    bluetooth_remove_output (vol);
    pulse_unmute_all_streams (vol);
    pulse_change_sink (vol, gtk_widget_get_name (widget));
    pulse_move_output_streams (vol);
    volumepulse_update_display (vol);
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

/* Handler for menu click to set a Bluetooth device as output */

static void menu_set_bluetooth_output (GtkWidget *widget, VolumePulsePlugin *vol)
{
    bluetooth_set_output (vol, gtk_widget_get_name (widget), gtk_menu_item_get_label (GTK_MENU_ITEM (widget)));
}

/* Handler for menu click to set a Bluetooth device as input */

static void menu_set_bluetooth_input (GtkWidget *widget, VolumePulsePlugin *vol)
{
    bluetooth_set_input (vol, gtk_widget_get_name (widget), gtk_menu_item_get_label (GTK_MENU_ITEM (widget)));
}

/* Handler for menu click to open the profiles dialog */

static void menu_open_profile_dialog (GtkWidget *widget, VolumePulsePlugin *vol)
{
    profiles_dialog_show (vol);
}

/*----------------------------------------------------------------------------*/
/* Profiles dialog                                                            */
/*----------------------------------------------------------------------------*/

/* Create the profiles dialog */

static void profiles_dialog_show (VolumePulsePlugin *vol)
{
    GtkWidget *btn, *wid, *box;
    char *lbl;

    // create the window itself
    vol->profiles_dialog = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title (GTK_WINDOW (vol->profiles_dialog), _("Device Profiles"));
    gtk_window_set_position (GTK_WINDOW (vol->profiles_dialog), GTK_WIN_POS_CENTER);
    gtk_window_set_default_size (GTK_WINDOW (vol->profiles_dialog), 400, -1);
    gtk_container_set_border_width (GTK_CONTAINER (vol->profiles_dialog), 10);
    gtk_window_set_icon_name (GTK_WINDOW (vol->profiles_dialog), "multimedia-volume-control");
    g_signal_connect (vol->profiles_dialog, "delete-event", G_CALLBACK (profiles_dialog_delete), vol);

    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 5);
    vol->profiles_int_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 5);
    vol->profiles_ext_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 5);
    vol->profiles_bt_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add (GTK_CONTAINER (vol->profiles_dialog), box);
    gtk_box_pack_start (GTK_BOX (box), vol->profiles_int_box, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), vol->profiles_ext_box, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), vol->profiles_bt_box, FALSE, FALSE, 0);

    // first loop through cards
    pulse_add_devices_to_profile_dialog (vol);

    // then loop through Bluetooth devices
    bluetooth_add_devices_to_profile_dialog (vol);

    wid = gtk_button_box_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout (GTK_BUTTON_BOX (wid), GTK_BUTTONBOX_END);
    gtk_box_pack_start (GTK_BOX (box), wid, FALSE, FALSE, 5);

    btn = gtk_button_new_with_mnemonic (_("_OK"));
    g_signal_connect (btn, "clicked", G_CALLBACK (profiles_dialog_ok), vol);
    gtk_box_pack_end (GTK_BOX (wid), btn, FALSE, FALSE, 5);

    gtk_widget_show_all (vol->profiles_dialog);
}

/* Add a title and combo box to the profiles dialog */

void profiles_dialog_add_combo (VolumePulsePlugin *vol, GtkListStore *ls, GtkWidget *dest, int sel, const char *label, const char *name)
{
    GtkWidget *lbl, *comb;
    GtkCellRenderer *rend;
    char *ltext;

    ltext = g_strdup_printf ("%s:", device_display_name (vol, label));
    lbl = gtk_label_new (ltext);
    gtk_label_set_xalign (GTK_LABEL (lbl), 0.0);
    gtk_box_pack_start (GTK_BOX (dest), lbl, FALSE, FALSE, 5);
    g_free (ltext);

    if (ls)
    {
        comb = gtk_combo_box_new_with_model (GTK_TREE_MODEL (ls));
        gtk_widget_set_name (comb, name);
        rend = gtk_cell_renderer_text_new ();
        gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (comb), rend, FALSE);
        gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (comb), rend, "text", 1);
    }
    else
    {
        comb = gtk_combo_box_text_new ();
        gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (comb), _("Device not connected"));
        gtk_widget_set_sensitive (comb, FALSE);
    }
    gtk_combo_box_set_active (GTK_COMBO_BOX (comb), sel);
    gtk_box_pack_start (GTK_BOX (dest), comb, FALSE, FALSE, 5);

    profiles_dialog_relocate_last_item (dest);

    if (ls) g_signal_connect (comb, "changed", G_CALLBACK (profiles_dialog_combo_changed), vol);
}

/* Alphabetically relocate the last item added to the profiles dialog */

static void profiles_dialog_relocate_last_item (GtkWidget *box)
{
    GtkWidget *elem, *newcomb, *newlab;
    GList *children = gtk_container_get_children (GTK_CONTAINER (box));
    int n = g_list_length (children);
    newcomb = g_list_nth_data (children, n - 1);
    newlab = g_list_nth_data (children, n - 2);
    const char *new_item = gtk_label_get_text (GTK_LABEL (newlab));
    n -= 2;
    while (n > 0)
    {
        elem = g_list_nth_data (children, n - 2);
        if (g_strcmp0 (new_item, gtk_label_get_text (GTK_LABEL (elem))) >= 0) break;
        n -= 2;
    }
    gtk_box_reorder_child (GTK_BOX (box), newlab, n);
    gtk_box_reorder_child (GTK_BOX (box), newcomb, n + 1);
    g_list_free (children);
}

/* Handler for "changed" signal from a profile combo box */

static void profiles_dialog_combo_changed (GtkComboBox *combo, VolumePulsePlugin *vol)
{
    const char *name, *option;
    GtkTreeIter iter;

    name = gtk_widget_get_name (GTK_WIDGET (combo));
    gtk_combo_box_get_active_iter (combo, &iter);
    gtk_tree_model_get (gtk_combo_box_get_model (combo), &iter, 0, &option, -1);
    pulse_set_profile (vol, name, option);

    // need to reconnect a Bluetooth device here to cause the profile to take effect...
    bluetooth_reconnect (vol, name, option);
}

/* Handler for 'OK' button on profiles dialog */

static void profiles_dialog_ok (GtkButton *button, VolumePulsePlugin *vol)
{
    close_widget (&vol->profiles_dialog);
}

/* Handler for "delete-event" signal from profiles dialog */

static gboolean profiles_dialog_delete (GtkWidget *wid, GdkEvent *event, VolumePulsePlugin *vol)
{
    close_widget (&vol->profiles_dialog);
    return TRUE;
}

/*----------------------------------------------------------------------------*/
/* Plugin handlers and graphics                                               */
/*----------------------------------------------------------------------------*/

/* Handler for "button-press-event" signal on main widget. */

static gboolean volumepulse_button_press_event (GtkWidget *widget, GdkEventButton *event, VolumePulsePlugin *vol)
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
                pulse_set_mute (vol, pulse_get_mute (vol, FALSE) ? 0 : 1, FALSE);
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

static void volumepulse_menu_set_position (GtkWidget *menu, gint *px, gint *py, gboolean *push_in, VolumePulsePlugin *vol)
{
    /* Determine the coordinates. */
    lxpanel_plugin_popup_set_position_helper (vol->panel, vol->plugin, menu, px, py);
    *push_in = TRUE;
}

/* Handler for "scroll-event" signal */

static void volumepulse_mouse_scrolled (GtkScale *scale, GdkEventScroll *evt, VolumePulsePlugin *vol)
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

/* Update icon and tooltip */

void volumepulse_update_display (VolumePulsePlugin *vol)
{
#ifdef ENABLE_NLS
    textdomain (GETTEXT_PACKAGE);
#endif

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
    char *tooltip = g_strdup_printf ("%s %d", _("Volume control"), level);
    gtk_widget_set_tooltip_text (vol->plugin, tooltip);
    g_free (tooltip);
}

/* Handler for icon theme change event from panel */

static void volumepulse_theme_change (GtkWidget *widget, VolumePulsePlugin *vol)
{
    volumepulse_update_display (vol);
}

/*----------------------------------------------------------------------------*/
/* Plugin structure                                                           */
/*----------------------------------------------------------------------------*/

/* Callback when panel configuration changes */

static void volumepulse_panel_configuration_changed (LXPanel *panel, GtkWidget *plugin)
{
    VolumePulsePlugin *vol = lxpanel_plugin_get_data (plugin);

    volumepulse_update_display (vol);
}

/* Callback when control message arrives */

static gboolean volumepulse_control_msg (GtkWidget *plugin, const char *cmd)
{
    VolumePulsePlugin *vol = lxpanel_plugin_get_data (plugin);

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
                volume += 5;
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
                volume -= 1; // effectively -5 + 4 for rounding...
                volume /= 5;
                volume *= 5;
            }
            pulse_set_volume (vol, volume, FALSE);
        }
        volumepulse_update_display (vol);
        return TRUE;
    }

    return FALSE;
}

/* Plugin constructor */

static GtkWidget *volumepulse_constructor (LXPanel *panel, config_setting_t *settings)
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
    lxpanel_plugin_set_data (vol->plugin, vol, volumepulse_destructor);

    /* Allocate icon as a child of top level */
    vol->tray_icon = gtk_image_new ();
    gtk_container_add (GTK_CONTAINER (vol->plugin), vol->tray_icon);

    /* Set up button */
    gtk_button_set_relief (GTK_BUTTON (vol->plugin), GTK_RELIEF_NONE);
    gtk_widget_add_events (vol->plugin, GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK);
    gtk_widget_set_tooltip_text (vol->plugin, _("Volume control"));

    /* Connect signals */
    g_signal_connect (vol->plugin, "button-press-event", G_CALLBACK (volumepulse_button_press_event), vol);
    g_signal_connect (vol->plugin, "scroll-event", G_CALLBACK (volumepulse_mouse_scrolled), vol);
    g_signal_connect (panel_get_icon_theme (panel), "changed", G_CALLBACK (volumepulse_theme_change), vol);

    /* Delete any old ALSA config */
    vsystem ("rm -f ~/.asoundrc");

    /* Find HDMIs */
    hdmi_init (vol);

    /* Set up PulseAudio */
    pulse_init (vol);

    /* Set up Bluez D-Bus interface */
    bluetooth_init (vol, TRUE);

    /* Update the display, show the widget, and return */
    volumepulse_update_display (vol);
    gtk_widget_show_all (vol->plugin);
    return vol->plugin;
}

/* Plugin destructor */

static void volumepulse_destructor (gpointer user_data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) user_data;

    close_widget (&vol->profiles_dialog);
    close_widget (&vol->conn_dialog);
    close_widget (&vol->popup_window);
    close_widget (&vol->menu_devices);

    g_signal_handlers_disconnect_by_func (panel_get_icon_theme (vol->panel), G_CALLBACK (volumepulse_theme_change), vol);

    bluetooth_terminate (vol);
    pulse_terminate (vol);

    /* Deallocate all memory. */
    g_free (vol);
}

FM_DEFINE_MODULE (lxpanel_gtk, volumepulse)

/* Plugin descriptor */

LXPanelPluginInit fm_module_init_lxpanel_gtk =
{
    .name = N_("Volume Control (PulseAudio)"),
    .description = N_("Display and control volume for PulseAudio"),
    .new_instance = volumepulse_constructor,
    .reconfigure = volumepulse_panel_configuration_changed,
    .control = volumepulse_control_msg,
    .gettext_package = GETTEXT_PACKAGE
};

/* End of file */
/*----------------------------------------------------------------------------*/
