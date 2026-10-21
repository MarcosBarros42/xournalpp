/*
 * Xournal++
 *
 * Google Drive cloud settings panel (fork feature)
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <gtk/gtk.h>  // for GtkButton, GtkLabel, GtkScrolledWindow, GtkWidget

#include "gui/Builder.h"

class Control;
class GladeSearchpath;

class DriveSettingsPanel {
public:
    DriveSettingsPanel(GladeSearchpath* gladeSearchPath, Control* control);
    ~DriveSettingsPanel();
    DriveSettingsPanel(const DriveSettingsPanel&) = delete;
    auto operator=(const DriveSettingsPanel&) -> DriveSettingsPanel& = delete;

    void refresh();

    inline auto getPanel() const -> GtkWidget* { return GTK_WIDGET(panel); }

private:
    Builder builder;
    GtkScrolledWindow* panel;
    GtkLabel* lbCloudStatus;
    GtkLabel* lbCloudPathInfo;
    GtkButton* btnCloudRefresh;
    GtkButton* btnCloudLogin;
    GtkButton* btnCloudSyncNow;
    GtkButton* btnCloudLogout;
    Control* control;
};
