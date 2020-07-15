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

static void pa_error_handler (VolumePulsePlugin *vol, char *name);
static int pulse_set_subscription (VolumePulsePlugin *vol);
static int pulse_set_default_sink (VolumePulsePlugin *vol, const char *sinkname);
static int pulse_move_stream_to_default_sink (VolumePulsePlugin *vol, int index);
static int pulse_move_stream_to_default_source (VolumePulsePlugin *vol, int index);
static int pulse_set_default_source (VolumePulsePlugin *vol, const char *sourcename);
static int pulse_get_current_vol_mute (VolumePulsePlugin *vol);
static int pulse_replace_cards_with_sinks (VolumePulsePlugin *vol);
static int pulse_replace_cards_with_sources (VolumePulsePlugin *vol);
static void pulse_disconnect (VolumePulsePlugin *vol);
static void pulse_close (VolumePulsePlugin *vol);
static int pulse_get_output_streams (VolumePulsePlugin *vol);
static int pulse_get_input_streams (VolumePulsePlugin *vol);
static gboolean pa_card_has_port (const pa_card_info *i, pa_direction_t dir);

/* Functions in volumepulse.c needed here */

extern gboolean volumepulse_update_disp_cb (gpointer userdata);
extern GtkWidget *volumepulse_add_item_to_menu (VolumePulsePlugin *vol, GtkWidget *menu, const char *label, const char *name, gboolean enabled, gboolean input, GCallback cb);
extern void volumepulse_add_combo_to_profiles (VolumePulsePlugin *vol, GtkListStore *ls, GtkWidget *dest, int sel, const char *name, const char *label);
extern const char *volumepulse_device_display_name (VolumePulsePlugin *vol, const char *name);
extern void volumepulse_set_external_output (GtkWidget *widget, VolumePulsePlugin *vol);
extern void volumepulse_set_external_input (GtkWidget *widget, VolumePulsePlugin *vol);

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
    VolumePulsePlugin *vol = (VolumePulsePlugin *) userdata;

    if (pacontext == NULL)
    {
        vol->pa_state = PA_CONTEXT_FAILED;
        pa_threaded_mainloop_signal (vol->pa_mainloop, 0);
        return;
    }

    vol->pa_state = pa_context_get_state (pacontext);
    pa_threaded_mainloop_signal (vol->pa_mainloop, 0);
}

void pulse_init (VolumePulsePlugin *vol)
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
    vol->pa_profile = NULL;
    vol->pa_indices = NULL;

    pulse_set_subscription (vol);
}

static void pulse_disconnect (VolumePulsePlugin *vol)
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

static void pulse_close (VolumePulsePlugin *vol)
{
    if (vol->pa_mainloop != NULL)
    {
        pa_threaded_mainloop_stop (vol->pa_mainloop);
        pa_threaded_mainloop_free (vol->pa_mainloop);
    }
}

void pulse_terminate (VolumePulsePlugin *vol)
{
    pulse_disconnect (vol);
    pulse_close (vol);
}

static void pa_error_handler (VolumePulsePlugin *vol, char *name)
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
    VolumePulsePlugin *vol = (VolumePulsePlugin *) userdata;
    if (!success) DEBUG ("pulse success callback failed");

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

/* Event notification
 * ------------------
 *
 * Subscribe to and handle notifications from the Pulse server
 */

static void pa_cb_subscription (pa_context *pacontext, pa_subscription_event_type_t event, uint32_t idx, void *userdata)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) userdata;

    g_idle_add (volumepulse_update_disp_cb, vol);

    pa_threaded_mainloop_signal (vol->pa_mainloop, 0);
}

static int pulse_set_subscription (VolumePulsePlugin *vol)
{
    pa_context_set_subscribe_callback (vol->pa_context, &pa_cb_subscription, vol);
    START_PA_OPERATION
    op = pa_context_subscribe (vol->pa_context, PA_SUBSCRIPTION_MASK_ALL, &pa_cb_generic_success, vol);
    END_PA_OPERATION ("subscribe")
}

/* Get defaults
 * ------------
 *
 * Updates the names of the current default sink and source in the plugin
 * global data structure.
 */

