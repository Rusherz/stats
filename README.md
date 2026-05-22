# stats

Small terminal monitor for Linux systems (AMD + NVIDIA) showing:

- CPU usage
- Memory usage
- GPU usage
- VRAM usage
- CPU temperature
- GPU temperature

![astats screenshot](assets/screenshot.png)

## Requirements

- Linux
- AMD GPU exposing `/sys/class/drm/card*/device/*` metrics, or NVIDIA GPU with `nvidia-smi` available
- C++17 compiler (`g++`)
- `make`

## Build

```bash
make
```

## Run

```bash
./astats
```

Press `Ctrl+C` to exit.

## Notes

- Data sources: `/proc` and `/sys` (no external libraries required)
- GPU/VRAM/temperature fields show `unavailable` if your kernel/driver does not expose a metric
- NVIDIA metrics are read from `nvidia-smi`; AMD metrics are read from `/sys/class/drm`
