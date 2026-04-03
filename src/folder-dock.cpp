#include "folder-dock.hpp"
#include <plugin-support.h>

#include <QVBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QFile>
#include <QDir>
#include <QApplication>
#include <QStyle>

/* ------------------------------------------------------------------ */
/*  Construction / destruction                                        */
/* ------------------------------------------------------------------ */

SceneFolderWidget::SceneFolderWidget(QWidget *parent) : QWidget(parent)
{
	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	/* Search bar */
	searchBar = new QLineEdit(this);
	searchBar->setPlaceholderText("Search scenes...");
	searchBar->setClearButtonEnabled(true);
	layout->addWidget(searchBar);

	tree = new QTreeWidget(this);
	tree->setHeaderHidden(true);
	tree->setRootIsDecorated(true);
	tree->setAnimated(true);
	tree->setIndentation(16);
	tree->setContextMenuPolicy(Qt::CustomContextMenu);
	tree->setSelectionMode(QAbstractItemView::SingleSelection);
	tree->setDragDropMode(QAbstractItemView::InternalMove);
	tree->setDefaultDropAction(Qt::MoveAction);
	tree->setDragEnabled(true);

	/* Style: keep selection highlight visible even when unfocused */
	tree->setStyleSheet(
		"QTreeWidget::item:selected {"
		"  background-color: #3A7BD5;"
		"  color: #ffffff;"
		"}"
		"QTreeWidget::item:hover:!selected {"
		"  background-color: #3a3a3a;"
		"}");
	tree->viewport()->setAcceptDrops(true);
	tree->setDropIndicatorShown(true);

	layout->addWidget(tree);

	/* Connections */
	connect(searchBar, &QLineEdit::textChanged, this,
		&SceneFolderWidget::filterScenes);
	connect(tree, &QTreeWidget::itemDoubleClicked, this,
		&SceneFolderWidget::onItemDoubleClicked);
	connect(tree, &QTreeWidget::customContextMenuRequested, this,
		&SceneFolderWidget::onCustomContextMenu);

	/* Save after drag-drop moves */
	connect(tree->model(), &QAbstractItemModel::rowsInserted, this,
		[this]() {
			if (!shuttingDown)
				saveTimer->start();
		});

	/* Debounced save timer */
	saveTimer = new QTimer(this);
	saveTimer->setSingleShot(true);
	saveTimer->setInterval(500);
	connect(saveTimer, &QTimer::timeout, this,
		&SceneFolderWidget::saveConfig);

	/* Frontend event callback */
	obs_frontend_add_event_callback(onFrontendEvent, this);

	/* Load saved config and populate */
	loadConfig();
	refreshSceneList();
}

SceneFolderWidget::~SceneFolderWidget()
{
	shuttingDown = true;
	obs_frontend_remove_event_callback(onFrontendEvent, this);

	if (filterClipboard) {
		obs_data_array_release(filterClipboard);
		filterClipboard = nullptr;
	}
}

void SceneFolderWidget::shutdown()
{
	shuttingDown = true;
	saveConfig();
}

/* ------------------------------------------------------------------ */
/*  Frontend events                                                   */
/* ------------------------------------------------------------------ */

void SceneFolderWidget::onFrontendEvent(enum obs_frontend_event event,
					void *data)
{
	auto *w = static_cast<SceneFolderWidget *>(data);
	if (w->shuttingDown)
		return;

	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		QTimer::singleShot(0, w, [w]() {
			if (!w->shuttingDown)
				w->refreshSceneList();
		});
		break;

	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		if (!w->ignoreSceneChange) {
			OBSSourceAutoRelease src =
				obs_frontend_get_current_scene();
			if (src) {
				QString name = QString::fromUtf8(
					obs_source_get_name(src));
				w->highlightActiveScene(name);
			}
		}
		break;

	case OBS_FRONTEND_EVENT_EXIT:
		w->shutdown();
		break;

	default:
		break;
	}
}

/* ------------------------------------------------------------------ */
/*  Tree item helpers                                                 */
/* ------------------------------------------------------------------ */

