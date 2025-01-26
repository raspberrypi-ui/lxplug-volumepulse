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

#include <glib/gi18n.h>
#include <glib/gprintf.h>
#include <pulse/pulseaudio.h>

#ifdef LXPLUG
#include "plugin.h"
#else
#include "lxutils.h"
#endif

#include "volumepulse.h"
#include "pulse.h"
#include "bluetooth.h"

#include "commongui.h"

/*----------------------------------------------------------------------------*/
/* Typedefs and macros                                                        */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* Plug-in global data                                                        */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* Prototypes                                                                 */
/*----------------------------------------------------------------------------*/

static void popup_window_scale_changed_vol (GtkRange *range, VolumePulsePlugin *vol);
static void popup_window_mute_toggled_vol (GtkWidget *widget, VolumePulsePlugin *vol);
static void popup_window_scale_changed_mic (GtkRange *range, VolumePulsePlugin *vol);
static void popup_window_mute_toggled_mic (GtkWidget *widget, VolumePulsePlugin *vol);
static void menu_mark_default_input (GtkWidget *widget, gpointer data);
static void menu_mark_default_output (GtkWidget *widget, gpointer data);
static void profiles_dialog_relocate_last_item (GtkWidget *box);
static void profiles_dialog_combo_changed (GtkComboBox *combo, VolumePulsePlugin *vol);
static void profiles_dialog_ok (GtkButton *button, VolumePulsePlugin *vol);
static gboolean profiles_dialog_delete (GtkWidget *wid, GdkEvent *event, VolumePulsePlugin *vol);

/*----------------------------------------------------------------------------*/
/* Function definitions                                                       */
/*----------------------------------------------------------------------------*/

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

/* Destroy a widget and null its pointer */

void close_widget (GtkWidget **wid)
{
    if (*wid)
    {
        gtk_widget_destroy (*wid);
        *wid = NULL;
    }
}

/* Remap internal to display names for BCM devices */

const char *device_display_name (VolumePulsePlugin *vol, const char *name)
{
    if (!g_strcmp0 (name, "bcm2835 HDMI 1")) return vol->hdmi_names[0];
    else if (!g_strcmp0 (name, "vc4-hdmi")) return vol->hdmi_names[0];
    else if (!g_strcmp0 (name, "vc4-hdmi-0")) return vol->hdmi_names[0];
    else if (!g_strcmp0 (name, "bcm2835 HDMI 2")) return vol->hdmi_names[1];
    else if (!g_strcmp0 (name, "vc4-hdmi-1")) return vol->hdmi_names[1];
    else if (!g_strcmp0 (name, "bcm2835 Headphones")) return _("AV Jack");
    else return name;
}

/*----------------------------------------------------------------------------*/
/* Icons                                                                      */
/*----------------------------------------------------------------------------*/

void update_display (VolumePulsePlugin *vol, gboolean input)
{
    const char *icon;

    pulse_count_devices (vol, input);
    if ((!input || !vol->wizard) && vol->pa_devices + bluetooth_count_devices (vol, input) > 0)
    {
        gtk_widget_show_all (vol->plugin[input ? 1 : 0]);
        gtk_widget_set_sensitive (vol->plugin[input ? 1 : 0], TRUE);
    }
    else
    {
        gtk_widget_hide (vol->plugin[input ? 1 : 0]);
        gtk_widget_set_sensitive (vol->plugin[input ? 1 : 0], FALSE);
    }

    /* read current mute and volume status */
    gboolean mute = pulse_get_mute (vol, input);
    int level = pulse_get_volume (vol, input);
    if (mute) level = 0;

    /* update icon */
    if (input)
    {
        if (mute) icon = "audio-input-mic-muted";
        else icon = "audio-input-microphone";
    }
    else
    {
        if (mute) icon = "audio-volume-muted";
        else
        {
            if (level >= 66) icon = "audio-volume-high";
            else if (level >= 33) icon = "audio-volume-medium";
            else if (level > 0) icon = "audio-volume-low";
            else icon = "audio-volume-silent";
        }
    }
    wrap_set_taskbar_icon (vol, vol->tray_icon[input ? 1 : 0], icon);

    /* update popup window controls */
    if (vol->popup_window[input ? 1 : 0])
    {
        g_signal_handler_block (vol->popup_mute_check[input ? 1 : 0], vol->mute_check_handler[input ? 1 : 0]);
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (vol->popup_mute_check[input ? 1 : 0]), mute);
        g_signal_handler_unblock (vol->popup_mute_check[input ? 1 : 0], vol->mute_check_handler[input ? 1 : 0]);

        g_signal_handler_block (vol->popup_volume_scale[input ? 1 : 0], vol->volume_scale_handler[input ? 1 : 0]);
        gtk_range_set_value (GTK_RANGE (vol->popup_volume_scale[input ? 1 : 0]), level);
        g_signal_handler_unblock (vol->popup_volume_scale[input ? 1 : 0], vol->volume_scale_handler[input ? 1 : 0]);

        gtk_widget_set_sensitive (vol->popup_volume_scale[input ? 1 : 0], !mute);
    }

    /* update tooltip */
    char *tooltip = g_strdup_printf ("%s %d", input ? _("Mic volume") : _("Volume control"), level);
    if (!vol->wizard) gtk_widget_set_tooltip_text (vol->plugin[input ? 1 : 0], tooltip);
    g_free (tooltip);
}

