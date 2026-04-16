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

Run as a long-lived daemon and control sessions over a Unix socket:

```bash
ee-eq-cli daemon start
```

Then from another terminal:

```bash
ee-eq-cli apply /path/to/preset.json [--sink <name-or-serial>]
ee-eq-cli status
ee-eq-cli health
ee-eq-cli current-sink
ee-eq-cli enable
ee-eq-cli disable
ee-eq-cli list-sinks
ee-eq-cli shutdown
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

## Advanced

- `EE_EQ_CLI_DEFAULT_PRESET=/path/to/preset.json`
   Fallback preset when --preset is omitted
   
- `EE_EQ_CLI_DISABLE_CONVOLVER=1`
   Disable the convolver

- `EE_EQ_CLI_CONVOLVER_RT_PROCESS=1`
   Enable RT PipeWire convolver processing

