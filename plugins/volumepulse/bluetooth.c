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

#include "volumepulse.h"
#include "pulse.h"
#include "bluetooth.h"

/*----------------------------------------------------------------------------*/
/* Local macros and definitions                                               */
/*----------------------------------------------------------------------------*/

#define BT_SERV_AUDIO_SOURCE    "0000110A"
#define BT_SERV_AUDIO_SINK      "0000110B"
#define BT_SERV_HSP             "00001108"
#define BT_SERV_HFP             "0000111E"

#define BT_PULSE_RETRIES    25000

typedef enum {
    CONNECT,
    DISCONNECT,
    RECONNECT
} bt_cd_t;

typedef enum {
    INPUT,
    OUTPUT,
    BOTH
} bt_dir_t;

typedef struct {
    const char *device;
    bt_cd_t conn_disc;
    bt_dir_t direction;
} bt_operation_t;

/*----------------------------------------------------------------------------*/
/* Static function prototypes                                                 */
/*----------------------------------------------------------------------------*/

static void bt_add_operation (VolumePulsePlugin *vol, const char *device, bt_cd_t cd, bt_dir_t dir);
static void bt_do_operation (VolumePulsePlugin *vol);
static void bt_next_operation (VolumePulsePlugin *vol);
static char *bt_to_pa_name (const char *bluez_name, char *type, char *profile);
static char *bt_from_pa_name (const char *pa_name);
static int bt_sink_source_compare (char *sink, char *source);
static void bt_cb_name_owned (GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer user_data);
static void bt_cb_name_unowned (GDBusConnection *connection, const gchar *name, gpointer user_data);
static void bt_cb_object_added (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data);
static void bt_cb_object_removed (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data);
static void bt_connect_device (VolumePulsePlugin *vol, const char *device);
static void bt_cb_connected (GObject *source, GAsyncResult *res, gpointer user_data);
static gboolean bt_get_profile (gpointer user_data);
static void bt_cb_trusted (GObject *source, GAsyncResult *res, gpointer user_data);
static void bt_disconnect_device (VolumePulsePlugin *vol, const char *device);
static void bt_cb_disconnected (GObject *source, GAsyncResult *res, gpointer user_data);
static gboolean bt_has_service (VolumePulsePlugin *vol, const gchar *path, const gchar *service);
static void bt_connect_dialog_show (VolumePulsePlugin *vol, const char *fmt, ...);
static void bt_connect_dialog_update (VolumePulsePlugin *vol, const char *msg);
static void bt_connect_dialog_ok (GtkButton *button, VolumePulsePlugin *vol);
static gboolean bt_connect_dialog_delete (GtkWidget *widget, GdkEvent *event, VolumePulsePlugin *vol);

/*----------------------------------------------------------------------------*/
/* Bluetooth operation list management                                        */
/*----------------------------------------------------------------------------*/

/* Add an operation to the list */

static void bt_add_operation (VolumePulsePlugin *vol, const char *device, bt_cd_t cd, bt_dir_t dir)
{
    bt_operation_t *newop = malloc (sizeof (bt_operation_t));

    newop->device = device;
    newop->conn_disc = cd;
    newop->direction = dir;

    vol->bt_ops = g_list_append (vol->bt_ops, newop);
}

/* Do the first operation in the list */

static void bt_do_operation (VolumePulsePlugin *vol)
{
    if (vol->bt_ops)
    {
        bt_operation_t *btop = (bt_operation_t *) vol->bt_ops->data;

        if (btop->conn_disc == DISCONNECT)
        {
            bt_disconnect_device (vol, btop->device);
        }
        else
        {
            bt_connect_device (vol, btop->device);
        }
    }
}

/* Advance to the next operation in the list */

