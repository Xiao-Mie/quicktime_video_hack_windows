#include "mainwindow.h"

#include <QApplication>
#include <QDebug>
#include <windows.h>

QString g_RecordFilePath = "D:/ios-record.mp4";
bool g_isCliMode = false;
MainWindow* g_pMainWindow = nullptr;

BOOL WINAPI ConsoleHandler(DWORD CEvent)
{
    if (CEvent == CTRL_C_EVENT)
    {
        qDebug() << "Ctrl+C received, stopping recording...";
        if (g_pMainWindow) {
            g_pMainWindow->AutoStop();
        }
        QApplication::quit();
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    for (int i = 1; i < argc; ++i) {
        QString arg = argv[i];
        if (arg == "--cli") {
            g_isCliMode = true;
        } else {
            g_RecordFilePath = arg;
        }
    }

    if (g_isCliMode) {
        SetConsoleCtrlHandler((PHANDLER_ROUTINE)ConsoleHandler, TRUE);
    }

    MainWindow w;
    g_pMainWindow = &w;
    
    if (!g_isCliMode) {
        w.show();
    } else {
        w.AutoStart();
        qDebug() << "Running in CLI mode. Press Ctrl+C to stop recording and exit.";
    }

    int ret = a.exec();
    g_pMainWindow = nullptr;
    return ret;
}
