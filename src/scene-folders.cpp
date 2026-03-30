#include "scene-folders.hpp"
#include "folder-dock.hpp"
#include <plugin-support.h>

#include <QMainWindow>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-scene-folders", "en-US")

static SceneFolderWidget *folderWidget = nullptr;

/* ------------------------------------------------------------------ */
/*  OBS module entry / exit                                           */
/* ------------------------------------------------------------------ */

bool obs_module_load()
{
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)",
		PLUGIN_VERSION);
	obs_frontend_add_event_callback(scene_folders_frontend_event, nullptr);
	return true;
}

void obs_module_unload()
{
	obs_log(LOG_INFO, "plugin unloaded");
	folderWidget = nullptr;
}

const char *obs_module_name()
{
	return "Scene Folders";
}

const char *obs_module_description()
{
	return "Organize your OBS scenes into folders for easier management.";
}

/* ------------------------------------------------------------------ */
/*  Frontend event - create dock when UI is ready                     */
/* ------------------------------------------------------------------ */

void scene_folders_frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING && !folderWidget) {
		QMainWindow *main = static_cast<QMainWindow *>(
			obs_frontend_get_main_window());

		folderWidget = new SceneFolderWidget(main);

		/* Register with OBS - adds to View > Docks menu */
		obs_frontend_add_dock_by_id("SceneFoldersDock",
					    "Scene Folders", folderWidget);

		obs_log(LOG_INFO, "Scene Folders dock created");
	}

	if (event == OBS_FRONTEND_EVENT_EXIT) {
		if (folderWidget) {
			folderWidget->shutdown();
			folderWidget = nullptr;
		}
	}
}
