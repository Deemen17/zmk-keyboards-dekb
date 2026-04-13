# Hướng Dẫn Sử Dụng DE60 BLE REV1 (Firmware Release Hardening)

Tài liệu này dành cho người dùng cơ bản, giúp bạn dùng nhanh bàn phím DE60 BLE REV1 với firmware mới.

## 1. Build Và Flash Firmware

Dùng board test release riêng:

```bash
cd /home/de/zmk/app
west build -p -b de60_ble_rev1_red -S studio-rpc-usb-uart -- \
  -DCONFIG_ZMK_STUDIO=y \
  -DZMK_EXTRA_MODULES="/home/de/dev/zmk-keyboards-dekb"
```

Sau khi build xong, flash file:

- `build/zephyr/zmk.uf2`

## 2. Các Layer Chính

Firmware hiện có 4 layer:

- `Layer 0`: Gõ phím thường (base layer)
- `Layer 1`: Function + Bluetooth + RGB + nguồn
- `Layer 2`: Để trống (đang để `Pass Through`)
- `Layer 3`: Để trống (đang để `Pass Through`)

Từ `Layer 0`, giữ phím `MO(1)` để vào `Layer 1`.

## 3. Bluetooth/USB Cho Người Dùng Cơ Bản

Trong `Layer 1`:

- `Output USB/BLE`: đổi output giữa USB/BLE
- `BT1 Connect/Clear`
- `BT2 Connect/Clear`
- `BT3 Connect/Clear`

Cách dùng key BT:

- Nhấn nhanh: kết nối sang slot BT tương ứng
- Giữ 3 giây: xóa pair slot BT tương ứng

## 4. Nguồn Và Reset

Trong `Layer 1` có:

- `Hold 1s Sleep`: giữ 1 giây để soft-off
- `Tap Reset Hold Boot`:
  - Nhấn nhanh: reset mạch
  - Giữ: vào bootloader

## 5. RGB Underglow

Trong `Layer 1`:

- `RGB_TOG`: bật/tắt RGB
- `RGB_BRI` / `RGB_BRD`: tăng/giảm sáng
- `RGB_HUI` / `RGB_HUD`: đổi màu
- `RGB_SAI` / `RGB_SAD`: đổi độ bão hòa
- `RGB_SPI` / `RGB_SPD`: đổi tốc độ hiệu ứng
- `RGB_EFF` / `RGB_EFR`: đổi hiệu ứng

## 6. Buzzer

Firmware mới có cải thiện buzzer:

- Sửa mapping PWM để buzzer hoạt động ổn định
- Trạng thái buzzer ON/OFF được lưu lại qua reboot/soft-off
- Đồng bộ lifecycle với hệ settings của ZMK

Lưu ý quan trọng:

- Behavior `Toggle Buzzer` đã có trong firmware
- Mặc định chưa gán ra một phím cụ thể trong keymap
- Nếu muốn bật/tắt buzzer bằng phím, bạn map behavior `Toggle Buzzer` trong ZMK Studio

## 7. Ý Nghĩa Tên Behavior Mới Trên ZMK Studio

Các tên đã được rút gọn để dễ nhìn hơn trên Studio:

- `MT Tap`: Mod-Tap ưu tiên tap
- `MT Hold`: Mod-Tap ưu tiên hold
- `LT Tap`: Layer-Tap ưu tiên tap
- `LT Hold`: Layer-Tap ưu tiên hold
- `Hold Layer`: giữ để vào layer
- `Go Layer`: nhảy layer
- `Toggle Layer`: bật/tắt layer
- `Pass Through`: trong suốt (`&trans`)
- `No Action`: không làm gì (`&none`)

## 8. Gợi Ý Test Nhanh Sau Khi Flash

1. Kiểm tra gõ phím thường ở `Layer 0`
2. Giữ `MO(1)` để vào `Layer 1`, test F1-F13
3. Test `BT1/BT2/BT3` (tap connect, hold clear)
4. Test `Output USB/BLE`
5. Test RGB controls
6. Test `Hold 1s Sleep`
7. Test `Tap Reset Hold Boot`
8. Nếu có map `Toggle Buzzer`, test ON/OFF và reboot lại để xác nhận trạng thái được giữ

## 9. Trạng Thái Bản Release Hardening

Bản `de60_ble_rev1_red` đã xử lý:

- Runtime safety cho indicator thread
- Lifecycle settings đúng cho buzzer
- Thống nhất MAX17048 driver giữa DTS và Kconfig
- Dọn warning chính ở DTS/keymap để log build sạch hơn
