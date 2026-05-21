#ifndef PILOT_UI_INTERFACE_H
#define PILOT_UI_INTERFACE_H

#include <QObject>
#include <QString>
#include "interface.h"

class PilotUiInterface : public PluginInterface
{
public:
    virtual ~PilotUiInterface() = default;
};

#define PilotUiInterface_iid "org.logos.PilotUiInterface"
Q_DECLARE_INTERFACE(PilotUiInterface, PilotUiInterface_iid)

#endif // PILOT_UI_INTERFACE_H
