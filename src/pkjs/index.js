// Wind Forecast - PebbleKit JS companion
// On each REQUEST from the watch: get GPS location, reverse-geocode it to a
// friendly place name, fetch an Open-Meteo forecast (model depends on the
// user's Global/Benelux setting), send both back to the watch in one
// AppMessage. Every stage has a watchdog timeout so the watch can never be
// left hanging indefinitely - something is ALWAYS sent back.

var GPS_TIMEOUT_MS = 20000;
var FETCH_TIMEOUT_MS = 18000;
var DEFAULT_WIND_MODEL = 'global';
var DEFAULT_WIND_UNIT = 'kmh';
var WIND_UNIT_CODES = { kmh: 0, mph: 1, ms: 2, kts: 3 }; // must match watch-side WIND_UNIT_SUFFIX order

function pad(n) {
  return n < 10 ? '0' + n : '' + n;
}

function getWindModelSetting() {
  try {
    var v = localStorage.getItem('wind_model');
    return (v === 'benelux') ? 'benelux' : DEFAULT_WIND_MODEL;
  } catch (e) {
    return DEFAULT_WIND_MODEL;
  }
}

function getWindUnitSetting() {
  try {
    var v = localStorage.getItem('wind_unit');
    return (v === 'mph' || v === 'ms' || v === 'kts') ? v : DEFAULT_WIND_UNIT;
  } catch (e) {
    return DEFAULT_WIND_UNIT;
  }
}

// Open-Meteo is always requested in km/h (wind_speed_unit=kmh) regardless of
// the user's display preference - conversion to mph/m/s/knots happens here,
// once, so the watch never needs to know about units at all, just display
// numbers.
function convertFromKmh(valueKmh, unit) {
  if (unit === 'mph') return valueKmh * 0.621371;
  if (unit === 'ms') return valueKmh / 3.6;
  if (unit === 'kts') return valueKmh * 0.539957;
  return valueKmh;
}

function sendError(code) {
  Pebble.sendAppMessage(
    { 'ERROR': code },
    function() {},
    function(e) { console.log('Failed to send error: ' + JSON.stringify(e)); }
  );
}

// highAltKey is whichever Open-Meteo field is being used as the "high
// altitude" wind reading - wind_speed_100m for Benelux/KNMI, or
// wind_speed_120m for the Global/best_match model (100m itself isn't a
// standard variable outside KNMI's own Netherlands-specific model).
function buildRowsPayload(json, highAltKey, unit) {
  var times = json.hourly.time;             // e.g. "2026-07-22T17:00"
  var wind10 = json.hourly.wind_speed_10m;
  var windHigh = json.hourly[highAltKey];
  var gusts = json.hourly.wind_gusts_10m;
  var dir10 = json.hourly.wind_direction_10m;

  if (!wind10 || !windHigh || !gusts || !dir10) {
    throw new Error('MISSING_FIELDS');
  }

  var now = new Date();
  var nowIso = now.getFullYear() + '-' + pad(now.getMonth() + 1) + '-' +
               pad(now.getDate()) + 'T' + pad(now.getHours()) + ':00';

  var startIdx = times.indexOf(nowIso);
  if (startIdx === -1) {
    startIdx = 0;
    for (var i = 0; i < times.length; i++) {
      if (new Date(times[i]) >= now) { startIdx = i; break; }
    }
  }

  var endIdx = Math.min(startIdx + 24, times.length);
  var rows = [];
  for (var j = startIdx; j < endIdx; j++) {
    var hour = parseInt(times[j].substring(11, 13), 10);
    rows.push(hour + ',' + Math.round(convertFromKmh(wind10[j], unit)) + ',' +
              Math.round(convertFromKmh(gusts[j], unit)) + ',' +
              Math.round(convertFromKmh(windHigh[j], unit)) + ',' +
              Math.round(dir10[j]));
  }
  return rows.join('|');
}

// Strips administrative-boundary prefixes that sometimes come attached to
// the raw boundary name (e.g. "Gemeente Tilburg", "Arrondissement Antwerpen")
// so only the actual place name is shown. Kept as a defensive fallback in
// case 'locality' is ever empty and we fall back to 'city'.
var ADMIN_PREFIX_RE = /^(gemeente|stad|stadsdeel|arrondissement|provincie|province of|municipality of|city of|county of|district of|borough of|commune de|ville de)\s+/i;

function cleanLocationName(name) {
  if (!name) return name;
  return name.replace(ADMIN_PREFIX_RE, '').trim();
}

// 'locality' is consistently the clean settlement name (confirmed against a
// real response: city="Gemeente Tilburg", locality="Tilburg") - 'city' is
// the field prone to echoing a raw administrative boundary label complete
// with its official prefix, so it's only used as a fallback here.
function extractLocationName(data) {
  var name = data.locality || data.city || data.principalSubdivision || null;
  return name ? cleanLocationName(name) : 'Unknown location';
}

