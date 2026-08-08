#include <pebble.h>

#define MAX_ROWS 24
#define GRAPH_POINTS 24
// ACTIONBAR_ICON_WIDTH removed - use the SDK's own ACTION_BAR_WIDTH macro
// instead, since it resolves per-platform and is the correct source of
// truth (a hardcoded 30px caused a slight overlap on Emery).
#define NO_DATA -999
#define HEADER_HEIGHT 22
#define ROW_HEIGHT 26
#define TIME_COL_W 54
#define POLLEN_BOX_HEIGHT 30

typedef struct {
  int hour;
  int pm25, pm10, no2, o3, temp, uv10; // uv10 = uv * 10 (one implied decimal)
} ForecastRow;

typedef struct {
  int hour;
  int trees, grass, weeds; // grains/m3, summed per category
} GraphPoint;

typedef struct {
  int pm25, pm10, no2, o3, temp, uv10;
  int aqi_pm25, aqi_pm10, aqi_no2, aqi_o3; // European AQI sub-indices, -1 = unknown
  int aqi_overall; // Consolidated European AQI (max of sub-indices), -1 = unknown
} CurrentData;

static ForecastRow s_rows[MAX_ROWS];
static int s_row_count = 0;
static GraphPoint s_graph[GRAPH_POINTS];
static int s_graph_count = 0;
static bool s_pollen_season = false;
// Category totals (grains/m3, sum of species per group), NO_DATA if every
// species in that group came back null. Trees = Alder+Birch+Olive,
// Weeds = Mugwort+Ragweed, Grass = Grass (only one species).
static int s_pollen_trees = NO_DATA;
static int s_pollen_grass = NO_DATA;
static int s_pollen_weeds = NO_DATA;
static CurrentData s_current;
// EAQI box content/colour, read by eaqi_box_update_proc at draw time.
static char s_eaqi_text[24] = "";
static GColor s_eaqi_bg = GColorClear;
static GColor s_eaqi_fg = GColorBlack;
static char s_location_name[32] = "Locating...";
static char s_status_text[48] = "Fetching air quality...";
static bool s_has_data = false;

// --- Windows ---
static Window *s_main_window;
static Window *s_forecast_window;
static Window *s_graph_window;

// --- Main window widgets ---
static ActionBarLayer *s_action_bar;
static GBitmap *s_icon_graph;
static GBitmap *s_icon_list_forecast;
static GBitmap *s_icon_refresh;
static TextLayer *s_location_layer;
static Layer *s_eaqi_layer;
static Layer *s_grid_layer;
static TextLayer *s_main_status_layer;

// --- Forecast window widgets ---
static Layer *s_forecast_header_layer;
static ScrollLayer *s_scroll_layer;
static Layer *s_forecast_content_layer;
static GPoint s_touch_last_point;
static TextLayer *s_forecast_status_layer;

// --- Graph window widgets ---
static Layer *s_graph_layer;
static TextLayer *s_graph_status_layer;
static TextLayer *s_pollen_box_layer;

static void refresh_main_window(void);
static void refresh_pollen_box(void);

// ---------------------------------------------------------------------
// Colour mapping - European AQI sub-index -> background colour
// ---------------------------------------------------------------------

static GColor eaqi_to_bg_color(int aqi) {
  if (aqi < 0) return GColorWhite;                 // unknown
  if (aqi < 20) return GColorFromRGB(0, 255, 0);    // Good
  if (aqi < 40) return GColorFromRGB(170, 255, 0);  // Fair
  if (aqi < 60) return GColorFromRGB(255, 170, 0);  // Moderate
  if (aqi < 80) return GColorFromRGB(255, 85, 0);  // Poor
  if (aqi < 100) return GColorFromRGB(255, 0, 0);   // Very poor
  return GColorFromRGB(170, 0, 85);                  // Extremely poor
}

static GColor eaqi_to_fg_color(int aqi) {
  if (aqi >= 80) return GColorWhite;
  return GColorBlack;
}

static const char *eaqi_to_label(int aqi) {
  if (aqi < 0) return "N/A";
  if (aqi < 20) return "Good";
  if (aqi < 40) return "Fair";
  if (aqi < 60) return "Moderate";
  if (aqi < 80) return "Poor";
  if (aqi < 100) return "Very Poor";
  return "Extremely Poor";
}

// ---------------------------------------------------------------------
// Pollen category tiers (Trees/Grass/Weeds) - thresholds from Kleenex/
// Ambee's public pollen risk table (grains/m3), NOT from Open-Meteo
// itself. Each category has its own distinct cutoffs, unlike EAQI which
// uses one universal scale for every pollutant.
// ---------------------------------------------------------------------

typedef enum { POLLEN_TREES, POLLEN_GRASS, POLLEN_WEEDS } PollenCategory;

// Returns 0=Low, 1=Moderate, 2=High, 3=Very High, or -1 for no data.
static int pollen_tier(int value, PollenCategory cat) {
  if (value < 0) return -1;

  int low_max, moderate_max, high_max;
  switch (cat) {
    case POLLEN_TREES: low_max = 95;  moderate_max = 207; high_max = 703; break;
    case POLLEN_GRASS: low_max = 29;  moderate_max = 60;  high_max = 341; break;
    case POLLEN_WEEDS: default: low_max = 20; moderate_max = 77; high_max = 266; break;
  }

  if (value <= low_max) return 0;
  if (value <= moderate_max) return 1;
  if (value <= high_max) return 2;
  return 3;
}

