// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

#include <QWidget>

class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;
class idEditorEngine;

/*
	What is selected, as values to edit. Every edit is one step of the engine's history.

	  an entity   its key/values: the map's own keys bold, the others what the entityDef documents
	  a brush     its faces with material and texture values; the fields below the list change the
	              selected face, or every face of the selected brushes when no face is selected
*/
class Inspector final : public QWidget {
	Q_OBJECT

public:
	explicit Inspector( idEditorEngine * engine, QWidget * parent = nullptr );

	// reads the selection and its values again
	void				Refresh();
	// puts a material on the selected face or brushes; false when no brush is selected
	bool				ApplyMaterial( const QString & name );

signals:
	// the button next to the material: the Content Browser's current material is wanted
	void				materialPickRequested();

private:
	void				ItemChanged( QTreeWidgetItem * item, int column );
	void				ItemActivated( QTreeWidgetItem * item, int column );
	void				AddKey();
	void				ResetKey();
	[[nodiscard]] QWidget *	CreateBrushPage();
	void				RefreshBrush();
	void				FaceRowSelected();
	void				ApplyTexture();

	idEditorEngine *	engine;
	QLabel *			title;
	QStackedWidget *	pages;
	QTreeWidget *		tree;
	QLineEdit *			newKey;
	QLineEdit *			newValue;
	QPushButton *		resetButton;
	int					entity = 0;			// 0 = nothing selected
	bool				refreshing = false;

	QTreeWidget *		faceList = nullptr;
	QLineEdit *			faceMaterial = nullptr;
	QDoubleSpinBox *	faceValues[5] = {};	// shift S, shift T, repeat S, repeat T, rotate
	double				loadedValues[5] = {};
	int					numBrushes = 0;
	int					brushEntity = 0;	// of the first selected brush
	int					brushPrimitive = -1;
	int					brushFace = -1;
};
