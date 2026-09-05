{
  description = "pilot-owner — talk to a Pilot agent over Logos Messaging from a separate app, no server in between";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    # The agent's own ECIES / ECDSA code, compiled INTO the client so it seals and signs exactly
    # the way the agent decrypts and verifies (pilot_crypto.cpp). A relative-path input: this
    # flake and pilot-module live in the same repository.
    pilot-module-src = { url = "path:../pilot-module"; flake = false; };
  };

  outputs = { self, nixpkgs, pilot-module-src }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
    in {
      packages.${system}.default = pkgs.stdenv.mkDerivation {
        pname = "pilot-owner";
        version = "0.1.0";
        src = ./.;
        nativeBuildInputs = [ pkgs.cmake pkgs.pkg-config pkgs.qt6.wrapQtAppsHook ];
        buildInputs = [ pkgs.qt6.qtbase pkgs.openssl ];
        cmakeFlags = [ "-DPILOT_CRYPTO_DIR=${pilot-module-src}/src" ];
        # A console tool: no Qt platform plugin needed, but the TLS backend (https relays) is.
        qtWrapperArgs = [ "--set QT_QPA_PLATFORM offscreen" ];
        doCheck = true;
        checkPhase = ''
          ./pilot-owner selftest
        '';
      };
    };
}
