#include <pebble.h>

#define MAX_ROWS 24
#define GRAPH_POINTS 12
#define ACTIONBAR_ICON_WIDTH 30

// Wind unit: 0 = km/h (default), 1 = mph, 2 = m/s, 3 = knots. Values
// arriving from the phone are already converted to this unit - the watch
// never converts.
static int s_wind_unit = 0;
static const char *WIND_UNIT_SUFFIX[4] = {"km/h", "mph", "m/s", "kts"};

// Model type: 0 = Global (best_match, ~120m altitude), 1 = Benelux (KNMI,
// true 100m altitude). Only affects the "At Xm:" label text.
static int s_model_type = 0;

// Per-unit graph scale: {0, low grid mark, red-warning threshold, axis max}.
// Roughly-equivalent round numbers per unit rather than exact conversions,
// so the axis still reads cleanly regardless of which unit is selected.
static const int GRAPH_MARKS_KMH[4] = {0, 30, 45, 60};
static const int GRAPH_MARKS_MPH[4] = {0, 15, 28, 40};
static const int GRAPH_MARKS_MS[4]  = {0, 7, 13, 20};
static const int GRAPH_MARKS_KTS[4] = {0, 15, 24, 32};

static const int *current_graph_marks(void) {
  if (s_wind_unit == 1) return GRAPH_MARKS_MPH;
  if (s_wind_unit == 2) return GRAPH_MARKS_MS;
  if (s_wind_unit == 3) return GRAPH_MARKS_KTS;
  return GRAPH_MARKS_KMH;
}

static int red_threshold(void) {
  return current_graph_marks()[2];
}

static int graph_y_max(void) {
  return current_graph_marks()[3];
}

typedef struct {
  int hour;
  int wind10;
  int gusts;
  int wind100;
  int dir; // degrees, 0-359, 0 = north
} ForecastRow;

static ForecastRow s_rows[MAX_ROWS];
static int s_row_count = 0;
static char s_location_name[32] = "Locating...";
static char s_status_text[48] = "Fetching forecast...";
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
static Layer *s_location_layer;
static GFont s_location_font;
static int s_location_scroll_offset = 0;
static int s_location_scroll_max = 0;
static bool s_location_scroll_forward = true;
static AppTimer *s_location_scroll_timer;
#define LOCATION_SCROLL_STEP_MS 50
#define LOCATION_SCROLL_PAUSE_MS 800
static TextLayer *s_lbl_10m_layer, *s_val_10m_layer;
static TextLayer *s_lbl_gusts_layer, *s_val_gusts_layer;
static TextLayer *s_lbl_100m_layer, *s_val_100m_layer;
static Layer *s_main_arrow_layer;
static TextLayer *s_main_status_layer;
static char s_val_10m_buf[16], s_val_gusts_buf[16], s_val_100m_buf[16];

// --- Forecast window widgets ---
static MenuLayer *s_menu_layer;
static TextLayer *s_forecast_status_layer;

// --- Graph window widgets ---
static Layer *s_graph_layer;
static TextLayer *s_graph_status_layer;

static GColor color_for_value(int value) {
  return value > red_threshold() ? GColorDarkCandyAppleRed : GColorBlack;
}

static void refresh_main_window(void);
static void update_all_status_displays(void);
static void trigger_location_scroll(void);

static AppTimer *s_timeout_timer;
#define FORECAST_TIMEOUT_MS 25000

static void cancel_timeout_timer(void) {
  if (s_timeout_timer) {
    app_timer_cancel(s_timeout_timer);
    s_timeout_timer = NULL;
  }
}

static void timeout_handler(void *data) {
  s_timeout_timer = NULL;
  if (!s_has_data) {
    strncpy(s_status_text, "Timed out. Press Select to retry.", sizeof(s_status_text) - 1);
    s_status_text[sizeof(s_status_text) - 1] = '\0';
    update_all_status_displays();
  }
}

// ---------------------------------------------------------------------
// Data request / parsing
// ---------------------------------------------------------------------

static void request_forecast(void) {
  s_has_data = false;
  s_row_count = 0;
  strncpy(s_status_text, "Fetching forecast...", sizeof(s_status_text) - 1);
  refresh_main_window();

  cancel_timeout_timer();
  s_timeout_timer = app_timer_register(FORECAST_TIMEOUT_MS, timeout_handler, NULL);

  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK && iter) {
    dict_write_int32(iter, MESSAGE_KEY_REQUEST, 1);
    app_message_outbox_send();
  }
}