QTreeWidgetItem *SceneFolderWidget::createFolderItem(const QString &name)
{
	auto *item = new QTreeWidgetItem();
	item->setText(0, name);
	item->setData(0, ROLE_ITEM_TYPE, TYPE_FOLDER);
	item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
		       Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled |
		       Qt::ItemIsEditable);
	item->setIcon(0,
		      qApp->style()->standardIcon(QStyle::SP_DirIcon));

	QFont f = item->font(0);
	f.setBold(true);
	item->setFont(0, f);

	return item;
}

QTreeWidgetItem *SceneFolderWidget::createSceneItem(const QString &name)
{
	auto *item = new QTreeWidgetItem();
	item->setText(0, name);
	item->setData(0, ROLE_ITEM_TYPE, TYPE_SCENE);
	item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
		       Qt::ItemIsDragEnabled);
	return item;
}

QTreeWidgetItem *SceneFolderWidget::findFolderItem(const QString &name)
{
	for (int i = 0; i < tree->topLevelItemCount(); i++) {
		QTreeWidgetItem *item = tree->topLevelItem(i);
		if (item->data(0, ROLE_ITEM_TYPE).toInt() == TYPE_FOLDER &&
		    item->text(0) == name)
			return item;
	}
	return nullptr;
}

/* ------------------------------------------------------------------ */
/*  Scene list synchronization                                        */
/* ------------------------------------------------------------------ */

void SceneFolderWidget::refreshSceneList()
{
	if (shuttingDown)
		return;

	/* Get all current OBS scenes */
	struct obs_frontend_source_list sceneList = {};
	obs_frontend_get_scenes(&sceneList);

	QSet<QString> obsScenes;
	for (size_t i = 0; i < sceneList.sources.num; i++) {
		const char *name =
			obs_source_get_name(sceneList.sources.array[i]);
		if (name)
			obsScenes.insert(QString::fromUtf8(name));
	}
	obs_frontend_source_list_free(&sceneList);

	/* Collect scenes currently in the tree */
	QSet<QString> treeScenes;
	QTreeWidgetItemIterator it(tree);
	while (*it) {
		if ((*it)->data(0, ROLE_ITEM_TYPE).toInt() == TYPE_SCENE)
			treeScenes.insert((*it)->text(0));
		++it;
	}

	/* Remove scenes that no longer exist in OBS */
	QTreeWidgetItemIterator removeIt(tree);
	while (*removeIt) {
		QTreeWidgetItem *item = *removeIt;
		++removeIt;
		if (item->data(0, ROLE_ITEM_TYPE).toInt() == TYPE_SCENE &&
		    !obsScenes.contains(item->text(0))) {
			delete item;
		}
	}

	/* Add new scenes that aren't in the tree */
	for (const QString &scene : obsScenes) {
		if (!treeScenes.contains(scene))
			tree->addTopLevelItem(createSceneItem(scene));
	}

	/* Highlight current scene */
	OBSSourceAutoRelease current = obs_frontend_get_current_scene();
	if (current) {
		QString currentName =
			QString::fromUtf8(obs_source_get_name(current));
		highlightActiveScene(currentName);
	}

	saveTimer->start();
}

/* ------------------------------------------------------------------ */
/*  User interactions                                                 */
/* ------------------------------------------------------------------ */

void SceneFolderWidget::onItemDoubleClicked(QTreeWidgetItem *item, int)
{
	if (!item || shuttingDown)
		return;

	int type = item->data(0, ROLE_ITEM_TYPE).toInt();

	if (type == TYPE_SCENE)
		switchToScene(item->text(0));
	else if (type == TYPE_FOLDER)
		item->setExpanded(!item->isExpanded());
}

