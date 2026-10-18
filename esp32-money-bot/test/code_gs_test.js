// Runs apps_script/Code.gs in Node against a fake SpreadsheetApp.
// Usage: node test/code_gs_test.js
const fs = require('fs');
const path = require('path');
const vm = require('vm');

function fakeSheet(name) {
  const rows = [];  // rows[0] = row 1
  const get = (r, c) => (rows[r - 1] || [])[c - 1];
  return {
    name, rows,
    getLastRow: () => rows.length,
    getRange(a, c, nr, nc) {
      if (typeof a === 'string') return { setNumberFormat() { return this; } };
      return {
        setValues(vals) {
          vals.forEach((v, i) => { rows[a - 1 + i] = v.slice(); });
          return this;
        },
        getFormulas() { return this.getValues().map((row) => row.map((v) => (v === undefined ? '' : v))); },
        getValues() {
          const out = [];
          for (let i = 0; i < nr; i++) {
            const row = [];
            for (let j = 0; j < nc; j++) row.push(get(a + i, c + j));
            out.push(row);
          }
          return out;
        },
        setFontWeight() { return this; },
      };
    },
    setFrozenRows() {},
    deleteRow(r) { rows.splice(r - 1, 1); },
  };
}

const sheets = {};
const drive = { folders: {}, files: {}, n: 0 };
function fakeFolder(name, parent) {
  const f = {
    id: 'fo' + ++drive.n, name, parent, kids: [], trashed: false,
    getId() { return this.id; }, isTrashed() { return this.trashed; },
    getFoldersByName(n) { const m = this.kids.filter((k) => k.name === n); return { hasNext: () => m.length > 0, next: () => m[0] }; },
    createFolder(n) { const k = fakeFolder(n, this); this.kids.push(k); return k; },
    createFile(blob) {
      const file = { id: 'fi' + ++drive.n, name: blob.name, folder: this, trashed: false, getId() { return this.id; }, setTrashed(v) { this.trashed = v; } };
      drive.files[file.id] = file;
      return file;
    },
  };
  drive.folders[f.id] = f;
  return f;
}
const pathOf = (file) => { const p = []; for (let f = file.folder; f; f = f.parent) p.unshift(f.name); return p.join('/') + '/' + file.name; };
const props = { SECRET: 'sek' };
const fetched = [];
let llmReply = '';
let typhoonStatus = 200;
function resp(code, body) {
  return { getResponseCode: () => code, getContentText: () => body, getBlob: () => ({ getBytes: () => [0xff, 0xd8, 1, 2] }) };
}
function fetchFake(url, opts) {
  fetched.push({ url, opts });
  if (url.includes('/getFile?file_id=')) {
    if (url.includes('flaky')) return resp(502, '{"ok":false,"error_code":502,"description":"Bad Gateway"}');
    return url.includes('bad') ? resp(400, '{"ok":false,"error_code":400,"description":"file not found"}')
      : resp(200, '{"ok":true,"result":{"file_path":"photos/file_1.jpg"}}');
  }
  if (url.includes('/file/bot')) {
    const r = resp(200, '');
    r.getBlob = () => ({ name: '', getBytes: () => [0xff, 0xd8, 1, 2], setName(n) { this.name = n; return this; } });
    return r;
  }
  if (url.includes('opentyphoon')) {
    if (typhoonStatus !== 200) return resp(typhoonStatus, '{"error":"rate limited"}');
    const body = JSON.parse(opts.payload);
    const isOcr = Array.isArray(body.messages[0].content);
    return resp(200, JSON.stringify({ choices: [{ message: { content: isOcr ? 'โอนเงินสำเร็จ 350.00 บาท' : llmReply } }] }));
  }
  throw new Error('unexpected fetch ' + url);
}
let tz = 'Etc/GMT';
const ctx = {
  SpreadsheetApp: {
    getActiveSpreadsheet: () => ({
      getSheetByName: (n) => sheets[n] || null,
      insertSheet: (n) => (sheets[n] = fakeSheet(n)),
      getSpreadsheetTimeZone: () => tz,
      setSpreadsheetTimeZone: (z) => { tz = z; },
    }),
  },
  PropertiesService: {
    getScriptProperties: () => ({ getProperty: (k) => props[k] || null, setProperty: (k, v) => { props[k] = v; } }),
  },
  DriveApp: {
    createFolder: (n) => fakeFolder(n),
    getFolderById: (id) => { if (!drive.folders[id]) throw new Error('no folder'); return drive.folders[id]; },
    getFileById: (id) => { if (!drive.files[id]) throw new Error('no file'); return drive.files[id]; },
  },
  UrlFetchApp: { fetch: (url, opts) => fetchFake(url, opts) },
  Utilities: {
    base64Encode: (b) => Buffer.from(b).toString('base64'),
    formatDate: (d, tz, fmt) => {
      const t = new Date(d.getTime() + 7 * 3600e3).toISOString(); // Asia/Bangkok
      return { yyyy: t.slice(0, 4), MM: t.slice(5, 7), dd: t.slice(8, 10), HHmm: t.slice(11, 16).replace(':', '') }[fmt];
    },
  },
  encodeURIComponent,
  LockService: { getScriptLock: () => ({ waitLock() {}, releaseLock() {} }) },
  ContentService: {
    MimeType: { JSON: 'json' },
    createTextOutput: (s) => ({ text: s, setMimeType() { return this; } }),
  },
  Date, JSON, Object,
};
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path.join(__dirname, '../apps_script/Code.gs'), 'utf8'), ctx);