static void bt_next_operation (VolumePulsePlugin *vol)
{
    if (vol->bt_ops)
    {
        bt_operation_t *btop = (bt_operation_t *) vol->bt_ops->data;
        g_free (btop);
        vol->bt_ops = vol->bt_ops->next;
    }
    if (vol->bt_ops == NULL)
    {
        if (vol->bt_oname) g_free (vol->bt_oname);
        if (vol->bt_iname) g_free (vol->bt_iname);
        vol->bt_oname = NULL;
        vol->bt_iname = NULL;

        // operations now all finished - move all streams to default devices
        pulse_get_default_sink_source (vol);
        pulse_move_output_streams (vol);
        pulse_move_input_streams (vol);
    }
    else bt_do_operation (vol);
}

/*----------------------------------------------------------------------------*/
/* Bluetooth name remapping                                                   */
/*----------------------------------------------------------------------------*/

/* Convert the BlueZ name of a device to a PulseAudio sink / source / card */

static char *bt_to_pa_name (const char *bluez_name, char *type, char *profile)
{
    unsigned int b1, b2, b3, b4, b5, b6;

    if (bluez_name == NULL) return NULL;
    if (sscanf (bluez_name, "/org/bluez/hci0/dev_%x_%x_%x_%x_%x_%x", &b1, &b2, &b3, &b4, &b5, &b6) != 6)
    {
        DEBUG ("Bluez name invalid : %s", bluez_name);
        return NULL;
    }
    return g_strdup_printf ("bluez_%s.%02X_%02X_%02X_%02X_%02X_%02X%s%s",
        type, b1, b2, b3, b4, b5, b6, profile ? "." : "", profile ? profile : "");
}

/* Convert a PulseAudio sink / source / card to a BlueZ device name */

static char *bt_from_pa_name (const char *pa_name)
{
    unsigned int b1, b2, b3, b4, b5, b6;
    char *adrs;

    if (pa_name == NULL) return NULL;
    if (strstr (pa_name, "bluez") == NULL) return NULL;
    adrs = strstr (pa_name, ".");
    if (adrs == NULL) return NULL;
    if (sscanf (adrs + 1, "%x_%x_%x_%x_%x_%x", &b1, &b2, &b3, &b4, &b5, &b6) != 6) return NULL;
    return g_strdup_printf ("/org/bluez/hci0/dev_%02X_%02X_%02X_%02X_%02X_%02X", b1, b2, b3, b4, b5, b6);
}

/* Compare a PulseAudio sink and source to see if they are the same BlueZ device */

static int bt_sink_source_compare (char *sink, char *source)
{
    if (sink == NULL || source == NULL) return 1;
    if (strstr (sink, "bluez") == NULL) return 1;
    if (strstr (source, "bluez") == NULL) return 1;
    return strncmp (sink + 11, source + 13, 17);
}

/*----------------------------------------------------------------------------*/
/* Bluetooth D-Bus interface                                                  */
/*----------------------------------------------------------------------------*/

/* Callback for BlueZ appearing on D-Bus */