static void pa_cb_get_default_sink_source (pa_context *context, const pa_server_info *i, void *userdata)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) userdata;

    DEBUG ("pa_cb_get_default_sink_source %s %s", i->default_sink_name, i->default_source_name);
    if (vol->pa_default_sink) g_free (vol->pa_default_sink);
    vol->pa_default_sink = g_strdup (i->default_sink_name);

    if (vol->pa_default_source) g_free (vol->pa_default_source);
    vol->pa_default_source = g_strdup (i->default_source_name);

    pa_threaded_mainloop_signal (vol->pa_mainloop, 0);
}

int pulse_get_default_sink_source (VolumePulsePlugin *vol)
{
    DEBUG ("pulse_get_default_sink_source");
    START_PA_OPERATION
    op = pa_context_get_server_info (vol->pa_context, &pa_cb_get_default_sink_source, vol);
    END_PA_OPERATION ("get_server_info")
}

/* Updating card names in menu
 * ---------------------------
 *
 * The device select menu reads the list of audio cards from Pulse,
 * which are stored with their card names. After discovery, the sink
 * and source info is queried and used to replace the card name with
 * the relevant sink or source name.
 */

static void pa_cb_replace_alsa_on_match (GtkWidget *widget, gpointer data)
{
    pa_sink_info *i = (pa_sink_info *) data;
    const char *alsaname = pa_proplist_gets (i->proplist, "alsa.card_name");

    if (!strcmp (alsaname, gtk_widget_get_name (widget)))
    {
        gtk_widget_set_name (widget, i->name);
        gtk_widget_set_sensitive (widget, TRUE);
        gtk_widget_set_tooltip_text (widget, NULL);
    }
}

static void pa_cb_replace_cards_with_sinks (pa_context *context, const pa_sink_info *i, int eol, void *userdata)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) userdata;

    if (!eol)
    {
        DEBUG ("pa_cb_replace_cards_with_sinks");
        const char *api = pa_proplist_gets (i->proplist, "device.api");
        if (!g_strcmp0 (api, "alsa"))
            gtk_container_foreach (GTK_CONTAINER (vol->outputs), pa_cb_replace_alsa_on_match, (void *) i);
    }

    pa_threaded_mainloop_signal (vol->pa_mainloop, 0);
}

static int pulse_replace_cards_with_sinks (VolumePulsePlugin *vol)
{
    DEBUG ("pulse_replace_cards_with_sinks");
    START_PA_OPERATION
    op = pa_context_get_sink_info_list (vol->pa_context, &pa_cb_replace_cards_with_sinks, vol);
    END_PA_OPERATION ("get_sink_info_list")
}

static void pa_cb_replace_cards_with_sources (pa_context *context, const pa_source_info *i, int eol, void *userdata)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) userdata;

    if (!eol)
    {
        DEBUG ("pa_cb_replace_cards_with_sources");
        const char *api = pa_proplist_gets (i->proplist, "device.api");
        if (!g_strcmp0 (api, "alsa"))
            gtk_container_foreach (GTK_CONTAINER (vol->inputs), pa_cb_replace_alsa_on_match, (void *) i);
    }

    pa_threaded_mainloop_signal (vol->pa_mainloop, 0);
}

static int pulse_replace_cards_with_sources (VolumePulsePlugin *vol)
{
    DEBUG ("pulse_replace_cards_with_sources");
    START_PA_OPERATION
    op = pa_context_get_source_info_list (vol->pa_context, &pa_cb_replace_cards_with_sources, vol);
    END_PA_OPERATION ("get_source_info_list")
}

void pulse_update_devices (VolumePulsePlugin *vol)
{
    pulse_replace_cards_with_sinks (vol);
    pulse_replace_cards_with_sources (vol);
}

/* Changing default sink
 * ---------------------
 *
 * To change a sink, first the default sink is updated to the new sink.
 * Then, all currently active output streams are listed in pa_indices.
 * Finally, all streams listed in pa_indices are moved to the new sink.
 */

static int pulse_set_default_sink (VolumePulsePlugin *vol, const char *sinkname)
{
    DEBUG ("pulse_set_default_sink %s", sinkname);
    START_PA_OPERATION
    op = pa_context_set_default_sink (vol->pa_context, sinkname, &pa_cb_generic_success, vol);
    END_PA_OPERATION ("set_default_sink")
}

static void pa_cb_get_output_streams (pa_context *context, const pa_sink_input_info *i, int eol, void *userdata)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) userdata;

    if (!eol)
    {
        DEBUG ("pa_cb_get_output_streams %d", i->index);
        vol->pa_indices = g_list_append (vol->pa_indices, (void *) i->index);
    }

    pa_threaded_mainloop_signal (vol->pa_mainloop, 0);
}