void SceneFolderWidget::onCustomContextMenu(const QPoint &pos)
{
	if (shuttingDown)
		return;

	QTreeWidgetItem *item = tree->itemAt(pos);
	QMenu menu(this);
	QString sceneName = item ? item->text(0) : QString();
	int type = item ? item->data(0, ROLE_ITEM_TYPE).toInt() : 0;

	/* ---- Add Scene (always available) ---- */
	QAction *addSceneAct = menu.addAction("Add Scene...");
	connect(addSceneAct, &QAction::triggered, this,
		&SceneFolderWidget::addScene);

	if (type == TYPE_SCENE) {
		/* ---- Duplicate ---- */
		QAction *dupAct = menu.addAction("Duplicate");
		connect(dupAct, &QAction::triggered, this,
			[this, sceneName]() { duplicateScene(sceneName); });

		/* ---- Copy / Paste Filters ---- */
		QAction *copyFilters = menu.addAction("Copy Filters");
		connect(copyFilters, &QAction::triggered, this,
			[this, sceneName]() {
				copySceneFilters(sceneName);
			});

		QAction *pasteFilters = menu.addAction("Paste Filters");
		pasteFilters->setEnabled(filterClipboard != nullptr);
		connect(pasteFilters, &QAction::triggered, this,
			[this, sceneName]() {
				pasteSceneFilters(sceneName);
			});

		menu.addSeparator();

		/* ---- Rename ---- */
		QAction *renameAct = menu.addAction("Rename");
		connect(renameAct, &QAction::triggered, this,
			[this, item]() { renameScene(item); });

		/* ---- Remove ---- */
		QAction *removeAct = menu.addAction("Remove");
		connect(removeAct, &QAction::triggered, this,
			[this, sceneName]() { removeScene(sceneName); });

		menu.addSeparator();

		/* ---- Projector ---- */
		QAction *projAct = menu.addAction("Fullscreen Projector");
		connect(projAct, &QAction::triggered, this,
			[this, sceneName]() {
				openSceneProjector(sceneName);
			});

		/* ---- Screenshot ---- */
		QAction *ssAct = menu.addAction("Screenshot Scene");
		connect(ssAct, &QAction::triggered, this,
			[this, sceneName]() { screenshotScene(sceneName); });

		menu.addSeparator();

		/* ---- Filters ---- */
		QAction *filtersAct = menu.addAction("Filters");
		connect(filtersAct, &QAction::triggered, this,
			[this, sceneName]() {
				openSceneFilters(sceneName);
			});

		menu.addSeparator();

		/* ---- Folder management ---- */
		if (item->parent()) {
			QAction *removeFromFolderAct =
				menu.addAction("Remove from Folder");
			connect(removeFromFolderAct, &QAction::triggered,
				this, [this, item]() {
					removeFromFolder(item);
				});
		}

		/* "Move to Folder" submenu */
		QMenu *moveMenu = menu.addMenu("Move to Folder");
		for (int i = 0; i < tree->topLevelItemCount(); i++) {
			QTreeWidgetItem *folder = tree->topLevelItem(i);
			if (folder->data(0, ROLE_ITEM_TYPE).toInt() !=
			    TYPE_FOLDER)
				continue;
			if (folder == item->parent())
				continue;

			QAction *moveAct =
				moveMenu->addAction(folder->text(0));
			connect(moveAct, &QAction::triggered, this,
				[this, item, folder]() {
					QTreeWidgetItem *parent =
						item->parent();
					if (parent) {
						parent->removeChild(item);
					} else {
						tree->takeTopLevelItem(
							tree->indexOfTopLevelItem(
								item));
					}
					folder->addChild(item);
					folder->setExpanded(true);
					saveTimer->start();
				});
		}
		if (moveMenu->actions().isEmpty()) {
			QAction *none =
				moveMenu->addAction("(no folders)");
			none->setEnabled(false);
		}

	} else if (type == TYPE_FOLDER) {
		menu.addSeparator();

		QAction *rename = menu.addAction("Rename Folder");
		connect(rename, &QAction::triggered, this,
			[this, item]() { renameFolder(item); });

		QAction *del = menu.addAction("Delete Folder");
		connect(del, &QAction::triggered, this,
			[this, item]() { deleteFolder(item); });

		menu.addSeparator();

		QAction *sortFolder = menu.addAction("Sort Folder A-Z");
		connect(sortFolder, &QAction::triggered, this,
			[this, item]() {
				item->sortChildren(0, Qt::AscendingOrder);
				saveTimer->start();
			});
	}

	menu.addSeparator();

	/* ---- Always available at bottom ---- */
	QAction *newFolder = menu.addAction("New Folder");
	connect(newFolder, &QAction::triggered, this,
		&SceneFolderWidget::createFolder);

	QAction *sortAZ = menu.addAction("Sort All A-Z");
	connect(sortAZ, &QAction::triggered, this,
		&SceneFolderWidget::sortAlphabetically);

	menu.exec(tree->viewport()->mapToGlobal(pos));
}

