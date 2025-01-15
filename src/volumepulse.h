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

/*----------------------------------------------------------------------------*/
/* Typedefs and macros */
/*----------------------------------------------------------------------------*/

#define DEBUG_ON
#ifdef DEBUG_ON
#define DEBUG(fmt,args...) if(getenv("DEBUG_VP"))g_message("vp: " fmt,##args)
#else
#define DEBUG(fmt,args...)
#endif

typedef struct {
#ifdef LXPLUG
    LXPanel *panel;                     /* Back pointer to panel */
    config_setting_t *settings;         /* Plugin settings */
    GtkWidget *box;                     /* Back pointer to widget */
#else
    int icon_size;                      /* Variables used under wf-panel */
    gboolean bottom;
    GtkGesture *gesture[2];
#endif
    GtkWidget *plugin[2];               /* Pointers to buttons */

    gboolean wizard;                    /* Used in wizard? */
    gboolean pipewire;                  /* Pipewire running? */
    gboolean popup_shown;

    /* graphics */
    GtkWidget *tray_icon[2];            /* Displayed icon */
    GtkWidget *popup_window[2];         /* Top level window for popup */
    GtkWidget *popup_volume_scale[2];   /* Scale for volume */
    GtkWidget *popup_mute_check[2];     /* Checkbox for mute state */
    GtkWidget *menu_devices[2];         /* Right-click menu */
    GtkWidget *profiles_dialog;         /* Device profiles dialog */
    GtkWidget *profiles_int_box;        /* Vbox for profile combos */
    GtkWidget *profiles_ext_box;        /* Vbox for profile combos */
    GtkWidget *profiles_bt_box;         /* Vbox for profile combos */
    GtkWidget *conn_dialog;             /* Connection dialog box */
    GtkWidget *conn_label;              /* Dialog box text field */
    GtkWidget *conn_ok;                 /* Dialog box button */
    guint volume_scale_handler[2];      /* Handler for volume_scale widget */
    guint mute_check_handler[2];        /* Handler for mute_check widget */
    gboolean separator;                 /* Flag to show whether a menu separator has been added */

    /* HDMI devices */
    char *hdmi_names[2];                /* Display names of HDMI devices */

    /* PulseAudio interface */
    pa_threaded_mainloop *pa_mainloop;  /* Controller loop variable */
    pa_context *pa_cont;                /* Controller context */
    pa_context_state_t pa_state;        /* Current controller state */
    char *pa_default_sink;              /* Current default sink name */
    char *pa_default_source;            /* Current default source name */
    char *pa_profile;                   /* Current profile for card */
    int pa_channels;                    /* Number of channels on default sink */
    int pa_volume;                      /* Volume setting on default sink */
    int pa_mute;                        /* Mute setting on default sink */
    GList *pa_indices;                  /* Indices for current streams */
    char *pa_error_msg;                 /* Error message from success / fail callback */
    int pa_devices;                     /* Counter for pulse devices */
    guint pa_idle_timer;

    /* Bluetooth interface */
    GDBusObjectManager *bt_objmanager;  /* D-Bus BlueZ object manager */
    guint bt_watcher_id;                /* D-Bus BlueZ watcher ID */
    char *bt_conname;                   /* Name of device being connected */
    gboolean bt_input;                  /* Flag to show if current connect operation is for input or output */
    gboolean bt_force_hsp;              /* Flag to override automatic profile selection */
    int bt_retry_count;                 /* Counter for polling read of profile on connection */
    guint bt_retry_timer;               /* Timer for retrying post-connection events */
    gboolean bt_card_found;
} VolumePulsePlugin;

/* Functions in volumepulse.c needed in other modules */

extern void vol_menu_show (VolumePulsePlugin *vol);
extern void mic_menu_show (VolumePulsePlugin *vol);
extern void vol_menu_add_item (VolumePulsePlugin *vol, const char *label, const char *name);
extern void mic_menu_add_item (VolumePulsePlugin *vol, const char *label, const char *name);
extern void profiles_dialog_add_combo (VolumePulsePlugin *vol, GtkListStore *ls, GtkWidget *dest, int sel, const char *label, const char *name);
extern void volumepulse_update_display (VolumePulsePlugin *vol);
extern void micpulse_update_display (VolumePulsePlugin *vol);

/* End of file */
/*----------------------------------------------------------------------------*/
