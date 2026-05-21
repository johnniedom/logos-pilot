#pragma once
// Override onInit in the generated PilotProviderObject to pass LogosAPI*
// to PilotImpl. The code generator produces onInit(QVariant) which hides
// the base class's virtual onInit(LogosAPI*). This macro-based hook
// is included by the generated glue and provides the correct override.

#define PILOT_PROVIDER_HOOK                                    \
protected:                                                     \
    void onInit(LogosAPI* api) override {                      \
        m_impl.setLogosAPI(api);                               \
    }
