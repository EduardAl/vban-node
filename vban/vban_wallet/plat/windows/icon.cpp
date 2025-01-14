#include <vban/vban_wallet/icon.hpp>

#include <QApplication>
#include <QtGui>
#include <QtWin>

void vban::set_application_icon (QApplication & application_a)
{
	HICON hIcon = static_cast<HICON> (LoadImage (GetModuleHandle (nullptr), MAKEINTRESOURCE (1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_LOADTRANSPARENT));
	application_a.setWindowIcon (QIcon (QtWin::fromHICON (hIcon)));
	DestroyIcon (hIcon);
}