function fetchLocationName(lat, lon, callback) {
  var url = 'https://api.bigdatacloud.net/data/reverse-geocode-client' +
            '?latitude=' + lat + '&longitude=' + lon + '&localityLanguage=en';
  var xhr = new XMLHttpRequest();
  xhr.onload = function() {
    if (xhr.status === 200) {
      try {
        var data = JSON.parse(xhr.responseText);
        callback(extractLocationName(data));
      } catch (e) {
        callback('Unknown location');
      }
    } else {
      callback('Unknown location');
    }
  };
  xhr.onerror = function() { callback('Unknown location'); };
  xhr.timeout = 10000;
  xhr.ontimeout = function() { callback('Unknown location'); };
  xhr.open('GET', url);
  xhr.send();
}

function fetchForecast(lat, lon, callback) {
  var modelSetting = getWindModelSetting();
  var unitSetting = getWindUnitSetting();
  var modelParam, highAltKey;
  if (modelSetting === 'benelux') {
    modelParam = 'knmi_seamless';
    highAltKey = 'wind_speed_100m';
  } else {
    modelParam = 'best_match';
    highAltKey = 'wind_speed_120m';
  }

  var url = 'https://api.open-meteo.com/v1/forecast' +
             '?latitude=' + lat + '&longitude=' + lon +
             '&hourly=wind_speed_10m,' + highAltKey + ',wind_gusts_10m,wind_direction_10m' +
             '&models=' + modelParam +
             '&wind_speed_unit=kmh' +
             '&timezone=auto' +
             '&forecast_days=2';

  var xhr = new XMLHttpRequest();
  xhr.onload = function() {
    if (xhr.status === 200) {
      try {
        var json = JSON.parse(xhr.responseText);
        callback(null, buildRowsPayload(json, highAltKey, unitSetting));
      } catch (e) {
        callback('PARSE_ERR');
      }
    } else {
      callback('HTTP_' + xhr.status);
    }
  };
  xhr.onerror = function() { callback('NET_ERR'); };
  xhr.timeout = 15000;
  xhr.ontimeout = function() { callback('TIMEOUT'); };
  xhr.open('GET', url);
  xhr.send();
}

function runFetchAndSend(lat, lon) {
  var modelCode = (getWindModelSetting() === 'benelux') ? 1 : 0;
  var unitCode = WIND_UNIT_CODES[getWindUnitSetting()];

  var locationName = null;
  var forecastPayload = null;
  var forecastErr = null;
  var settled = false;
  var pending = 2;

  // Watchdog: if location-name lookup and forecast fetch haven't BOTH
  // settled within this window, send whatever we have rather than leaving
  // the watch waiting forever.
  var watchdog = setTimeout(function() {
    if (settled) return;
    settled = true;
    if (forecastPayload) {
      Pebble.sendAppMessage(
        { 'FORECAST_DATA': forecastPayload, 'LOCATION_NAME': locationName || 'Unknown location',
          'WIND_UNIT': unitCode, 'MODEL_TYPE': modelCode },
        function() {}, function(e) { console.log('Send failed: ' + JSON.stringify(e)); }
      );
    } else {
      sendError(forecastErr || 'TIMEOUT');
    }
  }, FETCH_TIMEOUT_MS);

  function maybeSend() {
    pending--;
    if (pending > 0 || settled) { return; }
    settled = true;
    clearTimeout(watchdog);
    if (forecastErr) {
      sendError(forecastErr);
      return;
    }
    Pebble.sendAppMessage(
      { 'FORECAST_DATA': forecastPayload, 'LOCATION_NAME': locationName || 'Unknown location',
        'WIND_UNIT': unitCode, 'MODEL_TYPE': modelCode },
      function() { console.log('Forecast + location sent'); },
      function(e) { console.log('Send failed: ' + JSON.stringify(e)); }
    );
  }

  fetchLocationName(lat, lon, function(name) {
    locationName = name;
    maybeSend();
  });

  fetchForecast(lat, lon, function(err, payload) {
    if (err) { forecastErr = err; } else { forecastPayload = payload; }
    maybeSend();
  });
}

function fetchAndSend() {
  var gpsSettled = false;

  // Watchdog: navigator.geolocation's own "timeout" option is a request to
  // the OS, not a guarantee - on some phones (notably iOS under background
  // restrictions) neither the success nor error callback ever fires. This
  // guarantees the watch always gets *something* back.
  var gpsWatchdog = setTimeout(function() {
    if (gpsSettled) return;
    gpsSettled = true;
    sendError('GPS_TIMEOUT');
  }, GPS_TIMEOUT_MS);

  navigator.geolocation.getCurrentPosition(
    function(pos) {
      if (gpsSettled) return;
      gpsSettled = true;
      clearTimeout(gpsWatchdog);
      var lat = pos.coords.latitude.toFixed(4);
      var lon = pos.coords.longitude.toFixed(4);
      runFetchAndSend(lat, lon);
    },
    function() {
      if (gpsSettled) return;
      gpsSettled = true;
      clearTimeout(gpsWatchdog);
      sendError('GPS_ERR');
    },
    { timeout: 15000, maximumAge: 60000, enableHighAccuracy: false }
  );
}