void SceneFolderWidget::createFolder()
{
	bool ok;
	QString name = QInputDialog::getText(this, "New Folder",
					     "Folder name:", QLineEdit::Normal,
					     "", &ok);
	if (!ok || name.trimmed().isEmpty())
		return;

	name = name.trimmed();

	if (findFolderItem(name)) {
		QMessageBox::warning(this, "Scene Folders",
				     "A folder with that name already exists.");
		return;
	}

	QTreeWidgetItem *folder = createFolderItem(name);
	/* Insert after the currently selected item, or at the end */
	QTreeWidgetItem *current = tree->currentItem();
	if (current) {
		QTreeWidgetItem *topLevel =
			current->parent() ? current->parent() : current;
		int idx = tree->indexOfTopLevelItem(topLevel);
		tree->insertTopLevelItem(idx + 1, folder);
	} else {
		tree->addTopLevelItem(folder);
	}
	folder->setExpanded(true);
	saveTimer->start();
}

void SceneFolderWidget::renameFolder(QTreeWidgetItem *item)
{
	bool ok;
	QString name = QInputDialog::getText(
		this, "Rename Folder", "New name:", QLineEdit::Normal,
		item->text(0), &ok);
	if (!ok || name.trimmed().isEmpty())
		return;

	name = name.trimmed();

	if (findFolderItem(name) && name != item->text(0)) {
		QMessageBox::warning(this, "Scene Folders",
				     "A folder with that name already exists.");
		return;
	}

	item->setText(0, name);
	saveTimer->start();
}

void SceneFolderWidget::deleteFolder(QTreeWidgetItem *item)
{
	while (item->childCount() > 0) {
		QTreeWidgetItem *child = item->takeChild(0);
		tree->addTopLevelItem(child);
	}
	delete item;
	saveTimer->start();
}

void SceneFolderWidget::removeFromFolder(QTreeWidgetItem *item)
{
	QTreeWidgetItem *parent = item->parent();
	if (parent) {
		parent->removeChild(item);
		tree->addTopLevelItem(item);
		saveTimer->start();
	}
}

/* ------------------------------------------------------------------ */
/*  Scene actions (feature parity with OBS scenes dock)               */
/* ------------------------------------------------------------------ */

void SceneFolderWidget::addScene()
{
	bool ok;
	QString name = QInputDialog::getText(this, "Add Scene",
					     "Scene name:", QLineEdit::Normal,
					     "", &ok);
	if (!ok || name.trimmed().isEmpty())
		return;

	name = name.trimmed();

	/* Check if a scene with this name already exists */
	OBSSourceAutoRelease existing =
		obs_get_source_by_name(name.toUtf8().constData());
	if (existing) {
		QMessageBox::warning(
			this, "Scene Folders",
			"A source with that name already exists.");
		return;
	}

	obs_scene_t *scene = obs_scene_create(name.toUtf8().constData());
	if (scene) {
		obs_source_t *source = obs_scene_get_source(scene);
		obs_frontend_set_current_scene(source);
		obs_scene_release(scene);
		/* refreshSceneList will pick it up via
		   OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED */
	}
}

void SceneFolderWidget::duplicateScene(const QString &sceneName)
{
	OBSSourceAutoRelease source =
		obs_get_source_by_name(sceneName.toUtf8().constData());
	if (!source)
		return;

	obs_scene_t *scene = obs_scene_from_source(source);
	if (!scene)
		return;

	/* Generate a unique name */
	QString newName = sceneName + " (Copy)";
	int counter = 2;
	while (true) {
		OBSSourceAutoRelease check =
			obs_get_source_by_name(newName.toUtf8().constData());
		if (!check)
			break;
		newName = sceneName +
			  QStringLiteral(" (Copy %1)").arg(counter++);
	}

	obs_scene_t *dup = obs_scene_duplicate(
		scene, newName.toUtf8().constData(), OBS_SCENE_DUP_REFS);
	if (dup) {
		obs_source_t *dupSource = obs_scene_get_source(dup);
		obs_frontend_set_current_scene(dupSource);
		obs_scene_release(dup);
	}
}

