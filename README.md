# DEKB ZMK Firmware

Official firmware repository for **DEKB custom keyboards** powered by ZMK.

This repository contains the firmware configurations, boards, and shields used to build firmware for DEKB keyboards.

## GitHub Actions

Pushes, pull requests, and manual runs build the targets listed in `build.yaml`.
The workflow uses `config/west.yml`, which follows the PH-Lite manifest and
checks out `ph-design/zmk` at `v0.3-lts`. Build artifacts are published by the
workflow as the `firmware` artifact.

The repository itself remains a Zephyr module: custom boards and shields stay
under `boards/`, while the workflow passes the repository as `ZMK_EXTRA_MODULES`.

For a local workspace, initialize West from the canonical manifest:

```sh
west init -l config
west update
west zephyr-export
```
