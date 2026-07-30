// Air Quality - PebbleKit JS companion
// On each REQUEST from the watch: get GPS location, reverse-geocode it to a
// friendly place name, fetch Open-Meteo air-quality (current + hourly) and
// Open-Meteo weather (for temperature) in parallel, then send everything
// back to the watch in one AppMessage.

var NO_DATA = -999; // sentinel the watch renders as "--"

function pad(n) {
  return n < 10 ? '0' + n : '' + n;
}

function sendError(code) {
  Pebble.sendAppMessage(
    { 'ERROR': code },
    function() {},
    function(e) { console.log('Failed to send error: ' + JSON.stringify(e)); }
  );
}

function r(v) {
  return (v === null || v === undefined) ? NO_DATA : Math.round(v);
}

function r10(v) {
  // one implied decimal place, e.g. 5.2 -> 52
  return (v === null || v === undefined) ? NO_DATA : Math.round(v * 10);
}

function nowLocalIso() {
  var now = new Date();
  return now.getFullYear() + '-' + pad(now.getMonth() + 1) + '-' +
         pad(now.getDate()) + 'T' + pad(now.getHours()) + ':00';
}

function findStartIdx(times) {
  var nowIso = nowLocalIso();
  var startIdx = times.indexOf(nowIso);
  if (startIdx === -1) {
    startIdx = 0;
    var now = new Date();
    for (var i = 0; i < times.length; i++) {
      if (new Date(times[i]) >= now) { startIdx = i; break; }
    }
  }
  return startIdx;
}

function buildCurrentPayload(aq, weather) {
  var c = aq.current;
  var fields = [
    r(c.pm2_5), r(c.pm10), r(c.nitrogen_dioxide), r(c.ozone),
    r(weather.current.temperature_2m), r10(c.uv_index),
    r(c.european_aqi_pm2_5), r(c.european_aqi_pm10),
    r(c.european_aqi_nitrogen_dioxide), r(c.european_aqi_ozone),
    r(c.european_aqi)
  ];
  return fields.join(',');
}

function buildForecastPayload(aq, weather, startIdx) {
  var times = aq.hourly.time;
  var pm25 = aq.hourly.pm2_5, pm10 = aq.hourly.pm10;
  var no2 = aq.hourly.nitrogen_dioxide, o3 = aq.hourly.ozone;
  var uv = aq.hourly.uv_index;
  var temp = weather.hourly.temperature_2m;

  var endIdx = Math.min(startIdx + 24, times.length, temp.length);
  var rows = [];
  for (var j = startIdx; j < endIdx; j++) {
    var hour = parseInt(times[j].substring(11, 13), 10);
    rows.push([
      hour, r(pm25[j]), r(pm10[j]), r(no2[j]), r(o3[j]), r(temp[j]), r10(uv[j])
    ].join(','));
  }
  return rows.join('|');
}

function buildGraphPayload(aq, startIdx) {
  var times = aq.hourly.time;
  var birch = aq.hourly.birch_pollen;
  var grass = aq.hourly.grass_pollen;

  var endIdx = Math.min(startIdx + 24, times.length);
  var rows = [];
  var seasonActive = false;

  for (var j = startIdx; j < endIdx; j++) {
    var b = birch ? birch[j] : null;
    var g = grass ? grass[j] : null;
    if (b !== null && b !== undefined) seasonActive = true;
    if (g !== null && g !== undefined) seasonActive = true;

    var hour = parseInt(times[j].substring(11, 13), 10);
    rows.push([hour, r(b === null || b === undefined ? 0 : b),
                      r(g === null || g === undefined ? 0 : g)].join(','));
  }

  return { season: seasonActive, payload: rows.join('|') };
}

function fetchLocationName(lat, lon, callback) {
  var url = 'https://api.bigdatacloud.net/data/reverse-geocode-client' +
            '?latitude=' + lat + '&longitude=' + lon + '&localityLanguage=en';
  var xhr = new XMLHttpRequest();
  xhr.onload = function() {
    if (xhr.status === 200) {
      try {
        var data = JSON.parse(xhr.responseText);
        var name = data.city || data.locality || data.principalSubdivision || 'Unknown location';
        callback(name);
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

function fetchAirQuality(lat, lon, callback) {
  var url = 'https://air-quality-api.open-meteo.com/v1/air-quality' +
            '?latitude=' + lat + '&longitude=' + lon +
            '&current=pm2_5,pm10,nitrogen_dioxide,ozone,uv_index,european_aqi,' +
            'european_aqi_pm2_5,european_aqi_pm10,european_aqi_nitrogen_dioxide,european_aqi_ozone' +
            '&hourly=pm2_5,pm10,nitrogen_dioxide,ozone,uv_index,birch_pollen,grass_pollen' +
            '&timezone=auto&forecast_days=2';

  var xhr = new XMLHttpRequest();
  xhr.onload = function() {
    if (xhr.status === 200) {
      try {
        callback(null, JSON.parse(xhr.responseText));
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

function fetchWeather(lat, lon, callback) {
  var url = 'https://api.open-meteo.com/v1/forecast' +
            '?latitude=' + lat + '&longitude=' + lon +
            '&current=temperature_2m&hourly=temperature_2m' +
            '&temperature_unit=celsius&timezone=auto&forecast_days=2';

  var xhr = new XMLHttpRequest();
  xhr.onload = function() {
    if (xhr.status === 200) {
      try {
        callback(null, JSON.parse(xhr.responseText));
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

function fetchAndSend() {
  navigator.geolocation.getCurrentPosition(
    function(pos) {
      var lat = pos.coords.latitude.toFixed(4);
      var lon = pos.coords.longitude.toFixed(4);

      var locationName = null;
      var aqData = null, weatherData = null;
      var fetchErr = null;
      var pending = 3;

      function maybeSend() {
        pending--;
        if (pending > 0) { return; }
        if (fetchErr) {
          sendError(fetchErr);
          return;
        }

        var startIdx = findStartIdx(aqData.hourly.time);
        var graph = buildGraphPayload(aqData, startIdx);

        Pebble.sendAppMessage(
          {
            'LOCATION_NAME': locationName || 'Unknown location',
            'CURRENT_DATA': buildCurrentPayload(aqData, weatherData),
            'FORECAST_DATA': buildForecastPayload(aqData, weatherData, startIdx),
            'POLLEN_SEASON': graph.season ? 1 : 0,
            'GRAPH_DATA': graph.payload
          },
          function() { console.log('Air quality data sent'); },
          function(e) { console.log('Send failed: ' + JSON.stringify(e)); }
        );
      }

      fetchLocationName(lat, lon, function(name) {
        locationName = name;
        maybeSend();
      });

      fetchAirQuality(lat, lon, function(err, data) {
        if (err) { fetchErr = err; } else { aqData = data; }
        maybeSend();
      });

      fetchWeather(lat, lon, function(err, data) {
        if (err) { fetchErr = err; } else { weatherData = data; }
        maybeSend();
      });
    },
    function() { sendError('GPS_ERR'); },
    { timeout: 15000, maximumAge: 60000, enableHighAccuracy: false }
  );
}

Pebble.addEventListener('ready', function() {
  console.log('Air Quality JS ready');
});

Pebble.addEventListener('appmessage', function(e) {
  if (e.payload && e.payload.REQUEST) {
    fetchAndSend();
  }
});