static const char *pollen_tier_label(int tier) {
  switch (tier) {
    case 0: return "Low";
    case 1: return "Moderate";
    case 2: return "High";
    case 3: return "Very High";
    default: return "N/A";
  }
}

static GColor pollen_tier_bg_color(int tier) {
  switch (tier) {
    case 0: return GColorFromRGB(0, 255, 0);    // Low
    case 1: return GColorFromRGB(255, 190, 0);  // Moderate
    case 2: return GColorFromRGB(255, 120, 0);  // High
    case 3: return GColorFromRGB(220, 0, 0);    // Very High
    default: return GColorWhite;                 // N/A
  }
}

static GColor pollen_tier_fg_color(int tier) {
  if (tier >= 2) return GColorWhite;
  return GColorBlack;
}

// ---------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------

// Picks the largest font from a list (ordered biggest to smallest) whose
// rendered width still fits inside max_width.
static GFont pick_fit_font(const char *text, int max_width, const GFont *candidates, int count) {
  for (int i = 0; i < count; i++) {
    GSize sz = graphics_text_layout_get_content_size(text, candidates[i], GRect(0, 0, 500, 60),
                                                       GTextOverflowModeFill, GTextAlignmentLeft);
    if (sz.w <= max_width) return candidates[i];
  }
  return candidates[count - 1];
}

// Draws text vertically centered within box, at the given horizontal alignment.
static void draw_aligned_text(GContext *ctx, const char *text, GFont font, GRect box, GTextAlignment align) {
  GSize sz = graphics_text_layout_get_content_size(text, font, GRect(0, 0, box.size.w, 200),
                                                     GTextOverflowModeFill, align);
  int y_off = (box.size.h - sz.h) / 2;
  if (y_off < 0) y_off = 0;
  GRect r = GRect(box.origin.x, box.origin.y + y_off, box.size.w, sz.h + 2);
  graphics_draw_text(ctx, text, font, r, GTextOverflowModeFill, align, NULL);
}

// Draws text horizontally AND vertically centered within box.
static void draw_centered_text(GContext *ctx, const char *text, GFont font, GRect box) {
  draw_aligned_text(ctx, text, font, box, GTextAlignmentCenter);
}

static void fmt_int(char *buf, size_t buflen, int v) {
  if (v == NO_DATA) {
    strncpy(buf, "--", buflen - 1);
  } else {
    snprintf(buf, buflen, "%d", v);
  }
}

static void fmt_uv(char *buf, size_t buflen, int uv10) {
  if (uv10 == NO_DATA) {
    strncpy(buf, "--", buflen - 1);
  } else {
    snprintf(buf, buflen, "%d.%d", uv10 / 10, abs(uv10) % 10);
  }
}

// ---------------------------------------------------------------------
// Data request / parsing
// ---------------------------------------------------------------------

static void request_data(void) {
  s_has_data = false;
  s_row_count = 0;
  s_graph_count = 0;
  strncpy(s_status_text, "Fetching air quality...", sizeof(s_status_text) - 1);
  refresh_main_window();

  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK && iter) {
    dict_write_int32(iter, MESSAGE_KEY_REQUEST, 1);
    app_message_outbox_send();
  }
}

static void parse_current(const char *data) {
  int vals[11];
  char buffer[128];
  strncpy(buffer, data, sizeof(buffer) - 1);
  buffer[sizeof(buffer) - 1] = '\0';

  char *cursor = buffer;
  int idx = 0;
  while (cursor && *cursor && idx < 11) {
    char *next = strchr(cursor, ',');
    if (next) *next = '\0';
    vals[idx++] = atoi(cursor);
    cursor = next ? next + 1 : NULL;
  }
  if (idx < 11) return;

  s_current.pm25 = vals[0];
  s_current.pm10 = vals[1];
  s_current.no2 = vals[2];
  s_current.o3 = vals[3];
  s_current.temp = vals[4];
  s_current.uv10 = vals[5];
  s_current.aqi_pm25 = vals[6];
  s_current.aqi_pm10 = vals[7];
  s_current.aqi_no2 = vals[8];
  s_current.aqi_o3 = vals[9];
  s_current.aqi_overall = vals[10];
}

static void parse_forecast(const char *data) {
  s_row_count = 0;
  static char buffer[1200];
  strncpy(buffer, data, sizeof(buffer) - 1);
  buffer[sizeof(buffer) - 1] = '\0';

  char *cursor = buffer;
  while (cursor && *cursor && s_row_count < MAX_ROWS) {
    char *next_row = strchr(cursor, '|');
    if (next_row) *next_row = '\0';

    int vals[7];
    char *p = cursor;
    int idx = 0;
    bool ok = true;
    while (idx < 7) {
      char *c = strchr(p, ',');
      if (idx < 6 && !c) { ok = false; break; }
      if (c) *c = '\0';
      vals[idx++] = atoi(p);
      if (!c) break;
      p = c + 1;
    }

    if (ok && idx == 7) {
      ForecastRow *row = &s_rows[s_row_count];
      row->hour = vals[0];
      row->pm25 = vals[1];
      row->pm10 = vals[2];
      row->no2 = vals[3];
      row->o3 = vals[4];
      row->temp = vals[5];
      row->uv10 = vals[6];
      s_row_count++;
    }

    cursor = next_row ? next_row + 1 : NULL;
  }
}

