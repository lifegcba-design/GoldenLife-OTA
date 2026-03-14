/**
 * קוד Apps Script לקריאת לוגים מהגליון "גיבוי לוגים צמידים G Life"
 * 
 * הוראות התקנה:
 * 1. פתח את Google Sheets עם הגליון "גיבוי לוגים צמידים G Life"
 * 2. Extensions → Apps Script
 * 3. העתק את הקוד הזה לקובץ Code.gs (או הוסף אותו)
 * 4. Deploy → New deployment → Web app
 * 5. Execute as: Me, Who has access: Anyone
 * 6. העתק את ה-URL החדש ל-Admin.html (משתנה LOG_READ_URL)
 */

const SHEET_NAME = 'גיבוי לוגים צמידים G Life';

function doGet(e) {
  // פרמטרים מה-URL לסינון
  const macFilter = e && e.parameter && e.parameter.mac ? String(e.parameter.mac).trim() : '';
  const fromStr   = e && e.parameter && e.parameter.from ? String(e.parameter.from).trim() : '';
  const toStr     = e && e.parameter && e.parameter.to   ? String(e.parameter.to).trim()   : '';

  try {
    const ss = SpreadsheetApp.getActiveSpreadsheet();
    const sheet = ss.getSheetByName(SHEET_NAME);
    
    if (!sheet) {
      return jsonResponse({ success: false, error: 'Sheet not found: ' + SHEET_NAME });
    }

    const data = sheet.getDataRange().getValues();
    if (data.length < 2) {
      return jsonResponse({ success: true, rows: [] });
    }

    const headers = data[0];
    
    // מיפוי עמודות - מחפש לפי שם או חלק מהשם
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

    // המרת תאריכים לפילטר
    let fromDate = null;
    let toDate   = null;
    if (fromStr) fromDate = new Date(fromStr + 'T00:00:00');
    if (toStr)   toDate   = new Date(toStr   + 'T23:59:59');

    const rows = [];

    for (let i = 1; i < data.length; i++) {
      const row = data[i];
      
      // דלג על שורות ריקות
      if (!row || row.every(cell => cell === '' || cell === null || cell === undefined)) continue;

      // בדיקת תאריך
      let ts = null;
      if (idxTimestamp >= 0 && row[idxTimestamp]) {
        ts = new Date(row[idxTimestamp]);
        if (isNaN(ts.getTime())) ts = null;
        if (ts && fromDate && ts < fromDate) continue;
        if (ts && toDate   && ts > toDate)   continue;
      }

      // בדיקת MAC
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

    // מיין לפי תאריך (החדש למעלה)
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

/**
 * פונקציית בדיקה - הרץ אותה מתוך Apps Script Editor כדי לוודא שהכל עובד
 */
function testDoGet() {
  const result = doGet({ parameter: {} });
  Logger.log(result.getContent());
}
