// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

#include <QStringList>
#include <QWidget>

class QLineEdit;
class QPlainTextEdit;

// The engine's console output and a line that sends console commands, with a history on Up/Down.
class OutputLog final : public QWidget {
	Q_OBJECT

public:
	explicit OutputLog( QWidget * parent = nullptr );

	void				Append( const QString & text );
	void				FocusCommandLine();

signals:
	void				commandEntered( const QString & command );

protected:
	bool				eventFilter( QObject * watched, QEvent * event ) override;

private:
	void				Submit();
	void				ShowHistory( int index );

	QPlainTextEdit *	view;
	QLineEdit *			line;
	QStringList			history;
	qsizetype			historyAt = 0;
};