static void bt_cb_name_owned (GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer user_data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) user_data;
    DEBUG ("Name %s owned on D-Bus", name);

    /* BlueZ exists - get an object manager for it */
    GError *error = NULL;
    vol->bt_objmanager = g_dbus_object_manager_client_new_for_bus_sync (G_BUS_TYPE_SYSTEM, 0, "org.bluez", "/", NULL, NULL, NULL, NULL, &error);
    if (error)
    {
        DEBUG ("Error getting object manager - %s", error->message);
        vol->bt_objmanager = NULL;
        g_error_free (error);
    }
    else
    {
        /* register callbacks for devices being added or removed */
        //g_signal_connect (vol->bt_objmanager, "object-added", G_CALLBACK (bt_cb_object_added), vol);
        //g_signal_connect (vol->bt_objmanager, "object-removed", G_CALLBACK (bt_cb_object_removed), vol);

        DEBUG ("Reconnecting devices");
        vol->bt_oname = get_string ("cat ~/.btout 2> /dev/null");
        if (!g_strcmp0 (vol->bt_oname, ""))
        {
            g_free (vol->bt_oname);
            vol->bt_oname = NULL;
        }
        vol->bt_iname = get_string ("cat ~/.btin 2> /dev/null");
        if (!g_strcmp0 (vol->bt_iname, ""))
        {
            g_free (vol->bt_iname);
            vol->bt_iname = NULL;
        }

        if (vol->bt_oname || vol->bt_iname) bt_connect_dialog_show (vol, _("Reconnecting Bluetooth devices..."));
        if (vol->bt_oname) bt_add_operation (vol, vol->bt_oname, DISCONNECT, OUTPUT);
        if (vol->bt_iname) bt_add_operation (vol, vol->bt_iname, DISCONNECT, INPUT);
        if (vol->bt_oname)
        {
            if (!g_strcmp0 (vol->bt_oname, vol->bt_iname)) bt_add_operation (vol, vol->bt_oname, RECONNECT, BOTH);
            else bt_add_operation (vol, vol->bt_oname, RECONNECT, OUTPUT);
        }
        if (vol->bt_iname && g_strcmp0 (vol->bt_oname, vol->bt_iname)) bt_add_operation (vol, vol->bt_iname, RECONNECT, INPUT);
        vol->bt_input = vol->bt_iname ? TRUE : FALSE;
        vol->bt_force_hsp = FALSE;

        bt_do_operation (vol);
    }
}

/* Callback for BlueZ disappearing on D-Bus */

static void bt_cb_name_unowned (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) user_data;
    DEBUG ("Name %s unowned on D-Bus", name);

    if (vol->bt_objmanager)
    {
        //g_signal_handlers_disconnect_by_func (vol->bt_objmanager, G_CALLBACK (bt_cb_object_added), vol);
        //g_signal_handlers_disconnect_by_func (vol->bt_objmanager, G_CALLBACK (bt_cb_object_removed), vol);
        g_object_unref (vol->bt_objmanager);
    }
    vol->bt_objmanager = NULL;
}

/* Callback for BlueZ device connecting */

static void bt_cb_object_added (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) user_data;
    const char *obj = g_dbus_object_get_object_path (object);

    DEBUG ("Bluetooth object %s added", obj);
    pulse_get_default_sink_source (vol);
    char *device = bt_from_pa_name (vol->pa_default_sink);
    char *idevice = bt_from_pa_name (vol->pa_default_source);
    if (g_strcmp0 (obj, device) || g_strcmp0 (obj, idevice))
    {
        DEBUG ("Selected Bluetooth audio device has connected");
        volumepulse_update_display (vol);
        //!!!!! do we need to do something else here?
    }
    if (device) g_free (device);
    if (idevice) g_free (idevice);
}

/* Callback for BlueZ device disconnecting */

static void bt_cb_object_removed (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) user_data;
    const char *obj = g_dbus_object_get_object_path (object);

    DEBUG ("Bluetooth object %s removed", obj);
    pulse_get_default_sink_source (vol);
    char *device = bt_from_pa_name (vol->pa_default_sink);
    char *idevice = bt_from_pa_name (vol->pa_default_source);
    if (g_strcmp0 (obj, device) || g_strcmp0 (obj, idevice))
    {
        DEBUG ("Selected Bluetooth audio device has disconnected");
        volumepulse_update_display (vol);
        //!!!!! do we need to do something else here?
    }
    if (device) g_free (device);
    if (idevice) g_free (idevice);
}

/* Connect a BlueZ device */

static void bt_connect_device (VolumePulsePlugin *vol, const char *device)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (vol->bt_objmanager, device, "org.bluez.Device1");
    DEBUG ("Connecting device %s...", device);
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
        // should only happen on a reconnect if the device has been un-paired
        bt_operation_t *btop = (bt_operation_t *) vol->bt_ops->data;

        DEBUG ("Couldn't get device interface from object manager");
        char *msg = g_strdup_printf (_("Bluetooth %s device not found"), btop->direction == INPUT ? "input" : "output");
        bt_connect_dialog_update (vol, msg);
        g_free (msg);
        if (btop->conn_disc == RECONNECT)
        {
            if (btop->direction != OUTPUT) vsystem ("rm -f ~/.btin");
            if (btop->direction != INPUT) vsystem ("rm -f ~/.btout");
        }
        bt_next_operation (vol);
    }
}

