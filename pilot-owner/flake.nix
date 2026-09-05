{
  description = "pilot-owner — talk to a Pilot agent over Logos Messaging from a separate app, no server in between";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      # The agent's own ECIES / ECDSA code, compiled INTO the client so it seals and signs exactly
      # the way the agent decrypts and verifies. A path inside this repository's source tree (the
      # flake lives in a subdirectory of the same git checkout), not a flake input: relative-path
      # inputs need Nix >= 2.26 and the CI runners install 2.22.
      pilotCrypto = ../pilot-module/src;
    in {
      packages.${system}.default = pkgs.stdenv.mkDerivation {
        pname = "pilot-owner";
        version = "0.1.0";
        src = ./.;
        nativeBuildInputs = [ pkgs.cmake pkgs.pkg-config pkgs.qt6.wrapQtAppsHook ];
        buildInputs = [ pkgs.qt6.qtbase pkgs.openssl ];
        cmakeFlags = [ "-DPILOT_CRYPTO_DIR=${pilotCrypto}" ];
        # A console tool: no Qt platform plugin needed, but the TLS backend (https relays) is.
        qtWrapperArgs = [ "--set QT_QPA_PLATFORM offscreen" ];
        doCheck = true;
        checkPhase = ''
          ./pilot-owner selftest
        '';
      };
    };
}
