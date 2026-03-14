/**
 * קוד Apps Script לאדמין GoldenLife
 * 
 * - כשניגשים ל-URL ישירות → מציג את דף האדמין
 * - כשניגשים עם ?action=api → מחזיר JSON עם הלוגים
 * 
 * Deploy → Web app → Execute as: Me, Who has access: Anyone
 */

const SPREADSHEET_ID = '1PUs7N4w8gTZmDl4Wb86JiJFw1m5Q2CjNiY8P4ypo04c';
const SHEET_NAME = 'גיבוי לוגים צמידים G Life';

function doGet(e) {
  const action = e && e.parameter && e.parameter.action ? e.parameter.action : '';
  
  // אם זו קריאת API - החזר JSON
  if (action === 'api') {
    return getLogsAsJson(e);
  }
  
  // אחרת - הצג את דף האדמין
  return HtmlService.createHtmlOutput(getAdminHtml())
    .setTitle('GoldenLife Admin')
    .setXFrameOptionsMode(HtmlService.XFrameOptionsMode.ALLOWALL);
}

function getLogsAsJson(e) {
  const macFilter = e && e.parameter && e.parameter.mac ? String(e.parameter.mac).trim() : '';
  const fromStr   = e && e.parameter && e.parameter.from ? String(e.parameter.from).trim() : '';
  const toStr     = e && e.parameter && e.parameter.to   ? String(e.parameter.to).trim()   : '';

  try {
    const ss = SpreadsheetApp.openById(SPREADSHEET_ID);
    const sheet = ss.getSheetByName(SHEET_NAME);
    
    if (!sheet) {
      return jsonResponse({ success: false, error: 'Sheet not found: ' + SHEET_NAME });
    }

    const data = sheet.getDataRange().getValues();
    if (data.length < 2) {
      return jsonResponse({ success: true, rows: [] });
    }

    const headers = data[0];
    
    const col = (name) => {
      const exact = headers.indexOf(name);
      if (exact >= 0) return exact;
      return headers.findIndex(h => String(h).toLowerCase().includes(name.toLowerCase()));
    };

    const idxTimestamp      = col('timestamp');
    const idxSessionId      = col('sessionId');
    const idxEventType      = col('eventType');
    const idxSource         = col('source');
    const idxVibratePower   = col('vibratePower');
    const idxPulseInterval  = col('pulseInterval');
    const idxPulseNumber    = col('pulseNumber');
    const idxVibrationDur   = col('vibrationDuration');
    const idxPauseMinutes   = col('pauseMinutes');
    const idxRepeats        = col('repeats');
    const idxStopReason     = col('stopReason');
    const idxBatteryPercent = col('batteryPercent');
    const idxRawData        = col('rawData');
    const idxMac            = col('mac');

    let fromDate = null;
    let toDate   = null;
    if (fromStr) fromDate = new Date(fromStr + 'T00:00:00');
    if (toStr)   toDate   = new Date(toStr   + 'T23:59:59');

    const rows = [];

    for (let i = 1; i < data.length; i++) {
      const row = data[i];
      if (!row || row.every(cell => cell === '' || cell === null || cell === undefined)) continue;

      let ts = null;
      if (idxTimestamp >= 0 && row[idxTimestamp]) {
        ts = new Date(row[idxTimestamp]);
        if (isNaN(ts.getTime())) ts = null;
        if (ts && fromDate && ts < fromDate) continue;
        if (ts && toDate   && ts > toDate)   continue;
      }

      const macVal = idxMac >= 0 ? String(row[idxMac] || '').trim() : '';
      if (macFilter && macVal) {
        const macUpper = macVal.toUpperCase();
        const filterUpper = macFilter.toUpperCase();
        if (!macUpper.includes(filterUpper) && macUpper !== filterUpper) continue;
      }

      rows.push({
        timestamp:         ts ? ts.toISOString() : String(row[idxTimestamp] || ''),
        sessionId:         idxSessionId >= 0 ? String(row[idxSessionId] || '') : '',
        eventType:         idxEventType >= 0 ? String(row[idxEventType] || '') : '',
        source:            idxSource >= 0 ? String(row[idxSource] || '') : '',
        vibratePower:      idxVibratePower >= 0 ? row[idxVibratePower] : '',
        pulseInterval:     idxPulseInterval >= 0 ? row[idxPulseInterval] : '',
        pulseNumber:       idxPulseNumber >= 0 ? row[idxPulseNumber] : '',
        vibrationDuration: idxVibrationDur >= 0 ? row[idxVibrationDur] : '',
        pauseMinutes:      idxPauseMinutes >= 0 ? row[idxPauseMinutes] : '',
        repeats:           idxRepeats >= 0 ? row[idxRepeats] : '',
        stopReason:        idxStopReason >= 0 ? String(row[idxStopReason] || '') : '',
        batteryPercent:    idxBatteryPercent >= 0 ? row[idxBatteryPercent] : '',
        rawData:           idxRawData >= 0 ? String(row[idxRawData] || '') : '',
        mac:               macVal
      });
    }

    rows.sort((a, b) => {
      if (!a.timestamp) return 1;
      if (!b.timestamp) return -1;
      return new Date(b.timestamp) - new Date(a.timestamp);
    });

    return jsonResponse({ success: true, rows: rows });

  } catch (err) {
    return jsonResponse({ success: false, error: err.toString() });
  }
}

