# eq-cli

Headless PipeWire DSP runner for EasyEffects-compatible presets.
Creates a virtual sink, routes matching streams, and exposes a daemon/CLI control surface.

## Usage

### Standalone

Run a preset in the foreground until interrupted:

```bash
eq-cli --preset /path/to/preset.json
```

Optional flags:
- `--sink <name-or-serial>` — target a specific PipeWire sink
- `--dry-run` — validate preset and print what would be applied
- `--list-sinks` — list available PipeWire output sinks
- `--convert-autoeq input.txt --output preset.json` — convert AutoEQ parametric EQ text to EasyEffects JSON

### Daemon Mode

Start a foreground daemon that listens on a Unix socket:

```bash
eq-cli daemon start [--preset /path/to/preset.json] [--sink <name-or-serial>]
```

The daemon prints a ready line on startup:

```
daemon ready
socket: /run/user/1000/eq-cli/daemon.sock
session: disabled
pid: 12345
```

When `--preset` is provided (or `EQ_CLI_DEFAULT_PRESET` is set), the daemon auto-applies the preset on startup.

Then from another terminal:

```bash
eq-cli apply /path/to/preset.json [--sink ..]  # load/replace session
eq-cli enable                                  # restart from remembered config
eq-cli disable                                 # stop session, keep daemon alive
eq-cli bypass on|off                           # glitch-free DSP on/off
eq-cli volume <0.0-1.5>                        # set output volume (linear)
eq-cli switch-sink <name-or-serial>            # change sink without re-applying preset
eq-cli list-sinks                              # available PipeWire sinks
eq-cli status                                  # full JSON status
eq-cli doctor                                  # human-readable summary
eq-cli health                                  # one-line: ok / degraded / failed
eq-cli current-sink                            # one-line: sink name and serial
eq-cli shutdown                                # stop daemon
```

## Convolver

Local impulse-response kernels (`.irs`) are searched under `~/.local/share/eq-cli/irs/`. If the kernel referenced by the preset is missing, the tool warns and continues without the convolver.

## Build

### Arch Linux

```bash
makepkg -si
```

### Debian / Ubuntu

```bash
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
  libpipewire-0.3-dev liblilv-dev nlohmann-json3-dev libsndfile1-dev libzita-convolver-dev
cmake -B build -S . -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

## Known Limitations

Physical sink volume (wpctl, GNOME/KDE slider, Bluetooth hardware buttons) has no
audible effect when the filter chain is active. This is a known limitation of direct
PipeWire port-to-port linking (same as EasyEffects). Use `eq-cli volume` instead.

## Environment

- `EQ_CLI_DEFAULT_PRESET=/path/to/preset.json`
   Fallback preset when `--preset` is omitted (standalone mode and daemon start bootstrap only).

The following are available in standalone mode only (unsupported in daemon mode):

- `EQ_CLI_DISABLE_CONVOLVER=1`
   Disable the convolver

- `EQ_CLI_CONVOLVER_RT_PROCESS=1`
   Enable RT PipeWire convolver processing

- `EQ_CLI_CONVOLVER_SCHED_FIFO=1`
   Prefer SCHED_FIFO for the convolver worker thread