static void parse_forecast(const char *data) {
  s_row_count = 0;
  static char buffer[900];
  strncpy(buffer, data, sizeof(buffer) - 1);
  buffer[sizeof(buffer) - 1] = '\0';

  char *cursor = buffer;
  while (cursor && *cursor && s_row_count < MAX_ROWS) {
    char *next_row = strchr(cursor, '|');
    if (next_row) {
      *next_row = '\0';
    }

    char *p = cursor;
    char *hour_s = p;
    char *c1 = strchr(p, ',');
    if (!c1) { cursor = next_row ? next_row + 1 : NULL; continue; }
    *c1 = '\0'; p = c1 + 1;

    char *w10_s = p;
    char *c2 = strchr(p, ',');
    if (!c2) { cursor = next_row ? next_row + 1 : NULL; continue; }
    *c2 = '\0'; p = c2 + 1;

    char *gust_s = p;
    char *c3 = strchr(p, ',');
    if (!c3) { cursor = next_row ? next_row + 1 : NULL; continue; }
    *c3 = '\0'; p = c3 + 1;

    char *w100_s = p;
    char *c4 = strchr(p, ',');
    if (!c4) { cursor = next_row ? next_row + 1 : NULL; continue; }
    *c4 = '\0'; p = c4 + 1;

    char *dir_s = p;

    s_rows[s_row_count].hour    = atoi(hour_s);
    s_rows[s_row_count].wind10  = atoi(w10_s);
    s_rows[s_row_count].gusts   = atoi(gust_s);
    s_rows[s_row_count].wind100 = atoi(w100_s);
    s_rows[s_row_count].dir     = atoi(dir_s);
    s_row_count++;

    cursor = next_row ? next_row + 1 : NULL;
  }
}

static void update_all_status_displays(void) {
  refresh_main_window();

  if (s_has_data) {
    trigger_location_scroll();
  }

  if (s_menu_layer) {
    text_layer_set_text(s_forecast_status_layer, s_has_data ? "" : s_status_text);
    layer_set_hidden(text_layer_get_layer(s_forecast_status_layer), s_has_data);
    layer_set_hidden(menu_layer_get_layer(s_menu_layer), !s_has_data);
    menu_layer_reload_data(s_menu_layer);
  }

  if (s_graph_layer) {
    text_layer_set_text(s_graph_status_layer, s_has_data ? "" : s_status_text);
    layer_set_hidden(text_layer_get_layer(s_graph_status_layer), s_has_data);
    layer_set_hidden(s_graph_layer, !s_has_data);
    layer_mark_dirty(s_graph_layer);
  }
}

static void inbox_received_handler(DictionaryIterator *iter, void *context) {
  cancel_timeout_timer();

  Tuple *err_tuple    = dict_find(iter, MESSAGE_KEY_ERROR);
  Tuple *data_tuple   = dict_find(iter, MESSAGE_KEY_FORECAST_DATA);
  Tuple *loc_tuple    = dict_find(iter, MESSAGE_KEY_LOCATION_NAME);
  Tuple *unit_tuple   = dict_find(iter, MESSAGE_KEY_WIND_UNIT);
  Tuple *model_tuple  = dict_find(iter, MESSAGE_KEY_MODEL_TYPE);

  if (loc_tuple) {
    strncpy(s_location_name, loc_tuple->value->cstring, sizeof(s_location_name) - 1);
    s_location_name[sizeof(s_location_name) - 1] = '\0';
  }

  if (unit_tuple) {
    int u = (int)unit_tuple->value->int32;
    if (u >= 0 && u <= 3) {
      s_wind_unit = u;
    }
  }

  if (model_tuple) {
    int m = (int)model_tuple->value->int32;
    if (m == 0 || m == 1) {
      s_model_type = m;
    }
  }

  if (err_tuple) {
    snprintf(s_status_text, sizeof(s_status_text), "Error: %s", err_tuple->value->cstring);
    s_has_data = false;
  } else if (data_tuple) {
    parse_forecast(data_tuple->value->cstring);
    s_has_data = (s_row_count > 0);
    if (!s_has_data) {
      strncpy(s_status_text, "No data received", sizeof(s_status_text) - 1);
    }
  }

  update_all_status_displays();
}

static void inbox_dropped_handler(AppMessageResult reason, void *context) {
  cancel_timeout_timer();
  strncpy(s_status_text, "Message dropped. Press Select to retry.", sizeof(s_status_text) - 1);
  s_has_data = false;
  update_all_status_displays();
}