static void parse_graph(const char *data) {
  s_graph_count = 0;
  static char buffer[700];
  strncpy(buffer, data, sizeof(buffer) - 1);
  buffer[sizeof(buffer) - 1] = '\0';

  char *cursor = buffer;
  while (cursor && *cursor && s_graph_count < GRAPH_POINTS) {
    char *next_row = strchr(cursor, '|');
    if (next_row) *next_row = '\0';

    int vals[4];
    char *p = cursor;
    int idx = 0;
    bool ok = true;
    while (idx < 4) {
      char *c = strchr(p, ',');
      if (idx < 3 && !c) { ok = false; break; }
      if (c) *c = '\0';
      vals[idx++] = atoi(p);
      if (!c) break;
      p = c + 1;
    }

    if (ok && idx == 4) {
      s_graph[s_graph_count].hour = vals[0];
      s_graph[s_graph_count].trees = vals[1];
      s_graph[s_graph_count].grass = vals[2];
      s_graph[s_graph_count].weeds = vals[3];
      s_graph_count++;
    }

    cursor = next_row ? next_row + 1 : NULL;
  }
}

// Parses "trees,grass,weeds" (each already summed on the phone side).
static void parse_pollen_category(const char *data) {
  char buffer[48];
  strncpy(buffer, data, sizeof(buffer) - 1);
  buffer[sizeof(buffer) - 1] = '\0';

  int vals[3] = { NO_DATA, NO_DATA, NO_DATA };
  char *cursor = buffer;
  int idx = 0;
  while (cursor && *cursor && idx < 3) {
    char *next = strchr(cursor, ',');
    if (next) *next = '\0';
    vals[idx++] = atoi(cursor);
    cursor = next ? next + 1 : NULL;
  }
  if (idx < 3) return;

  s_pollen_trees = vals[0];
  s_pollen_grass = vals[1];
  s_pollen_weeds = vals[2];
}

static void inbox_received_handler(DictionaryIterator *iter, void *context) {
  Tuple *err_tuple = dict_find(iter, MESSAGE_KEY_ERROR);
  Tuple *cur_tuple = dict_find(iter, MESSAGE_KEY_CURRENT_DATA);
  Tuple *fc_tuple = dict_find(iter, MESSAGE_KEY_FORECAST_DATA);
  Tuple *loc_tuple = dict_find(iter, MESSAGE_KEY_LOCATION_NAME);
  Tuple *season_tuple = dict_find(iter, MESSAGE_KEY_POLLEN_SEASON);
  Tuple *graph_tuple = dict_find(iter, MESSAGE_KEY_GRAPH_DATA);
  Tuple *pollen_cat_tuple = dict_find(iter, MESSAGE_KEY_POLLEN_CATEGORY_DATA);

  if (loc_tuple) {
    strncpy(s_location_name, loc_tuple->value->cstring, sizeof(s_location_name) - 1);
    s_location_name[sizeof(s_location_name) - 1] = '\0';
  }

  if (err_tuple) {
    snprintf(s_status_text, sizeof(s_status_text), "Error: %s", err_tuple->value->cstring);
    s_has_data = false;
  } else if (cur_tuple && fc_tuple) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "CURRENT_DATA raw: %s", cur_tuple->value->cstring);
    parse_current(cur_tuple->value->cstring);
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Parsed aqi_overall: %d", s_current.aqi_overall);
    parse_forecast(fc_tuple->value->cstring);
    s_pollen_season = season_tuple ? (season_tuple->value->int32 != 0) : false;
    if (s_pollen_season && graph_tuple) {
      parse_graph(graph_tuple->value->cstring);
    }
    if (pollen_cat_tuple) {
      parse_pollen_category(pollen_cat_tuple->value->cstring);
    }
    s_has_data = (s_row_count > 0);
    APP_LOG(APP_LOG_LEVEL_DEBUG, "row_count: %d, has_data: %d", s_row_count, s_has_data);
    if (!s_has_data) {
      strncpy(s_status_text, "No data received", sizeof(s_status_text) - 1);
    }
  }

  refresh_main_window();

  if (s_scroll_layer) {
    text_layer_set_text(s_forecast_status_layer, s_has_data ? "" : s_status_text);
    layer_set_hidden(text_layer_get_layer(s_forecast_status_layer), s_has_data);
    layer_set_hidden(scroll_layer_get_layer(s_scroll_layer), !s_has_data);
    layer_set_hidden(s_forecast_header_layer, !s_has_data);

    int content_h = s_row_count * ROW_HEIGHT;
    GRect frame = layer_get_bounds(scroll_layer_get_layer(s_scroll_layer));
    if (content_h < frame.size.h) content_h = frame.size.h;
    scroll_layer_set_content_size(s_scroll_layer, GSize(frame.size.w, content_h));
    scroll_layer_set_content_offset(s_scroll_layer, GPointZero, false); // reset to top on refresh
    layer_set_frame(s_forecast_content_layer, GRect(0, 0, frame.size.w, content_h));
    layer_mark_dirty(s_forecast_content_layer);
  }

  if (s_graph_layer) {
    text_layer_set_text(s_graph_status_layer, s_has_data ? "" : s_status_text);
    layer_set_hidden(text_layer_get_layer(s_graph_status_layer), s_has_data);
    layer_set_hidden(s_graph_layer, !s_has_data);
    layer_mark_dirty(s_graph_layer);
  }

  refresh_pollen_box();
}

static void inbox_dropped_handler(AppMessageResult reason, void *context) {
  strncpy(s_status_text, "Message dropped", sizeof(s_status_text) - 1);
  s_has_data = false;
  refresh_main_window();
}

static void outbox_failed_handler(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  strncpy(s_status_text, "Send to phone failed", sizeof(s_status_text) - 1);
  s_has_data = false;
  refresh_main_window();
}

// ---------------------------------------------------------------------
// MAIN window - location + 2x3 colour-coded box grid
// ---------------------------------------------------------------------

