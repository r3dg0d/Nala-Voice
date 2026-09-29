# NixOS

Nala is developed on NixOS with Hyprland and an NVIDIA card. Everything is
declarative: nothing here uses `apt`, `dnf` or `pacman`, and a missing tool is
fixed in the flake or your configuration, not with an installer.

## Try it, install it, hack on it

```bash
nix run github:r3dg0d/Nala-Voice            # try it
nix profile install github:r3dg0d/Nala-Voice
nix develop                                 # Qt, the runtime tools, cmake, gdb
```

From a checkout: `nix develop`, then `cmake -S . -B build -G Ninja && cmake --build build`,
and run the tests with `ctest --test-dir build` (or `build/nala-model-tests`,
`build/nala-assistant-tests`). `nix build` runs the suites in its check phase.

## In your system configuration

```nix
{ inputs, pkgs, ... }:
{
  # Nala ships her user unit; enable it per user with
  #   systemctl --user enable --now nala
  environment.systemPackages = [ inputs.nala-voice.packages.${pkgs.stdenv.hostPlatform.system}.default ];
  systemd.packages = [ inputs.nala-voice.packages.${pkgs.stdenv.hostPlatform.system}.default ];

  # ydotoold: the uinput daemon behind clicking and scrolling. The module runs
  # it as a system service and sets YDOTOOL_SOCKET; the socket belongs to a
  # group, so your user has to be in it.
  programs.ydotool.enable = true;
  users.users.YOU.extraGroups = [ "ydotool" ];

  # A warm whisper.cpp server, so each utterance skips the model load.
  systemd.user.services.whisper-server = {
    description = "whisper.cpp speech-recognition server for Nala";
    wantedBy = [ "default.target" ];
    unitConfig.ConditionPathExists = "%h/.local/share/nala/whisper/ggml-base.en.bin";
    serviceConfig = {
      ExecStart = "${pkgs.whisper-cpp}/bin/whisper-server --host 127.0.0.1 --port 8178 --model %h/.local/share/nala/whisper/ggml-base.en.bin --language en";
      Restart = "on-failure";
    };
  };

  services.ollama = {
    enable = true;
    package = pkgs.ollama-cuda;          # NVIDIA
    # Optional, and worth having on a 24 GB card:
    environmentVariables = {
      OLLAMA_FLASH_ATTENTION = "1";
      OLLAMA_KV_CACHE_TYPE = "q8_0";     # halves the KV cache
    };
  };
}
```

Port 8178 is Nala's `stt.serverUrl`; whisper-server's own default of 8080 is
Fish Speech's, hence the different port. Download the whisper model once:
`whisper-cpp-download-ggml-model base.en ~/.local/share/nala/whisper`.

`services.ollama.environmentVariables` is optional. Nala asks Ollama for its own
context size and keep-alive on every request, so `OLLAMA_CONTEXT_LENGTH` is not
needed (and would not override a Modelfile that sets `num_ctx` anyway).

## Runtime tools

The flake puts these on Nala's `PATH` as a fallback (a system copy wins):
`grim wtype ydotool whisper-cpp wireplumber (wpctl) playerctl wl-clipboard
libnotify`. `nvidia-smi` comes from the driver, not the flake, so it is looked up
on the system path (`/run/current-system/sw/bin`); if it is missing Nala simply
reports no GPU information. A screen recorder for the video-recording tool is
not bundled: install `gpu-screen-recorder` or `wf-recorder`.

## Hyprland

Wayland-only, no X11 tools. Screenshots use `grim`, typing `wtype`, the
clipboard `wl-clipboard`, window control Hyprland's IPC socket, audio
PipeWire (through `wpctl`), notifications `notify-send`. A push-to-talk binding:

```
bind = SUPER, N, exec, nala --ptt
```

## The NVIDIA driver and CUDA

Check with `nala doctor` (GPU line, and each loaded model's line, which warns
when a model is partly running from system RAM) and `nala model status`. Nala
does not need CUDA itself; the inference server does. `pkgs.ollama-cuda` is
the CUDA build.
