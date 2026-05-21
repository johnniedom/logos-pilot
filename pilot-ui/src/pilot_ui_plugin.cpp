#include "pilot_ui_plugin.h"
#include "logos_api.h"
#include <QDebug>

PilotUiPlugin::PilotUiPlugin(QObject* parent)
    : PilotUiSimpleSource(parent)
{
    setStatus("Ready");
}

PilotUiPlugin::~PilotUiPlugin() = default;

void PilotUiPlugin::initLogos(LogosAPI* api)
{
    m_logosAPI = api;
    setBackend(this);
    qDebug() << "PilotUiPlugin: initialized";
}

int PilotUiPlugin::add(int a, int b)
{
    int result = a + b;
    setStatus(QStringLiteral("%1 + %2 = %3").arg(a).arg(b).arg(result));
    return result;
}
