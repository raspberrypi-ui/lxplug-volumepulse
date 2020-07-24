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

#define BT_SERV_AUDIO_SOURCE    "0000110A"
#define BT_SERV_AUDIO_SINK      "0000110B"
#define BT_SERV_HSP             "00001108"
#define BT_SERV_HFP             "0000111E"

typedef struct {
    const char *device;
    gboolean disconnect;
    gboolean input;
} bt_operation_t;

typedef enum {
    CONNECT,
    DISCONNECT
} cd_t;

typedef enum {
    INPUT,
    OUTPUT
} dir_t;

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
static void bt_add_operation (VolumePulsePlugin *vol, const char *device, cd_t cd, dir_t dir);
static void bt_do_operation (VolumePulsePlugin *vol);
static void bt_next_operation (VolumePulsePlugin *vol);
static void bt_connect_device (VolumePulsePlugin *vol, const char *device);
static void bt_disconnect_device (VolumePulsePlugin *vol, const char *device);
static int pa_bt_sink_source_compare (char *sink, char *source);
static char *bluez_to_pa_name (const char *bluez_name, char *type, char *profile);
static char *bluez_from_pa_name (const char *pa_name);


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



static void bt_add_operation (VolumePulsePlugin *vol, const char *device, cd_t cd, dir_t dir)
{
    bt_operation_t *newop = malloc (sizeof (bt_operation_t));

    newop->device = device;
    newop->disconnect = (cd == DISCONNECT);
    newop->input = (dir == INPUT);

    vol->bt_ops = g_list_append (vol->bt_ops, newop);
}

static void bt_do_operation (VolumePulsePlugin *vol)
{
    if (vol->bt_ops)
    {
        bt_operation_t *btop = (bt_operation_t *) vol->bt_ops->data;
        if (btop->disconnect)
        {
            bt_disconnect_device (vol, btop->device);
        }
        else
        {
            bt_connect_device (vol, btop->device);
        }
    }
}

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
    }
    else bt_do_operation (vol);
}

void bluetooth_init (VolumePulsePlugin *vol)
{
    /* Set up callbacks to see if BlueZ is on DBus */
    g_bus_watch_name (G_BUS_TYPE_SYSTEM, "org.bluez", 0, bt_cb_name_owned, bt_cb_name_unowned, vol, NULL);
    vol->bt_oname = NULL;
    vol->bt_iname = NULL;
    vol->bt_ops = NULL;
}

/* Bluetooth name remapping
 * ------------------------
 *
 * Helper functions to remap PulseAudio sink and source names to and from
 * Bluez device names.
 */

static char *bluez_to_pa_name (const char *bluez_name, char *type, char *profile)
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

static char *bluez_from_pa_name (const char *pa_name)
{
    unsigned int b1, b2, b3, b4, b5, b6;

    if (pa_name == NULL) return NULL;
    if (strstr (pa_name, "bluez") == NULL) return NULL;
    if (sscanf (strstr (pa_name, ".") + 1, "%x_%x_%x_%x_%x_%x", &b1, &b2, &b3, &b4, &b5, &b6) != 6) return NULL;
    return g_strdup_printf ("/org/bluez/hci0/dev_%02X_%02X_%02X_%02X_%02X_%02X", b1, b2, b3, b4, b5, b6);
}

static int pa_bt_sink_source_compare (char *sink, char *source)
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

        DEBUG ("Reconnecting devices");
        bt_reconnect_devices (vol);
    }
}

static void bt_cb_name_unowned (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) user_data;
    DEBUG ("Name %s unowned on DBus", name);

    if (vol->objmanager) g_object_unref (vol->objmanager);
    vol->objmanager = NULL;
}

static void bt_connect_device (VolumePulsePlugin *vol, const char *device)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (vol->objmanager, device, "org.bluez.Device1");
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
        DEBUG ("Couldn't get device interface from object manager");
        volumepulse_connect_dialog_update (vol, _("Could not get BlueZ interface for device"));
        bt_next_operation (vol);
    }
}

static void bt_cb_connected (GObject *source, GAsyncResult *res, gpointer user_data)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) user_data;
    GError *error = NULL;
    char *paname, *pacard;
    int count;

    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
    if (var) g_variant_unref (var);

    if (error)
    {
        DEBUG ("Connect error %s", error->message);

        // update dialog to show a warning
        volumepulse_connect_dialog_update (vol, error->message);
        g_error_free (error);
    }
    else
    {
        DEBUG ("Connected OK");

        bt_operation_t *btop = (bt_operation_t *) vol->bt_ops->data;

        // some devices take a very long time to be valid PulseAudio cards after connection
        pacard = bluez_to_pa_name (btop->device, "card", NULL);
        count = 0;
        do
        {
            pulse_get_profile (vol, pacard);
            count++;
        }
        while (vol->pa_profile == NULL && count < 100);

        if (vol->pa_profile == NULL)
        {
            DEBUG ("No PulseAudio device");

            // update dialog to show a warning
            volumepulse_connect_dialog_update (vol, _("Device not found by PulseAudio"));
        }
        else
        {
            DEBUG ("Current profile %s", vol->pa_profile);

            // set connected device as PulseAudio default
            if (btop->input)
            {
                vsystem ("echo %s > ~/.btin", btop->device);

                paname = bluez_to_pa_name (btop->device, "source", "headset_head_unit");
                pulse_set_profile (vol, pacard, "headset_head_unit");
                DEBUG ("Profile set to headset_head_unit");
                pulse_change_source (vol, paname);
            }
            else
            {
                vsystem ("echo %s > ~/.btout", btop->device);

                const char *nextdev = NULL;
                if (vol->bt_ops->next)
                {
                    bt_operation_t *nop = (bt_operation_t *) vol->bt_ops->next->data;
                    nextdev = nop->device;
                }
                if (!g_strcmp0 (btop->device, nextdev))
                {
                    paname = bluez_to_pa_name (btop->device, "sink", "headset_head_unit");
                    pulse_set_profile (vol, pacard, "headset_head_unit");
                    DEBUG ("Profile set to headset_head_unit");
                }
                else
                {
                    paname = bluez_to_pa_name (btop->device, "sink", "a2dp_sink");
                    pulse_set_profile (vol, pacard, "a2dp_sink");
                    DEBUG ("Profile set to a2dp_sink");
                }
                pulse_change_sink (vol, paname);
            }
            g_free (paname);
            g_free (pacard);

            volumepulse_connect_dialog_update (vol, NULL);
        }
    }

    bt_next_operation (vol);

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