/* Callback for connect completed */

static void bt_cb_connected (GObject *source, GAsyncResult *res, gpointer user_data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) user_data;
    GError *error = NULL;

    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
    if (var) g_variant_unref (var);

    bt_operation_t *btop = (bt_operation_t *) vol->bt_ops->data;

    if (error)
    {
        DEBUG ("Connect error %s", error->message);

        // update dialog to show a warning
        bt_connect_dialog_update (vol, error->message);
        g_error_free (error);
        if (btop->conn_disc == RECONNECT)
        {
            if (btop->direction != OUTPUT) vsystem ("rm -f ~/.btin");
            if (btop->direction != INPUT) vsystem ("rm -f ~/.btout");
        }
    }
    else
    {
        DEBUG ("Connected OK - polling for profile");

        // start polling for the PulseAudio profile of the device
        vol->bt_profile_count = 0;
        g_idle_add (bt_get_profile, vol);
    }
}

/* Function polled after connection to get profile to confirm PA has found the device */

static gboolean bt_get_profile (gpointer user_data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) user_data;
    bt_operation_t *btop = (bt_operation_t *) vol->bt_ops->data;
    char *paname, *pacard, *msg;

    // some devices take a very long time to be valid PulseAudio cards after connection
    pacard = bt_to_pa_name (btop->device, "card", NULL);
    pulse_get_profile (vol, pacard);
    if (vol->pa_profile == NULL && vol->bt_profile_count++ < BT_PULSE_RETRIES)
    {
        g_free (pacard);
        return TRUE;
    }

    DEBUG ("Profile polled %d times", vol->bt_profile_count);

    if (vol->pa_profile == NULL)
    {
        DEBUG ("Bluetooth device not found by PulseAudio - profile not available");

        // update dialog to show a warning
        bt_connect_dialog_update (vol, _("Device not found by PulseAudio"));
    }
    else
    {
        DEBUG ("Bluetooth device found by PulseAudio with profile %s", vol->pa_profile);
        if (!pulse_set_profile (vol, pacard, btop->direction == OUTPUT && vol->bt_force_hsp == FALSE ? "a2dp_sink" : "headset_head_unit"))
        {
            DEBUG ("Failed to set device profile : %s", vol->pa_error_msg);
            msg = g_strdup_printf (_("Could not set profile for device : %s"), vol->pa_error_msg);
            bt_connect_dialog_update (vol, msg);
            g_free (msg);
        }
        else
        {
            if (btop->direction == OUTPUT && vol->bt_force_hsp == FALSE) DEBUG ("Profile set to a2dp_sink");
            else DEBUG ("Profile set to headset_head_unit");

            if (btop->direction != OUTPUT)
            {
                paname = bt_to_pa_name (btop->device, "source", "headset_head_unit");
                pulse_change_source (vol, paname);
                vsystem ("echo %s > ~/.btin", btop->device);
                g_free (paname);
            }

            if (btop->direction != INPUT)
            {
                paname = bt_to_pa_name (btop->device, "sink", btop->direction == OUTPUT && vol->bt_force_hsp == FALSE ? "a2dp_sink" : "headset_head_unit");
                pulse_change_sink (vol, paname);
                vsystem ("echo %s > ~/.btout", btop->device);
                g_free (paname);
            }
        }

        if ((vol->bt_input == FALSE || btop->direction != OUTPUT) && vol->conn_ok == NULL)
            close_widget (&vol->conn_dialog);
    }
    g_free (pacard);

    pulse_unmute_all_streams (vol);

    bt_next_operation (vol);

    volumepulse_update_display (vol);
    return FALSE;
}

/* Callback for trust completed */

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

/* Disconnect a BlueZ device */

