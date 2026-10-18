# ตั้ง Google Sheet สำหรับ backup

บอทเป็นตัวจริง ชีตเป็น backup — ข้อมูลส่งขึ้นอัตโนมัติ ถ้าเน็ตหลุดจะรอในบอร์ดแล้วส่งทีหลัง

## ข้อมูลไปชีตไหน

| บัญชี | ไปที่ |
|---|---|
| ตั้งชีตแยกให้บัญชีไว้ (`/backup shop URL SECRET`) | ชีตนั้น |
| บัญชีส่วนตัว | ชีตของเจ้าของ (`/backup URL SECRET`) เท่านั้น |
| บัญชีแชร์ที่ไม่ได้ตั้งชีตแยก | ชีตของสมาชิกคนแรกที่ตั้งชีตไว้ |

พิมพ์ `/backup` ในบอทเพื่อดูว่าตอนนี้แต่ละบัญชีไปชีตไหน

**แนะนำ:** ทำ 2 ชีต
1. **ชีตส่วนตัวของคุณ** → `/backup URL SECRET` (ไม่ต้องแชร์ให้ใคร)
2. **ชีตร้าน** → `/backup shop URL SECRET` แล้วแชร์ชีตนี้ให้แฟนใน Google Drive

แฟนทำชีตส่วนตัวของตัวเองได้แบบเดียวกัน (ข้อ 1) — คุณจะไม่เห็นของเขา

## สร้างชีต (ทำทีละชีต)

1. สร้าง Google Sheet ใหม่ เช่น "ร้าน - backup"
2. เมนู **Extensions → Apps Script**
3. ลบโค้ดเดิม วางโค้ดจาก [`Code.gs`](Code.gs) แล้วกด 💾 Save
4. ⚙️ **Project Settings → Script Properties → Add script property**
   - Property: `SECRET`
   - Value: รหัสยาวๆ เดาไม่ได้ ไม่มีช่องว่าง เช่น `k8Qz2mX9vLp4wR7t`
5. **Deploy → New deployment** → ⚙️ **Web app**
   - Execute as: **Me**
   - Who has access: **Anyone**
   - Deploy → Authorize access → (Advanced → Go to … unsafe — เป็นสคริปต์ของเราเอง)
   - คัดลอก **Web app URL** `https://script.google.com/macros/s/…/exec`
6. ส่งหาบอทใน Telegram:
   ```
   /backup shop https://script.google.com/macros/s/xxxx/exec k8Qz2mX9vLp4wR7t
   ```
   (ชีตส่วนตัว: ไม่ต้องใส่ `shop`)
   บอทจะทดสอบก่อน ถ้าผ่านจะส่งข้อมูลเดิมทั้งหมดขึ้นชีต — แล้วลบข้อความนั้นทิ้งได้

"Anyone" = ใครมี URL ก็ส่งมาได้ แต่ไม่มี SECRET จะถูกปฏิเสธ อย่าแชร์ URL + SECRET

## ในชีต

- 1 แท็บต่อ 1 บัญชี: `id | วันเวลา | ประเภท | จำนวน | หมวด | โน้ต | โดย`
- จำนวนติดลบ = รายจ่าย → `=SUM(D:D)` = คงเหลือ
- ส่งซ้ำไม่เกิดแถวซ้ำ (อัปเดตตาม id) รายการที่ลบหรือแก้ในบอทจะตามไปในชีต
- **อย่าแก้ตัวเลขในชีตเอง** — จะถูกทับเมื่อรายการนั้นถูกแก้ในบอท
  แต่เพิ่มแท็บสรุป/กราฟ/pivot เองได้ตามสบาย

## แก้โค้ดสคริปต์ทีหลัง

**Deploy → Manage deployments → ✏️ → Version: New version → Deploy**
URL เดิมใช้ต่อได้ (ถ้ากด New deployment จะได้ URL ใหม่ ต้อง `/backup` ใหม่)

## มีปัญหา

| บอทตอบ | แก้ |
|---|---|
| ติดต่อไม่ได้ | URL ผิด หรือยังไม่ได้ Deploy เป็น Web app |
| ตอบกลับไม่ใช่ JSON | Who has access ไม่ใช่ **Anyone** |
| wrong secret | SECRET ใน Script Properties ไม่ตรงกับที่พิมพ์ |
| SECRET not set | ยังไม่ได้ทำข้อ 4 |