static void bt_disconnect_device (VolumePulsePlugin *vol, const char *device)
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
        bt_next_operation (vol);
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

    bt_next_operation (vol);
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

static void bt_reconnect_devices (VolumePulsePlugin *vol)
{
    vol->bt_oname = get_string ("cat ~/.btout 2> /dev/null");
    vol->bt_iname = get_string ("cat ~/.btin 2> /dev/null");

    if (vol->bt_oname) bt_add_operation (vol, vol->bt_oname, DISCONNECT, OUTPUT);
    if (vol->bt_iname) bt_add_operation (vol, vol->bt_iname, DISCONNECT, INPUT);
    if (vol->bt_oname) bt_add_operation (vol, vol->bt_oname, CONNECT, OUTPUT);
    if (vol->bt_iname) bt_add_operation (vol, vol->bt_iname, CONNECT, INPUT);

    bt_do_operation (vol);
}

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


void bluetooth_set_output (VolumePulsePlugin *vol, const char *name)
{
    pulse_get_default_sink_source (vol);
    vol->bt_oname = bluez_from_pa_name (vol->pa_default_sink);
    vol->bt_iname = bluez_from_pa_name (vol->pa_default_source);

    // to ensure an output device connects with the correct profile, disconnect
    // any existing input device first and then reconnect the input after
    // connecting the output...
    if (vol->bt_oname) bt_add_operation (vol, vol->bt_oname, DISCONNECT, OUTPUT);
    if (vol->bt_iname) bt_add_operation (vol, vol->bt_iname, DISCONNECT, INPUT);
    bt_add_operation (vol, name, CONNECT, OUTPUT);
    if (vol->bt_iname) bt_add_operation (vol, vol->bt_iname, CONNECT, INPUT);

    bt_do_operation (vol);
}


void bluetooth_set_input (VolumePulsePlugin *vol, const char *name)
{
    pulse_get_default_sink_source (vol);
    vol->bt_oname = bluez_from_pa_name (vol->pa_default_sink);
    vol->bt_iname = bluez_from_pa_name (vol->pa_default_source);

    // profiles load correctly for inputs, but may need to change the profile of
    // a device which is currently being used for output, so reload them both anyway...
    if (vol->bt_oname) bt_add_operation (vol, vol->bt_oname, DISCONNECT, OUTPUT);
    if (vol->bt_iname) bt_add_operation (vol, vol->bt_iname, DISCONNECT, INPUT);
    if (vol->bt_oname) bt_add_operation (vol, vol->bt_oname, CONNECT, OUTPUT);
    bt_add_operation (vol, name, CONNECT, INPUT);

    bt_do_operation (vol);
}

void bluetooth_remove_output (VolumePulsePlugin *vol)
{
    vsystem ("rm ~/.btout");
    pulse_get_default_sink_source (vol);
    if (strstr (vol->pa_default_sink, "bluez"))
    {
        if (pa_bt_sink_source_compare (vol->pa_default_sink, vol->pa_default_source))
        {
            // if the current default sink is Bluetooth and not also the default source, disconnect it
            vol->bt_oname = bluez_from_pa_name (vol->pa_default_sink);
            bt_add_operation (vol, vol->bt_oname, DISCONNECT, OUTPUT);

            bt_do_operation (vol);
        }
    }
}

gboolean bluetooth_remove_input (VolumePulsePlugin *vol)
{
    vsystem ("rm ~/.btin");
    pulse_get_default_sink_source (vol);
    if (strstr (vol->pa_default_source, "bluez"))
    {
        if (pa_bt_sink_source_compare (vol->pa_default_sink, vol->pa_default_source))
        {
            // if the current default source is Bluetooth and not also the default sink, disconnect it
            vol->bt_iname = bluez_from_pa_name (vol->pa_default_source);
            bt_add_operation (vol, vol->bt_iname, DISCONNECT, INPUT);

            bt_do_operation (vol);
        }
        else
        {
            // if the current default source and sink are both the same device, disconnect the input and force the output
            // to reconnect as A2DP...
            vol->bt_oname = bluez_from_pa_name (vol->pa_default_sink);
            bt_add_operation (vol, vol->bt_oname, DISCONNECT, OUTPUT);
            bt_add_operation (vol, vol->bt_oname, DISCONNECT, INPUT);
            bt_add_operation (vol, vol->bt_oname, CONNECT, OUTPUT);

            bt_do_operation (vol);
            return TRUE;
        }
    }
    return FALSE;
}