static void bt_disconnect_device (VolumePulsePlugin *vol, const char *device)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (vol->bt_objmanager, device, "org.bluez.Device1");
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
        bt_next_operation (vol);
    }
}

/* Callback for disconnect completed */

static void bt_cb_disconnected (GObject *source, GAsyncResult *res, gpointer user_data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) user_data;
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
    if (var) g_variant_unref (var);

    if (error)
    {
        DEBUG ("Disconnect error %s", error->message);
        g_error_free (error);
    }
    else DEBUG ("Disconnected OK");

    bt_next_operation (vol);
}

/* Check to see if a device has a particular service; i.e. is it input or output */

static gboolean bt_has_service (VolumePulsePlugin *vol, const gchar *path, const gchar *service)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (vol->bt_objmanager, path, "org.bluez.Device1");
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

/*----------------------------------------------------------------------------*/
/* Bluetooth connection dialog                                                */
/*----------------------------------------------------------------------------*/

/* Show the Bluetooth connection dialog */

static void bt_connect_dialog_show (VolumePulsePlugin *vol, const char *fmt, ...)
{
    char *msg;
    va_list arg;

    va_start (arg, fmt);
    g_vasprintf (&msg, fmt, arg);
    va_end (arg);

    vol->conn_dialog = gtk_dialog_new_with_buttons (_("Connecting Audio Device"), NULL, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, NULL);
    gtk_window_set_icon_name (GTK_WINDOW (vol->conn_dialog), "preferences-system-bluetooth");
    gtk_window_set_position (GTK_WINDOW (vol->conn_dialog), GTK_WIN_POS_CENTER);
    gtk_container_set_border_width (GTK_CONTAINER (vol->conn_dialog), 10);
    vol->conn_label = gtk_label_new (msg);
    gtk_label_set_line_wrap (GTK_LABEL (vol->conn_label), TRUE);
    gtk_label_set_justify (GTK_LABEL (vol->conn_label), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment (GTK_MISC (vol->conn_label), 0.0, 0.0);
    gtk_widget_set_size_request (vol->conn_label, 350, -1);
    gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (vol->conn_dialog))), vol->conn_label, TRUE, TRUE, 0);
    g_signal_connect (GTK_OBJECT (vol->conn_dialog), "delete_event", G_CALLBACK (bt_connect_dialog_delete), vol);
    vol->conn_ok = NULL;
    gtk_widget_show_all (vol->conn_dialog);
    g_free (msg);
}

/* Either update the message on the connection dialog to show an error, or close the dialog */

static void bt_connect_dialog_update (VolumePulsePlugin *vol, const gchar *msg)
{
    if (!vol->conn_dialog) return;

    char *buffer = g_strdup_printf (_("Failed to connect to Bluetooth device - %s"), msg);
    gtk_label_set_text (GTK_LABEL (vol->conn_label), buffer);
    g_free (buffer);

    if (vol->conn_ok == NULL)
    {
        vol->conn_ok = gtk_dialog_add_button (GTK_DIALOG (vol->conn_dialog), _("_OK"), 1);
        g_signal_connect (vol->conn_ok, "clicked", G_CALLBACK (bt_connect_dialog_ok), vol);
        gtk_widget_show (vol->conn_ok);
    }
}

/* Handler for 'OK' button on connection dialog */

static void bt_connect_dialog_ok (GtkButton *button, VolumePulsePlugin *vol)
{
    close_widget (&vol->conn_dialog);
}

/* Handler for "delete-event" signal from connection dialog */

static gboolean bt_connect_dialog_delete (GtkWidget *widget, GdkEvent *event, VolumePulsePlugin *vol)
{
    close_widget (&vol->conn_dialog);
    return TRUE;
}

/*----------------------------------------------------------------------------*/
/* External API                                                               */
/*----------------------------------------------------------------------------*/

/* Initialise BlueZ interface */