static void outbox_failed_handler(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  cancel_timeout_timer();
  strncpy(s_status_text, "Send to phone failed. Press Select to retry.", sizeof(s_status_text) - 1);
  s_has_data = false;
  update_all_status_displays();
}

// ---------------------------------------------------------------------
// Wind direction arrow (shared helper) - 0 degrees = up = north, clockwise
// ---------------------------------------------------------------------

static const GPathInfo ARROW_PATH_SMALL = {
  .num_points = 4,
  .points = (GPoint[]) {{0, -10}, {7, 8}, {0, 3}, {-7, 8}}
};

static const GPathInfo ARROW_PATH_MEDIUM = {
  .num_points = 4,
  .points = (GPoint[]) {{0, -15}, {10, 12}, {0, 4}, {-10, 12}}
};

static void draw_wind_arrow_ex(GContext *ctx, GPoint center, int direction_degrees,
                                const GPathInfo *path_info, GColor color) {
  GPath *path = gpath_create(path_info);
  gpath_move_to(path, center);
  // Vane convention: arrow points into the wind (where it's blowing FROM),
  // which is 180 degrees opposite the raw "blowing toward" bearing.
  int flipped_degrees = (direction_degrees + 180) % 360;
  int32_t angle = (flipped_degrees * TRIG_MAX_ANGLE) / 360;
  gpath_rotate_to(path, angle);
  graphics_context_set_fill_color(ctx, color);
  gpath_draw_filled(ctx, path);
  gpath_destroy(path);
}

static void draw_wind_arrow_small(GContext *ctx, GPoint center, int direction_degrees, GColor color) {
  draw_wind_arrow_ex(ctx, center, direction_degrees, &ARROW_PATH_SMALL, color);
}

static void draw_wind_arrow_medium(GContext *ctx, GPoint center, int direction_degrees, GColor color) {
  draw_wind_arrow_ex(ctx, center, direction_degrees, &ARROW_PATH_MEDIUM, color);
}

// ---------------------------------------------------------------------
// MAIN window
// ---------------------------------------------------------------------

static void main_arrow_update_proc(Layer *layer, GContext *ctx) {
  if (!s_has_data) return;
  GRect bounds = layer_get_bounds(layer);
  GPoint center = GPoint(bounds.size.w / 2, bounds.size.h / 2);
  if (bounds.size.w >= 36) {
    draw_wind_arrow_medium(ctx, center, s_rows[0].dir, GColorBlack);
  } else {
    draw_wind_arrow_small(ctx, center, s_rows[0].dir, GColorBlack);
  }
}