// --- Settings page (wind unit, and Global vs Benelux wind model) ---

function buildConfigHtml() {
  var currentModel = getWindModelSetting();
  var currentUnit = getWindUnitSetting();
  var html = '<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width, initial-scale=1">' +
    '<style>' +
    ':root{--bg:#fff;--fg:#111;--sub:#666;--border:#ccc;--accent:#0a84ff;}' +
    '@media (prefers-color-scheme: dark){' +
    ':root{--bg:#1c1c1e;--fg:#f2f2f2;--sub:#a0a0a5;--border:#3a3a3c;--accent:#0a84ff;}' +
    '}' +
    'body{font-family:-apple-system,Roboto,sans-serif;padding:16px;background:var(--bg);color:var(--fg);margin:0;}' +
    'h2{font-size:20px;margin:0 0 8px;color:var(--fg);}' +
    'h3{font-size:15px;margin:22px 0 8px;color:var(--fg);text-transform:uppercase;letter-spacing:0.03em;}' +
    'p.intro{font-size:15px;color:var(--sub);margin:0 0 18px;line-height:1.4;}' +
    'label{display:flex;align-items:flex-start;gap:10px;margin:0 0 12px;padding:12px;font-size:17px;' +
    'line-height:1.4;color:var(--fg);border:1px solid var(--border);border-radius:10px;}' +
    'label input{margin-top:3px;accent-color:var(--accent);}' +
    'small{color:var(--sub);display:block;font-size:13px;margin-top:2px;}' +
    'button{margin-top:16px;padding:12px 24px;font-size:16px;width:100%;border:none;border-radius:8px;' +
    'background:var(--accent);color:#fff;}' +
    '</style></head><body>' +
    '<h2>Wind Forecast Settings</h2>' +

    '<h3>Wind speed unit</h3>' +
    '<label><input type="radio" name="unit" value="kmh"' + (currentUnit === 'kmh' ? ' checked' : '') + '>' +
    '<span>km/h<small>Default</small></span></label>' +
    '<label><input type="radio" name="unit" value="mph"' + (currentUnit === 'mph' ? ' checked' : '') + '>' +
    '<span>mph<small>Miles per hour</small></span></label>' +
    '<label><input type="radio" name="unit" value="ms"' + (currentUnit === 'ms' ? ' checked' : '') + '>' +
    '<span>m/s<small>Meters per second</small></span></label>' +
    '<label><input type="radio" name="unit" value="kts"' + (currentUnit === 'kts' ? ' checked' : '') + '>' +
    '<span>Knots<small>Nautical miles per hour</small></span></label>' +

    '<h3>Wind model</h3>' +
    '<p class="intro">Choose the wind model below that best fits your location. Global is the default, KNMI is more accurate for Benelux</p>' +
    '<label><input type="radio" name="model" value="global"' + (currentModel === 'global' ? ' checked' : '') + '>' +
    '<span>Global<small>Worldwide coverage, ~120m high wind reading</small></span></label>' +
    '<label><input type="radio" name="model" value="benelux"' + (currentModel === 'benelux' ? ' checked' : '') + '>' +
    '<span>Benelux (KNMI)<small>120m column will actually show 100m high wind</small></span></label>' +

    '<button onclick="save()">Save</button>' +
    '<script>function save(){' +
    'var unit=document.querySelector(\'input[name=unit]:checked\').value;' +
    'var model=document.querySelector(\'input[name=model]:checked\').value;' +
    'document.location="pebblejs://close#"+encodeURIComponent(JSON.stringify({wind_unit:unit,wind_model:model}));' +
    '}</script>' +
    '</body></html>';
  return 'data:text/html;charset=utf-8,' + encodeURIComponent(html);
}

Pebble.addEventListener('showConfiguration', function() {
  Pebble.openURL(buildConfigHtml());
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e.response) return;
  try {
    var settings = JSON.parse(decodeURIComponent(e.response));
    if (settings.wind_model === 'global' || settings.wind_model === 'benelux') {
      localStorage.setItem('wind_model', settings.wind_model);
    }
    if (settings.wind_unit === 'kmh' || settings.wind_unit === 'mph' || settings.wind_unit === 'ms' || settings.wind_unit === 'kts') {
      localStorage.setItem('wind_unit', settings.wind_unit);
    }
  } catch (err) {
    console.log('Config parse error: ' + err);
  }
});

Pebble.addEventListener('ready', function() {
  console.log('Wind Forecast JS ready');
});

Pebble.addEventListener('appmessage', function(e) {
  if (e.payload && e.payload.REQUEST) {
    fetchAndSend();
  }
});