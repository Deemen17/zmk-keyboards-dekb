# DE Indicator System

Custom LED indicator system for ZMK-based keyboards.

---

## 🧠 Overview

DE Indicator là hệ thống LED được thiết kế theo hướng:

* Event-driven (dựa trên event của ZMK)
* State machine rõ ràng
* Tối ưu UX (phản hồi nhanh, dễ hiểu)
* Không chỉnh sửa ZMK core

---

## 🎯 Design Goals

* Phản hồi trực quan cho người dùng
* Không gây nhiễu trải nghiệm (no flicker, no spam)
* Hoạt động ổn định trên BLE và USB
* Dễ mở rộng và maintain

---

## ⚙️ Architecture

```
ZMK Events
   ↓
Listeners
   ↓
Context (ctx: flags + timers + data)
   ↓
State Resolver
   ↓
Render Engine
   ↓
LED Output
```

---

## 🔌 Supported States

### System

* BOOT
* IDLE
* SLEEP

### Output

* OUTPUT_CHECK (USB / BLE)
* OUTPUT_USB (intent-based USB feedback)

### BLE

* PAIRING
* CONNECTING
* CONNECTED
* SWITCH EVENT

### Battery

* BATTERY_CHECK
* BATTERY_LOW
* BATTERY_CRITICAL

### Other

* LAYER_EVENT
* CAPSLOCK

---

## 🧠 UX Principles

### 1. Intent vs State

* User action → hiển thị theo **intent**
* System event → sync lại theo **state thật**

---

### 2. Separation of Concerns

* OUTPUT ≠ BLE STATE
* EVENT ≠ RENDER

---

### 3. Non-blocking

* Không delay
* Không block main loop
* Sử dụng timer + state machine

---

## 🚀 Features

* LED RGB GPIO control
* BLE visual feedback (pairing / connecting / connected)
* USB / BLE output indication
* Battery warning system
* Layer indication
* Capslock indicator
* Pre-sleep LED off

---

## 🔧 Current Status

### ✅ Done

* [x] State machine core
* [x] BLE indicator
* [x] Output check
* [x] Pre-sleep handling
* [x] Keycode-triggered events

---

### ⚠️ In Progress

* [ ] Refactor naming (flags / data / timers)
* [ ] Clean API layer
* [ ] Improve BLE UX consistency

---

### 🔮 Future

* [ ] Configurable profiles
* [ ] Multi-LED support
* [ ] Indicator framework extraction

---

## 🧪 Development Workflow

1. Implement feature
2. Flash to hardware
3. Test real usage
4. Fix edge cases
5. Commit with clear message

---

## 📦 Versioning

Current version: `v0.2.0`

* v0.x.x → experimental / developing
* v1.0.0 → stable release

---

## 🧑‍💻 Author

Deemen17 (DE)

---

## 💡 Notes

Đây là hệ thống được phát triển từ đầu với mục tiêu:

> "LED không chỉ để hiển thị, mà là một phần của trải nghiệm người dùng"

---