static void main_grid_update_proc(Layer *layer, GContext *ctx) {
  if (!s_has_data) return;
  GRect b = layer_get_bounds(layer);

  int row_h = b.size.h / 6;
  int border = 2;
  int side_pad = 6;

  static const char *labels[6] = {
    "PM2.5 ug/m3", "PM10 ug/m3", "NO2 ug/m3", "Ozone (O3)", "Temp C", "UV Index"
  };

  char values[6][12];
  fmt_int(values[0], sizeof(values[0]), s_current.pm25);
  fmt_int(values[1], sizeof(values[1]), s_current.pm10);
  fmt_int(values[2], sizeof(values[2]), s_current.no2);
  fmt_int(values[3], sizeof(values[3]), s_current.o3);
  fmt_int(values[4], sizeof(values[4]), s_current.temp);
  fmt_uv(values[5], sizeof(values[5]), s_current.uv10);

  GColor bg[6] = {
    eaqi_to_bg_color(s_current.aqi_pm25),
    eaqi_to_bg_color(s_current.aqi_pm10),
    eaqi_to_bg_color(s_current.aqi_no2),
    eaqi_to_bg_color(s_current.aqi_o3),
    GColorWhite, // Temperature: no colour coding
    GColorWhite  // UV Index: no colour coding
  };
  GColor fg[6] = {
    eaqi_to_fg_color(s_current.aqi_pm25),
    eaqi_to_fg_color(s_current.aqi_pm10),
    eaqi_to_fg_color(s_current.aqi_no2),
    eaqi_to_fg_color(s_current.aqi_o3),
    GColorBlack,
    GColorBlack
  };

  // Fixed size for everything, rather than auto-fit per bar - PM2.5's
  // label is the longest text of the six ("PM2.5 ug/m3"), so whatever
  // size comfortably fits that is guaranteed to fit every shorter label
  // and value too. GOTHIC_14_BOLD is what PM2.5 was already landing on.
  GFont bar_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

  for (int i = 0; i < 6; i++) {
    GRect bar = GRect(border, i * row_h + border, b.size.w - 2 * border, row_h - 2 * border);

    graphics_context_set_fill_color(ctx, bg[i]);
    graphics_fill_rect(ctx, bar, 0, GCornerNone);

    // 65/35 split: labels are consistently longer than the short numeric
    // values, so give them the bigger share rather than an even half.
    int label_w = (bar.size.w * 65) / 100;
    GRect label_box = GRect(bar.origin.x + side_pad, bar.origin.y, label_w - side_pad, bar.size.h);
    GRect value_box = GRect(bar.origin.x + label_w, bar.origin.y,
                             bar.size.w - label_w - side_pad, bar.size.h);

    graphics_context_set_text_color(ctx, fg[i]);
    draw_aligned_text(ctx, labels[i], bar_font, label_box, GTextAlignmentLeft);
    draw_aligned_text(ctx, values[i], bar_font, value_box, GTextAlignmentRight);
  }
}

