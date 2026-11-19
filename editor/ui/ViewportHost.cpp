// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "ViewportHost.h"

#include <QApplication>
#include <QEvent>

#include "../api/EditorEngineApi.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

ViewportHost::ViewportHost( QWidget * parent ) : QWidget( parent ) {
	setAttribute( Qt::WA_NativeWindow );
	setAttribute( Qt::WA_PaintOnScreen );
	setAttribute( Qt::WA_NoSystemBackground );
	setAttribute( Qt::WA_OpaquePaintEvent );
	setMinimumSize( 160, 90 );
}

void ViewportHost::Attach( idEditorEngine * engine_ ) {
	engine = engine_;
	qApp->installEventFilter( this );
	SyncSize();
}

void ViewportHost::resizeEvent( QResizeEvent * event ) {
	QWidget::resizeEvent( event );
	SyncSize();
}

void ViewportHost::SyncSize() {
	if ( engine != nullptr ) {
		const qreal scale = devicePixelRatioF();
		engine->ResizeViewport( qRound( width() * scale ), qRound( height() * scale ) );
	}
}

bool ViewportHost::eventFilter( QObject * watched, QEvent * event ) {
	if ( event->type() == QEvent::MouseButtonPress && engine != nullptr && engine->ViewportHasFocus() ) {
		if ( const QWidget * const widget = qobject_cast<QWidget *>( watched ); widget != nullptr && widget != this ) {
			QWidget * const top = widget->window();
			top->activateWindow();
			SetFocus( reinterpret_cast<HWND>( top->winId() ) );
		}
	}
	return QWidget::eventFilter( watched, event );
}
