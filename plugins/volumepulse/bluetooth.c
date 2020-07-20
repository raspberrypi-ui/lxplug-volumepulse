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

static void bt_cb_object_added (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data);
static void bt_cb_object_removed (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data);
static void bt_cb_name_owned (GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer user_data);
static void bt_cb_name_unowned (GDBusConnection *connection, const gchar *name, gpointer user_data);
static void bt_cb_connected (GObject *source, GAsyncResult *res, gpointer user_data);
static void bt_cb_trusted (GObject *source, GAsyncResult *res, gpointer user_data);
static void bt_cb_reconnected (GObject *source, GAsyncResult *res, gpointer user_data);
static void bt_cb_disconnected (GObject *source, GAsyncResult *res, gpointer user_data);
static void bt_reconnect_devices (VolumePulsePlugin *vol);
static gboolean bt_has_service (VolumePulsePlugin *vol, const gchar *path, const gchar *service);

//static gboolean bt_is_connected (VolumePulsePlugin *vol, const gchar *path);


void bluetooth_init (VolumePulsePlugin *vol)
{
    /* Set up callbacks to see if BlueZ is on DBus */
    g_bus_watch_name (G_BUS_TYPE_SYSTEM, "org.bluez", 0, bt_cb_name_owned, bt_cb_name_unowned, vol, NULL);      
}

/* Bluetooth name remapping
 * ------------------------
 *
 * Helper functions to remap PulseAudio sink and source names to and from
 * Bluez device names.
 */

char *bluez_to_pa_name (char *bluez_name, char *type, char *profile)
{
    unsigned int b1, b2, b3, b4, b5, b6;

    if (bluez_name == NULL) return NULL;
    if (sscanf (bluez_name, "/org/bluez/hci0/dev_%x_%x_%x_%x_%x_%x", &b1, &b2, &b3, &b4, &b5, &b6) != 6)
    {
        DEBUG ("Bluez name invalid : %s", bluez_name);
        return NULL;
    }
    return g_strdup_printf ("bluez_%s.%02X_%02X_%02X_%02X_%02X_%02X%s%s", type, b1, b2, b3, b4, b5, b6, profile ? "." : "", profile ? profile : "");
}

char *bluez_from_pa_name (char *pa_name)
{
    unsigned int b1, b2, b3, b4, b5, b6;

    if (pa_name == NULL) return NULL;
    if (strstr (pa_name, "bluez") == NULL) return NULL;
    if (sscanf (strstr (pa_name, ".") + 1, "%x_%x_%x_%x_%x_%x", &b1, &b2, &b3, &b4, &b5, &b6) != 6) return NULL;
    return g_strdup_printf ("/org/bluez/hci0/dev_%02X_%02X_%02X_%02X_%02X_%02X", b1, b2, b3, b4, b5, b6);
}

int pa_bt_sink_source_compare (char *sink, char *source)
{
    if (sink == NULL || source == NULL) return 1;
    if (strstr (sink, "bluez") == NULL) return 1;
    if (strstr (source, "bluez") == NULL) return 1;
    return strncmp (sink + 11, source + 13, 17);
}

int pa_bluez_device_same (const char *padev, const char *btdev)
{
    if (strstr (btdev, "bluez") && strstr (padev, btdev + 20)) return 1;
    return 0;
}


/*----------------------------------------------------------------------------*/
/* Bluetooth D-Bus interface                                                  */
/*----------------------------------------------------------------------------*/

static void bt_cb_object_added (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) user_data;
    const char *obj = g_dbus_object_get_object_path (object);
    pulse_get_default_sink_source (vol);
    char *device = bluez_from_pa_name (vol->pa_default_sink);
    char *idevice = bluez_from_pa_name (vol->pa_default_source);
    if (g_strcmp0 (obj, device) || g_strcmp0 (obj, idevice))
    {
        DEBUG ("Selected Bluetooth audio device has connected");
        volumepulse_update_display (vol);
    }
    if (device) g_free (device);
    if (idevice) g_free (idevice);
}

static void bt_cb_object_removed (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) user_data;
    const char *obj = g_dbus_object_get_object_path (object);
    pulse_get_default_sink_source (vol);
    char *device = bluez_from_pa_name (vol->pa_default_sink);
    char *idevice = bluez_from_pa_name (vol->pa_default_source);
    if (g_strcmp0 (obj, device) || g_strcmp0 (obj, idevice))
    {
        DEBUG ("Selected Bluetooth audio device has disconnected");
        volumepulse_update_display (vol);
    }
    if (device) g_free (device);
    if (idevice) g_free (idevice);
}

static void bt_cb_name_owned (GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer user_data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) user_data;
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
        g_signal_connect (vol->objmanager, "object-added", G_CALLBACK (bt_cb_object_added), vol);
        g_signal_connect (vol->objmanager, "object-removed", G_CALLBACK (bt_cb_object_removed), vol);

        /* Check whether a Bluetooth audio device is the current default output or input - connect to one or both if so */
        pulse_get_default_sink_source (vol);
        char *device = bluez_from_pa_name (vol->pa_default_sink);
        char *idevice = bluez_from_pa_name (vol->pa_default_source);
        if (device || idevice)
        {
            /* Reconnect the current Bluetooth audio device */
            if (vol->bt_conname) g_free (vol->bt_conname);
            if (vol->bt_reconname) g_free (vol->bt_reconname);
            if (device) vol->bt_conname = g_strdup (device);
            else if (idevice) vol->bt_conname = g_strdup (idevice);

            if (device && idevice && g_strcmp0 (device, idevice)) vol->bt_reconname = idevice;
            else vol->bt_reconname = NULL;

            DEBUG ("Reconnecting devices");
            bt_reconnect_devices (vol);
        }
        if (device) g_free (device);
        if (idevice) g_free (idevice);
    }
}

