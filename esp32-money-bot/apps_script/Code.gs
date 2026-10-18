/**
 * Google Apps Script backend for the ESP32 money bot.
 * Paste into Extensions → Apps Script of YOUR OWN Google Sheet.
 * Setup steps: see README.md next to this file.
 *
 * action "rows": backup. Each account gets its own tab. Rows are upserted by
 *   transaction id, so re-sending is harmless; deleted ones are removed.
 *   Slip photos (Telegram file_id) are copied into this Google Drive and
 *   linked in the sheet; needs BOT_TOKEN in Script Properties.
 * action "slip": reads a bank slip photo from Telegram with Typhoon
 *   (needs TYPHOON_KEY and BOT_TOKEN in Script Properties). Nothing is stored.
 */

const HEADER = ['id', 'วันเวลา', 'ประเภท', 'จำนวน', 'หมวด', 'โน้ต', 'โดย', 'เลขอ้างอิง', 'สลิป'];
const PHOTO_COL = 9;
const PHOTOS_PER_CALL = 8; // Drive uploads per request; the rest are asked for again (`retry`)
const PHOTO_ROOT = 'money-bot สลิป';
const TZ = 'Asia/Bangkok';
const TYPHOON_URL = 'https://api.opentyphoon.ai/v1/chat/completions';
// Override in Script Properties (OCR_MODEL / LLM_MODEL) when Typhoon renames models.
const DEFAULT_OCR_MODEL = 'typhoon-ocr';
const DEFAULT_LLM_MODEL = 'typhoon-v2.5-30b-a3b-instruct';

function doPost(e) {
  let req;
  try {
    req = JSON.parse(e.postData.contents);
  } catch (err) {
    return reply({ ok: false, error: 'bad json' });
  }
  const secret = PropertiesService.getScriptProperties().getProperty('SECRET');
  if (!secret) return reply({ ok: false, error: 'SECRET not set in Script Properties' });
  if (req.secret !== secret) return reply({ ok: false, error: 'wrong secret' });

  if (req.action === 'ping') return reply({ ok: true });
  if (req.action === 'slip') {
    try {
      return reply(readSlip(req.file_id));
    } catch (err) {
      return reply({ ok: false, error: String(err && err.message || err).slice(0, 200) });
    }
  }
  if (req.action === 'rows') {
    const lock = LockService.getScriptLock();
    lock.waitLock(30000);
    try {
      const rows = req.rows || [];
      return reply({ ok: true, n: rows.length, retry: saveRows(rows) });
    } finally {
      lock.releaseLock();
    }
  }
  return reply({ ok: false, error: 'unknown action' });
}

// Returns the ids whose photo should be sent again (upload budget or a
// temporary error); the bot keeps those rows pending.
function saveRows(rows) {
  const bySheet = {};
  rows.forEach(function (r) {
    (bySheet[r.sheet] = bySheet[r.sheet] || []).push(r);
  });
  const photos = { budget: PHOTOS_PER_CALL, retry: [] };
  Object.keys(bySheet).forEach(function (name) {
    upsert(sheetFor(name), name, bySheet[name], photos);
  });
  return photos.retry;
}

function sheetFor(name) {
  const ss = SpreadsheetApp.getActiveSpreadsheet();
  let sh = ss.getSheetByName(name);
  if (sh) {
    // Tabs made by an older version have fewer columns.
    if (sh.getRange(1, 1, 1, HEADER.length).getValues()[0][HEADER.length - 1] !== HEADER[HEADER.length - 1]) {
      sh.getRange(1, 1, 1, HEADER.length).setValues([HEADER]).setFontWeight('bold');
    }
    return sh;
  }
  if (ss.getSpreadsheetTimeZone() !== TZ) ss.setSpreadsheetTimeZone(TZ);
  sh = ss.insertSheet(name);
  sh.getRange(1, 1, 1, HEADER.length).setValues([HEADER]).setFontWeight('bold');
  sh.setFrozenRows(1);
  sh.getRange('B:B').setNumberFormat('yyyy-mm-dd hh:mm');
  sh.getRange('D:D').setNumberFormat('#,##0.00');
  return sh;
}

