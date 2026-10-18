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
  PropertiesService: { getScriptProperties: () => ({ getProperty: (k) => (k === 'SECRET' ? 'sek' : null) }) },
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

console.log(failures ? `❌ ${failures} failure(s)` : '✅ Code.gs: 0 failure(s)');
process.exit(failures ? 1 : 0);
