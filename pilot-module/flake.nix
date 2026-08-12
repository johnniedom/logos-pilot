{
  description = "Logos Pilot Module";

  inputs = {
    # Pinned to the revision this project has always built with. Floating it (or pinning it
    # forward to today's ddddd8cc) resolves to a builder that REJECTS our May-era
    # delivery_module — "cannot be consumed by an lp (Qt-free) module ... rebuild / re-pin
    # against a current logos-module-builder" — so moving this one pin forces the entire module
    # stack (delivery/storage/chat/waku) forward with it. Pinning holds that cascade off while
    # lez_core below still delivers the payment fix.
    logos-module-builder.url = "github:logos-co/logos-module-builder/ddddd8cc4025";
    nix-bundle-lgx.url = "github:logos-co/nix-bundle-lgx";
    # 24e09df2 (2026-07-03), NOT the newer 033f807a. Both pin execution-zone 571f35b3 — the
    # revision that fixes "Nullifier already seen" on pay-by-keys to an account with on-chain
    # history (upstream #268) — but 033f807a's generated client headers need an SDK from
    # 2026-08-03, which drags in a builder that rejects the rest of our pinned module stack.
    # This revision buys the payment fix without that cascade.
    lez_core.url = "github:logos-blockchain/logos-execution-zone-module/24e09df2af5774cab3f83d2f521533abb7ddffdd";
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
      # No module-build preConfigure. The old one hand-ran `logos-cpp-generator --backend qt`
      # and then sed-patched an onInit override into the generated Qt glue. Both are obsolete
      # on the current builder: it generates the universal glue itself, and Qt glue moved out
      # to logos-qt-generator. Modules on this builder (e.g. lez_core 24e09df2) declare
      # codegen in metadata.json and carry no preConfigure — matched here.
      tests = {
        dir = ./tests;
        # mkLogosModuleTests assembles ./generated_code by copying each dep's QT-style
        # client headers and generating a qt umbrella — but this module is
        # `interface: "universal"`, so its sources compile against the STD-style clients
        # (std::string / StdLogosResult / timeout_ms), exactly what the module build
        # generates in ITS sandbox from each dep's published LIDL contract. Without this
        # hook the test compile of every impl TU fails on qt/std signature mismatches
        # (CI run 31440835689). Regenerate the whole tree to match the module build.
        # x86_64-linux is fixed here because preConfigure is a single string shared by
        # all systems and this project builds/tests on x86_64-linux only (CI + dev box).
        preConfigure = ''
          echo "tests: regenerating dep clients std-style from published LIDL contracts"
          rm -rf ./generated_code
          mkdir -p ./generated_code
          logos-cpp-generator --metadata "$PWD/metadata.json" --api-style std \
            --dep lez_core=${inputs.lez_core.packages.x86_64-linux.lidl}/lez_core.lidl \
            --dep delivery_module=${inputs.delivery_module.packages.x86_64-linux.lidl}/delivery_module.lidl \
            --dep storage_module=${inputs.storage_module.packages.x86_64-linux.lidl}/storage_module.lidl \
            --output-dir ./generated_code
        '';
      };
    };
}
