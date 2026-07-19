# DEKB ZMK Firmware

Official firmware repository for **DEKB custom keyboards** powered by ZMK.

This repository contains the firmware configurations, boards, and shields used to build firmware for DEKB keyboards.

## Repository layout

- `boards/arm/de_core_nrf52`: shared DEKB nRF52840 core board.
- `boards/shields/de60_ble_rev1`: DE60 BLE Rev1 shield variants.
- `boards/shields/de108_ble_proto`: DE108 BLE prototype shield migrated from the old standalone ARM board.
- `boards/shields/curve_e`: Curve E shield.
- `boards/shields/dengle`: DEKB dongle shield.
- `src`: shared DEKB behavior/indicator/buzzer modules used by shields.
- `dts/bindings`: custom devicetree bindings.
- `boards/shields/common/common_60_layouts.dtsi`: reusable DE60 physical-layout macros shared by DE60 shields.

Old standalone board copies, WIP experiments, generated CMake build files, personal keymaps, and duplicated feature libraries have been removed so the repo follows the `de_core_nrf52` + shield model.

## GitHub Actions builds

The workflow at `.github/workflows/build.yml` delegates to the upstream ZMK user-config workflow. The default build matrix is defined in `build.yaml` and only includes maintained `de_core_nrf52` + shield targets.

| Board | Shield | Notes |
| --- | --- | --- |
| `de_core_nrf52` | `de60_ble_rev1_red` | DE60 BLE Rev1 red variant. |
| `de_core_nrf52` | `de60_ble_rev1_white_ansi_7u` | DE60 BLE Rev1 white ANSI 7u variant. |
| `de_core_nrf52` | `de60_ble_rev1_white_ansi_625u` | DE60 BLE Rev1 white ANSI 6.25u variant. |
| `de_core_nrf52` | `curve_e` | Curve E shield. |
| `de_core_nrf52` | `de108_ble_proto` | DE108 BLE prototype migrated from standalone ARM board to shield. |

Planned shields can be added under `boards/shields` as they are migrated, including `de60_red_proto`, `de60_ble_minila`, and `deky65`.

## Local build example

From a ZMK workspace, build a core + shield target with this module as an extra module:

```sh
west build -p -b de_core_nrf52 -S studio-rpc-usb-uart -- \
  -DSHIELD=de60_ble_rev1_red \
  -DZMK_EXTRA_MODULES=/path/to/zmk-keyboards-dekb
```