/*----------------------------------------------------------------------------*/
/* Volume scale popup window                                                  */
/*----------------------------------------------------------------------------*/

static void vol_destroyed (GtkWidget *widget, gpointer data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) data;
    if (widget == vol->popup_window[0])
    {
        g_signal_handlers_disconnect_matched (vol->popup_window[0], G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, vol);
        g_signal_handlers_disconnect_matched (vol->popup_volume_scale[0], G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, vol);
        g_signal_handlers_disconnect_matched (vol->popup_mute_check[0], G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, vol);
        vol->popup_window[0] = NULL;
    }
    if (widget == vol->popup_window[1])
    {
        g_signal_handlers_disconnect_matched (vol->popup_window[1], G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, vol);
        g_signal_handlers_disconnect_matched (vol->popup_volume_scale[1], G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, vol);
        g_signal_handlers_disconnect_matched (vol->popup_mute_check[1], G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, vol);
        vol->popup_window[1] = NULL;
    }
}

/* Create the pop-up volume window */

void popup_window_show (VolumePulsePlugin *vol, gboolean input_control)
{
    int index = input_control ? 1 : 0;

    /* Create a new window. */
    vol->popup_window[index] = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name (vol->popup_window[index], "panelpopup");

    gtk_container_set_border_width (GTK_CONTAINER (vol->popup_window[index]), 0);

    /* Create a vertical box as the child of the window. */
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add (GTK_CONTAINER (vol->popup_window[index]), box);

    /* Create a vertical scale as the child of the vertical box. */
    vol->popup_volume_scale[index] = gtk_scale_new (GTK_ORIENTATION_VERTICAL, GTK_ADJUSTMENT (gtk_adjustment_new (100, 0, 100, 0, 0, 0)));
    g_object_set (vol->popup_volume_scale[index], "height-request", 120, NULL);
    gtk_scale_set_draw_value (GTK_SCALE (vol->popup_volume_scale[index]), FALSE);
    gtk_range_set_inverted (GTK_RANGE (vol->popup_volume_scale[index]), TRUE);
    gtk_box_pack_start (GTK_BOX (box), vol->popup_volume_scale[index], TRUE, TRUE, 0);
    gtk_widget_set_can_focus (vol->popup_volume_scale[index], FALSE);

    /* Value-changed and scroll-event signals. */
    vol->volume_scale_handler[index] = g_signal_connect (vol->popup_volume_scale[index], "value-changed", input_control ? G_CALLBACK (popup_window_scale_changed_mic) : G_CALLBACK (popup_window_scale_changed_vol), vol);
    g_signal_connect (vol->popup_volume_scale[index], "scroll-event", G_CALLBACK (volumepulse_mouse_scrolled), vol);

    /* Create a check button as the child of the vertical box. */
    vol->popup_mute_check[index] = gtk_check_button_new_with_label (_("Mute"));
    gtk_box_pack_end (GTK_BOX (box), vol->popup_mute_check[index], FALSE, FALSE, 0);
    vol->mute_check_handler[index] = g_signal_connect (vol->popup_mute_check[index], "toggled", input_control ? G_CALLBACK (popup_window_mute_toggled_mic) : G_CALLBACK (popup_window_mute_toggled_vol), vol);
    gtk_widget_set_can_focus (vol->popup_mute_check[index], FALSE);
    g_signal_connect (vol->popup_window[index], "destroy", G_CALLBACK (vol_destroyed), vol);

    /* Realise the window */
    wrap_popup_at_button (vol, vol->popup_window[index], vol->plugin[index]);
}

/* Handler for "value_changed" signal on popup window vertical scale */

static void popup_window_scale_changed_vol (GtkRange *range, VolumePulsePlugin *vol)
{
    if (pulse_get_mute (vol, FALSE)) return;

    /* Update the PulseAudio volume */
    pulse_set_volume (vol, gtk_range_get_value (range), FALSE);

    update_display (vol, FALSE);
}

static void popup_window_scale_changed_mic (GtkRange *range, VolumePulsePlugin *vol)
{
    if (pulse_get_mute (vol, TRUE)) return;

    /* Update the PulseAudio volume */
    pulse_set_volume (vol, gtk_range_get_value (range), TRUE);

    update_display (vol, TRUE);
}

