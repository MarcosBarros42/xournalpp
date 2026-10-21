/*
 * Xournal++
 *
 * Google Drive cloud settings panel (fork feature)
 *
 * @license GNU GPLv2 or later
 */

#include "DriveSettingsPanel.h"

#include <string>

#include "control/Control.h"
#include "control/cloud/CloudSyncController.h"
#include "util/XojMsgBox.h"
#include "util/i18n.h"

constexpr auto UI_FILE = "cloudSettings.glade";
constexpr auto UI_PANEL_NAME = "cloudSettingsPanel";

DriveSettingsPanel::DriveSettingsPanel(GladeSearchpath* gladeSearchPath, Control* controlIn):
        builder(gladeSearchPath, UI_FILE),
        panel(GTK_SCROLLED_WINDOW(builder.get(UI_PANEL_NAME))),
        lbCloudStatus(GTK_LABEL(builder.get("lbCloudStatus"))),
        lbCloudPathInfo(GTK_LABEL(builder.get("lbCloudPathInfo"))),
        btnCloudRefresh(GTK_BUTTON(builder.get("btnCloudRefresh"))),
        btnCloudLogin(GTK_BUTTON(builder.get("btnCloudLogin"))),
        btnCloudSyncNow(GTK_BUTTON(builder.get("btnCloudSyncNow"))),
        btnCloudLogout(GTK_BUTTON(builder.get("btnCloudLogout"))),
        control(controlIn) {

    CloudSyncController* cloud = this->control->getCloudSyncController();
    if (cloud) {
        cloud->setStatusCallback([lab = lbCloudStatus](const std::string& status) {
            gtk_label_set_text(lab, status.c_str());
        });

        gtk_label_set_text(this->lbCloudPathInfo,
                           FS(_F("Coloque o JSON de credenciais em: {1}") % cloud->getCredentialsPath()).c_str());
    }

    g_signal_connect_swapped(this->btnCloudRefresh, "clicked",
                             G_CALLBACK(+[](DriveSettingsPanel* self) { self->refresh(); }), this);
    g_signal_connect_swapped(this->btnCloudLogin, "clicked",
                             G_CALLBACK(+[](DriveSettingsPanel* self) { self->control->getCloudSyncController()->login(); }),
                             this);
    g_signal_connect_swapped(this->btnCloudSyncNow, "clicked",
                             G_CALLBACK(+[](DriveSettingsPanel* self) { self->control->getCloudSyncController()->syncNow(); }),
                             this);
    g_signal_connect_swapped(this->btnCloudLogout, "clicked",
                             G_CALLBACK(+[](DriveSettingsPanel* self) { self->control->getCloudSyncController()->logout(); }),
                             this);

    refresh();
}

DriveSettingsPanel::~DriveSettingsPanel() {
    if (CloudSyncController* cloud = this->control ? this->control->getCloudSyncController() : nullptr) {
        cloud->clearStatusCallback();
    }
}

void DriveSettingsPanel::refresh() {
    if (CloudSyncController* cloud = this->control->getCloudSyncController()) {
        if (cloud->isLoginRunning()) {
            gtk_label_set_text(this->lbCloudStatus, _("Google Drive: aguardando autorização no navegador..."));
        } else if (cloud->isLoggedIn()) {
            gtk_label_set_text(this->lbCloudStatus, _("Google Drive: conectado."));
        } else if (cloud->isConfigured()) {
            gtk_label_set_text(this->lbCloudStatus, _("Google Drive: desconectado. Faça login."));
        } else {
            gtk_label_set_text(this->lbCloudStatus, _("Google Drive: credenciais ausentes."));
        }
    }
}
