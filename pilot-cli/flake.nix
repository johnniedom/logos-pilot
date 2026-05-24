{
  description = "Pilot CLI — Logos autonomous agent (Nim)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
    in {
      packages.${system}.default = pkgs.stdenv.mkDerivation {
        pname = "pilot-cli";
        version = "1.0.0";
        src = ./.;
        nativeBuildInputs = [ pkgs.nim ];
        buildPhase = ''
          export HOME=$TMPDIR
          nim c -d:release \
            --nimcache:$TMPDIR/nimcache \
            --path:. \
            -o:pilot \
            pilot.nim
        '';
        installPhase = ''
          mkdir -p $out/bin
          cp pilot $out/bin/
        '';
      };

      devShells.${system}.default = pkgs.mkShell {
        buildInputs = [ pkgs.nim ];
      };
    };
}