void bluetooth_init (VolumePulsePlugin *vol)
{
    /* Reset Bluetooth variables */
    vol->bt_oname = NULL;
    vol->bt_iname = NULL;
    vol->bt_ops = NULL;

    /* Set up callbacks to see if BlueZ is on D-Bus */
    vol->bt_watcher_id = g_bus_watch_name (G_BUS_TYPE_SYSTEM, "org.bluez", 0, bt_cb_name_owned, bt_cb_name_unowned, vol, NULL);
}

/* Teardown BlueZ interface */

void bluetooth_terminate (VolumePulsePlugin *vol)
{
    /* Remove signal handlers on D-Bus object manager */
    if (vol->bt_objmanager)
    {
        g_signal_handlers_disconnect_by_func (vol->bt_objmanager, G_CALLBACK (bt_cb_object_added), vol);
        g_signal_handlers_disconnect_by_func (vol->bt_objmanager, G_CALLBACK (bt_cb_object_removed), vol);
        g_object_unref (vol->bt_objmanager);
    }
    vol->bt_objmanager = NULL;

    /* Remove the watch on D-Bus */
    g_bus_unwatch_name (vol->bt_watcher_id);
}

/* Check to see if a Bluetooth device is connected */

gboolean bluetooth_is_connected (VolumePulsePlugin *vol, const char *path)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (vol->bt_objmanager, path, "org.bluez.Device1");
    GVariant *var = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Connected");
    gboolean res = g_variant_get_boolean (var);
    g_variant_unref (var);
    g_object_unref (interface);
    return res;
}

/* Set a BlueZ device as the default PulseAudio sink */

void bluetooth_set_output (VolumePulsePlugin *vol, const char *name, const char *label)
{
    bt_connect_dialog_show (vol, _("Connecting Bluetooth device '%s' as output..."), label);

    pulse_get_default_sink_source (vol);
    vol->bt_oname = bt_from_pa_name (vol->pa_default_sink);
    vol->bt_iname = bt_from_pa_name (vol->pa_default_source);
    if (vol->bt_oname) pulse_mute_all_streams (vol);

    // If there is a current output device, disconnect it
    if (vol->bt_oname) bt_add_operation (vol, vol->bt_oname, DISCONNECT, OUTPUT);

    // The logic below is complicated by adding the ability to use the option of re-selecting an existing output
    // to force its reconnection as an output only, disconnecting it as an input and forcing it to the A2DP profile.
    if (!vol->bt_iname)
    {
        // No current input device, so just connect new device as output
        bt_add_operation (vol, name, CONNECT, OUTPUT);
    }
    else
    {
        // If current input device was not the same as current output device (and hence not already disconnected), disconnect the current input device
        if (g_strcmp0 (vol->bt_oname, vol->bt_iname)) bt_add_operation (vol, vol->bt_iname, DISCONNECT, INPUT);
        bt_add_operation (vol, vol->bt_iname, DISCONNECT, INPUT);

        if (g_strcmp0 (vol->bt_iname, name))
        {
            // New output device is not the same as current input device, so connect new output and then reconnect existing input
            bt_add_operation (vol, name, CONNECT, OUTPUT);
            bt_add_operation (vol, vol->bt_iname, CONNECT, INPUT);
        }
        else
        {
            // New output device is the same as current input device

            if (vol->bt_oname && !g_strcmp0 (vol->bt_oname, name))
            {
                // Current output device exists and is the same as new output device, so connect new device as output only to force A2DP when reconnecting
                bt_add_operation (vol, name, CONNECT, OUTPUT);
            }
            else
            {
                // No current output device, or current output device is not the same as new output device, so connect new device as both input and output
                bt_add_operation (vol, name, CONNECT, BOTH); 
            }
        }
    }

    vol->bt_input = FALSE;
    vol->bt_force_hsp = FALSE;

    bt_do_operation (vol);
}

/* Set a BlueZ device as the default PulseAudio source */