static int pulse_get_output_streams (VolumePulsePlugin *vol)
{
    DEBUG ("pulse_get_output_streams");
    START_PA_OPERATION
    op = pa_context_get_sink_input_info_list (vol->pa_context, &pa_cb_get_output_streams, vol);
    END_PA_OPERATION ("get_sink_input_info_list")
}

static int pulse_move_stream_to_default_sink (VolumePulsePlugin *vol, int index)
{
    DEBUG ("pulse_move_stream_to_default_sink %d", index);
    START_PA_OPERATION
    op = pa_context_move_sink_input_by_name (vol->pa_context, index, vol->pa_default_sink, &pa_cb_generic_success, vol);
    END_PA_OPERATION ("move_sink_input_by_name")
}

static void pa_cb_list_move_to_default_sink (gpointer data, gpointer userdata)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) userdata;

    pulse_move_stream_to_default_sink (vol, (int) data);
}

void pulse_change_sink (VolumePulsePlugin *vol, const char *sinkname)
{
    DEBUG ("pulse_change_sink %s", sinkname);
    if (vol->pa_default_sink) g_free (vol->pa_default_sink);
    vol->pa_default_sink = g_strdup (sinkname);

    vol->pa_indices = NULL;
    pulse_set_default_sink (vol, sinkname);
    pulse_get_output_streams (vol);
    g_list_foreach (vol->pa_indices, pa_cb_list_move_to_default_sink, vol);
    g_list_free (vol->pa_indices);
    DEBUG ("pulse_change_sink done");
}

/* Changing default source
 * -----------------------
 *
 * As for changing sink.
 */

static int pulse_set_default_source (VolumePulsePlugin *vol, const char *sourcename)
{
    DEBUG ("pulse_set_default_source %s", sourcename);
    START_PA_OPERATION
    op = pa_context_set_default_source (vol->pa_context, sourcename, &pa_cb_generic_success, vol);
    END_PA_OPERATION ("set_default_source")
}

static void pa_cb_get_input_streams (pa_context *context, const pa_source_output_info *i, int eol, void *userdata)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) userdata;

    if (!eol)
    {
        DEBUG ("pa_cb_get_input_streams %d", i->index);
        vol->pa_indices = g_list_append (vol->pa_indices, (void *) i->index);
    }

    pa_threaded_mainloop_signal (vol->pa_mainloop, 0);
}

static int pulse_get_input_streams (VolumePulsePlugin *vol)
{
    DEBUG ("pulse_get_input_streams");
    START_PA_OPERATION
    op = pa_context_get_source_output_info_list (vol->pa_context, &pa_cb_get_input_streams, vol);
    END_PA_OPERATION ("get_sink_input_info_list")
}

static int pulse_move_stream_to_default_source (VolumePulsePlugin *vol, int index)
{
    DEBUG ("pulse_move_stream_to_default_source %d", index);
    START_PA_OPERATION
    op = pa_context_move_source_output_by_name (vol->pa_context, index, vol->pa_default_source, &pa_cb_generic_success, vol);
    END_PA_OPERATION ("move_source_output_by_name")
}

static void pa_cb_list_move_to_default_source (gpointer data, gpointer userdata)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) userdata;

    pulse_move_stream_to_default_source (vol, (int) data);
}

void pulse_change_source (VolumePulsePlugin *vol, const char *sourcename)
{
    DEBUG ("pulse_change_source %s", sourcename);
    if (vol->pa_default_source) g_free (vol->pa_default_source);
    vol->pa_default_source = g_strdup (sourcename);

    vol->pa_indices = NULL;
    pulse_set_default_source (vol, sourcename);
    pulse_get_input_streams (vol);
    g_list_foreach (vol->pa_indices, pa_cb_list_move_to_default_source, vol);
    g_list_free (vol->pa_indices);
    DEBUG ("pulse_change_source done");
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

static void pa_cb_get_current_vol_mute (pa_context *context, const pa_sink_info *i, int eol, void *userdata)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) userdata;

    if (!eol)
    {
        DEBUG ("pa_cb_get_current_vol_mute %d %d %d", i->volume.channels, i->volume.values[0], i->mute);
        vol->pa_channels = i->volume.channels;
        vol->pa_volume = i->volume.values[0];
        vol->pa_mute = i->mute;
    }

    pa_threaded_mainloop_signal (vol->pa_mainloop, 0);
}