/* Handler for "toggled" signal on popup window mute checkbox */

static void popup_window_mute_toggled_vol (GtkWidget *widget, VolumePulsePlugin *vol)
{
    /* Toggle the PulseAudio mute */
    pulse_set_mute (vol, gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget)), FALSE);

    update_display (vol, FALSE);
}

static void popup_window_mute_toggled_mic (GtkWidget *widget, VolumePulsePlugin *vol)
{
    /* Toggle the PulseAudio mute */
    pulse_set_mute (vol, gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget)), TRUE);

    update_display (vol, TRUE);
}

/*----------------------------------------------------------------------------*/
/* Device select menu                                                         */
/*----------------------------------------------------------------------------*/

/* Create the device select menu */

gboolean menu_create (VolumePulsePlugin *vol, gboolean input_control)
{
    GtkWidget *mi;
    GList *items;
    int index = input_control ? 1 : 0;

    // create input selector
    if (vol->menu_devices[index]) gtk_widget_destroy (vol->menu_devices[index]);
    vol->menu_devices[index] = gtk_menu_new ();
    gtk_widget_set_name (vol->menu_devices[index], "panelmenu");

    // add internal devices
    pulse_add_devices_to_menu (vol, TRUE, input_control);

    // add ALSA devices
    pulse_add_devices_to_menu (vol, FALSE, input_control);

    // add Bluetooth devices
    bluetooth_add_devices_to_menu (vol, input_control);

    // update the menu item names, which are currently ALSA device names, to PulseAudio sink/source names
    pulse_update_devices_in_menu (vol, input_control);

    // show the default sink and source in the menu
    pulse_get_default_sink_source (vol);
    gtk_container_foreach (GTK_CONTAINER (vol->menu_devices[index]), input_control ? menu_mark_default_input : menu_mark_default_output, vol);

    // did we find any devices? if not, the menu will be empty...
    items = gtk_container_get_children (GTK_CONTAINER (vol->menu_devices[index]));
    if (items == NULL)
    {
        mi = gtk_menu_item_new_with_label (_("No audio devices found"));
        gtk_widget_set_sensitive (GTK_WIDGET (mi), FALSE);
        gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_devices[index]), mi);
        return FALSE;
    }
    else g_list_free (items);
    return TRUE;
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
    }
    vol->separator = TRUE;
    g_list_free (list);
}

/* Add a tickmark to the supplied widget if it is the default item in its parent menu */

void menu_mark_default_output (GtkWidget *widget, gpointer data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) data;
    const char *def, *wid = gtk_widget_get_name (widget);

    def = vol->pa_default_sink;
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

void menu_mark_default_input (GtkWidget *widget, gpointer data)
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

/* Handler for menu click to set an ALSA device as output or input */

void menu_set_alsa_device_output (GtkWidget *widget, VolumePulsePlugin *vol)
{
    pulse_change_sink (vol, gtk_widget_get_name (widget));
    pulse_move_output_streams (vol);
    update_display (vol, FALSE);
}

void menu_set_alsa_device_input (GtkWidget *widget, VolumePulsePlugin *vol)
{
    pulse_change_source (vol, gtk_widget_get_name (widget));
    pulse_move_input_streams (vol);
    update_display (vol, TRUE);
}

/* Handler for menu click to set a Bluetooth device as output or input */

void menu_set_bluetooth_device_output (GtkWidget *widget, VolumePulsePlugin *vol)
{
    bluetooth_set_output (vol, gtk_widget_get_name (widget), gtk_menu_item_get_label (GTK_MENU_ITEM (widget)));
}

void menu_set_bluetooth_device_input (GtkWidget *widget, VolumePulsePlugin *vol)
{
    bluetooth_set_input (vol, gtk_widget_get_name (widget), gtk_menu_item_get_label (GTK_MENU_ITEM (widget)));
}

/*----------------------------------------------------------------------------*/
/* Profiles dialog                                                            */
/*----------------------------------------------------------------------------*/

/* Create the profiles dialog */

void profiles_dialog_show (VolumePulsePlugin *vol)
{
    GtkWidget *btn, *wid, *box;

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
}

/* Handler for 'OK' button on profiles dialog */

static void profiles_dialog_ok (GtkButton *, VolumePulsePlugin *vol)
{
    close_widget (&vol->profiles_dialog);
}

/* Handler for "delete-event" signal from profiles dialog */

static gboolean profiles_dialog_delete (GtkWidget *, GdkEvent *, VolumePulsePlugin *vol)
{
    close_widget (&vol->profiles_dialog);
    return TRUE;
}

/* End of file */
/*----------------------------------------------------------------------------*/