// Draws the EAQI box: background fill over the full (unchanged) frame,
// text vertically centered but nudged up 2px within it - clamped so it
// never rises above the box's own top edge.
static void eaqi_box_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);

  graphics_context_set_fill_color(ctx, s_eaqi_bg);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  if (s_eaqi_text[0] == '\0') return;

  GFont eaqi_candidates[3] = {
    fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
    fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD)
  };
  GFont font = pick_fit_font(s_eaqi_text, b.size.w - 4, eaqi_candidates, 3);

  GSize sz = graphics_text_layout_get_content_size(s_eaqi_text, font, GRect(0, 0, b.size.w, 200),
                                                     GTextOverflowModeFill, GTextAlignmentCenter);
  int y_off = (b.size.h - sz.h) / 2 - 6; // 2px higher than dead-centre
  if (y_off < -6) y_off = -6; // allow a couple px of top overflow rather than
                               // cancelling the shift back to 0 when there's
                               // little natural padding to begin with

  graphics_context_set_text_color(ctx, s_eaqi_fg);
  GRect r = GRect(b.origin.x, b.origin.y + y_off, b.size.w, sz.h + 2);
  graphics_draw_text(ctx, s_eaqi_text, font, r, GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

static void refresh_main_window(void) {
  if (!s_location_layer) return; // main window not currently loaded

  text_layer_set_text(s_location_layer, s_location_name);

  if (!s_has_data) {
    text_layer_set_text(s_main_status_layer, s_status_text);
    layer_set_hidden(text_layer_get_layer(s_main_status_layer), false);
    layer_set_hidden(s_grid_layer, true);
    s_eaqi_text[0] = '\0';
    s_eaqi_bg = GColorClear;
    layer_mark_dirty(s_eaqi_layer);
    return;
  }
  layer_set_hidden(text_layer_get_layer(s_main_status_layer), true);
  layer_set_hidden(s_grid_layer, false);
  layer_mark_dirty(s_grid_layer);

  snprintf(s_eaqi_text, sizeof(s_eaqi_text), "EAQI: %s", eaqi_to_label(s_current.aqi_overall));
  s_eaqi_bg = eaqi_to_bg_color(s_current.aqi_overall);
  s_eaqi_fg = eaqi_to_fg_color(s_current.aqi_overall);
  layer_mark_dirty(s_eaqi_layer);
}

// Populates the graph screen's pollen category box from whatever global
// pollen data is currently available. Called both reactively (when new
// data arrives) and at graph_window_load time, since the box may be
// created well after the data it needs to display already arrived (e.g.
// data fetched while the person was still on the main screen).
static void refresh_pollen_box(void) {
  if (!s_pollen_box_layer) return; // graph window not currently loaded

  if (!s_has_data) {
    text_layer_set_text(s_pollen_box_layer, "");
    text_layer_set_background_color(s_pollen_box_layer, GColorClear);
    return;
  }

  int tier_trees = pollen_tier(s_pollen_trees, POLLEN_TREES);
  int tier_grass = pollen_tier(s_pollen_grass, POLLEN_GRASS);
  int tier_weeds = pollen_tier(s_pollen_weeds, POLLEN_WEEDS);

  // Overall = worst (highest) tier among the three categories that
  // actually has data; -1 (N/A) only if all three are absent.
  int overall_tier = -1;
  if (tier_trees > overall_tier) overall_tier = tier_trees;
  if (tier_grass > overall_tier) overall_tier = tier_grass;
  if (tier_weeds > overall_tier) overall_tier = tier_weeds;

  static char pollen_buf[20];
  snprintf(pollen_buf, sizeof(pollen_buf), "Pollen: %s", pollen_tier_label(overall_tier));
  text_layer_set_text(s_pollen_box_layer, pollen_buf);
  text_layer_set_background_color(s_pollen_box_layer, pollen_tier_bg_color(overall_tier));
  text_layer_set_text_color(s_pollen_box_layer, pollen_tier_fg_color(overall_tier));
}

static void main_up_click(ClickRecognizerRef recognizer, void *context) {
  window_stack_push(s_graph_window, true);
}

static void main_down_click(ClickRecognizerRef recognizer, void *context) {
  window_stack_push(s_forecast_window, true);
}

static void main_select_click(ClickRecognizerRef recognizer, void *context) {
  request_data();
}

static void main_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, main_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, main_down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, main_select_click);
}

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  int content_width = bounds.size.w - ACTION_BAR_WIDTH;

  s_action_bar = action_bar_layer_create();
  action_bar_layer_add_to_window(s_action_bar, window);
  action_bar_layer_set_background_color(s_action_bar, GColorLightGray);
  action_bar_layer_set_click_config_provider(s_action_bar, main_click_config_provider);

  s_icon_graph = gbitmap_create_with_resource(RESOURCE_ID_GRAPH);
  s_icon_list_forecast = gbitmap_create_with_resource(RESOURCE_ID_LIST_FORECAST);
  s_icon_refresh = gbitmap_create_with_resource(RESOURCE_ID_REFRESH);

  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_UP, s_icon_graph);
  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_DOWN, s_icon_list_forecast);
  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_SELECT, s_icon_refresh);

  int loc_h = 30; // was 26 - a few extra px so the location text stops clipping at the bottom
  int eaqi_h = 26; // same size as location, per request

  s_location_layer = text_layer_create(GRect(4, 0, content_width - 8, loc_h));
  text_layer_set_font(s_location_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_location_layer, GTextAlignmentCenter);
  text_layer_set_text(s_location_layer, s_location_name);
  layer_add_child(window_layer, text_layer_get_layer(s_location_layer));

  s_grid_layer = layer_create(GRect(0, loc_h, content_width, bounds.size.h - loc_h - eaqi_h));
  layer_set_update_proc(s_grid_layer, main_grid_update_proc);
  layer_add_child(window_layer, s_grid_layer);

  s_eaqi_layer = layer_create(GRect(4, bounds.size.h - eaqi_h, content_width - 8, eaqi_h));
  layer_set_update_proc(s_eaqi_layer, eaqi_box_update_proc);
  layer_add_child(window_layer, s_eaqi_layer);

  s_main_status_layer = text_layer_create(GRect(0, bounds.size.h / 2 - 20, content_width, 40));
  text_layer_set_text_alignment(s_main_status_layer, GTextAlignmentCenter);
  text_layer_set_font(s_main_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text(s_main_status_layer, s_status_text);
  layer_add_child(window_layer, text_layer_get_layer(s_main_status_layer));

  refresh_main_window();
}

static void main_window_unload(Window *window) {
  action_bar_layer_destroy(s_action_bar);
  gbitmap_destroy(s_icon_graph);
  gbitmap_destroy(s_icon_list_forecast);
  gbitmap_destroy(s_icon_refresh);
  text_layer_destroy(s_location_layer);
  layer_destroy(s_grid_layer);
  layer_destroy(s_eaqi_layer);
  text_layer_destroy(s_main_status_layer);

  s_location_layer = NULL;
  s_grid_layer = NULL;
  s_eaqi_layer = NULL;
  s_main_status_layer = NULL;
}

// ---------------------------------------------------------------------
// FORECAST window - sticky header + scrolling list, narrow time column
// ---------------------------------------------------------------------

static int forecast_col_x(int window_width, int col_index) {
  int value_area = window_width - TIME_COL_W;
  int col_w = value_area / 4;
  return TIME_COL_W + col_index * col_w;
}

static int forecast_col_w(int window_width) {
  int value_area = window_width - TIME_COL_W;
  return value_area / 4;
}

