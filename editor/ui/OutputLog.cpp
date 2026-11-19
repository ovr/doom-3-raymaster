// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "OutputLog.h"

#include <QFontDatabase>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextCursor>
#include <QVBoxLayout>

namespace {

constexpr int MAX_LOG_LINES = 10000;
constexpr qsizetype MAX_HISTORY = 100;

}	// namespace

OutputLog::OutputLog( QWidget * parent ) : QWidget( parent ) {
	const QFont mono = QFontDatabase::systemFont( QFontDatabase::FixedFont );

	view = new QPlainTextEdit( this );
	view->setReadOnly( true );
	view->setMaximumBlockCount( MAX_LOG_LINES );
	view->setLineWrapMode( QPlainTextEdit::NoWrap );
	view->setFont( mono );

	line = new QLineEdit( this );
	line->setPlaceholderText( tr( "Console command" ) );
	line->setFont( mono );
	line->installEventFilter( this );
	connect( line, &QLineEdit::returnPressed, this, &OutputLog::Submit );

	auto * const layout = new QVBoxLayout( this );
	layout->setContentsMargins( 0, 0, 0, 0 );
	layout->setSpacing( 2 );
	layout->addWidget( view );
	layout->addWidget( line );
}

void OutputLog::Append( const QString & text ) {
	// keep the view where the user scrolled it, follow the output only at the end
	QScrollBar * const bar = view->verticalScrollBar();
	const bool atEnd = ( bar->value() >= bar->maximum() - 2 );

	QTextCursor cursor( view->document() );
	cursor.movePosition( QTextCursor::End );
	cursor.insertText( text );

	if ( atEnd ) {
		bar->setValue( bar->maximum() );
	}
}

void OutputLog::FocusCommandLine() {
	line->setFocus( Qt::ShortcutFocusReason );
	line->selectAll();
}

void OutputLog::Submit() {
	const QString command = line->text().trimmed();
	line->clear();
	if ( command.isEmpty() ) {
		return;
	}
	history.removeAll( command );
	history.append( command );
	if ( history.size() > MAX_HISTORY ) {
		history.removeFirst();
	}
	historyAt = history.size();
	emit commandEntered( command );
}

void OutputLog::ShowHistory( int step ) {
	const qsizetype at = qBound<qsizetype>( 0, historyAt + step, history.size() );
	if ( at == historyAt ) {
		return;
	}
	historyAt = at;
	line->setText( ( at < history.size() ) ? history[at] : QString() );
}

bool OutputLog::eventFilter( QObject * watched, QEvent * event ) {
	if ( watched == line && event->type() == QEvent::KeyPress ) {
		const int key = static_cast<QKeyEvent *>( event )->key();
		if ( key == Qt::Key_Up || key == Qt::Key_Down ) {
			ShowHistory( ( key == Qt::Key_Up ) ? -1 : 1 );
			return true;
		}
	}
	return QWidget::eventFilter( watched, event );
}
