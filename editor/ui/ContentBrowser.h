// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

#include <QWidget>

class AssetListModel;
class EngineClient;
class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class ThumbnailCache;

// The assets of the game data as a grid of pictures: materials, models, entity classes, sounds
// and maps, one kind at a time, filtered by a part of the name.
class ContentBrowser final : public QWidget {
	Q_OBJECT

public:
	explicit ContentBrowser( EngineClient * client, QWidget * parent = nullptr );

	// reads the names; the engine has them only after its start
	void				Load();
	[[nodiscard]] ThumbnailCache *	Thumbnails() const { return thumbnails; }
	// the material that is selected in the grid; "" when the grid shows another kind or nothing is
	[[nodiscard]] QString	CurrentMaterial() const;

signals:
	// a double click; kind is an editorAssetKind_t
	void				activated( int kind, const QString & name );
	// "Apply to Selection": the material is wanted on the selected face or brushes
	void				applyMaterialRequested( const QString & name );

private:
	void				ShowKind( int kind );
	void				ApplyFilter();
	void				ShowMenu( const QPoint & position );

	EngineClient *		client;
	ThumbnailCache *	thumbnails;
	AssetListModel *	model;
	QComboBox *			kinds;
	QLineEdit *			filter;
	QLabel *			count;
	QListView *			view;
	QPushButton *		applyButton;
	bool				loaded = false;
};