const call = (body) => JSON.parse(ctx.doPost({ postData: { contents: JSON.stringify(body) } }).text);
let failures = 0;
const check = (ok, what) => { if (!ok) { console.log('❌ FAIL:', what); failures++; } };
const row = (id, amt, extra) => Object.assign({ sheet: 'ร้าน (shop)', id, ts: 1760774400, amt, cat: 'ขาย', note: 'n' + id, by: 'me' }, extra);

check(call({ secret: 'x', action: 'ping' }).error === 'wrong secret', 'wrong secret rejected');
check(call({ secret: 'sek', action: 'ping' }).ok, 'ping ok');
check(call({ secret: 'sek', action: 'nope' }).error === 'unknown action', 'unknown action');
check(ctx.doPost({ postData: { contents: '{bad' } }).text.includes('bad json'), 'bad json');

check(call({ secret: 'sek', action: 'rows', rows: [row(1, 50000), row(2, -12000), row(3, -500)] }).n === 3, 'insert 3');
const sh = sheets['ร้าน (shop)'];
check(sh.rows.length === 4 && sh.rows[0][0] === 'id', 'header + 3 rows');
check(sh.rows[1][3] === 500 && sh.rows[2][3] === -120 && sh.rows[2][2] === 'จ่าย', 'baht + type');
check(sh.rows[1][1] instanceof Date && sh.rows[1][1].getTime() === 1760774400000, 'date');
check(tz === 'Asia/Bangkok', 'timezone set');

// resend same + update + delete in one batch
call({ secret: 'sek', action: 'rows', rows: [row(1, 50000), row(2, -15000, { cat: 'วัตถุดิบ' }), row(3, 0, { del: true }), row(4, 100)] });
check(sh.rows.length === 4, 'no duplicates, one deleted, one added');
check(sh.rows.find((r) => r[0] === 2)[3] === -150 && sh.rows.find((r) => r[0] === 2)[4] === 'วัตถุดิบ', 'update in place');
check(!sh.rows.find((r) => r[0] === 3), 'deleted');
// delete several, bottom-up must keep the right ones
call({ secret: 'sek', action: 'rows', rows: [row(1, 0, { del: true }), row(4, 0, { del: true }), row(99, 0, { del: true })] });
check(sh.rows.length === 2 && sh.rows[1][0] === 2, 'multi delete keeps the right row');
call({ secret: 'sek', action: 'rows', rows: [row(7, -100, { sheet: 'ส่วนตัว me (me)', cat: '' })] });
check(sheets['ส่วนตัว me (me)'].rows[1][4] === 'อื่นๆ', 'second tab, empty category -> อื่นๆ');

// ---- slip ----
const slip = (id) => call({ secret: 'sek', action: 'slip', file_id: id });
check(/not set/.test(slip('a').error), 'slip without TYPHOON_KEY/BOT_TOKEN');
props.TYPHOON_KEY = 'tk';
props.BOT_TOKEN = '123:abc';
llmReply = 'นี่คือผลลัพธ์\n```json\n{"is_slip":true,"amount":"1,350.50","date":"2568-10-18","time":"14:32",' +
  '"from_name":"นาย สมชาย ใ***","from_bank":"KBank","to_name":"แม็คโคร","to_bank":"SCB","ref":"0161 2345 ABC","memo":""}\n```';
let r = slip('good');
check(r.ok, 'slip ok: ' + JSON.stringify(r));
check(r.slip.amt === 135050, 'amount with comma -> satang');
check(r.slip.ts === Date.UTC(2025, 9, 18, 7, 32) / 1000, 'Buddhist year + Thai time -> unix');
check(r.slip.ref === '01612345ABC' && r.slip.to === 'แม็คโคร', 'fields');
const ocrCall = fetched.find((f) => f.url.includes('opentyphoon') && JSON.parse(f.opts.payload).model === 'typhoon-ocr');
check(ocrCall && JSON.parse(ocrCall.opts.payload).messages[0].content[1].image_url.url.startsWith('data:image/jpeg;base64,'), 'OCR gets jpeg data URL');
check(ocrCall.opts.headers.Authorization === 'Bearer tk', 'Typhoon auth header');
check(fetched.some((f) => f.url.includes('typhoon') === false && f.url.includes('/bot123:abc/getFile')), 'bot token only used toward Telegram');
props.OCR_MODEL = 'typhoon-ocr-new';
slip('good');
check(fetched.some((f) => f.url.includes('opentyphoon') && JSON.parse(f.opts.payload).model === 'typhoon-ocr-new'), 'OCR_MODEL override');