static int pulse_get_current_vol_mute (VolumePulsePlugin *vol)
{
    DEBUG ("pulse_get_current_vol_mute");
    START_PA_OPERATION
    op = pa_context_get_sink_info_by_name (vol->pa_context, vol->pa_default_sink, &pa_cb_get_current_vol_mute, vol);
    END_PA_OPERATION ("get_sink_info_by_name")
}

int pulse_get_volume (VolumePulsePlugin *vol)
{
    pulse_get_current_vol_mute (vol);
    return vol->pa_volume / PA_VOL_SCALE;
}

int pulse_get_mute (VolumePulsePlugin *vol)
{
    pulse_get_current_vol_mute (vol);
    return vol->pa_mute;
}

int pulse_set_volume (VolumePulsePlugin *vol, int volume)
{
    pa_cvolume cvol;
    cvol.channels = vol->pa_channels;
    cvol.values[0] = volume * PA_VOL_SCALE;
    cvol.values[1] = volume * PA_VOL_SCALE;

    DEBUG ("pulse_set_volume %d", volume);
    START_PA_OPERATION
    op = pa_context_set_sink_volume_by_name (vol->pa_context, vol->pa_default_sink, &cvol, &pa_cb_generic_success, vol);
    END_PA_OPERATION ("set_sink_volume_by_name")
}

int pulse_set_mute (VolumePulsePlugin *vol, int mute)
{
    DEBUG ("pulse_set_mute %d", mute);
    START_PA_OPERATION
    op = pa_context_set_sink_mute_by_name (vol->pa_context, vol->pa_default_sink, mute, &pa_cb_generic_success, vol);
    END_PA_OPERATION ("set_sink_mute_by_name");
}

/* Profiles
 * --------
 *
 * Under PA, cards have various profiles which control which sources and sinks
 * are available to the system. These are exposed through the profile setting
 * dialog.
 */

int pulse_set_profile (VolumePulsePlugin *vol, const char *card, const char *profile)
{
    DEBUG ("pulse_set_profile %s %s", card, profile);
    START_PA_OPERATION
    op = pa_context_set_card_profile_by_name (vol->pa_context, card, profile, &pa_cb_generic_success, vol);
    END_PA_OPERATION ("set_card_profile_by_name")
}

static void pa_cb_get_profile (pa_context *c, const pa_card_info *i, int eol, void *userdata)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) userdata;

    if (!eol)
    {
        DEBUG ("pulse get profile : %s", i->active_profile2->name);
        if (vol->pa_profile) g_free (vol->pa_profile);
        vol->pa_profile = g_strdup (i->active_profile2->name);
    }

    pa_threaded_mainloop_signal (vol->pa_mainloop, 0);
}

int pulse_get_profile (VolumePulsePlugin *vol, const char *card)
{
    DEBUG ("pulse_get_profile %s", card);
    START_PA_OPERATION
    op = pa_context_get_card_info_by_name (vol->pa_context, card, &pa_cb_get_profile, vol);
    END_PA_OPERATION ("get_card_info_by_name")
}

/* Profiles dialog
 * ---------------
 *
 * Populate the boxes on the profiles dialog with input, external output
 * and internal output devices.
 */

static void pa_cb_add_devices_to_profile_dialog (pa_context *c, const pa_card_info *i, int eol, void *userdata)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) userdata;

    GtkListStore *ls;
    int index = 0, sel;

    if (!eol)
    {
        // loop through profiles, adding each to list store
        ls = gtk_list_store_new (2, G_TYPE_STRING, G_TYPE_STRING);
        pa_card_profile_info2 **profile = i->profiles2;
        while (*profile)
        {
            if (*profile == i->active_profile2) sel = index;
            gtk_list_store_insert_with_values (ls, NULL, index++, 0, (*profile)->name, 1, (*profile)->description, -1);
            profile++;
        }

        if (!g_strcmp0 (pa_proplist_gets (i->proplist, "device.api"), "bluez"))
            volumepulse_add_combo_to_profiles (vol, ls, vol->btprofiles, sel, i->name, pa_proplist_gets (i->proplist, "device.description"));
        else
        {
            if (g_strcmp0 (pa_proplist_gets (i->proplist, "device.description"), "Built-in Audio"))
                volumepulse_add_combo_to_profiles (vol, ls, vol->alsaprofiles, sel, i->name, volumepulse_device_display_name (vol, pa_proplist_gets (i->proplist, "alsa.card_name")));
            else
                volumepulse_add_combo_to_profiles (vol, ls, vol->intprofiles, sel, i->name, volumepulse_device_display_name (vol, pa_proplist_gets (i->proplist, "alsa.card_name")));
        }
    }

    pa_threaded_mainloop_signal (vol->pa_mainloop, 0);
}

