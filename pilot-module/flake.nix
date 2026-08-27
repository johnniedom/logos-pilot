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
    # 549cf115 (2026-08-06) = the module's "bump to LEZ v0.2.2" merge: it pins
    # logos-execution-zone at the v0.2.2 tag. That is the wallet generation the public
    # testnet (chain restarted 2026-08-05, the day v0.2.2 shipped) accepts; our previous pin
    # (24e09df2 -> execution-zone 571f35b3 = v0.2.0+13) built proofs against program IDs
    # the testnet does not know, so reads worked and every spend was rejected. LEZ v0.2.2 ->
    # v0.2.4 touches only sequencer/storage/indexer code — no wallet-ffi, no program
    # artifacts — so this wallet is proof-identical to the v0.2.4 the other LP-0008
    # submissions pin. The next module revs (cd47b9e1+) pin execution-zone `dev` past
    # v0.2.4 with regenerated program artifacts and a new tx-status FFI; not this one.
    # Cost on our side: wallet_ffi_open/create_new gained a statistics_path argument and
    # WalletConfig became `sequencers: [{sequencer_addr}]` (pilot_identity.cpp). The
    # module locks the same logos-module-builder (6ef42ea8) as 24e09df2 did, so no
    # delivery/storage/chat cascade comes with it.
    lez_core.url = "github:logos-blockchain/logos-execution-zone-module/549cf1159f20fa0c3fe8e88a5ab71de68a5aa34b";
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
        # `interface: "universal"`, so its sources compile against the LP-style clients
        # (Qt-free std types: std::string / StdLogosResult / timeout_ms over the
        # logos-protocol C ABI), exactly what the module build generates in ITS sandbox
        # from each dep's published LIDL contract. Without this hook the test compile of
        # every impl TU fails on qt/lp signature mismatches (CI run 31440835689).
        # NOTE the style name is `lp`, NOT `std` — `--api-style=std` was retired by the
        # generator ("Use 'lp' for the Qt-free std-typed surface", CI run on c701b05).
        # And --general-only is REQUIRED: in --metadata mode without it the generator
        # only PRINTS the dependency names and generates nothing (legacy/main.cpp:1099,
        # observed on the 2c7fc4c CI run). With it, --dep specs become <dep>_api.{h,cpp}
        # (BindMode::Static) plus the lp umbrella with this module's origin baked in.
        # x86_64-linux is fixed here because preConfigure is a single string shared by
        # all systems and this project builds/tests on x86_64-linux only (CI + dev box).
        preConfigure = ''
          echo "tests: regenerating dep clients std-style from published LIDL contracts"
          rm -rf ./generated_code
          mkdir -p ./generated_code
          logos-cpp-generator --metadata "$PWD/metadata.json" --general-only --api-style lp \
            --dep lez_core=${inputs.lez_core.packages.x86_64-linux.lidl}/lez_core.lidl \
            --dep delivery_module=${inputs.delivery_module.packages.x86_64-linux.lidl}/delivery_module.lidl \
            --dep storage_module=${inputs.storage_module.packages.x86_64-linux.lidl}/storage_module.lidl \
            --output-dir ./generated_code
        '';
      };
    };
}
