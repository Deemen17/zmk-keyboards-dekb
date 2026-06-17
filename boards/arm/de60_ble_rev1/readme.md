cd ~/zmk/app && west build -p -b de60_ble_rev1 -S studio-rpc-usb-uart -- -DZMK_EXTRA_MODULES="/home/de/dev/zmk-keyboards-dekb"

cd ~/zmk/app && west build -p -b de60_ble_proto -S studio-rpc-usb-uart -- -DCONFIG_ZMK_SLEEP=n -DZMK_EXTRA_MODULES="/home/de/dev/zmk-keyboards-dekb" && west flash

    