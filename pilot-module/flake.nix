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
      # No preConfigure. The old one hand-ran `logos-cpp-generator --backend qt` and then
      # sed-patched an onInit override into the generated Qt glue. Both are obsolete on the
      # current builder: it generates the universal glue itself, and Qt glue moved out to
      # logos-qt-generator ("Error: Qt glue generation moved to logos-qt-generator ... this tool
      # keeps the Qt-free outputs"). Modules on this builder (e.g. lez_core 24e09df2) declare
      # codegen in metadata.json and carry no preConfigure — matched here.
    };
}
