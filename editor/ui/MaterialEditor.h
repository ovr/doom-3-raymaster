// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

#include <QByteArray>
#include <QString>
#include <QTimer>
#include <QWidget>

class EngineClient;
class QImage;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class ThumbnailCache;

/*
	One material declaration, edited as text and as a list of keys at once.

	  the text (QPlainTextEdit) ── typing ── a pause ──► mtr::ParseDecl ──► the key list
	        ▲                                                                  │
	        └────────── the bytes of one value are replaced ◄── a value edited ┘

	The text is the document; the list only points into it, so comments and spacing survive.
	Apply gives the text to the engine: the viewport and the preview show it at once and no file
	changes. Save applies and writes the material's .mtr into the project folder.
*/
class MaterialEditor final : public QWidget {
	Q_OBJECT

public:
	MaterialEditor( EngineClient * client, ThumbnailCache * thumbnails, QWidget * parent = nullptr );

	void				Open( const QString & name );

private:
	void				RebuildKeys();
	void				ValueEdited( QTreeWidgetItem * item, int column );
	bool				Apply();
	void				Save();
	void				Revert();
	void				RequestPreview();
	void				PreviewReady( int id, const QImage & image );
	void				UpdateState();
	// the text as the engine takes it: 8 bit, with the line ends the material had
	[[nodiscard]] QByteArray	EngineText() const;

	EngineClient *		client;
	ThumbnailCache *	thumbnails;
	QLabel *			title;
	QLabel *			preview;
	QLabel *			status;
	QTreeWidget *		keys;
	QPlainTextEdit *	text;
	QPushButton *		applyButton;
	QPushButton *		saveButton;
	QPushButton *		revertButton;
	QTimer				parseTimer;

	QString				name;
	QString				fileName;
	QString				openedText;			// as it was when opened, for Revert
	QString				appliedText;		// what the engine has
	bool				implicit = false;
	bool				crlf = false;
	bool				unsaved = false;	// applied, but not written to the file
	bool				fillingKeys = false;
	int					previewId = 0;
};