static void bt_cb_name_unowned (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) user_data;
    DEBUG ("Name %s unowned on DBus", name);

    if (vol->objmanager) g_object_unref (vol->objmanager);
    if (vol->bt_conname) g_free (vol->bt_conname);
    if (vol->bt_reconname) g_free (vol->bt_reconname);
    vol->objmanager = NULL;
    vol->bt_conname = NULL;
    vol->bt_reconname = NULL;
}

void bluetooth_connect_device (VolumePulsePlugin *vol)
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
        if (vol->conn_dialog) volumepulse_connect_dialog_update (vol, _("Could not get BlueZ interface"));
        if (vol->bt_conname) g_free (vol->bt_conname);
        vol->bt_conname = NULL;
    }
}

static void bt_cb_connected (GObject *source, GAsyncResult *res, gpointer user_data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) user_data;
    GError *error = NULL;
    char *paname, *pacard;

    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
    if (var) g_variant_unref (var);

    if (error)
    {
        DEBUG ("Connect error %s", error->message);

        // update dialog to show a warning
        if (vol->conn_dialog) volumepulse_connect_dialog_update (vol, error->message);
        g_error_free (error);
    }
    else
    {
        DEBUG ("Connected OK");

        // some devices take a very long time to be valid PulseAudio cards after connection
        pacard = bluez_to_pa_name (vol->bt_conname, "card", NULL);
        do pulse_get_profile (vol, pacard);
        while (vol->pa_profile == NULL);
        DEBUG ("profile %s", vol->pa_profile);

        // set connected device as PulseAudio default
        if (vol->bt_input)
        {
            paname = bluez_to_pa_name (vol->bt_conname, "source", "headset_head_unit");
            pulse_set_profile (vol, pacard, "headset_head_unit");
            pulse_change_source (vol, paname);
        }
        else
        {
            paname = bluez_to_pa_name (vol->bt_conname, "sink", vol->pa_profile);
            pulse_change_sink (vol, paname);
        }
        g_free (paname);
        g_free (pacard);

        // close the connection dialog
        volumepulse_connect_dialog_update (vol, NULL);
    }

    // delete the connection information
    g_free (vol->bt_conname);
    vol->bt_conname = NULL;

    volumepulse_update_display (vol);
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

static void bt_reconnect_devices (VolumePulsePlugin *vol)
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
    VolumePulsePlugin *vol = (VolumePulsePlugin *) user_data;
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
    else volumepulse_update_display (vol);
}

void bluetooth_disconnect_device (VolumePulsePlugin *vol, char *device)
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
            bluetooth_connect_device (vol);
        }
    }
}

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

    // call BlueZ over DBus to connect to the device
    if (vol->bt_conname)
    {
        DEBUG ("Connecting to %s...", vol->bt_conname);
        bluetooth_connect_device (vol);
    }
}

static gboolean bt_has_service (VolumePulsePlugin *vol, const gchar *path, const gchar *service)
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

#if 0
static gboolean bt_is_connected (VolumePulsePlugin *vol, const gchar *path)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (vol->objmanager, path, "org.bluez.Device1");
    GVariant *var = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Connected");
    gboolean res = g_variant_get_boolean (var);
    g_variant_unref (var);
    g_object_unref (interface);
    return res;
}
#endif

void bluetooth_add_devices_to_profile_dialog (VolumePulsePlugin *vol)
{
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
                            char *pacard = bluez_to_pa_name ((char *) objpath, "card", NULL);
                            pulse_get_profile (vol, pacard);
                            if (vol->pa_profile == NULL)
                                volumepulse_profiles_add_combo (vol, NULL, vol->btprofiles, 0, g_variant_get_string (name, NULL), NULL);
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

void bluetooth_add_devices_to_menu (VolumePulsePlugin *vol, gboolean input)
{
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
                    if (bt_has_service (vol, g_dbus_proxy_get_object_path (G_DBUS_PROXY (interface)), input ? BT_SERV_HSP : BT_SERV_AUDIO_SINK))
                    {
                        GVariant *name = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Alias");
                        GVariant *icon = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Icon");
                        GVariant *paired = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Paired");
                        GVariant *trusted = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Trusted");
                        if (name && icon && paired && trusted && g_variant_get_boolean (paired) && g_variant_get_boolean (trusted))
                        {
                            if (input)
                            {
                                // create a menu if there isn't one already
                                if (!vol->inputs) vol->inputs = gtk_menu_new ();
                                volumepulse_menu_add_item (vol, g_variant_get_string (name, NULL), objpath, TRUE);
                            }
                            else
                                volumepulse_menu_add_item (vol, g_variant_get_string (name, NULL), objpath, FALSE);
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



