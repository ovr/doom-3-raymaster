// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

#include <QWidget>

class idEditorEngine;

// The native window the engine's own window is a child of. Qt draws nothing here: the engine's
// window covers it. The engine is told when the size changes.
class ViewportHost final : public QWidget {
	Q_OBJECT

public:
	explicit ViewportHost( QWidget * parent = nullptr );

	// from here on the engine window exists and follows this widget
	void				Attach( idEditorEngine * engine );

	[[nodiscard]] QPaintEngine *	paintEngine() const override { return nullptr; }

protected:
	void				resizeEvent( QResizeEvent * event ) override;
	// A click on a Qt widget does not move the Win32 focus away from the engine's window,
	// so the keys would stay in the game: take the focus back for Qt.
	bool				eventFilter( QObject * watched, QEvent * event ) override;

private:
	void				SyncSize();

	idEditorEngine *	engine = nullptr;
};