static void location_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_text_color(ctx, GColorBlack);
  // Text is drawn into a much wider box than the layer itself; Pebble clips
  // all drawing to the layer's own bounds automatically, so shifting the
  // box left (negative x) progressively reveals more of the tail end while
  // everything outside stays invisible - this is what makes the scroll work.
  graphics_draw_text(ctx, s_location_name, s_location_font,
                      GRect(-s_location_scroll_offset, 0, bounds.size.w + 400, bounds.size.h),
                      GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

static void location_scroll_timer_callback(void *data) {
  if (s_location_scroll_forward) {
    s_location_scroll_offset += 1;
    if (s_location_scroll_offset >= s_location_scroll_max) {
      s_location_scroll_offset = s_location_scroll_max;
      layer_mark_dirty(s_location_layer);
      s_location_scroll_forward = false; // now just waiting out the pause before snapping back
      s_location_scroll_timer = app_timer_register(LOCATION_SCROLL_PAUSE_MS, location_scroll_timer_callback, NULL);
      return;
    }
    layer_mark_dirty(s_location_layer);
    s_location_scroll_timer = app_timer_register(LOCATION_SCROLL_STEP_MS, location_scroll_timer_callback, NULL);
  } else {
    // Pause is over - snap straight back to the start, no animated return.
    s_location_scroll_offset = 0;
    layer_mark_dirty(s_location_layer);
    s_location_scroll_timer = NULL;
  }
}

// Starts a single scroll-right-then-back cycle revealing the full location
// name once, then resets to the normal (possibly clipped) resting position.
// Callable both automatically after a fresh fetch and independently (e.g.
// from a touch tap) without needing a data refresh.
static void trigger_location_scroll(void) {
  if (!s_location_layer) return;

  GRect bounds = layer_get_bounds(s_location_layer);
  GSize content_size = graphics_text_layout_get_content_size(
      s_location_name, s_location_font, GRect(0, 0, 1000, bounds.size.h + 10),
      GTextOverflowModeFill, GTextAlignmentLeft);
  int overflow = content_size.w - bounds.size.w;
  s_location_scroll_max = overflow > 0 ? overflow : 0;

  if (s_location_scroll_timer) {
    app_timer_cancel(s_location_scroll_timer);
    s_location_scroll_timer = NULL;
  }

  s_location_scroll_offset = 0;
  layer_mark_dirty(s_location_layer);

  if (s_location_scroll_max <= 0) {
    return; // text already fits fully - nothing to scroll
  }

  s_location_scroll_forward = true;
  s_location_scroll_timer = app_timer_register(LOCATION_SCROLL_STEP_MS, location_scroll_timer_callback, NULL);
}

#if defined(PBL_TOUCH)
static void main_touch_handler(const TouchEvent *event, void *context) {
  if (event->type != TouchEvent_Touchdown) return;
  if (!s_location_layer) return;

  GRect frame = layer_get_frame(s_location_layer);
  GPoint p = GPoint(event->x, event->y);
  if (grect_contains_point(&frame, &p)) {
    trigger_location_scroll();
  }
}
#endif

static void main_window_appear(Window *window) {
#if defined(PBL_TOUCH)
  if (touch_service_is_enabled()) {
    touch_service_subscribe(main_touch_handler, NULL);
  }
#endif
}

static void main_window_disappear(Window *window) {
#if defined(PBL_TOUCH)
  touch_service_unsubscribe();
#endif
}

static void refresh_main_window(void) {
  if (!s_location_layer) return; // main window not currently loaded

  layer_mark_dirty(s_location_layer);

  if (!s_has_data) {
    text_layer_set_text(s_main_status_layer, s_status_text);
    layer_set_hidden(text_layer_get_layer(s_main_status_layer), false);
    return;
  }
  layer_set_hidden(text_layer_get_layer(s_main_status_layer), true);

  ForecastRow *now = &s_rows[0];
  const char *unit = WIND_UNIT_SUFFIX[s_wind_unit];
  snprintf(s_val_10m_buf, sizeof(s_val_10m_buf), "%d %s", now->wind10, unit);
  snprintf(s_val_gusts_buf, sizeof(s_val_gusts_buf), "%d %s", now->gusts, unit);
  snprintf(s_val_100m_buf, sizeof(s_val_100m_buf), "%d %s", now->wind100, unit);

  text_layer_set_text(s_val_10m_layer, s_val_10m_buf);
  text_layer_set_text(s_val_gusts_layer, s_val_gusts_buf);
  text_layer_set_text(s_val_100m_layer, s_val_100m_buf);

  text_layer_set_text_color(s_val_10m_layer, color_for_value(now->wind10));
  text_layer_set_text_color(s_val_gusts_layer, color_for_value(now->gusts));
  text_layer_set_text_color(s_val_100m_layer, color_for_value(now->wind100));

  text_layer_set_text(s_lbl_100m_layer, s_model_type == 1 ? "At 100m:" : "At 120m:");

  layer_mark_dirty(s_main_arrow_layer);
}

static void main_up_click(ClickRecognizerRef recognizer, void *context) {
  window_stack_push(s_graph_window, true);
}

static void main_down_click(ClickRecognizerRef recognizer, void *context) {
  window_stack_push(s_forecast_window, true);
}

static void main_select_click(ClickRecognizerRef recognizer, void *context) {
  request_forecast();
}

static void main_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, main_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, main_down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, main_select_click);
}

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  int content_width = bounds.size.w - ACTIONBAR_ICON_WIDTH;
  bool compact = bounds.size.h < 200;

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

  // Fonts stay at (or very close to) their original sizes - the built-in
  // font ladder doesn't offer a clean 10-20% step, so compact screens keep
  // the original size exactly and only the roomier tier gets a small nudge.
  GFont font_location = fonts_get_system_font(compact ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_28_BOLD);
  GFont font_label    = fonts_get_system_font(compact ? FONT_KEY_GOTHIC_14_BOLD : FONT_KEY_GOTHIC_18_BOLD);
  GFont font_value    = fonts_get_system_font(compact ? FONT_KEY_GOTHIC_28_BOLD : FONT_KEY_BITHAM_30_BLACK);

  int loc_h      = compact ? 20 : 34;
  int label_h    = compact ? 16 : 18;
  int value_h    = compact ? 28 : 36;
  int gap        = compact ? 0 : 1;
  int arrow_size = compact ? 28 : 40;

  s_location_font = font_location;
  s_location_layer = layer_create(GRect(4, 0, content_width - 8, loc_h));
  layer_set_update_proc(s_location_layer, location_layer_update_proc);
  layer_add_child(window_layer, s_location_layer);

  int y = loc_h + gap;

  // Direction arrow sits inline on the same row as "At 10m:" rather than
  // overlapping the location name above it - avoids any cutoff.
  int row1_h = arrow_size > label_h ? arrow_size : label_h;
  int label_y_offset = row1_h - label_h; // align label to bottom within the taller row

  s_lbl_10m_layer = text_layer_create(GRect(4, y + label_y_offset, content_width - arrow_size - 12, label_h));
  text_layer_set_font(s_lbl_10m_layer, font_label);
  text_layer_set_text(s_lbl_10m_layer, "At 10m:");
  layer_add_child(window_layer, text_layer_get_layer(s_lbl_10m_layer));

  s_main_arrow_layer = layer_create(GRect(content_width - arrow_size - 4, y, arrow_size, arrow_size));
  layer_set_update_proc(s_main_arrow_layer, main_arrow_update_proc);
  layer_add_child(window_layer, s_main_arrow_layer);
  y += row1_h;

  s_val_10m_layer = text_layer_create(GRect(4, y, content_width - 8, value_h));
  text_layer_set_font(s_val_10m_layer, font_value);
  text_layer_set_text_alignment(s_val_10m_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_val_10m_layer));
  y += value_h + gap;

  s_lbl_gusts_layer = text_layer_create(GRect(4, y, content_width - 8, label_h));
  text_layer_set_font(s_lbl_gusts_layer, font_label);
  text_layer_set_text(s_lbl_gusts_layer, "Gusts:");
  layer_add_child(window_layer, text_layer_get_layer(s_lbl_gusts_layer));
  y += label_h;

  s_val_gusts_layer = text_layer_create(GRect(4, y, content_width - 8, value_h));
  text_layer_set_font(s_val_gusts_layer, font_value);
  text_layer_set_text_alignment(s_val_gusts_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_val_gusts_layer));
  y += value_h + gap;

  s_lbl_100m_layer = text_layer_create(GRect(4, y, content_width - 8, label_h));
  text_layer_set_font(s_lbl_100m_layer, font_label);
  text_layer_set_text(s_lbl_100m_layer, "At 120m:");
  layer_add_child(window_layer, text_layer_get_layer(s_lbl_100m_layer));
  y += label_h;

  s_val_100m_layer = text_layer_create(GRect(4, y, content_width - 8, value_h));
  text_layer_set_font(s_val_100m_layer, font_value);
  text_layer_set_text_alignment(s_val_100m_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_val_100m_layer));

  s_main_status_layer = text_layer_create(GRect(0, bounds.size.h / 2 - 20, content_width, 40));
  text_layer_set_text_alignment(s_main_status_layer, GTextAlignmentCenter);
  text_layer_set_font(s_main_status_layer, fonts_get_system_font(compact ? FONT_KEY_GOTHIC_14 : FONT_KEY_GOTHIC_18));
  text_layer_set_text(s_main_status_layer, s_status_text);
  layer_add_child(window_layer, text_layer_get_layer(s_main_status_layer));

  refresh_main_window();
  trigger_location_scroll();
}