function upsert(sh, tab, rows, photos) {
  // id -> sheet row number, and the photo link already in that row
  const index = {};
  const links = {};
  const last = sh.getLastRow();
  if (last > 1) {
    const ids = sh.getRange(2, 1, last - 1, 1).getValues();
    const cells = sh.getRange(2, PHOTO_COL, last - 1, 1).getFormulas();
    ids.forEach(function (v, i) {
      index[v[0]] = i + 2;
      links[v[0]] = cells[i][0] || '';
    });
  }

  const append = [];
  const remove = [];
  rows.forEach(function (r) {
    const at = index[r.id];
    let link = links[r.id] || '';
    if (r.del) {
      if (at) remove.push(at);
      if (link) trashPhoto(link);
      return;
    }
    if (!link && r.photo) {
      if (photos.budget > 0) {
        photos.budget--;
        try {
          link = storePhoto(r, tab);
        } catch (e) {
          photos.retry.push(r.id); // Telegram/Drive hiccup: try again next sync
        }
      } else {
        photos.retry.push(r.id);
      }
    }
    const values = [
      r.id,
      new Date(r.ts * 1000),
      r.amt > 0 ? 'รับ' : 'จ่าย',
      r.amt / 100, // satang -> baht; negative = expense, so SUM works
      r.cat || 'อื่นๆ',
      r.note || '',
      r.by || '',
      r.ref ? "'" + r.ref : '', // leading ' keeps long digit strings as text
      link,
    ];
    if (at) sh.getRange(at, 1, 1, values.length).setValues([values]);
    else append.push(values);
  });

  if (append.length) sh.getRange(sh.getLastRow() + 1, 1, append.length, HEADER.length).setValues(append);
  // Bottom-up so earlier row numbers stay valid.
  remove.sort(function (a, b) { return b - a; }).forEach(function (row) { sh.deleteRow(row); });
}

// ---------- slip photos in Drive ----------

// Copies a Telegram photo into Drive: money-bot สลิป / <tab> / <yyyy-MM>.
// Returns a HYPERLINK formula, "" if it can't ever work (no BOT_TOKEN, file
// gone), throws on errors worth retrying.
function storePhoto(r, tab) {
  const token = PropertiesService.getScriptProperties().getProperty('BOT_TOKEN');
  if (!token) return '';
  const info = JSON.parse(
    UrlFetchApp.fetch('https://api.telegram.org/bot' + token + '/getFile?file_id=' + encodeURIComponent(r.photo), {
      muteHttpExceptions: true,
    }).getContentText()
  );
  if (!info.ok) {
    if (info.error_code === 400) return ''; // bad/expired file_id: retrying won't help
    throw new Error('getFile: ' + info.description);
  }
  const when = new Date(r.ts * 1000);
  const name = Utilities.formatDate(when, TZ, 'yyyyMMdd-HHmm') + '_' + r.id + '_' + Math.abs(r.amt / 100) + '.jpg';
  const blob = UrlFetchApp.fetch('https://api.telegram.org/file/bot' + token + '/' + info.result.file_path)
    .getBlob()
    .setName(name);
  const folder = child(child(photoRoot(), tab), Utilities.formatDate(when, TZ, 'yyyy-MM'));
  const file = folder.createFile(blob);
  return '=HYPERLINK("https://drive.google.com/file/d/' + file.getId() + '/view","ดูสลิป")';
}

function trashPhoto(link) {
  const m = /\/d\/([\w-]+)/.exec(link);
  if (!m) return;
  try {
    DriveApp.getFileById(m[1]).setTrashed(true);
  } catch (e) {
    // already gone
  }
}

function photoRoot() {
  const props = PropertiesService.getScriptProperties();
  const id = props.getProperty('PHOTO_FOLDER_ID');
  if (id) {
    try {
      const f = DriveApp.getFolderById(id);
      if (!f.isTrashed()) return f;
    } catch (e) {
      // deleted: make a new one
    }
  }
  const root = DriveApp.createFolder(PHOTO_ROOT);
  props.setProperty('PHOTO_FOLDER_ID', root.getId());
  return root;
}

function child(parent, name) {
  const it = parent.getFoldersByName(name);
  return it.hasNext() ? it.next() : parent.createFolder(name);
}

// ---------- slip reading ----------

const OCR_PROMPT =
  'Extract all text from this image exactly as written, line by line, as plain text. ' +
  'Keep Thai text, numbers, dates and reference codes exactly.';

const EXTRACT_PROMPT =
  'ข้อความต่อไปนี้ได้จาก OCR ของสลิปโอนเงิน/ใบเสร็จธนาคารไทย ดึงข้อมูลออกมาเป็น JSON เท่านั้น ห้ามมีข้อความอื่น\n' +
  'รูปแบบ: {"is_slip":true,"amount":0,"date":"YYYY-MM-DD","time":"HH:MM","from_name":"","from_bank":"",' +
  '"to_name":"","to_bank":"","ref":"","memo":""}\n' +
  'กติกา:\n' +
  '- amount = จำนวนเงินที่โอน (ตัวเลขบาท ไม่ใส่คอมมา) ไม่ใช่ค่าธรรมเนียมหรือยอดคงเหลือ\n' +
  '- date เป็น ค.ศ. (ถ้าเป็น พ.ศ. ให้ลบ 543, ปีย่อ 2 หลักเช่น 68 = พ.ศ. 2568 = 2025) เดือนย่อไทย ม.ค.=01 ... ธ.ค.=12\n' +
  '- from = ผู้โอน/จาก, to = ผู้รับ/ไปยัง/ร้านค้า, bank = ชื่อธนาคารหรือแอป (เช่น KBank, SCB, PromptPay, TrueMoney)\n' +
  '- ref = เลขที่รายการ/รหัสอ้างอิง/Transaction ID\n' +
  '- memo = บันทึกช่วยจำ ถ้ามี\n' +
  '- ไม่รู้ให้เป็น "" ถ้าไม่ใช่สลิปให้ is_slip เป็น false\n\n' +
  'ข้อความ OCR:\n';

