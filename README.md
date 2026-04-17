# ee-eq-cli

`ee-eq-cli` is a headless tool for loading EasyEffects output presets into a minimal PipeWire/LV2 runtime. It supports a limited subset of LV2 plugins (`equalizer`, `convolver`, `limiter`).


It:
- Loads **local presets**
- Creates a **temporary virtual sink**
- Routes matching output streams automatically


## Usage

```bash
ee-eq-cli --preset /path/to/preset.json
```

Optional flags:
- `--sink <name-or-serial>`
- `--dry-run`
- `--list-sinks`

### Daemon Mode

Start a foreground daemon that listens on a Unix socket:

```bash
ee-eq-cli daemon start [--preset /path/to/preset.json] [--sink <name-or-serial>]
```

The daemon runs in the foreground and prints a ready line on startup:

```
daemon ready
socket: /run/user/1000/ee-eq-cli/daemon.sock
session: disabled
pid: 12345
```

When `--preset` is provided (or `EE_EQ_CLI_DEFAULT_PRESET` is set), the daemon auto-applies the preset on startup.

Then from another terminal:

```bash
ee-eq-cli status                                  # full JSON status
ee-eq-cli doctor                                  # human-readable summary
ee-eq-cli health                                  # one-line: ok / degraded / failed
ee-eq-cli current-sink                            # one-line: sink name and serial
ee-eq-cli apply /path/to/preset.json [--sink ..]  # load/replace session
ee-eq-cli enable                                  # restart from remembered config
ee-eq-cli disable                                 # stop session, keep daemon alive
ee-eq-cli bypass on|off                           # glitch-free DSP on/off
ee-eq-cli volume <0.0-1.5>                        # set output volume (linear)
ee-eq-cli list-sinks                              # available PipeWire sinks
ee-eq-cli shutdown                                # stop daemon
```

Convenience:

```bash
ee-eq-cli switch-sink <name-or-serial>            # change sink without re-applying preset
```

### AutoEQ Converter

Convert AutoEQ parametric EQ text files to EasyEffects JSON:

```bash
ee-eq-cli --convert-autoeq input.txt --output preset.json
```


## Convolver

Local kernels are searched under:

```text
~/.local/share/ee-eq-cli/irs/
```

Supported local names:
- `<kernel-name>.irs`

If the kernel is missing, the tool warns and continues without the convolver.

## Build

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
libpipewire-0.3-dev liblilv-dev nlohmann-json3-dev libsndfile1-dev libzita-convolver-dev
cmake -B build -S . -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

## Known Limitations

Physical sink volume (wpctl, GNOME/KDE slider, Bluetooth hardware buttons) has no
audible effect when the filter chain is active. This is a known limitation of direct
PipeWire port-to-port linking (same as EasyEffects). Use `ee-eq-cli volume` instead.

## Environment

- `EE_EQ_CLI_DEFAULT_PRESET=/path/to/preset.json`
   Fallback preset when `--preset` is omitted (standalone mode and daemon start bootstrap only).

The following are available in standalone mode only (unsupported in daemon mode):

- `EE_EQ_CLI_DISABLE_CONVOLVER=1`
   Disable the convolver

- `EE_EQ_CLI_CONVOLVER_RT_PROCESS=1`
   Enable RT PipeWire convolver processing

