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
	tree->viewport()->setAcceptDrops(true);
	tree->setDropIndicatorShown(true);

	layout->addWidget(tree);

	/* Connections */
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
				QTreeWidgetItemIterator it(w->tree);
				while (*it) {
					if ((*it)->data(0, ROLE_ITEM_TYPE)
						    .toInt() == TYPE_SCENE &&
					    (*it)->text(0) == name) {
						w->tree->setCurrentItem(*it);
						break;
					}
					++it;
				}
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
		       Qt::ItemIsDropEnabled | Qt::ItemIsEditable);
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
		QTreeWidgetItemIterator selIt(tree);
		while (*selIt) {
			if ((*selIt)->data(0, ROLE_ITEM_TYPE).toInt() ==
				    TYPE_SCENE &&
			    (*selIt)->text(0) == currentName) {
				tree->setCurrentItem(*selIt);
				break;
			}
			++selIt;
		}
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

	if (!item) {
		/* Right-clicked on empty space */
		QAction *newFolder = menu.addAction("New Folder");
		connect(newFolder, &QAction::triggered, this,
			&SceneFolderWidget::createFolder);
	} else {
		int type = item->data(0, ROLE_ITEM_TYPE).toInt();

		if (type == TYPE_FOLDER) {
			QAction *rename = menu.addAction("Rename Folder");
			connect(rename, &QAction::triggered, this,
				[this, item]() { renameFolder(item); });

			QAction *del = menu.addAction("Delete Folder");
			connect(del, &QAction::triggered, this,
				[this, item]() { deleteFolder(item); });

			menu.addSeparator();

			QAction *newFolder = menu.addAction("New Folder");
			connect(newFolder, &QAction::triggered, this,
				&SceneFolderWidget::createFolder);

		} else if (type == TYPE_SCENE) {
			QAction *switchTo =
				menu.addAction("Switch to Scene");
			connect(switchTo, &QAction::triggered, this,
				[this, item]() {
					switchToScene(item->text(0));
				});

			if (item->parent()) {
				menu.addSeparator();
				QAction *remove = menu.addAction(
					"Remove from Folder");
				connect(remove, &QAction::triggered, this,
					[this, item]() {
						removeFromFolder(item);
					});
			}

			/* "Move to Folder" submenu */
			QMenu *moveMenu = menu.addMenu("Move to Folder");
			for (int i = 0; i < tree->topLevelItemCount(); i++) {
				QTreeWidgetItem *folder =
					tree->topLevelItem(i);
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
							parent->removeChild(
								item);
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

			menu.addSeparator();

			QAction *newFolder = menu.addAction("New Folder");
			connect(newFolder, &QAction::triggered, this,
				&SceneFolderWidget::createFolder);
		}
	}

	if (!menu.actions().isEmpty())
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
	tree->insertTopLevelItem(0, folder);
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

	QJsonArray foldersArray;

	for (int i = 0; i < tree->topLevelItemCount(); i++) {
		QTreeWidgetItem *item = tree->topLevelItem(i);

		if (item->data(0, ROLE_ITEM_TYPE).toInt() == TYPE_FOLDER) {
			QJsonObject folder;
			folder["name"] = item->text(0);
			folder["expanded"] = item->isExpanded();

			QJsonArray scenes;
			for (int j = 0; j < item->childCount(); j++)
				scenes.append(item->child(j)->text(0));
			folder["scenes"] = scenes;
			foldersArray.append(folder);
		}
	}

	QJsonObject root;
	root["folders"] = foldersArray;

	QJsonArray unassigned;
	for (int i = 0; i < tree->topLevelItemCount(); i++) {
		QTreeWidgetItem *item = tree->topLevelItem(i);
		if (item->data(0, ROLE_ITEM_TYPE).toInt() == TYPE_SCENE)
			unassigned.append(item->text(0));
	}
	root["unassigned_order"] = unassigned;

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
	QJsonArray foldersArray = root["folders"].toArray();

	for (const QJsonValue &val : foldersArray) {
		QJsonObject folderObj = val.toObject();
		QString name = folderObj["name"].toString();
		bool expanded = folderObj["expanded"].toBool(true);

		QTreeWidgetItem *folder = createFolderItem(name);
		tree->addTopLevelItem(folder);
		folder->setExpanded(expanded);

		QJsonArray scenes = folderObj["scenes"].toArray();
		for (const QJsonValue &sceneVal : scenes)
			folder->addChild(createSceneItem(sceneVal.toString()));
	}
}