static void main_window_unload(Window *window) {
  if (s_location_scroll_timer) {
    app_timer_cancel(s_location_scroll_timer);
    s_location_scroll_timer = NULL;
  }
#if defined(PBL_TOUCH)
  touch_service_unsubscribe();
#endif

  action_bar_layer_destroy(s_action_bar);
  gbitmap_destroy(s_icon_graph);
  gbitmap_destroy(s_icon_list_forecast);
  gbitmap_destroy(s_icon_refresh);
  layer_destroy(s_location_layer);
  text_layer_destroy(s_lbl_10m_layer);
  text_layer_destroy(s_val_10m_layer);
  text_layer_destroy(s_lbl_gusts_layer);
  text_layer_destroy(s_val_gusts_layer);
  text_layer_destroy(s_lbl_100m_layer);
  text_layer_destroy(s_val_100m_layer);
  text_layer_destroy(s_main_status_layer);
  layer_destroy(s_main_arrow_layer);

  s_location_layer = NULL;
  s_main_arrow_layer = NULL;
  s_main_status_layer = NULL;
}

// ---------------------------------------------------------------------
// FORECAST window (no action bar - Back button returns to main)
// ---------------------------------------------------------------------

static uint16_t forecast_get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return s_row_count;
}

static int16_t forecast_get_row_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  return 26;
}