void SceneFolderWidget::renameScene(QTreeWidgetItem *item)
{
	if (!item)
		return;

	QString oldName = item->text(0);

	bool ok;
	QString newName = QInputDialog::getText(
		this, "Rename Scene", "New name:", QLineEdit::Normal, oldName,
		&ok);
	if (!ok || newName.trimmed().isEmpty())
		return;

	newName = newName.trimmed();
	if (newName == oldName)
		return;

	/* Check for duplicate name */
	OBSSourceAutoRelease existing =
		obs_get_source_by_name(newName.toUtf8().constData());
	if (existing) {
		QMessageBox::warning(
			this, "Scene Folders",
			"A source with that name already exists.");
		return;
	}

	OBSSourceAutoRelease source =
		obs_get_source_by_name(oldName.toUtf8().constData());
	if (source) {
		obs_source_set_name(source, newName.toUtf8().constData());
		item->setText(0, newName);
		saveTimer->start();
	}
}

void SceneFolderWidget::removeScene(const QString &sceneName)
{
	QMessageBox::StandardButton reply = QMessageBox::question(
		this, "Remove Scene",
		QStringLiteral("Are you sure you want to remove \"%1\"?")
			.arg(sceneName),
		QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

	if (reply != QMessageBox::Yes)
		return;

	OBSSourceAutoRelease source =
		obs_get_source_by_name(sceneName.toUtf8().constData());
	if (source)
		obs_source_remove(source);
	/* refreshSceneList will remove it from the tree */
}

void SceneFolderWidget::openSceneFilters(const QString &sceneName)
{
	OBSSourceAutoRelease source =
		obs_get_source_by_name(sceneName.toUtf8().constData());
	if (source)
		obs_frontend_open_source_filters(source);
}

void SceneFolderWidget::copySceneFilters(const QString &sceneName)
{
	OBSSourceAutoRelease source =
		obs_get_source_by_name(sceneName.toUtf8().constData());
	if (!source)
		return;

	if (filterClipboard) {
		obs_data_array_release(filterClipboard);
		filterClipboard = nullptr;
	}

	filterClipboard = obs_source_backup_filters(source);
}

void SceneFolderWidget::pasteSceneFilters(const QString &sceneName)
{
	if (!filterClipboard)
		return;

	OBSSourceAutoRelease source =
		obs_get_source_by_name(sceneName.toUtf8().constData());
	if (source)
		obs_source_restore_filters(source, filterClipboard);
}

void SceneFolderWidget::screenshotScene(const QString &sceneName)
{
	OBSSourceAutoRelease source =
		obs_get_source_by_name(sceneName.toUtf8().constData());
	if (source)
		obs_frontend_take_source_screenshot(source);
}

void SceneFolderWidget::openSceneProjector(const QString &sceneName)
{
	obs_frontend_open_projector("Scene", -1, nullptr,
				    sceneName.toUtf8().constData());
}

void SceneFolderWidget::sortAlphabetically()
{
	/* Sort scenes inside each folder */
	for (int i = 0; i < tree->topLevelItemCount(); i++) {
		QTreeWidgetItem *item = tree->topLevelItem(i);
		if (item->data(0, ROLE_ITEM_TYPE).toInt() == TYPE_FOLDER)
			item->sortChildren(0, Qt::AscendingOrder);
	}

	/* Sort all top-level items alphabetically,
	   folders and scenes mixed together. */
	tree->sortItems(0, Qt::AscendingOrder);

	saveTimer->start();
}

void SceneFolderWidget::filterScenes(const QString &text)
{
	QString filter = text.trimmed().toLower();

	for (int i = 0; i < tree->topLevelItemCount(); i++) {
		QTreeWidgetItem *item = tree->topLevelItem(i);
		int type = item->data(0, ROLE_ITEM_TYPE).toInt();

		if (type == TYPE_SCENE) {
			bool match = filter.isEmpty() ||
				     item->text(0).toLower().contains(filter);
			item->setHidden(!match);
		} else if (type == TYPE_FOLDER) {
			/* Show folder if any child scene matches */
			bool anyChildVisible = false;
			for (int j = 0; j < item->childCount(); j++) {
				bool match =
					filter.isEmpty() ||
					item->child(j)
						->text(0)
						.toLower()
						.contains(filter);
				item->child(j)->setHidden(!match);
				if (match)
					anyChildVisible = true;
			}

			/* Also match folder name itself */
			bool folderMatch =
				item->text(0).toLower().contains(filter);

			if (folderMatch && !filter.isEmpty()) {
				/* Folder name matches: show all children */
				for (int j = 0; j < item->childCount(); j++)
					item->child(j)->setHidden(false);
				item->setHidden(false);
			} else {
				item->setHidden(!filter.isEmpty() &&
						!anyChildVisible);
			}

			/* Auto-expand folders when searching */
			if (!filter.isEmpty() && !item->isHidden())
				item->setExpanded(true);
		}
	}
}

void SceneFolderWidget::highlightActiveScene(const QString &name)
{
	activeSceneName = name;

	QTreeWidgetItemIterator it(tree);
	while (*it) {
		if ((*it)->data(0, ROLE_ITEM_TYPE).toInt() == TYPE_SCENE &&
		    (*it)->text(0) == name) {
			tree->setCurrentItem(*it);
			return;
		}
		++it;
	}
}

void SceneFolderWidget::switchToScene(const QString &sceneName)
{
	if (shuttingDown)
		return;

	ignoreSceneChange = true;
	OBSSourceAutoRelease source =
		obs_get_source_by_name(sceneName.toUtf8().constData());
	if (source)
		obs_frontend_set_current_scene(source);
	ignoreSceneChange = false;
}

/* ------------------------------------------------------------------ */
/*  Config persistence                                                */
/* ------------------------------------------------------------------ */

QString SceneFolderWidget::configFilePath()
{
	char *configDir = obs_module_config_path("");
	QString dir = QString::fromUtf8(configDir);
	bfree(configDir);

	QDir().mkpath(dir);
	return dir + "/scene-folders.json";
}

void SceneFolderWidget::saveConfig()
{
	if (!tree)
		return;

	/* Save the full top-level order: folders and scenes
	   interleaved in their exact positions. */
	QJsonArray topLevelOrder;

	for (int i = 0; i < tree->topLevelItemCount(); i++) {
		QTreeWidgetItem *item = tree->topLevelItem(i);
		int type = item->data(0, ROLE_ITEM_TYPE).toInt();

		QJsonObject entry;
		if (type == TYPE_FOLDER) {
			entry["type"] = "folder";
			entry["name"] = item->text(0);
			entry["expanded"] = item->isExpanded();

			QJsonArray scenes;
			for (int j = 0; j < item->childCount(); j++)
				scenes.append(item->child(j)->text(0));
			entry["scenes"] = scenes;
		} else {
			entry["type"] = "scene";
			entry["name"] = item->text(0);
		}

		topLevelOrder.append(entry);
	}

	QJsonObject root;
	root["version"] = 2;
	root["items"] = topLevelOrder;

	QFile file(configFilePath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(root).toJson());
		file.close();
	}
}