function jsonResponse(obj) {
  return ContentService
    .createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}

function getAdminHtml() {
  return `<!DOCTYPE html>
<html lang="he" dir="rtl">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>GoldenLife Admin</title>
  <style>
    body { font-family: "Segoe UI", Tahoma, sans-serif; margin: 0; padding: 0; background: #f5f7fb; color: #333; }
    header { background: linear-gradient(90deg, #1565c0, #42a5f5); color: #fff; padding: 16px 24px; }
    header h1 { margin: 0; font-size: 1.4rem; }
    header p { margin: 4px 0 0; font-size: 0.9rem; opacity: 0.9; }
    .container { max-width: 1200px; margin: 24px auto; padding: 0 16px 24px; }
    .filters { display: flex; flex-wrap: wrap; gap: 12px; align-items: flex-end; background: #fff; padding: 16px; border-radius: 12px; box-shadow: 0 2px 8px rgba(15,23,42,0.06); margin-bottom: 18px; }
    .filter-group { display: flex; flex-direction: column; min-width: 160px; }
    label { font-size: 0.8rem; margin-bottom: 4px; color: #555; }
    input[type="date"], input[type="text"] { padding: 6px 10px; border-radius: 8px; border: 1px solid #d0d7e2; font-size: 0.85rem; background: #f9fbff; }
    input:focus { border-color: #1e88e5; box-shadow: 0 0 0 2px rgba(30,136,229,0.16); background: #fff; outline: none; }
    button { padding: 8px 16px; border-radius: 8px; border: none; cursor: pointer; font-size: 0.9rem; }
    .btn-primary { background: #1e88e5; color: #fff; }
    .btn-primary:hover { background: #1565c0; }
    .btn-secondary { background: #e3e7f0; color: #333; }
    .btn-secondary:hover { background: #cfd5e3; }
    .table-wrapper { background: #fff; border-radius: 12px; box-shadow: 0 2px 10px rgba(15,23,42,0.08); overflow: hidden; }
    .table-header { padding: 10px 16px; font-size: 0.9rem; color: #555; border-bottom: 1px solid #e1e6f0; display: flex; justify-content: space-between; background: #f8f9ff; }
    .table-scroll { max-height: 500px; overflow: auto; }
    table { width: 100%; border-collapse: collapse; min-width: 960px; }
    th, td { padding: 8px 10px; font-size: 0.8rem; border-bottom: 1px solid #eef1f7; text-align: right; white-space: nowrap; }
    th { position: sticky; top: 0; background: #f1f4ff; z-index: 1; font-weight: 600; color: #374151; }
    tbody tr:nth-child(even) { background: #fafbff; }
    tbody tr:hover { background: #eef5ff; }
    .badge { display: inline-block; padding: 2px 8px; border-radius: 999px; font-size: 0.7rem; font-weight: 600; }
    .badge-session { background: #e3f2fd; color: #1565c0; }
    .badge-start { background: #e8f5e9; color: #2e7d32; }
    .badge-end { background: #ffebee; color: #c62828; }
    .badge-web { background: #e8f5e9; color: #2e7d32; }
    .badge-system { background: #fff3e0; color: #ef6c00; }
    .footer-note { margin-top: 8px; font-size: 0.78rem; color: #6b7280; }
    @media (max-width: 768px) { .filters { flex-direction: column; } .filter-group { width: 100%; } }
  </style>
</head>
<body>
  <header>
    <h1>GoldenLife Admin - גיבוי לוגים</h1>
    <p>צפייה בשינויים לפי תאריך ו-MAC Address</p>
  </header>
  <div class="container">
    <section class="filters">
      <div class="filter-group">
        <label for="fromDate">מתאריך</label>
        <input id="fromDate" type="date" />
      </div>
      <div class="filter-group">
        <label for="toDate">עד תאריך</label>
        <input id="toDate" type="date" />
      </div>
      <div class="filter-group">
        <label for="macFilter">MAC Address</label>
        <input id="macFilter" type="text" placeholder="לדוגמה: 001122334455" />
      </div>
      <div class="filter-group">
        <button class="btn-primary" id="searchBtn">חפש</button>
      </div>
      <div class="filter-group">
        <button class="btn-secondary" id="clearBtn">נקה</button>
      </div>
    </section>
    <section class="table-wrapper">
      <div class="table-header">
        <span id="summaryText">טוען...</span>
        <span id="rowsCount"></span>
      </div>
      <div class="table-scroll">
        <table>
          <thead>
            <tr>
              <th>תאריך / שעה</th>
              <th>MAC</th>
              <th>מזהה סשן</th>
              <th>סוג אירוע</th>
              <th>מקור</th>
              <th>עוצמת רטט</th>
              <th>מרווח (ms)</th>
              <th>מס' פולסים</th>
              <th>משך רטט</th>
              <th>השהייה (דק')</th>
              <th>חזרות</th>
              <th>סיבת סיום</th>
              <th>% סוללה</th>
            </tr>
          </thead>
          <tbody id="resultsBody">
            <tr><td colspan="13" style="text-align:center;">טוען...</td></tr>
          </tbody>
        </table>
      </div>
    </section>
    <p class="footer-note">נתונים מהגליון: גיבוי לוגים צמידים G Life</p>
  </div>
  <script>
    const API_URL = window.location.href.split('?')[0] + '?action=api';
    
    document.getElementById('searchBtn').addEventListener('click', loadLogs);
    document.getElementById('clearBtn').addEventListener('click', () => {
      document.getElementById('fromDate').value = '';
      document.getElementById('toDate').value = '';
      document.getElementById('macFilter').value = '';
    });
    
    async function loadLogs() {
      const mac = document.getElementById('macFilter').value.trim();
      const from = document.getElementById('fromDate').value;
      const to = document.getElementById('toDate').value;
      
      let url = API_URL;
      if (mac) url += '&mac=' + encodeURIComponent(mac);
      if (from) url += '&from=' + encodeURIComponent(from);
      if (to) url += '&to=' + encodeURIComponent(to);
      
      document.getElementById('summaryText').textContent = 'טוען...';
      document.getElementById('resultsBody').innerHTML = '<tr><td colspan="13" style="text-align:center;">טוען...</td></tr>';
      
      try {
        const res = await fetch(url);
        const json = await res.json();
        if (!json.success) {
          document.getElementById('summaryText').textContent = 'שגיאה: ' + (json.error || '');
          return;
        }
        renderTable(json.rows || []);
      } catch (err) {
        document.getElementById('summaryText').textContent = 'שגיאת רשת: ' + err.message;
      }
    }
    
    function renderTable(rows) {
      if (!rows.length) {
        document.getElementById('summaryText').textContent = 'אין תוצאות';
        document.getElementById('rowsCount').textContent = 'סה"כ: 0';
        document.getElementById('resultsBody').innerHTML = '<tr><td colspan="13" style="text-align:center;">לא נמצאו רשומות</td></tr>';
        return;
      }
      document.getElementById('summaryText').textContent = 'גיבוי לוגים צמידים G Life';
      document.getElementById('rowsCount').textContent = 'סה"כ: ' + rows.length;
      
      let html = '';
      for (const r of rows) {
        const ts = r.timestamp ? formatTs(r.timestamp) : '';
        const evtBadge = r.eventType ? (r.eventType.includes('START') ? '<span class="badge badge-start">' + r.eventType + '</span>' : '<span class="badge badge-end">' + r.eventType + '</span>') : '';
        const srcBadge = r.source === 'WEB' ? '<span class="badge badge-web">WEB</span>' : (r.source ? '<span class="badge badge-system">' + r.source + '</span>' : '');
        html += '<tr><td>' + ts + '</td><td>' + (r.mac||'') + '</td><td><span class="badge badge-session">' + (r.sessionId||'') + '</span></td><td>' + evtBadge + '</td><td>' + srcBadge + '</td><td>' + (r.vibratePower||'') + '</td><td>' + (r.pulseInterval||'') + '</td><td>' + (r.pulseNumber||'') + '</td><td>' + (r.vibrationDuration||'') + '</td><td>' + (r.pauseMinutes||'') + '</td><td>' + (r.repeats||'') + '</td><td>' + (r.stopReason||'') + '</td><td>' + (r.batteryPercent||'') + '</td></tr>';
      }
      document.getElementById('resultsBody').innerHTML = html;
    }
    
    function formatTs(iso) {
      try {
        const d = new Date(iso);
        return d.getDate().toString().padStart(2,'0') + '/' + (d.getMonth()+1).toString().padStart(2,'0') + '/' + d.getFullYear() + ' ' + d.getHours().toString().padStart(2,'0') + ':' + d.getMinutes().toString().padStart(2,'0');
      } catch { return iso; }
    }
    
    loadLogs();
  </script>
</body>
</html>`;
}
