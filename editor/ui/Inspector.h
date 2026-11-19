// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class idEditorEngine;

// The key/values of the first selected entity. The map's own keys are bold; the others are what
// the entityDef documents, with its defaults. Every edit is one step of the engine's history.
class Inspector final : public QWidget {
	Q_OBJECT

public:
	explicit Inspector( idEditorEngine * engine, QWidget * parent = nullptr );

	// reads the selection and its keys again
	void				Refresh();

private:
	void				ItemChanged( QTreeWidgetItem * item, int column );
	void				ItemActivated( QTreeWidgetItem * item, int column );
	void				AddKey();
	void				ResetKey();

	idEditorEngine *	engine;
	QLabel *			title;
	QTreeWidget *		tree;
	QLineEdit *			newKey;
	QLineEdit *			newValue;
	QPushButton *		resetButton;
	int					entity = 0;			// 0 = nothing selected
	bool				refreshing = false;
};