void SceneFolderWidget::loadConfig()
{
	QFile file(configFilePath());
	if (!file.open(QIODevice::ReadOnly))
		return;

	QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
	file.close();

	if (doc.isNull())
		return;

	QJsonObject root = doc.object();
	int version = root["version"].toInt(1);

	if (version >= 2) {
		/* V2 format: ordered list of folders and scenes */
		QJsonArray items = root["items"].toArray();
		for (const QJsonValue &val : items) {
			QJsonObject entry = val.toObject();
			QString type = entry["type"].toString();
			QString name = entry["name"].toString();

			if (type == "folder") {
				bool expanded =
					entry["expanded"].toBool(true);
				QTreeWidgetItem *folder =
					createFolderItem(name);
				tree->addTopLevelItem(folder);
				folder->setExpanded(expanded);

				QJsonArray scenes =
					entry["scenes"].toArray();
				for (const QJsonValue &s : scenes)
					folder->addChild(createSceneItem(
						s.toString()));
			} else {
				tree->addTopLevelItem(
					createSceneItem(name));
			}
		}
	} else {
		/* V1 format: backwards compatibility */
		QJsonArray foldersArray = root["folders"].toArray();
		for (const QJsonValue &val : foldersArray) {
			QJsonObject folderObj = val.toObject();
			QString name = folderObj["name"].toString();
			bool expanded =
				folderObj["expanded"].toBool(true);

			QTreeWidgetItem *folder = createFolderItem(name);
			tree->addTopLevelItem(folder);
			folder->setExpanded(expanded);

			QJsonArray scenes =
				folderObj["scenes"].toArray();
			for (const QJsonValue &sceneVal : scenes)
				folder->addChild(createSceneItem(
					sceneVal.toString()));
		}
	}
}
