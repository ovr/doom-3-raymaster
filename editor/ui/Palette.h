// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

#include <QWidget>

class QLineEdit;
class QListWidget;
class idEditorEngine;

// The entity classes of the game data. A double click makes one in front of the camera.
class Palette final : public QWidget {
	Q_OBJECT

public:
	explicit Palette( idEditorEngine * engine, QWidget * parent = nullptr );

	// reads the classes once; the engine has them only after its start
	void				Load();

private:
	void				ApplyFilter( const QString & text );

	idEditorEngine *	engine;
	QLineEdit *			filter;
	QListWidget *		list;
};