static void forecast_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  if (cell_index->row >= (uint16_t)s_row_count) return;
  ForecastRow *r = &s_rows[cell_index->row];
  GRect bounds = layer_get_bounds(cell_layer);
  bool compact = bounds.size.w < 180;

  GFont time_font  = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  GFont value_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

  bool highlighted = menu_cell_layer_is_highlighted(cell_layer);
  bool gust_warning = r->gusts > red_threshold();

  GColor bg_color = highlighted ? GColorBlack : (gust_warning ? GColorDarkCandyAppleRed : GColorWhite);
  GColor fg_color = (highlighted || gust_warning) ? GColorWhite : GColorBlack;

  // Explicit background fill - do not rely solely on the framework's
  // automatic highlight background, since we're fully custom-drawing this row.
  graphics_context_set_fill_color(ctx, bg_color);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  char hour_buf[8], w10_buf[6], gust_buf[6], w100_buf[6];
  if (compact) {
    snprintf(hour_buf, sizeof(hour_buf), "%d", r->hour);
  } else {
    snprintf(hour_buf, sizeof(hour_buf), "%02d:00", r->hour);
  }
  snprintf(w10_buf, sizeof(w10_buf), "%d", r->wind10);
  snprintf(gust_buf, sizeof(gust_buf), "%d", r->gusts);
  snprintf(w100_buf, sizeof(w100_buf), "%d", r->wind100);

  graphics_context_set_text_color(ctx, fg_color);

  if (compact) {
    graphics_draw_text(ctx, hour_buf, time_font, GRect(2, 2, 30, bounds.size.h),
                        GTextOverflowModeFill, GTextAlignmentLeft, NULL);
    draw_wind_arrow_small(ctx, GPoint(38, bounds.size.h / 2), r->dir, fg_color);
    graphics_draw_text(ctx, w10_buf, value_font, GRect(50, 2, 30, bounds.size.h),
                        GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, gust_buf, value_font, GRect(82, 2, 30, bounds.size.h),
                        GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, w100_buf, value_font, GRect(114, 2, 28, bounds.size.h),
                        GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  } else {
    graphics_draw_text(ctx, hour_buf, time_font, GRect(4, 3, 50, bounds.size.h),
                        GTextOverflowModeFill, GTextAlignmentLeft, NULL);
    draw_wind_arrow_medium(ctx, GPoint(62, bounds.size.h / 2), r->dir, fg_color);
    graphics_draw_text(ctx, w10_buf, value_font, GRect(78, 3, 40, bounds.size.h),
                        GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, gust_buf, value_font, GRect(120, 3, 40, bounds.size.h),
                        GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, w100_buf, value_font, GRect(162, 3, 34, bounds.size.h),
                        GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  }
}

#if defined(PBL_TOUCH)
static int s_forecast_touch_start_y;
static GPoint s_forecast_touch_start_offset;
static bool s_forecast_touch_dragging = false;

static void forecast_touch_handler(const TouchEvent *event, void *context) {
  if (!s_menu_layer) return;
  ScrollLayer *scroll_layer = menu_layer_get_scroll_layer(s_menu_layer);

  switch (event->type) {
    case TouchEvent_Touchdown:
      s_forecast_touch_start_y = event->y;
      s_forecast_touch_start_offset = scroll_layer_get_content_offset(scroll_layer);
      s_forecast_touch_dragging = true;
      break;
    case TouchEvent_PositionUpdate:
      if (s_forecast_touch_dragging) {
        int delta = event->y - s_forecast_touch_start_y;
        GPoint new_offset = GPoint(s_forecast_touch_start_offset.x, s_forecast_touch_start_offset.y + delta);
        scroll_layer_set_content_offset(scroll_layer, new_offset, false);
      }
      break;
    case TouchEvent_Liftoff:
      s_forecast_touch_dragging = false;
      break;
  }
}
#endif

static void forecast_window_appear(Window *window) {
#if defined(PBL_TOUCH)
  if (touch_service_is_enabled()) {
    touch_service_subscribe(forecast_touch_handler, NULL);
  }
#endif
}

static void forecast_window_disappear(Window *window) {
#if defined(PBL_TOUCH)
  touch_service_unsubscribe();
#endif
}

static void forecast_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  bool compact = bounds.size.w < 180;

  s_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_rows = forecast_get_num_rows,
    .get_cell_height = forecast_get_row_height,
    .draw_row = forecast_draw_row,
  });
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  menu_layer_set_highlight_colors(s_menu_layer, GColorBlack, GColorWhite);
  layer_add_child(window_layer, menu_layer_get_layer(s_menu_layer));
  layer_set_hidden(menu_layer_get_layer(s_menu_layer), !s_has_data);

  s_forecast_status_layer = text_layer_create(GRect(0, bounds.size.h / 2 - 20, bounds.size.w, 40));
  text_layer_set_text_alignment(s_forecast_status_layer, GTextAlignmentCenter);
  text_layer_set_font(s_forecast_status_layer, fonts_get_system_font(compact ? FONT_KEY_GOTHIC_14 : FONT_KEY_GOTHIC_18));
  text_layer_set_text(s_forecast_status_layer, s_has_data ? "" : s_status_text);
  layer_set_hidden(text_layer_get_layer(s_forecast_status_layer), s_has_data);
  layer_add_child(window_layer, text_layer_get_layer(s_forecast_status_layer));
}

