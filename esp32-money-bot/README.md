# ESP32 Money Bot

Telegram bot บันทึกรายรับรายจ่าย รันบน ESP32 dev (4MB flash, ไม่มี PSRAM/SD)

- หลายบัญชี: **แชร์** (เช่น ร้าน — สมาชิกทุกคนทำได้ทุกอย่าง) และ **ส่วนตัว** (เจ้าของเห็นคนเดียว ไม่มี admin เห็นแทน)
- เก็บข้อมูลบน LittleFS ในบอร์ด (~1.9MB ≈ 14,000 รายการ)
- คุยกับบอทได้เฉพาะแชทส่วนตัว (ในกลุ่มบอทจะเงียบ กันข้อมูลส่วนตัวหลุด)

## สถานะ

- [x] 1. WiFi + NTP + Telegram long polling + whitelist
- [x] 2. บันทึก `+/-` ลงบอร์ด, สรุปยอด, ลบ/undo
- [x] 3. หลายบัญชี + สิทธิ์ (shared / private)
- [ ] 4. Backup ไป Google Sheet (Apps Script) + คิว sync
- [ ] 5. อ่านสลิปด้วย Typhoon (ผ่าน Apps Script) + ปุ่มยืนยัน + กันซ้ำ

## ติดตั้ง (Arduino IDE)

1. Boards Manager → ติดตั้ง **esp32 by Espressif** (3.x)
2. Library Manager → ติดตั้ง **ArduinoJson** (7.x)
3. สร้างบอทกับ [@BotFather](https://t.me/BotFather) → ได้ token
4. คัดลอก `money_bot/secrets.example.h` เป็น `money_bot/secrets.h` แล้วใส่ WiFi, token, chat id
   - ไม่รู้ chat id: แฟลชไปก่อน แล้วส่ง `/id` หาบอท บอทจะตอบเลขมาให้
   - **`secrets.h` อยู่ใน .gitignore ห้าม commit** (repo นี้ public)
5. เปิด `money_bot/money_bot.ino`
   - Board: **ESP32 Dev Module**
   - Partition Scheme: **No OTA (2MB APP/2MB SPIFFS)**
6. Upload แล้วเปิด Serial Monitor 115200

PlatformIO ก็ใช้ได้: `pio run -t upload` (มี `platformio.ini` ให้แล้ว)

> ถ้าแก้ `BOOT_USERS` หลังแฟลชไปแล้ว จะไม่มีผล (ใช้แค่ครั้งแรก) ให้ใช้ `/adduser` แทน
> หรือล้างข้อมูลทั้งหมดด้วย Tools → Erase All Flash Before Sketch Upload

## คำสั่ง

| พิมพ์ | ผล |
|---|---|
| `+500 ขายของ` | รายรับ 500 เข้าบัญชีที่ใช้อยู่ |
| `-120.50 ค่าน้ำแข็ง` | รายจ่าย |
| `-80 กาแฟ #me` | ลงบัญชี `me` (ส่วนตัว) โดยไม่ต้องเปลี่ยนบัญชี |
| `/today [บัญชี]` `/month [บัญชี] [2026-09]` | สรุปยอด |
| `/sum` | ทุกบัญชีที่เห็นได้ เดือนนี้ |
| `/last [n]` | รายการล่าสุด |
| `/del 12` `/undo` | ลบรายการ (บัญชีแชร์จะแจ้งสมาชิกคนอื่น) |
| `/acc` `/use shop` | ดู/เปลี่ยนบัญชี |
| `/newacc key ชื่อ` | สร้างบัญชีแชร์ |
| `/newpriv key ชื่อ` | สร้างบัญชีส่วนตัวเพิ่ม |
| `/share key คน` `/unshare key คน` `/members key` | จัดการสมาชิก (บัญชีแชร์เท่านั้น) |
| `/adduser chat_id ชื่อ` | เพิ่มผู้ใช้ (ได้บัญชีส่วนตัว `me` อัตโนมัติ) |
| `/status` | พื้นที่, RAM, WiFi |

ทุกคนมีบัญชีส่วนตัวชื่อ `me` ของตัวเอง — พิมพ์ `#me` เหมือนกันแต่ลงคนละบัญชี

## โครงสร้าง

```
money_bot/
  money_bot.ino   setup/loop, WiFi, first boot
  tg.*            Telegram API (getUpdates, sendMessage) ผ่าน HTTPS ตรวจ cert จริง
  store.*         users/accounts (/db.json) + รายการ (/tx/<acc>/<YYYYMM>.jsonl)
  bot.*           แปลคำสั่ง, เช็คสิทธิ์, ตอบกลับ
  config.h        ค่าตั้งที่ไม่ลับ
  ca_bundle.h     root CA (สร้างจาก tools/make_ca_bundle.py)
```

เงินเก็บเป็นสตางค์ (int) ไม่ใช้ float จะได้ไม่มีเศษเพี้ยน
ทุกรายการมี flag `s` (synced) รอขั้นที่ 4 ส่งขึ้น Google Sheet
