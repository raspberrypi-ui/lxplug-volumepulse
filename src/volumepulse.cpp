/*============================================================================
Copyright (c) 2024 Raspberry Pi Holdings Ltd.
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

#include <glibmm.h>
#include <pulse/pulseaudio.h>
#include "volumepulse.hpp"

extern "C" {
    WayfireWidget *create () { return new WayfireVolumepulse; }
    void destroy (WayfireWidget *w) { delete w; }

    static constexpr conf_table_t conf_table[1] = {
        {CONF_NONE, NULL, NULL}
    };
    const conf_table_t *config_params (void) { return conf_table; };
    const char *display_name (void) { return N_("Volume"); };
    const char *package_name (void) { return GETTEXT_PACKAGE; };
}

void WayfireVolumepulse::bar_pos_changed_cb (void)
{
    if ((std::string) bar_pos == "bottom") vol->bottom = TRUE;
    else vol->bottom = FALSE;
}

void WayfireVolumepulse::icon_size_changed_cb (void)
{
    vol->icon_size = icon_size;
    volumepulse_update_display (vol);
    micpulse_update_display (vol);
}

void WayfireVolumepulse::command (const char *cmd)
{
    volumepulse_control_msg (vol, cmd);
}

bool WayfireVolumepulse::set_icon (void)
{
    volumepulse_update_display (vol);
    micpulse_update_display (vol);
    return false;
}

void WayfireVolumepulse::init (Gtk::HBox *container)
{
    /* Create the button */
    plugin_vol = std::make_unique <Gtk::Button> ();
    plugin_vol->set_name (PLUGIN_NAME);
    container->pack_start (*plugin_vol, false, false);
    plugin_mic = std::make_unique <Gtk::Button> ();
    plugin_mic->set_name (PLUGIN_NAME);
    container->pack_start (*plugin_mic, false, false);

    /* Setup structure */
    vol = g_new0 (VolumePulsePlugin, 1);
    vol->plugin[0] = (GtkWidget *)((*plugin_vol).gobj());
    vol->plugin[1] = (GtkWidget *)((*plugin_mic).gobj());
    vol->icon_size = icon_size;
    icon_timer = Glib::signal_idle().connect (sigc::mem_fun (*this, &WayfireVolumepulse::set_icon));
    bar_pos_changed_cb ();

    /* Initialise the plugin */
    volumepulse_init (vol);

    /* Setup callbacks */
    icon_size.set_callback (sigc::mem_fun (*this, &WayfireVolumepulse::icon_size_changed_cb));
    bar_pos.set_callback (sigc::mem_fun (*this, &WayfireVolumepulse::bar_pos_changed_cb));
}

WayfireVolumepulse::~WayfireVolumepulse()
{
    icon_timer.disconnect ();
    volumepulse_destructor (vol);
}

/* End of file */
/*----------------------------------------------------------------------------*/
