#pragma once

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>

#include <QWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QMenu>
#include <QAction>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QString>
#include <QTimer>
#include <QSet>

class SceneFolderWidget : public QWidget {
	Q_OBJECT

public:
	explicit SceneFolderWidget(QWidget *parent = nullptr);
	~SceneFolderWidget();

	void saveConfig();
	void loadConfig();
	void shutdown();

private slots:
	void onItemDoubleClicked(QTreeWidgetItem *item, int column);
	void onCustomContextMenu(const QPoint &pos);
	void refreshSceneList();

private:
	QTreeWidgetItem *createFolderItem(const QString &name);
	QTreeWidgetItem *createSceneItem(const QString &name);
	QTreeWidgetItem *findFolderItem(const QString &name);

	void createFolder();
	void renameFolder(QTreeWidgetItem *item);
	void deleteFolder(QTreeWidgetItem *item);
	void removeFromFolder(QTreeWidgetItem *item);
	void switchToScene(const QString &sceneName);

	QString configFilePath();

	QTreeWidget *tree = nullptr;
	QTimer *saveTimer = nullptr;
	bool shuttingDown = false;
	bool ignoreSceneChange = false;

	static constexpr int ROLE_ITEM_TYPE = Qt::UserRole + 1;
	static constexpr int TYPE_FOLDER = 1;
	static constexpr int TYPE_SCENE = 2;

	static void onFrontendEvent(enum obs_frontend_event event, void *data);
};