llmReply = '{"is_slip":false}';
check(slip('good').error === 'รูปนี้ไม่ใช่สลิป', 'not a slip');
llmReply = '{"is_slip":true,"amount":0}';
check(/จำนวนเงิน/.test(slip('good').error), 'no amount');
llmReply = 'sorry';
check(slip('good').error === 'อ่านผลจาก AI ไม่ได้', 'garbage from LLM');
check(/file not found/.test(slip('bad').error), 'telegram error passed on');
typhoonStatus = 429;
check(/HTTP 429/.test(slip('good').error), 'Typhoon HTTP error passed on');
check(call({ secret: 'nope', action: 'slip', file_id: 'good' }).error === 'wrong secret', 'slip needs the secret');

// ---- slip photos to Drive ----
const ok = (rows) => call({ secret: 'sek', action: 'rows', rows });
let res = ok([row(50, -35000, { photo: 'ph50', folder: 'ร้าน', ts: 1760774400 })]);
const shp = sheets['ร้าน (shop)'];
const shp_row = (id) => shp.rows.find((r) => r[0] === id);
const r50 = () => shp.rows.find((r) => r[0] === 50);
check(res.ok && res.retry.length === 0, 'photo row ok');
const link = r50()[8];
check(/^=HYPERLINK\("https:\/\/drive\.google\.com\/file\/d\/fi\d+\/view","ดูสลิป"\)$/.test(link), 'link formula: ' + link);
const f50 = drive.files[/d\/(fi\d+)/.exec(link)[1]];
check(pathOf(f50) === 'money-bot สลิป/ร้าน/2025/10/18/1500_50_350.jpg', 'drive path: ' + pathOf(f50));
check(props.PHOTO_FOLDER_ID === f50.folder.parent.parent.parent.parent.id, 'root folder id remembered');
ok([row(51, -100, { sheet: 'ส่วนตัว me (me)', photo: 'ph51', folder: 'ส่วนตัว', ts: 1760774400 + 86400 })]);
const f51 = drive.files[/d\/(fi\d+)/.exec(sheets['ส่วนตัว me (me)'].rows.find((r) => r[0] === 51)[8])[1]];
check(pathOf(f51) === 'money-bot สลิป/ส่วนตัว/2025/10/19/1500_51_1.jpg', 'private path: ' + pathOf(f51));
check(f51.folder.parent.parent.parent.parent === f50.folder.parent.parent.parent.parent, 'one root folder');
const sameDay = ok([row(52, -1, { photo: 'ph52', folder: 'ร้าน', ts: 1760774400 + 60 })]);
check(drive.files[/d\/(fi\d+)/.exec(shp_row(52)[8])[1]].folder === f50.folder, 'same day reuses the folder');

const filesBefore = Object.keys(drive.files).length;
ok([row(50, -40000, { photo: 'ph50', ts: 1760774400 })]); // edit amount
check(Object.keys(drive.files).length === filesBefore && r50()[8] === link && r50()[3] === -400, 'edit keeps the same photo');

const many = [];
for (let i = 60; i < 72; i++) many.push(row(i, -100, { photo: 'p' + i }));
res = ok(many);
check(res.retry.length === 12 - 8, 'upload budget: 8 now, rest retried (' + res.retry + ')');
res = ok(many.filter((r) => res.retry.includes(r.id)));
check(res.retry.length === 0 && shp.rows.filter((r) => r[0] >= 60 && r[0] < 72 && r[8]).length === 12, 'retried photos stored');

res = ok([row(80, -1, { photo: 'flaky' }), row(81, -1, { photo: 'bad' })]);
check(res.retry.includes(80) && !res.retry.includes(81), 'temporary error retried, permanent not');
check(shp.rows.find((r) => r[0] === 81)[8] === '', 'bad photo -> empty cell');

ok([row(50, 0, { del: true })]);
check(f50.trashed && !r50(), 'deleting the row trashes the photo');

delete props.BOT_TOKEN;
res = ok([row(90, -1, { photo: 'ph90' })]);
check(res.retry.length === 0 && shp.rows.find((r) => r[0] === 90)[8] === '', 'no BOT_TOKEN: row saved, no photo, no retry loop');
check(shp.rows[0][8] === 'สลิป', 'header has photo column');

console.log(failures ? `❌ ${failures} failure(s)` : '✅ Code.gs: 0 failure(s)');
process.exit(failures ? 1 : 0);
