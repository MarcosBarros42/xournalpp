#include "Menubar.h"

#include "control/cloud/CloudSyncController.h"  // fork: Drive connection indicator
#include "control/Control.h"
#include "gui/GladeSearchpath.h"
#include "gui/MainWindow.h"
#include "util/Assert.h"
#include "util/XojMsgBox.h"
#include "util/i18n.h"
#include "util/raii/GVariantSPtr.h"

#include "PageTypeSubmenu.h"
#include "PluginsSubmenu.h"
#include "RecentDocumentsSubmenu.h"
#include "ToolbarSelectionSubmenu.h"
#include "config-features.h"  // for ENABLE_PLUGINS

constexpr auto MENU_XML_FILE = "mainmenubar.xml";
constexpr auto MENU_ID = "menubar";
constexpr auto UNDO_REDO_SECTION_ID = "sectionUndoRedo";

// fork: Return the internally created widget GtkMenuBar from the GtkApplicationWindow (GMenuModel menubar)
static GtkWidget* findInternalMenuBar(GtkWidget* widget) {
    if (GTK_IS_MENU_BAR(widget)) {
        return widget;
    }
    if (GTK_IS_CONTAINER(widget)) {
        GList* children = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList* l = children; l; l = l->next) {
            if (GtkWidget* found = findInternalMenuBar(GTK_WIDGET(l->data))) {
                g_list_free(children);
                return found;
            }
        }
        g_list_free(children);
    }
    return nullptr;
}

Menubar::Menubar() = default;
Menubar::~Menubar() noexcept = default;

/// @brief Recursively removes all items with attribute "class" with value classname
static void removeItemsWithClass(GMenu* menu, const char* classname) {
    int N = g_menu_model_get_n_items(G_MENU_MODEL(menu));
    for (int n = 0; n < N; ++n) {
        xoj::util::GVariantSPtr c(
                g_menu_model_get_item_attribute_value(G_MENU_MODEL(menu), n, "class", G_VARIANT_TYPE_STRING),
                xoj::util::adopt);
        if (c && std::string_view(g_variant_get_string(c.get(), nullptr)) == classname) {
            g_menu_remove(menu, n--);
            N--;
        } else {
            xoj::util::GObjectSPtr<GMenuLinkIter> it(g_menu_model_iterate_item_links(G_MENU_MODEL(menu), n),
                                                     xoj::util::adopt);
            while (g_menu_link_iter_next(it.get())) {
                xoj::util::GObjectSPtr<GMenuModel> sub(g_menu_link_iter_get_value(it.get()), xoj::util::adopt);
                if (sub && G_IS_MENU(sub.get())) {
                    removeItemsWithClass(G_MENU(sub.get()), classname);
                }
            }
        }
    }
}

void Menubar::populate(const GladeSearchpath* gladeSearchPath, MainWindow* win) {
    builder.reset(gtk_builder_new(), xoj::util::adopt);

    auto filepath = gladeSearchPath->findFile("", MENU_XML_FILE);
    GError* error = nullptr;

    if (auto u8fp = filepath.u8string(); !gtk_builder_add_from_file(builder.get(), char_cast(u8fp.c_str()), &error)) {
        std::string msg = FS(_F("Error loading menubar XML file (try to load \"{1}\")") % u8fp);

        if (error != nullptr) {
            msg += "\n";
            msg += error->message;
            g_error_free(error);
        }
        XojMsgBox::showErrorToUser(nullptr, msg);
        return;
    }

    menu = G_MENU_MODEL(gtk_builder_get_object(builder.get(), MENU_ID));

    Control* ctrl = win->getControl();
    this->ctrl = ctrl;

    recentDocumentsSubmenu = std::make_unique<RecentDocumentsSubmenu>(ctrl, GTK_APPLICATION_WINDOW(win->getWindow()));
    toolbarSelectionSubmenu =
            std::make_unique<ToolbarSelectionSubmenu>(win, ctrl->getSettings(), win->getToolMenuHandler());
    pageTypeSubmenu = std::make_unique<PageTypeSubmenu>(ctrl->getPageTypes(), ctrl->getPageBackgroundChangeController(),
                                                        ctrl->getSettings(), GTK_APPLICATION_WINDOW(win->getWindow()));
#ifdef ENABLE_PLUGINS
    pluginsSubmenu =
            std::make_unique<PluginsSubmenu>(ctrl->getPluginController(), GTK_APPLICATION_WINDOW(win->getWindow()));
#endif

    forEachSubmenu([&](auto& subm) { subm.addToMenubar(*this); });

    if (!ctrl->getAudioController()) {
        removeItemsWithClass(G_MENU(menu), "audio");
    }

    undoRedoSection = G_MENU(gtk_builder_get_object(builder.get(), UNDO_REDO_SECTION_ID));

    // fork: add the Drive connection indicator as a real widget at the end of the internal menubar
    if (!this->setupDriveStatusWidget(GTK_WIDGET(win->getWindow()))) {
        // The internal menubar may be created lazily -> retry as soon as the window is realized
        g_signal_connect(win->getWindow(), "realize",
                         G_CALLBACK(+[](GtkWidget* widget, gpointer self) {
                             static_cast<Menubar*>(self)->setupDriveStatusWidget(widget);
                         }),
                         this);
    }

    if (auto* cloud = ctrl->getCloudSyncController(); cloud) {
        this->setDriveStatus(cloud->isLoggedIn());
    }
}

bool Menubar::setupDriveStatusWidget(GtkWidget* window) {
    if (this->driveStatusItem) {
        return true;
    }
    GtkWidget* menuBarWidget = findInternalMenuBar(window);
    if (!menuBarWidget) {
        return false;
    }

    driveStatusLabel = GTK_LABEL(gtk_label_new(nullptr));

    // bold like the surrounding menubar labels
    gtk_widget_set_name(GTK_WIDGET(driveStatusLabel), "driveStatusLabel");
    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, "#driveStatusLabel { font-weight: bold; }", -1, nullptr);
    gtk_style_context_add_provider(gtk_widget_get_style_context(GTK_WIDGET(driveStatusLabel)),
                                   GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    driveStatusItem = gtk_menu_item_new();
    gtk_container_add(GTK_CONTAINER(driveStatusItem), GTK_WIDGET(driveStatusLabel));
    gtk_menu_shell_append(GTK_MENU_SHELL(menuBarWidget), driveStatusItem);
    gtk_widget_show_all(driveStatusItem);

    // apply the current state, in case it was set before the widget existed
    if (auto* cloud = this->ctrl ? this->ctrl->getCloudSyncController() : nullptr) {
        this->setDriveStatus(cloud->isLoggedIn());
    }
    return true;
}

void Menubar::setUndoDescription(const std::string& description) {
    xoj_assert(undoRedoSection);
    g_menu_remove(undoRedoSection, 0);
    g_menu_prepend(undoRedoSection, description.c_str(), "win.undo");
}

void Menubar::setRedoDescription(const std::string& description) {
    xoj_assert(undoRedoSection);
    g_menu_remove(undoRedoSection, 1);
    g_menu_append(undoRedoSection, description.c_str(), "win.redo");
}

void Menubar::setDriveStatus(bool connected) {
    if (!driveStatusItem || !driveStatusLabel) {
        return;
    }
    // fork: uppercase Portuguese label, as in the reference design
    gtk_label_set_text(driveStatusLabel, connected ? "DRIVE : CONECTADO" : "DRIVE : DESCONECTADO");
}

void Menubar::setDisabled(bool disabled) {
    forEachSubmenu([&](auto& subm) { subm.setDisabled(disabled); });
}
