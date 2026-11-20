// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

#include <QCache>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPixmap>
#include <QString>

class EngineClient;
class QImage;

/*
	The pictures of the content browser.

	  Get() ─┬─ memory (QCache, a fixed budget) ───────────────────────────► the pixmap
	         ├─ disk: <cache>/thumbnails/<kind>/<sha1 of the name>.png ─────► the pixmap
	         └─ wanted (the newest first, a bounded list) ── Feed() ── engine->RequestPreview()
	                                                                         │ a frame or two later
	                                 ready() ◄── memory + disk ◄── previewReady( id, image )

	Only a few requests are with the engine at a time, so scrolling past thousands of items
	leaves no queue behind: what scrolled out of the wanted list is asked for again when seen.
*/
class ThumbnailCache final : public QObject {
	Q_OBJECT

public:
	explicit ThumbnailCache( EngineClient * client, QObject * parent = nullptr );

	// kind is an editorPreviewKind_t. A null pixmap: the picture is being made, ready() follows.
	[[nodiscard]] QPixmap	Get( int kind, const QString & name );
	// the view shows other things now
	void					DropWanted();
	// the asset changed: its picture is made again when asked for
	void					Invalidate( int kind, const QString & name );

signals:
	void					ready( int kind, const QString & name );

private:
	struct request_t {
		int					kind = 0;
		QString				name;
	};

	void					Feed();
	void					Received( int id, const QImage & image );
	[[nodiscard]] QString	FilePath( int kind, const QString & name ) const;

	EngineClient *			client;
	QString					folder;
	QCache<QString, QPixmap>	memory;
	QList<request_t>		wanted;			// the newest last
	QHash<int, request_t>	inFlight;		// by the id given to the engine
	int						nextId = 1;
};
