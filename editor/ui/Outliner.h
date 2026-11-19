// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;
class idEditorEngine;

// The entities of the open map, with a filter on name and class. A double click puts the camera
// in front of the entity.
class Outliner final : public QWidget {
	Q_OBJECT

public:
	explicit Outliner( idEditorEngine * engine, QWidget * parent = nullptr );

	// reads the entities again, after a map was opened or closed
	void				Reload();

private:
	void				ApplyFilter( const QString & text );
	void				Activate( QTreeWidgetItem * item );

	idEditorEngine *	engine;
	QLineEdit *			filter;
	QTreeWidget *		tree;
	QLabel *			count;
};
