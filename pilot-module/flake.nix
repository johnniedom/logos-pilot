{
  description = "Logos Pilot Module";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    nix-bundle-lgx.url = "github:logos-co/nix-bundle-lgx";
    lez_wallet_module.url = "github:logos-blockchain/logos-execution-zone-module";
    delivery_module.url = "github:logos-co/logos-delivery-module";
    storage_module.url = "github:logos-co/logos-storage-module";
    waku_module.url = "github:logos-co/logos-waku-module";
    chat_module.url = "github:logos-co/logos-chat-module";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
      preConfigure = ''
        logos-cpp-generator --from-header src/pilot_impl.h \
          --backend qt \
          --impl-class PilotImpl \
          --impl-header pilot_impl.h \
          --metadata metadata.json \
          --output-dir ./generated_code

        # The generator does NOT produce an onInit wrapper (we removed it from
        # the impl header). Inject an override so the framework can pass the
        # LogosAPI pointer to PilotImpl when the module loads.
        sed -i '/^private:/i\    void onInit(LogosAPI* api) override {\n        m_impl.logosAPI_ = api;\n    }\n' \
          ./generated_code/pilot_qt_glue.h
      '';
    };
}