static void forecast_header_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  static const char *headers[4] = {"PM2.5", "PM10", "NO2", "O3"};
  int col_w = forecast_col_w(b.size.w);

  // Try the biggest font first; only use it if every label still fits its
  // column at that size, so a short label like "O3" doesn't force a size
  // too big for "PM2.5" to fit.
  GFont header_candidates[2] = {
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    fonts_get_system_font(FONT_KEY_GOTHIC_09)
  };
  GFont font = header_candidates[1];
  for (int f = 0; f < 2; f++) {
    bool all_fit = true;
    for (int i = 0; i < 4; i++) {
      GSize sz = graphics_text_layout_get_content_size(headers[i], header_candidates[f],
                                                         GRect(0, 0, 500, 30),
                                                         GTextOverflowModeFill, GTextAlignmentLeft);
      if (sz.w > col_w - 4) { all_fit = false; break; }
    }
    if (all_fit) { font = header_candidates[f]; break; }
  }

  graphics_context_set_fill_color(ctx, GColorLightGray);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  graphics_context_set_text_color(ctx, GColorBlack);
  for (int i = 0; i < 4; i++) {
    GRect cell = GRect(forecast_col_x(b.size.w, i), 0, col_w, b.size.h);
    draw_centered_text(ctx, headers[i], font, cell);
  }

  graphics_context_set_stroke_color(ctx, GColorDarkGray);
  graphics_draw_line(ctx, GPoint(0, b.size.h - 1), GPoint(b.size.w, b.size.h - 1));
}

// Draws one forecast row at a fixed y-offset within the scrollable content
// layer. No highlight/selection concept here - ScrollLayer just scrolls, it
// doesn't have MenuLayer's row-selection styling to fight with.
static void draw_forecast_row(GContext *ctx, int row_index, int y, int width) {
  ForecastRow *r = &s_rows[row_index];
  GRect bounds = GRect(0, y, width, ROW_HEIGHT);

  GFont time_font = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  GFont value_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  char hour_buf[6], v[4][6];
  snprintf(hour_buf, sizeof(hour_buf), "%02d:00", r->hour);
  fmt_int(v[0], sizeof(v[0]), r->pm25);
  fmt_int(v[1], sizeof(v[1]), r->pm10);
  fmt_int(v[2], sizeof(v[2]), r->no2);
  fmt_int(v[3], sizeof(v[3]), r->o3);

  graphics_context_set_text_color(ctx, GColorBlack);
  draw_centered_text(ctx, hour_buf, time_font, GRect(0, y, TIME_COL_W, ROW_HEIGHT));

  int col_w = forecast_col_w(width);
  for (int i = 0; i < 4; i++) {
    GRect cell = GRect(forecast_col_x(width, i), y, col_w, ROW_HEIGHT);
    draw_centered_text(ctx, v[i], value_font, cell);
  }

  graphics_context_set_stroke_color(ctx, GColorLightGray);
  graphics_draw_line(ctx, GPoint(0, y + ROW_HEIGHT - 1), GPoint(width, y + ROW_HEIGHT - 1));
}

static void forecast_content_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  for (int i = 0; i < s_row_count; i++) {
    draw_forecast_row(ctx, i, i * ROW_HEIGHT, b.size.w);
  }
}

// Clamp a ScrollLayer's content offset to its valid [min, 0] range. Needed
// because we set the offset directly from touch deltas rather than only
// ever nudging it by a known-safe button-scroll increment.
static void clamp_scroll_offset(void) {
  GRect frame = layer_get_bounds(scroll_layer_get_layer(s_scroll_layer));
  GSize content = scroll_layer_get_content_size(s_scroll_layer);
  int min_y = frame.size.h - content.h;
  if (min_y > 0) min_y = 0;

  GPoint offset = scroll_layer_get_content_offset(s_scroll_layer);
  if (offset.y > 0) offset.y = 0;
  if (offset.y < min_y) offset.y = min_y;
  scroll_layer_set_content_offset(s_scroll_layer, offset, false);
}

static void forecast_touch_handler(const TouchEvent *event, void *context) {
  switch (event->type) {
    case TouchEvent_Touchdown:
      s_touch_last_point = GPoint(event->x, event->y);
      break;
    case TouchEvent_PositionUpdate: {
      int dy = event->y - s_touch_last_point.y;
      GPoint offset = scroll_layer_get_content_offset(s_scroll_layer);
      offset.y += dy;
      scroll_layer_set_content_offset(s_scroll_layer, offset, false);
      clamp_scroll_offset();
      s_touch_last_point = GPoint(event->x, event->y);
      break;
    }
    case TouchEvent_Liftoff:
    default:
      break;
  }
}

static void forecast_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_forecast_header_layer = layer_create(GRect(0, 0, bounds.size.w, HEADER_HEIGHT));
  layer_set_update_proc(s_forecast_header_layer, forecast_header_update_proc);
  layer_add_child(window_layer, s_forecast_header_layer);
  layer_set_hidden(s_forecast_header_layer, !s_has_data);

  // Scroll area sits below the fixed header, so scrolled rows disappear underneath it.
  GRect scroll_bounds = GRect(0, HEADER_HEIGHT, bounds.size.w, bounds.size.h - HEADER_HEIGHT);
  s_scroll_layer = scroll_layer_create(scroll_bounds);
  scroll_layer_set_click_config_onto_window(s_scroll_layer, window); // keeps UP/DOWN button scrolling

  int content_h = s_row_count * ROW_HEIGHT;
  if (content_h < scroll_bounds.size.h) content_h = scroll_bounds.size.h;
  scroll_layer_set_content_size(s_scroll_layer, GSize(scroll_bounds.size.w, content_h));

  s_forecast_content_layer = layer_create(GRect(0, 0, scroll_bounds.size.w, content_h));
  layer_set_update_proc(s_forecast_content_layer, forecast_content_update_proc);
  scroll_layer_add_child(s_scroll_layer, s_forecast_content_layer);

  layer_add_child(window_layer, scroll_layer_get_layer(s_scroll_layer));
  layer_set_hidden(scroll_layer_get_layer(s_scroll_layer), !s_has_data);

  // Touch-drag scrolling: only real on Emery hardware, but harmless to
  // subscribe anywhere - it's simply a no-op on platforms without a
  // touchscreen (touch_service_is_enabled() would return false there).
  if (touch_service_is_enabled()) {
    touch_service_subscribe(forecast_touch_handler, NULL);
  }

  s_forecast_status_layer = text_layer_create(GRect(0, bounds.size.h / 2 - 20, bounds.size.w, 40));
  text_layer_set_text_alignment(s_forecast_status_layer, GTextAlignmentCenter);
  text_layer_set_font(s_forecast_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text(s_forecast_status_layer, s_has_data ? "" : s_status_text);
  layer_set_hidden(text_layer_get_layer(s_forecast_status_layer), s_has_data);
  layer_add_child(window_layer, text_layer_get_layer(s_forecast_status_layer));
}

