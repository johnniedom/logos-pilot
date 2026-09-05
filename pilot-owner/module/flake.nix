{
  description = "pilot_owner - the owner side of a Pilot agent's owner channel as a Logos module (what the Basecamp plugin pilot_remote talks to)";

  inputs = {
    # The same builder revision pilot-module/flake.nix pins, for the same reason (see the note
    # there). A universal module: the interface header is pure C++; the implementation may use
    # Qt, exactly as pilot-module's own sources do.
    logos-module-builder.url = "github:logos-co/logos-module-builder/ddddd8cc4025";
    nix-bundle-lgx.url = "github:logos-co/nix-bundle-lgx";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
      # The shared owner-client library (pilot-owner/src) and the agent's crypto
      # (pilot-module/src) are compiled IN from their one home each, so the console client and
      # this module put identical bytes on the wire. Path literals inside this repository's
      # tree, not flake inputs: relative-path inputs need Nix >= 2.26 and CI installs 2.22.
      # The builder runs its own code generation first; this hook runs after it, before CMake.
      preConfigure = ''
        cp ${../src/owner_client.h} src/owner_client.h
        cp ${../src/owner_client.cpp} src/owner_client.cpp
        cp ${../../pilot-module/src/pilot_crypto.h} src/pilot_crypto.h
        cp ${../../pilot-module/src/pilot_crypto.cpp} src/pilot_crypto.cpp
        chmod u+w src/owner_client.h src/owner_client.cpp src/pilot_crypto.h src/pilot_crypto.cpp
      '';
    };
}
