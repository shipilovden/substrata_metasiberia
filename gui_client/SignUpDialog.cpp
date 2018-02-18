/*=====================================================================
SignUpDialog.cpp
----------------
=====================================================================*/
#include "SignUpDialog.h"


#include "LoginDialog.h"
#include "../qt/QtUtils.h"
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QErrorMessage>
#include <QtWidgets/QPushButton>
#include <QtCore/QSettings>


SignUpDialog::SignUpDialog(QSettings* settings_)
:	settings(settings_)
{
	setupUi(this);

	// Remove question mark from the title bar (see https://stackoverflow.com/questions/81627/how-can-i-hide-delete-the-help-button-on-the-title-bar-of-a-qt-dialog)
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);


	// Load main window geometry and state
	this->restoreGeometry(settings->value("SignUpDialog/geometry").toByteArray());

	this->usernameLineEdit->setText(settings->value("SignUpDialog/username").toString());
	this->emailLineEdit   ->setText(settings->value("SignUpDialog/email"   ).toString());
	//this->passwordLineEdit->setText(settings->value("SignUpDialog/password").toString());

	this->buttonBox->button(QDialogButtonBox::Ok)->setText("Sign up");

	connect(this->buttonBox, SIGNAL(accepted()), this, SLOT(accepted()));
}


SignUpDialog::~SignUpDialog()
{
	settings->setValue("SignUpDialog/geometry", saveGeometry());
}


void SignUpDialog::accepted()
{
	settings->setValue("SignUpDialog/username", this->usernameLineEdit->text());
	settings->setValue("SignUpDialog/email",    this->emailLineEdit->text());
	//settings->setValue("SignUpDialog/password", this->passwordLineEdit->text());

	// Save these credentials in the LoginDialog settings as well, so that next time the user starts cyberspace it can log them in.
	settings->setValue("LoginDialog/username", this->usernameLineEdit->text());
	settings->setValue("LoginDialog/password", QtUtils::toQString(LoginDialog::encryptPassword(QtUtils::toStdString(this->passwordLineEdit->text()))));
}