static void forecast_window_unload(Window *window) {
  touch_service_unsubscribe();
  layer_destroy(s_forecast_content_layer);
  scroll_layer_destroy(s_scroll_layer);
  layer_destroy(s_forecast_header_layer);
  text_layer_destroy(s_forecast_status_layer);
  s_forecast_content_layer = NULL;
  s_scroll_layer = NULL;
  s_forecast_header_layer = NULL;
  s_forecast_status_layer = NULL;
}

// ---------------------------------------------------------------------
// GRAPH window - 12h Birch/Grass pollen graph with dynamic Y scale
// ---------------------------------------------------------------------

static void draw_dashed_line(GContext *ctx, GPoint p1, GPoint p2, int num_dashes) {
  for (int i = 0; i < num_dashes; i++) {
    int x1 = p1.x + (p2.x - p1.x) * i / num_dashes;
    int y1 = p1.y + (p2.y - p1.y) * i / num_dashes;
    int x2 = p1.x + (p2.x - p1.x) * (i * 2 + 1) / (num_dashes * 2);
    int y2 = p1.y + (p2.y - p1.y) * (i * 2 + 1) / (num_dashes * 2);
    graphics_draw_line(ctx, GPoint(x1, y1), GPoint(x2, y2));
  }
}

// Picks a "nice" axis step (1/2/5 x10^n) so the range fits in ~4-5 gridlines.
static int nice_step(int max_val) {
  static const int steps[] = {1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 5000};
  int n = sizeof(steps) / sizeof(steps[0]);
  for (int i = 0; i < n; i++) {
    if (max_val <= steps[i] * 5) return steps[i];
  }
  return steps[n - 1];
}

static void graph_no_season_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  graphics_context_set_text_color(ctx, GColorBlack);
  draw_centered_text(ctx, "No Pollen data", font, GRect(0, b.size.h / 2 - 24, b.size.w, 24));
  draw_centered_text(ctx, "available at this time", font, GRect(0, b.size.h / 2, b.size.w, 24));
}

