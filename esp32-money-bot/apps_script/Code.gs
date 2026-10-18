/**
 * Google Apps Script backend for the ESP32 money bot.
 * Paste into Extensions → Apps Script of YOUR OWN Google Sheet.
 * Setup steps: see README.md next to this file.
 *
 * Each account gets its own tab. Rows are upserted by transaction id, so
 * re-sending is harmless; deleted transactions are removed from the sheet.
 */

const HEADER = ['id', 'วันเวลา', 'ประเภท', 'จำนวน', 'หมวด', 'โน้ต', 'โดย'];
const TZ = 'Asia/Bangkok';

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
  if (req.action === 'rows') {
    const lock = LockService.getScriptLock();
    lock.waitLock(30000);
    try {
      return reply({ ok: true, n: saveRows(req.rows || []) });
    } finally {
      lock.releaseLock();
    }
  }
  return reply({ ok: false, error: 'unknown action' });
}

function saveRows(rows) {
  const bySheet = {};
  rows.forEach(function (r) {
    (bySheet[r.sheet] = bySheet[r.sheet] || []).push(r);
  });
  Object.keys(bySheet).forEach(function (name) {
    upsert(sheetFor(name), bySheet[name]);
  });
  return rows.length;
}

function sheetFor(name) {
  const ss = SpreadsheetApp.getActiveSpreadsheet();
  let sh = ss.getSheetByName(name);
  if (sh) return sh;
  if (ss.getSpreadsheetTimeZone() !== TZ) ss.setSpreadsheetTimeZone(TZ);
  sh = ss.insertSheet(name);
  sh.getRange(1, 1, 1, HEADER.length).setValues([HEADER]).setFontWeight('bold');
  sh.setFrozenRows(1);
  sh.getRange('B:B').setNumberFormat('yyyy-mm-dd hh:mm');
  sh.getRange('D:D').setNumberFormat('#,##0.00');
  return sh;
}

function upsert(sh, rows) {
  // id -> sheet row number
  const index = {};
  const last = sh.getLastRow();
  if (last > 1) {
    sh.getRange(2, 1, last - 1, 1).getValues().forEach(function (v, i) {
      index[v[0]] = i + 2;
    });
  }

  const append = [];
  const remove = [];
  rows.forEach(function (r) {
    const at = index[r.id];
    if (r.del) {
      if (at) remove.push(at);
      return;
    }
    const values = [
      r.id,
      new Date(r.ts * 1000),
      r.amt > 0 ? 'รับ' : 'จ่าย',
      r.amt / 100, // satang -> baht; negative = expense, so SUM works
      r.cat || 'อื่นๆ',
      r.note || '',
      r.by || '',
    ];
    if (at) sh.getRange(at, 1, 1, values.length).setValues([values]);
    else append.push(values);
  });

  if (append.length) sh.getRange(sh.getLastRow() + 1, 1, append.length, HEADER.length).setValues(append);
  // Bottom-up so earlier row numbers stay valid.
  remove.sort(function (a, b) { return b - a; }).forEach(function (row) { sh.deleteRow(row); });
}

function reply(obj) {
  return ContentService.createTextOutput(JSON.stringify(obj)).setMimeType(ContentService.MimeType.JSON);
}