static void forecast_window_unload(Window *window) {
#if defined(PBL_TOUCH)
  touch_service_unsubscribe();
#endif
  menu_layer_destroy(s_menu_layer);
  text_layer_destroy(s_forecast_status_layer);
  s_menu_layer = NULL;
  s_forecast_status_layer = NULL;
}

// ---------------------------------------------------------------------
// GRAPH window (no action bar - Back button returns to main)
// ---------------------------------------------------------------------

// Draws a straight line as a series of short dashes rather than solid -
// integer-only math (no sqrt/float), splits the segment into num_dashes
// evenly-spaced dash/gap pairs regardless of segment length.
static void draw_dashed_line(GContext *ctx, GPoint p1, GPoint p2, int num_dashes) {
  for (int i = 0; i < num_dashes; i++) {
    int x1 = p1.x + (p2.x - p1.x) * i / num_dashes;
    int y1 = p1.y + (p2.y - p1.y) * i / num_dashes;
    int x2 = p1.x + (p2.x - p1.x) * (i * 2 + 1) / (num_dashes * 2);
    int y2 = p1.y + (p2.y - p1.y) * (i * 2 + 1) / (num_dashes * 2);
    graphics_draw_line(ctx, GPoint(x1, y1), GPoint(x2, y2));
  }
}

// Draws a line as small filled dots along its length - visually distinct
// from both the solid and dashed styles, for the 100m series.
static void draw_dotted_line(GContext *ctx, GPoint p1, GPoint p2, int num_dots) {
  for (int i = 0; i <= num_dots; i++) {
    int x = p1.x + (p2.x - p1.x) * i / num_dots;
    int y = p1.y + (p2.y - p1.y) * i / num_dots;
    graphics_fill_circle(ctx, GPoint(x, y), 1);
  }
}