void bluetooth_set_input (VolumePulsePlugin *vol, const char *name, const char *label)
{
    bt_connect_dialog_show (vol, _("Connecting Bluetooth device '%s' as input..."), label);

    pulse_get_default_sink_source (vol);
    vol->bt_oname = bt_from_pa_name (vol->pa_default_sink);
    vol->bt_iname = bt_from_pa_name (vol->pa_default_source);
    if (vol->bt_oname) pulse_mute_all_streams (vol);

    // If there is a current input device, disconnect it
    if (vol->bt_iname) bt_add_operation (vol, vol->bt_iname, DISCONNECT, INPUT);

    if (vol->bt_oname && !g_strcmp0 (vol->bt_oname, name))
    {
        // Current output device exists and is the same as new input device

        // If current output device was not the same as current input device (and hence not already disconnected), disconnect the current output device
        if (g_strcmp0 (vol->bt_oname, vol->bt_iname)) bt_add_operation (vol, vol->bt_oname, DISCONNECT, OUTPUT);

        // Connect the new device as both input and output
        bt_add_operation (vol, name, CONNECT, BOTH);
    }
    else
    {
        // No current output device, or current output device is not the same as new input device

        // If current output device was the same as current input device (and hence has been disconnected), reconnect the current output device
        if (vol->bt_oname && !g_strcmp0 (vol->bt_oname, vol->bt_iname)) bt_add_operation (vol, vol->bt_oname, CONNECT, OUTPUT);

        // Connect the new device as input
        bt_add_operation (vol, name, CONNECT, INPUT);
    }

    vol->bt_input = TRUE;
    vol->bt_force_hsp = TRUE;

    bt_do_operation (vol);
}

/* Remove a BlueZ device which is the current PulseAudio sink */

void bluetooth_remove_output (VolumePulsePlugin *vol)
{
    vsystem ("rm -f ~/.btout");
    pulse_get_default_sink_source (vol);
    if (strstr (vol->pa_default_sink, "bluez"))
    {
        if (bt_sink_source_compare (vol->pa_default_sink, vol->pa_default_source))
        {
            // if the current default sink is Bluetooth and not also the default source, disconnect it
            vol->bt_oname = bt_from_pa_name (vol->pa_default_sink);
            bt_add_operation (vol, vol->bt_oname, DISCONNECT, OUTPUT);
            bt_do_operation (vol);
        }
    }
}

/* Remove a BlueZ device which is the current PulseAudio source */

void bluetooth_remove_input (VolumePulsePlugin *vol)
{
    vsystem ("rm -f ~/.btin");
    pulse_get_default_sink_source (vol);
    if (strstr (vol->pa_default_source, "bluez"))
    {
        if (bt_sink_source_compare (vol->pa_default_sink, vol->pa_default_source))
        {
            // if the current default source is Bluetooth and not also the default sink, disconnect it
            vol->bt_iname = bt_from_pa_name (vol->pa_default_source);
            bt_add_operation (vol, vol->bt_iname, DISCONNECT, INPUT);
        }
        else
        {
            // if the current default source and sink are both the same device, disconnect it and reconnect to
            // put it into A2DP rather than HSP
            bt_connect_dialog_show (vol, _("Reconnecting Bluetooth input device as output only..."));
            vol->bt_oname = bt_from_pa_name (vol->pa_default_sink);
            bt_add_operation (vol, vol->bt_oname, CONNECT, OUTPUT);
            vol->bt_input = FALSE;
            vol->bt_force_hsp = FALSE;
        }

        bt_do_operation (vol);
    }
}

/* Reconnect the current Bluetooth device if the user changes the profile */