static void graph_update_proc(Layer *layer, GContext *ctx) {
  if (!s_has_data) return;

  if (!s_pollen_season || s_graph_count < 2) {
    graph_no_season_update_proc(layer, ctx);
    return;
  }

  GRect bounds = layer_get_bounds(layer);
  int margin_left = 28;
  int margin_top = 6;
  int hour_label_h = 16;
  int legend_h = 48; // three lines of legend text
  int margin_bottom = hour_label_h + legend_h;

  int plot_w = bounds.size.w - margin_left - 6;
  int plot_h = bounds.size.h - margin_bottom - margin_top;
  int origin_x = margin_left;
  int origin_y = margin_top + plot_h;

  GColor trees_color = GColorFromRGB(140, 90, 40); // brown
  GColor grass_color = GColorFromRGB(0, 140, 0);   // green
  GColor weeds_color = GColorFromRGB(140, 0, 140); // purple

  int max_val = 1;
  for (int i = 0; i < s_graph_count; i++) {
    if (s_graph[i].trees > max_val) max_val = s_graph[i].trees;
    if (s_graph[i].grass > max_val) max_val = s_graph[i].grass;
    if (s_graph[i].weeds > max_val) max_val = s_graph[i].weeds;
  }
  int step = nice_step(max_val);
  int y_max = ((max_val / step) + 1) * step;

  GFont label_font = fonts_get_system_font(FONT_KEY_GOTHIC_09);
  GFont hour_font = fonts_get_system_font(FONT_KEY_GOTHIC_09);
  GFont legend_font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);

  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_line(ctx, GPoint(origin_x, margin_top), GPoint(origin_x, origin_y));
  graphics_draw_line(ctx, GPoint(origin_x, origin_y), GPoint(origin_x + plot_w, origin_y));

  for (int gv = 0; gv <= y_max; gv += step) {
    int y = origin_y - (gv * plot_h) / y_max;
    graphics_context_set_stroke_color(ctx, GColorLightGray);
    graphics_draw_line(ctx, GPoint(origin_x, y), GPoint(origin_x + plot_w, y));

    char buf[6];
    snprintf(buf, sizeof(buf), "%d", gv);
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, buf, label_font, GRect(0, y - 7, margin_left - 3, 14),
                        GTextOverflowModeFill, GTextAlignmentRight, NULL);
  }

  GPoint prev_trees = GPoint(0, 0), prev_grass = GPoint(0, 0), prev_weeds = GPoint(0, 0);
  for (int i = 0; i < s_graph_count; i++) {
    int x = origin_x + (i * plot_w) / (s_graph_count - 1);

    int vt = s_graph[i].trees > y_max ? y_max : s_graph[i].trees;
    GPoint pt = GPoint(x, origin_y - (vt * plot_h) / y_max);

    int vg = s_graph[i].grass > y_max ? y_max : s_graph[i].grass;
    GPoint pg = GPoint(x, origin_y - (vg * plot_h) / y_max);

    int vw = s_graph[i].weeds > y_max ? y_max : s_graph[i].weeds;
    GPoint pw = GPoint(x, origin_y - (vw * plot_h) / y_max);

    if (i > 0) {
      graphics_context_set_stroke_color(ctx, trees_color); // Trees - solid
      graphics_context_set_stroke_width(ctx, 2);
      graphics_draw_line(ctx, prev_trees, pt);

      graphics_context_set_stroke_color(ctx, grass_color); // Grass - dashed
      graphics_context_set_stroke_width(ctx, 2);
      draw_dashed_line(ctx, prev_grass, pg, 4);

      graphics_context_set_stroke_color(ctx, weeds_color); // Weeds - dotted (denser dashing)
      graphics_context_set_stroke_width(ctx, 2);
      draw_dashed_line(ctx, prev_weeds, pw, 8);
    }

    graphics_context_set_fill_color(ctx, trees_color);
    graphics_fill_circle(ctx, pt, 2);
    graphics_context_set_fill_color(ctx, grass_color);
    graphics_fill_circle(ctx, pg, 2);
    graphics_context_set_fill_color(ctx, weeds_color);
    graphics_fill_circle(ctx, pw, 2);

    if (i % 4 == 0) { // thin out hour labels to avoid crowding
      char hbuf[4];
      snprintf(hbuf, sizeof(hbuf), "%02d", s_graph[i].hour);
      graphics_context_set_text_color(ctx, GColorBlack);
      graphics_draw_text(ctx, hbuf, hour_font, GRect(x - 12, origin_y + 3, 24, hour_label_h),
                          GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    }

    prev_trees = pt;
    prev_grass = pg;
    prev_weeds = pw;
  }

  int legend_line_h = legend_h / 3;
  graphics_context_set_text_color(ctx, trees_color);
  draw_centered_text(ctx, "Trees ----", legend_font,
                      GRect(0, bounds.size.h - legend_h, bounds.size.w, legend_line_h));
  graphics_context_set_text_color(ctx, grass_color);
  draw_centered_text(ctx, "Grass - - -", legend_font,
                      GRect(0, bounds.size.h - legend_h + legend_line_h, bounds.size.w, legend_line_h));
  graphics_context_set_text_color(ctx, weeds_color);
  draw_centered_text(ctx, "Weeds ......", legend_font,
                      GRect(0, bounds.size.h - legend_line_h, bounds.size.w, legend_line_h));
}

static void graph_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_pollen_box_layer = text_layer_create(GRect(0, 0, bounds.size.w, POLLEN_BOX_HEIGHT));
  text_layer_set_font(s_pollen_box_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_pollen_box_layer, GTextAlignmentCenter);
  text_layer_set_text(s_pollen_box_layer, "");
  layer_add_child(window_layer, text_layer_get_layer(s_pollen_box_layer));
  refresh_pollen_box(); // data may already be available even if this box wasn't

  GRect graph_bounds = GRect(0, POLLEN_BOX_HEIGHT, bounds.size.w, bounds.size.h - POLLEN_BOX_HEIGHT);
  s_graph_layer = layer_create(graph_bounds);
  layer_set_update_proc(s_graph_layer, graph_update_proc);
  layer_set_hidden(s_graph_layer, !s_has_data);
  layer_add_child(window_layer, s_graph_layer);

  s_graph_status_layer = text_layer_create(GRect(0, POLLEN_BOX_HEIGHT + graph_bounds.size.h / 2 - 20,
                                                  bounds.size.w, 40));
  text_layer_set_text_alignment(s_graph_status_layer, GTextAlignmentCenter);
  text_layer_set_font(s_graph_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text(s_graph_status_layer, s_has_data ? "" : s_status_text);
  layer_set_hidden(text_layer_get_layer(s_graph_status_layer), s_has_data);
  layer_add_child(window_layer, text_layer_get_layer(s_graph_status_layer));
}

static void graph_window_unload(Window *window) {
  layer_destroy(s_graph_layer);
  text_layer_destroy(s_graph_status_layer);
  text_layer_destroy(s_pollen_box_layer);
  s_graph_layer = NULL;
  s_graph_status_layer = NULL;
  s_pollen_box_layer = NULL;
}

// ---------------------------------------------------------------------
// App init
// ---------------------------------------------------------------------

static void init(void) {
  s_current.aqi_pm25 = s_current.aqi_pm10 = s_current.aqi_no2 = s_current.aqi_o3 = -1;
  s_current.aqi_overall = -1;

  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload,
  });

  s_forecast_window = window_create();
  window_set_window_handlers(s_forecast_window, (WindowHandlers) {
    .load = forecast_window_load,
    .unload = forecast_window_unload,
  });

  s_graph_window = window_create();
  window_set_window_handlers(s_graph_window, (WindowHandlers) {
    .load = graph_window_load,
    .unload = graph_window_unload,
  });

  app_message_register_inbox_received(inbox_received_handler);
  app_message_register_inbox_dropped(inbox_dropped_handler);
  app_message_register_outbox_failed(outbox_failed_handler);
  app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum());

  window_stack_push(s_main_window, true);
  request_data();
}

static void deinit(void) {
  window_destroy(s_main_window);
  window_destroy(s_forecast_window);
  window_destroy(s_graph_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