int pulse_add_devices_to_profile_dialog (VolumePulsePlugin *vol)
{
    DEBUG ("pulse_add_devices_to_profile_dialog");
    START_PA_OPERATION
    op = pa_context_get_card_info_list (vol->pa_context, &pa_cb_add_devices_to_profile_dialog, vol);
    END_PA_OPERATION ("get_card_info_list")
}

/* Device menu
 * -----------
 *
 * Populate the device menu with input, external output and internal
 * output devices.
 */

static gboolean pa_card_has_port (const pa_card_info *i, pa_direction_t dir)
{
    pa_card_port_info **port = i->ports;
    while (*port)
    {
        if ((*port)->direction == dir) return TRUE;
        port++;
    }
    return FALSE;
}

static void pa_cb_get_info_inputs (pa_context *c, const pa_card_info *i, int eol, void *userdata)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) userdata;

    if (!eol)
    {
        if (pa_card_has_port (i, PA_DIRECTION_INPUT))
        {
            const char *nam = pa_proplist_gets (i->proplist, "alsa.card_name");
            DEBUG ("pa_cb_get_info_inputs %s", nam);
            if (!vol->inputs) vol->inputs = gtk_menu_new ();
            volumepulse_add_item_to_menu (vol, vol->inputs, nam, nam, FALSE, TRUE, G_CALLBACK (volumepulse_set_external_input));
        }
    }

    pa_threaded_mainloop_signal (vol->pa_mainloop, 0);
}

static void pa_cb_get_info_internal (pa_context *c, const pa_card_info *i, int eol, void *userdata)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) userdata;

    if (!eol)
    {
        if (!g_strcmp0 (pa_proplist_gets (i->proplist, "device.description"), "Built-in Audio"))
        {
            if (pa_card_has_port (i, PA_DIRECTION_OUTPUT))
            {
                const char *nam = pa_proplist_gets (i->proplist, "alsa.card_name");
                DEBUG ("pa_cb_get_info_internal %s", nam);
                volumepulse_add_item_to_menu (vol, vol->outputs, volumepulse_device_display_name (vol, nam), nam, FALSE, FALSE, G_CALLBACK (volumepulse_set_external_output));
            }
        }
    }

    pa_threaded_mainloop_signal (vol->pa_mainloop, 0);
}

static void pa_cb_get_info_external (pa_context *c, const pa_card_info *i, int eol, void *userdata)
{
    VolumePulsePlugin *vol = (VolumePulsePlugin *) userdata;

    if (!eol)
    {
        if (g_strcmp0 (pa_proplist_gets (i->proplist, "device.description"), "Built-in Audio"))
        {
            if (pa_card_has_port (i, PA_DIRECTION_OUTPUT))
            {
                const char *nam = pa_proplist_gets (i->proplist, "alsa.card_name");
                DEBUG ("pa_cb_get_info_external %s", nam);
                volumepulse_add_item_to_menu (vol, vol->outputs, nam, nam, FALSE, FALSE, G_CALLBACK (volumepulse_set_external_output));
            }
        }
    }

    pa_threaded_mainloop_signal (vol->pa_mainloop, 0);
}

int pulse_add_devices_to_menu (VolumePulsePlugin *vol, gboolean input, gboolean internal)
{
    DEBUG ("pulse_add_devices_to_menu %d %d", input, internal);
    START_PA_OPERATION
    if (input)
        op = pa_context_get_card_info_list (vol->pa_context, &pa_cb_get_info_inputs, vol);
    else if (internal)
        op = pa_context_get_card_info_list (vol->pa_context, &pa_cb_get_info_internal, vol);
    else
        op = pa_context_get_card_info_list (vol->pa_context, &pa_cb_get_info_external, vol);
    END_PA_OPERATION ("get_card_info_list")
}

/* End of file */
/*----------------------------------------------------------------------------*/