void bluetooth_reconnect (VolumePulsePlugin *vol, const char *name, const char *profile)
{
    char *btname = bt_from_pa_name (name);
    if (btname == NULL) return;

    pulse_get_default_sink_source (vol);
    vol->bt_oname = bt_from_pa_name (vol->pa_default_sink);
    if (g_strcmp0 (btname, vol->bt_oname))
    {
        g_free (vol->bt_oname);
        vol->bt_oname = NULL;
    }
    vol->bt_iname = bt_from_pa_name (vol->pa_default_source);
    if (g_strcmp0 (btname, vol->bt_iname))
    {
        g_free (vol->bt_iname);
        vol->bt_iname = NULL;
    }
    g_free (btname);
    if (vol->bt_oname == NULL && vol->bt_iname == NULL) return;

    // disconnect the device if it was an input; don't reconnect, because it was either connected as 
    // headset, or not connected, so changing profile can only ever disconnect an input...
    if (vol->bt_iname)
    {
        bt_add_operation (vol, vol->bt_iname, DISCONNECT, INPUT);
        // in an ideal world, we'd remove the input device as the default Pulse source here, but that seems impossible...
    }

    // if it was an output, reconnect it if the new profile is anything but "off"
    if (vol->bt_oname && g_strcmp0 (profile, "off"))
    {
        bt_connect_dialog_show (vol, _("Reconnecting Bluetooth device..."));
        pulse_mute_all_streams (vol);
        bt_add_operation (vol, vol->bt_oname, CONNECT, OUTPUT);
        vol->bt_input = FALSE;
        if (!g_strcmp0 (profile, "headset_head_unit")) vol->bt_force_hsp = TRUE;
        else vol->bt_force_hsp = FALSE;
    }

    bt_do_operation (vol);
}

/* Loop through the devices BlueZ knows about, adding them to the device menu */

void bluetooth_add_devices_to_menu (VolumePulsePlugin *vol, gboolean input)
{
    vol->separator = FALSE;
    if (vol->bt_objmanager)
    {
        // iterate all the objects the manager knows about
        GList *objects = g_dbus_object_manager_get_objects (vol->bt_objmanager);
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
                    if (bt_has_service (vol, g_dbus_proxy_get_object_path (G_DBUS_PROXY (interface)), input ? BT_SERV_HSP : BT_SERV_AUDIO_SINK))
                    {
                        GVariant *name = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Alias");
                        GVariant *icon = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Icon");
                        GVariant *paired = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Paired");
                        GVariant *trusted = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Trusted");
                        if (name && icon && paired && trusted && g_variant_get_boolean (paired) && g_variant_get_boolean (trusted))
                        {
                            // create a menu if there isn't one already
                            if (input && !vol->menu_inputs) vol->menu_inputs = gtk_menu_new ();
                            menu_add_separator (vol, input ? vol->menu_inputs : vol->menu_outputs);
                            if (input) menu_add_item (vol, g_variant_get_string (name, NULL), objpath, TRUE);
                            else menu_add_item (vol, g_variant_get_string (name, NULL), objpath, FALSE);
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
}

/* Loop through the devices BlueZ knows about, adding them to the profiles dialog */

void bluetooth_add_devices_to_profile_dialog (VolumePulsePlugin *vol)
{
    if (vol->bt_objmanager)
    {
        // iterate all the objects the manager knows about
        GList *objects = g_dbus_object_manager_get_objects (vol->bt_objmanager);
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
                    if (bt_has_service (vol, g_dbus_proxy_get_object_path (G_DBUS_PROXY (interface)), BT_SERV_HSP)
                        || bt_has_service (vol, g_dbus_proxy_get_object_path (G_DBUS_PROXY (interface)), BT_SERV_AUDIO_SINK))
                    {
                        GVariant *name = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Alias");
                        GVariant *icon = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Icon");
                        GVariant *paired = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Paired");
                        GVariant *trusted = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Trusted");
                        if (name && icon && paired && trusted && g_variant_get_boolean (paired) && g_variant_get_boolean (trusted))
                        {
                            // only disconnected devices here...
                            char *pacard = bt_to_pa_name ((char *) objpath, "card", NULL);
                            pulse_get_profile (vol, pacard);
                            if (vol->pa_profile == NULL)
                                profiles_dialog_add_combo (vol, NULL, vol->profiles_bt_box, 0, g_variant_get_string (name, NULL), NULL);
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
}

/* End of file */
/*----------------------------------------------------------------------------*/
