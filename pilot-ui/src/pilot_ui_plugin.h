#ifndef PILOT_UI_PLUGIN_H
#define PILOT_UI_PLUGIN_H

#include <QString>
#include <QVariantList>
#include "pilot_ui_interface.h"
#include "LogosViewPluginBase.h"
#include "rep_pilot_ui_source.h"

class LogosAPI;

class PilotUiPlugin : public PilotUiSimpleSource,
                        public PilotUiInterface,
                        public PilotUiViewPluginBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PilotUiInterface_iid FILE "metadata.json")
    Q_INTERFACES(PilotUiInterface)

public:
    explicit PilotUiPlugin(QObject* parent = nullptr);
    ~PilotUiPlugin() override;

    QString name()    const override { return "pilot_ui"; }
    QString version() const override { return "1.0.0"; }

    Q_INVOKABLE void initLogos(LogosAPI* api);

    // Slots from pilot_ui.rep
    int add(int a, int b) override;

signals:
    void eventResponse(const QString& eventName, const QVariantList& args);

private:
    LogosAPI* m_logosAPI = nullptr;
};

#endif // PILOT_UI_PLUGIN_H
