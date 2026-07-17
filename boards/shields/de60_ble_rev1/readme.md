# DE60 BLE REV1 ZMK Config Refactor

cd ~/zmk/app && west build -p -d build/de60 -b de_core_nrf52 -S studio-rpc-usb-uart -- -DSHIELD=de60_ble_rev1_red -DZMK_EXTRA_MODULES="/home/de/dev/zmk-keyboards-dekb" && west flash -d build/de60_ble_rev1_red


cd ~/zmk/app && west build -p -d build/de60 -b de_core_nrf52 -S studio-rpc-usb-uart -- -DSHIELD=de60_ble_rev1_white_ansi_7u -DZMK_EXTRA_MODULES="/home/de/dev/zmk-keyboards-dekb"

Default Layout Support: 60 Tsangan with ANSI Enter/Left Shift, Split Backspace, Split Right Shift