function readSlip(fileId) {
  const props = PropertiesService.getScriptProperties();
  const token = props.getProperty('BOT_TOKEN');
  const key = props.getProperty('TYPHOON_KEY');
  if (!token || !key) return { ok: false, error: 'BOT_TOKEN / TYPHOON_KEY not set in Script Properties' };
  if (!fileId) return { ok: false, error: 'no file_id' };

  // 1. Photo from Telegram (the bot token never leaves Google)
  const info = JSON.parse(
    UrlFetchApp.fetch('https://api.telegram.org/bot' + token + '/getFile?file_id=' + encodeURIComponent(fileId), {
      muteHttpExceptions: true,
    }).getContentText()
  );
  if (!info.ok) return { ok: false, error: 'Telegram getFile: ' + (info.description || 'failed') };
  const bytes = UrlFetchApp.fetch('https://api.telegram.org/file/bot' + token + '/' + info.result.file_path).getBlob().getBytes();
  const mime = /\.png$/i.test(info.result.file_path) ? 'image/png' : 'image/jpeg';

  // 2. OCR
  const text = typhoon(key, props.getProperty('OCR_MODEL') || DEFAULT_OCR_MODEL, [
    { type: 'text', text: OCR_PROMPT },
    { type: 'image_url', image_url: { url: 'data:' + mime + ';base64,' + Utilities.base64Encode(bytes) } },
  ], 2048);

  // 3. Fields
  const out = typhoon(key, props.getProperty('LLM_MODEL') || DEFAULT_LLM_MODEL, EXTRACT_PROMPT + text, 512);
  const j = parseJsonLoose(out);
  if (!j) return { ok: false, error: 'อ่านผลจาก AI ไม่ได้' };
  if (j.is_slip === false) return { ok: false, error: 'รูปนี้ไม่ใช่สลิป' };

  const amount = Number(String(j.amount || '').replace(/[^0-9.]/g, ''));
  if (!(amount > 0)) return { ok: false, error: 'หาจำนวนเงินในรูปไม่เจอ' };
  return {
    ok: true,
    slip: {
      amt: Math.round(amount * 100),
      ts: slipTime(j.date, j.time),
      from: str(j.from_name),
      from_bank: str(j.from_bank),
      to: str(j.to_name),
      to_bank: str(j.to_bank),
      ref: str(j.ref).replace(/\s+/g, ''),
      memo: str(j.memo),
    },
  };
}

function typhoon(key, model, content, maxTokens) {
  const res = UrlFetchApp.fetch(TYPHOON_URL, {
    method: 'post',
    contentType: 'application/json',
    headers: { Authorization: 'Bearer ' + key },
    payload: JSON.stringify({
      model: model,
      messages: [{ role: 'user', content: content }],
      max_tokens: maxTokens,
      temperature: 0,
    }),
    muteHttpExceptions: true,
  });
  if (res.getResponseCode() !== 200) {
    throw new Error('Typhoon ' + model + ' HTTP ' + res.getResponseCode() + ': ' + res.getContentText().slice(0, 120));
  }
  return JSON.parse(res.getContentText()).choices[0].message.content || '';
}

// First {...} in the text; models sometimes wrap JSON in ``` or add words.
function parseJsonLoose(s) {
  const m = String(s).match(/\{[\s\S]*\}/);
  if (!m) return null;
  try {
    return JSON.parse(m[0]);
  } catch (e) {
    return null;
  }
}

// "2025-10-18" + "14:32" in Thai time -> unix seconds; 0 if unusable.
function slipTime(date, time) {
  const d = String(date || '').match(/(\d{4})-(\d{1,2})-(\d{1,2})/);
  if (!d) return 0;
  let y = Number(d[1]);
  if (y > 2400) y -= 543; // Buddhist year slipped through
  const t = String(time || '').match(/(\d{1,2}):(\d{2})/) || [0, '0', '0'];
  const ms = Date.UTC(y, Number(d[2]) - 1, Number(d[3]), Number(t[1]) - 7, Number(t[2])); // UTC+7
  return isNaN(ms) ? 0 : Math.floor(ms / 1000);
}

function str(v) {
  return v == null ? '' : String(v).trim().slice(0, 60);
}

function reply(obj) {
  return ContentService.createTextOutput(JSON.stringify(obj)).setMimeType(ContentService.MimeType.JSON);
}
