# Pebble AIQ monitor
Air Quality and Pollen monitor for Pebble Time 2 (Emery)

## Features
- Main screen with PM2.5, PM10, NO2, Ozone (O3) polutant values as wel as Temperature and UV Index at the current location (shown on top) and at the bottom the general European Air Quality Index
- Background of cells changes colours according to EU threshold values (severity)
- Forecast screen with 24hr hourly forecast for each polutant, scrollable with both touch and buttons
- Pollen screen with Birch and Grass pollen 24hr forecast values in a graph (Europe only!)


<img src="https://github.com/Moaske/PebbleAIQmonitor/blob/main/docs/main.png"></img>&nbsp;&nbsp;&nbsp;<img src="https://github.com/Moaske/PebbleAIQmonitor/blob/main/docs/forecast.png">&nbsp;&nbsp;&nbsp;<img src="https://github.com/Moaske/PebbleAIQmonitor/blob/main/docs/pollen.png">

Data comes from https://open-meteo.com and should provide worldwide coverage. The threshold values for the colouring of the boxes (severity) are according to EU norms, and so is the general EAQI index at the bottom (which is constructed from the other polutants so should again work worldwide, just EU norms). Location resolving to name by BigDataCloud reverse geoloc call.
Pollen data is ONLY available in the EU, AND only during pollen season. It will show 'No data available at this time' if nothing comes in (for your location).

This Pebble app needs colours, so that's why I only target Emery with this one.

Fully coded with Claude and CloudPebble
