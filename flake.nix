{
  description = "Nala, a desktop companion for Hyprland";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems
        (system: f nixpkgs.legacyPackages.${system});

      qtDeps = pkgs: with pkgs; [
        qt6.qtbase
        qt6.qtdeclarative
        qt6.qtshadertools
        qt6.qtsvg
        qt6.qtwayland
        qt6.qtmultimedia
        kdePackages.layer-shell-qt
        onnxruntime   # the wake-word detector
      ];

      # Optional helpers Nala shells out to. Missing ones are reported by
      # `nala doctor` rather than failing, so none of these is required.
      runtimeTools = pkgs: with pkgs; [ grim wtype ydotool whisper-cpp ];
    in {
      packages = forAll (pkgs: {
        default = pkgs.stdenv.mkDerivation {
          pname = "nala";
          version = "1.2.0";
          src = self;
          nativeBuildInputs = with pkgs; [ cmake ninja qt6.wrapQtAppsHook ];
          buildInputs = qtDeps pkgs;
          doCheck = true;
          checkPhase = ''
            QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME= \
              QT_PLUGIN_PATH=${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix} \
              QML_IMPORT_PATH=${pkgs.qt6.qtdeclarative}/${pkgs.qt6.qtbase.qtQmlPrefix} \
              HYPRLAND_INSTANCE_SIGNATURE= ./nala-assistant-tests
            QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME= \
              QT_PLUGIN_PATH=${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix} \
              QML_IMPORT_PATH=${pkgs.qt6.qtdeclarative}/${pkgs.qt6.qtbase.qtQmlPrefix} \
              ./nala --self-test
          '';
          # Found on PATH if the system has its own; these are the fallback.
          qtWrapperArgs = [
            "--suffix PATH : ${pkgs.lib.makeBinPath (runtimeTools pkgs)}"
          ];
          meta = {
            description = "A desktop companion for Hyprland with a local voice assistant";
            license = pkgs.lib.licenses.mit;
            platforms = systems;
            mainProgram = "nala";
          };
        };
      });

      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          packages = (qtDeps pkgs) ++ (runtimeTools pkgs)
            ++ (with pkgs; [ cmake ninja gdb clang-tools ]);
          # mkShell does not run wrapQtAppsHook, so point an unwrapped build at
          # Qt's plugins and QML modules by hand.
          shellHook = ''
            export QT_PLUGIN_PATH=${pkgs.lib.concatMapStringsSep ":"
              (p: "${p}/${pkgs.qt6.qtbase.qtPluginPrefix}") (qtDeps pkgs)}
            export QML_IMPORT_PATH=${pkgs.lib.concatMapStringsSep ":"
              (p: "${p}/${pkgs.qt6.qtbase.qtQmlPrefix}") (qtDeps pkgs)}
          '';
        };
      });
    };
}