static void graph_update_proc(Layer *layer, GContext *ctx) {
  if (!s_has_data) return;
  GRect bounds = layer_get_bounds(layer);
  bool compact = bounds.size.w < 180;

  int margin_left = compact ? 22 : 30;
  int margin_top = 6;
  int hour_label_h = compact ? 14 : 18;
  int legend_h = compact ? 18 : 24;
  int margin_bottom = hour_label_h + legend_h;

  int plot_w = bounds.size.w - margin_left - 6;
  int plot_h = bounds.size.h - margin_bottom - margin_top;
  int origin_x = margin_left;
  int origin_y = margin_top + plot_h;

  GFont label_font = fonts_get_system_font(compact ? FONT_KEY_GOTHIC_09 : FONT_KEY_GOTHIC_14_BOLD);
  GFont legend_font = fonts_get_system_font(compact ? FONT_KEY_GOTHIC_14_BOLD : FONT_KEY_GOTHIC_18_BOLD);
  int label_half_h = compact ? 7 : 10;
  int hour_half_w = compact ? 12 : 18;
  int hour_w = compact ? 24 : 36;
  // With 12 points on the time axis, only every Nth hour gets a label so
  // they don't collide - narrower (compact) screens skip more aggressively.
  int hour_label_step = compact ? 3 : 2;

  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_line(ctx, GPoint(origin_x, margin_top), GPoint(origin_x, origin_y));
  graphics_draw_line(ctx, GPoint(origin_x, origin_y), GPoint(origin_x + plot_w, origin_y));

  const int *marks = current_graph_marks();
  int y_max = graph_y_max();
  for (int i = 0; i < 4; i++) {
    int y = origin_y - (marks[i] * plot_h) / y_max;
    GColor c = (marks[i] == red_threshold()) ? GColorDarkCandyAppleRed : GColorLightGray;
    graphics_context_set_stroke_color(ctx, c);
    graphics_draw_line(ctx, GPoint(origin_x, y), GPoint(origin_x + plot_w, y));

    char buf[4];
    snprintf(buf, sizeof(buf), "%d", marks[i]);
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, buf, label_font,
                        GRect(0, y - label_half_h, margin_left - 4, hour_label_h),
                        GTextOverflowModeFill, GTextAlignmentRight, NULL);
  }

  int points = s_row_count < GRAPH_POINTS ? s_row_count : GRAPH_POINTS;
  if (points >= 2) {
    GPoint prev10 = GPoint(0, 0);
    GPoint prev_gust = GPoint(0, 0);
    GPoint prev_100 = GPoint(0, 0);

    for (int i = 0; i < points; i++) {
      int x = origin_x + (i * plot_w) / (points - 1);

      int val10 = s_rows[i].wind10;
      if (val10 > y_max) val10 = y_max;
      GPoint p10 = GPoint(x, origin_y - (val10 * plot_h) / y_max);

      int valg = s_rows[i].gusts;
      if (valg > y_max) valg = y_max;
      GPoint pgust = GPoint(x, origin_y - (valg * plot_h) / y_max);

      int val100 = s_rows[i].wind100;
      if (val100 > y_max) val100 = y_max;
      GPoint p100 = GPoint(x, origin_y - (val100 * plot_h) / y_max);

      if (i > 0) {
        graphics_context_set_stroke_color(ctx, GColorBlack);
        graphics_context_set_stroke_width(ctx, 2);
        graphics_draw_line(ctx, prev10, p10);

        graphics_context_set_stroke_color(ctx, GColorDarkCandyAppleRed);
        graphics_context_set_stroke_width(ctx, 3);
        draw_dashed_line(ctx, prev_gust, pgust, 2);

        graphics_context_set_fill_color(ctx, GColorBlack);
        draw_dotted_line(ctx, prev_100, p100, 5);
      }

      graphics_context_set_fill_color(ctx, color_for_value(s_rows[i].wind10));
      graphics_fill_circle(ctx, p10, 3);

      if (i % hour_label_step == 0 || i == points - 1) {
        char hbuf[4];
        snprintf(hbuf, sizeof(hbuf), "%02d", s_rows[i].hour);
        graphics_context_set_text_color(ctx, GColorBlack);
        graphics_draw_text(ctx, hbuf, label_font,
                            GRect(x - hour_half_w, origin_y + 4, hour_w, hour_label_h),
                            GTextOverflowModeFill, GTextAlignmentCenter, NULL);
      }

      prev10 = p10;
      prev_gust = pgust;
      prev_100 = p100;
    }
  }

  // Legend line at the very bottom explaining which line style is which.
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, "10m—— Gusts--- 120m……", legend_font,
                      GRect(0, bounds.size.h - legend_h, bounds.size.w, legend_h),
                      GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

static void graph_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  bool compact = bounds.size.w < 180;

  s_graph_layer = layer_create(bounds);
  layer_set_update_proc(s_graph_layer, graph_update_proc);
  layer_set_hidden(s_graph_layer, !s_has_data);
  layer_add_child(window_layer, s_graph_layer);

  s_graph_status_layer = text_layer_create(GRect(0, bounds.size.h / 2 - 20, bounds.size.w, 40));
  text_layer_set_text_alignment(s_graph_status_layer, GTextAlignmentCenter);
  text_layer_set_font(s_graph_status_layer, fonts_get_system_font(compact ? FONT_KEY_GOTHIC_14 : FONT_KEY_GOTHIC_18));
  text_layer_set_text(s_graph_status_layer, s_has_data ? "" : s_status_text);
  layer_set_hidden(text_layer_get_layer(s_graph_status_layer), s_has_data);
  layer_add_child(window_layer, text_layer_get_layer(s_graph_status_layer));
}

static void graph_window_unload(Window *window) {
  layer_destroy(s_graph_layer);
  text_layer_destroy(s_graph_status_layer);
  s_graph_layer = NULL;
  s_graph_status_layer = NULL;
}

// ---------------------------------------------------------------------
// App init
// ---------------------------------------------------------------------

static void init(void) {
  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload,
    .appear = main_window_appear,
    .disappear = main_window_disappear,
  });

  s_forecast_window = window_create();
  window_set_window_handlers(s_forecast_window, (WindowHandlers) {
    .load = forecast_window_load,
    .unload = forecast_window_unload,
    .appear = forecast_window_appear,
    .disappear = forecast_window_disappear,
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
  request_forecast();
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