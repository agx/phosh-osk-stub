/*
 * Copyright (C) 2022 Guido Günther <agx@sigxcpu.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "pos-osk-widget"

#include "config.h"

#include "util.h"
#include "pos-char-popup.h"
#include "pos-osk-key.h"
#include "pos-osk-widget.h"

#include <json-glib/json-glib.h>
#include <pango/pangocairo.h>

#include <math.h>

#define KEY_HEIGHT 50
#define KEY_ICON_SIZE 16

/* Default us layout */
#define LAYOUT_COLS 10
#define LAYOUT_ROWS 4

/**
 * SECTION:osk-widget
 * @short_description: An osk widget
 * @Title: PosOskWidget
 *
 * Renders the keyboard and reacts to keypresses by signal emissions.
 */
enum {
  OSK_KEY_DOWN,
  OSK_KEY_UP,
  OSK_KEY_CANCELLED,
  OSK_KEY_SYMBOL,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

enum {
  PROP_0,
  PROP_LAYER,
  PROP_NAME,
  PROP_LAST_PROP,
};
static GParamSpec *props[PROP_LAST_PROP];

/**
 * PosOskWidgetRow:
 * @width: number in key units
 * @offset_x: offset from the right in key units
 *
 * A key row on a #PosOskWidgetKeyboardLayer of the #PosOskWidget.
 * Renders the keys and reacts to touch and pointer events. Much
 * of the logic can go if we use GtkWidgets for the keys itself.
 */
typedef struct {
  GPtrArray *keys;
  double     width;
  double     offset_x;
} PosOskWidgetRow;

/**
 * PosOskWidgetKeyboardLayer:
 * @width: The maximum width in key units
 * @offset_x: Offset of this layer from the left side in pixels
 * @key_width: Key width in pixels of a 1 unit wide key
 * @key_height: key height in pixels of a 1 unit high key
 *
 * Describes the character layout of one layer of keys.
 */
typedef struct {
  PosOskWidgetRow rows[LAYOUT_ROWS];
  double          width;

  int             offset_x;
  double          key_width;
  double          key_height;
} PosOskWidgetKeyboardLayer;

/**
 * PosOskWidgetLayout
 *
 * Information about a keyboard layout. The keys
 * are grouped in different layers that are displayed
 * depending on modifier state.
 */
typedef struct {
  char                     *name;
  PosOskWidgetKeyboardLayer layers[4];
  guint                     n_layers;
  guint                     n_cols;
  guint                     n_rows;
  double                    width;
} PosOskWidgetLayout;


struct _PosOskWidget {
  GtkDrawingArea       parent;

  int                  width, height;
  PosOskWidgetLayout   layout;

  GtkStyleContext     *key_context;
  PosOskWidgetLayer    layer;
  PosOskWidgetMode     mode;

  char                *name;
  PosOskKey           *current;
  GtkGestureLongPress *long_press;
  GtkWidget           *char_popup;

  /* Cursor movement */
  GtkGesture          *cursor_drag;
  double               last_x, last_y;
};
G_DEFINE_TYPE (PosOskWidget, pos_osk_widget, GTK_TYPE_DRAWING_AREA)


static void
on_drag_begin (PosOskWidget *self,
               double        start_x,
               double        start_y)
{
  if (self->mode != POS_OSK_WIDGET_MODE_CURSOR)
    return;
}

#define KEY_DIST_X 5
#define KEY_DIST_Y 10

static void
on_drag_update (PosOskWidget *self,
                double        off_x,
                double        off_y)
{
  const char *symbol = NULL;
  double delta_x, delta_y;

  if (self->mode != POS_OSK_WIDGET_MODE_CURSOR)
    return;

  g_debug ("%s: %f, %f", __func__, off_x, off_y);

  delta_x = self->last_x - off_x;
  delta_y = self->last_y - off_y;

  if (ABS (delta_x) > KEY_DIST_X) {
    symbol =  delta_x > 0 ? OSK_SYMBOL_LEFT : OSK_SYMBOL_RIGHT;
    self->last_x = off_x;
  } else if (ABS (delta_y) > KEY_DIST_Y) {
    symbol =  delta_y > 0 ? OSK_SYMBOL_UP : OSK_SYMBOL_DOWN;
    self->last_y = off_y;
  }

  if (symbol)
    g_signal_emit (self, signals[OSK_KEY_SYMBOL], 0, symbol);
}


static void
on_drag_end (PosOskWidget *self)
{
  if (self->mode != POS_OSK_WIDGET_MODE_CURSOR)
    return;

  pos_osk_widget_set_mode (self, POS_OSK_WIDGET_MODE_KEYBOARD);
}


static void
on_drag_cancel (PosOskWidget *self)
{
  if (self->mode != POS_OSK_WIDGET_MODE_CURSOR)
    return;

  pos_osk_widget_set_mode (self, POS_OSK_WIDGET_MODE_KEYBOARD);
}


static PosOskWidgetKeyboardLayer *
pos_osk_widget_get_layer (PosOskWidget *self, PosOskWidgetLayer layer)
{
  g_return_val_if_fail (layer <= POS_OSK_WIDGET_LAST_LAYER,
                        &self->layout.layers[POS_OSK_WIDGET_LAYER_NORMAL]);

  return &self->layout.layers[layer];
}


static PosOskWidgetKeyboardLayer *
pos_osk_widget_get_current_layer (PosOskWidget *self)
{
  return &self->layout.layers[self->layer];
}


static PosOskWidgetRow *
pos_osk_widget_get_layer_row (PosOskWidget *self, PosOskWidgetLayer layer, guint row)
{
  PosOskWidgetKeyboardLayer *l;

  g_return_val_if_fail (row < self->layout.n_rows, 0);

  l = pos_osk_widget_get_layer (self, layer);
  return &l->rows[row];
}


static PosOskWidgetRow *
pos_osk_widget_get_row (PosOskWidget *self, guint row)
{
  PosOskWidgetKeyboardLayer *l;

  l = pos_osk_widget_get_current_layer (self);
  return &l->rows[row];
}


static guint
pos_osk_widget_row_get_num_keys (PosOskWidgetRow *row)
{
  return row->keys->len;
}


static PosOskKey *
pos_osk_widget_row_get_key (PosOskWidgetRow *row, guint n)
{
  g_return_val_if_fail (n < pos_osk_widget_row_get_num_keys (row), NULL);

  return g_ptr_array_index (row->keys, n);
}


static void
pos_osk_widget_layout_free (PosOskWidgetLayout *layout)
{
  g_clear_pointer (&layout->name, g_free);

  for (int l = 0; l < layout->n_layers; l++) {
    for (int r = 0; r < layout->n_rows; r++) {
      g_ptr_array_free (layout->layers[l].rows[r].keys, TRUE);
      layout->layers[l].rows[r].keys = NULL;
    }
  }
}


static PosOskKey *
get_common_key_post (PosOskWidgetLayer l, gint row)
{
  PosOskKey *key;

  /* TODO: we could create these only once and g_object_ref them */
  switch (row) {
  case 2:
    key = g_object_new (POS_TYPE_OSK_KEY,
                        "symbol", "KEY_BACKSPACE",
                        "icon", "edit-clear-symbolic",
                        "width", 1.5,
                        NULL);
    break;
  case 3:
    key = g_object_new (POS_TYPE_OSK_KEY,
                        "symbol", "KEY_ENTER",
                        "icon", "keyboard-enter-symbolic",
                        "width", 1.5,
                        "style", "return",
                        NULL);
    break;
  case 0:
  case 1:
  default:
    key = NULL;
    break;
  }

  return key;
}


static PosOskKey *
get_common_key_pre (PosOskWidgetLayer layer, gint row)
{
  PosOskKey *key;
  PosOskWidgetLayer l;
  const char *icon;

  /* TODO: we could create these only once and g_object_ref them */
  switch (row) {
  case 2:
    l = POS_OSK_WIDGET_LAYER_CAPS;
    icon = "keyboard-shift-filled-symbolic";
    key = g_object_new (POS_TYPE_OSK_KEY,
                        "use", POS_OSK_KEY_USE_TOGGLE,
                        "icon", icon,
                        "width", 1.5,
                        "style", "toggle",
                        "layer", l,
                        NULL);
    break;
  case 3:
    key = g_object_new (POS_TYPE_OSK_KEY,
                        "label", "?123",
                        "use", POS_OSK_KEY_USE_TOGGLE,
                        "width", 1.5,
                        "layer", POS_OSK_WIDGET_LAYER_SYMBOLS,
                        "style", "toggle",
                        NULL);
    break;
  case 0:
  case 1:
  default:
    key = NULL;
    break;
  }

  return key;
}


static PosOskKey *
get_key (PosOskWidget *self, const char *symbol, GStrv symbols, guint num_keys)
{
  if (g_strcmp0 (symbol, " ") == 0) {
    double width;

    width = (num_keys == 5) ? 3.0 : 5.0;
    return g_object_new (POS_TYPE_OSK_KEY,
                         "label", self->name,
                         "symbol", symbol,
                         "symbols", symbols,
                         "width", width,
                         NULL);
  }
  return g_object_new (POS_TYPE_OSK_KEY,
                       "symbol", symbol,
                       "symbols", symbols,
                       NULL);
}


static GStrv
parse_symbols (JsonArray *array)
{
  g_autoptr (GPtrArray) syms_array = g_ptr_array_new_with_free_func (g_free);

  if (json_array_get_length (array) == 1)
    return NULL;

  for (int i = 1; i < json_array_get_length (array); i++) {
    char *sym = g_strdup (json_array_get_string_element (array, i));

    g_ptr_array_add (syms_array, sym);
  }
  g_ptr_array_add (syms_array, NULL);

  return (GStrv) g_ptr_array_steal (syms_array, NULL);
}


static void
parse_row (PosOskWidget *self, PosOskWidgetRow *row, JsonArray *arow, PosOskWidgetLayer l, guint r)
{
  gsize num_keys;
  PosOskKey *common_key;
  double width = 0.0;

  num_keys = json_array_get_length (arow);
  row->keys = g_ptr_array_sized_new (num_keys + 2);
  g_ptr_array_set_free_func (row->keys, g_object_unref);

  for (int i = 0; i < num_keys; i++) {
    JsonArray *all_symbols = json_array_get_array_element (arow, i);
    const gchar *symbol = json_array_get_string_element (all_symbols, 0);
    g_autoptr (PosOskKey) key = NULL;
    g_auto (GStrv) symbols = NULL;

    symbols = parse_symbols (all_symbols);
    key = get_key (self, symbol, symbols, num_keys);

    width += pos_osk_key_get_width (key);
    g_ptr_array_insert (row->keys, -1, g_steal_pointer (&key));
  }

  common_key = get_common_key_pre (l, r);
  if (common_key) {
    width += pos_osk_key_get_width (common_key);
    g_ptr_array_insert (row->keys, 0, g_steal_pointer (&common_key));
  }

  common_key = get_common_key_post (l, r);
  if (common_key) {
    width += pos_osk_key_get_width (common_key);
    g_ptr_array_insert (row->keys, -1, g_steal_pointer (&common_key));
  }

  row->width = width;
}



static gboolean
parse_rows (PosOskWidget *self, PosOskWidgetKeyboardLayer *layer, JsonArray *rows, PosOskWidgetLayer l)
{
  gsize num_rows;
  gboolean ret = FALSE;
  gdouble max_width = 0.0;

  num_rows = json_array_get_length (rows);
  for (int r = 0; r < num_rows; r++) {
    PosOskWidgetRow *row;
    JsonArray *arow;

    row = pos_osk_widget_get_layer_row (self, l, r);
    arow = json_array_get_array_element (rows, r);
    if (arow == NULL) {
      g_warning ("Failed to get row %d", r);
      ret = FALSE;
      continue;
    }
    parse_row (self, row, arow, l, r);

    max_width = MAX (row->width, max_width);
  }
  layer->width = max_width;

  /* We now the max width, now we can calc offsets */
  for (int r = 0; r < num_rows; r++) {
    PosOskWidgetRow *row = pos_osk_widget_get_layer_row (self, l, r);

    row->offset_x = 0.5 * (layer->width - row->width);
  }

  return ret;
}


static gboolean
parse_layers (PosOskWidget *self, JsonArray *layers)
{
  gsize len;
  JsonArray *rows;
  gboolean ret = FALSE;
  double width = 0.0;

  /* TODO: make dynamic */
  self->layout.n_rows = LAYOUT_ROWS;

  len = json_array_get_length (layers);
  for (int l = 0; l < len; l++) {
    PosOskWidgetKeyboardLayer *layer;
    JsonObject *alayer;

    layer = pos_osk_widget_get_layer (self, l);
    if (l > POS_OSK_WIDGET_LAST_LAYER) {
      g_warning ("Skipping layer %d", l);
      continue;
    }

    /* TODO: honor layer name to support 3 layer layouts better */
    alayer = json_array_get_object_element (layers, l);
    if (alayer == NULL) {
      g_warning ("Failed to get layer %d", l);
      ret = FALSE;
      continue;
    }

    rows = json_object_get_array_member (alayer, "rows");
    if (rows == NULL) {
      g_warning ("Failed to get rows for layer %d", l);
      ret = FALSE;
      continue;
    }

    parse_rows (self, layer, rows, l);
    width = MAX (layer->width, width);
  }

  self->layout.n_layers = len;
  self->layout.n_cols = ceil (width);

  g_debug ("Using %ux%u layout, %d layers", self->layout.n_cols, self->layout.n_rows, self->layout.n_layers);

  return ret;
}


static gboolean
parse_layout (PosOskWidget *self, const char *json, gsize size)
{
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (GError) err = NULL;
  const char *name;
  JsonNode *keyboard_node;
  JsonObject *keyboard;
  JsonArray *levels;

  parser = json_parser_new ();
  json_parser_load_from_data (parser, json, size, &err);

  keyboard_node = json_parser_get_root (parser);

  if (JSON_NODE_TYPE (keyboard_node) != JSON_NODE_OBJECT) {
    g_critical ("Failed to parse layout, root node not an object");
    return FALSE;
  }
  keyboard = json_node_get_object (keyboard_node);

  name = json_object_get_string_member (keyboard, "name");
  if (name == NULL) {
    g_critical ("Failed to parse layout without name");
    return FALSE;
  }

  self->layout.name = g_strdup (name);
  levels = json_object_get_array_member (keyboard, "levels");
  if (levels == NULL) {
    g_critical ("Failed to parse layout, malformed levels");
    return FALSE;
  }
  parse_layers (self, levels);

  return TRUE;
}


static void
pos_osk_widget_get_property (GObject    *object,
                             guint       property_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  PosOskWidget *self = POS_OSK_WIDGET (object);

  switch (property_id) {
  case PROP_LAYER:
    g_value_set_enum (value, self->layer);
    break;
  case PROP_NAME:
    g_value_set_string (value, self->layout.name);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static PosOskWidgetLayer
select_symbols2 (PosOskWidget *self, PosOskKey *key)
{
  /* Only shift key can toggle symbols2 */
  if (pos_osk_key_get_layer (key) != POS_OSK_WIDGET_LAYER_CAPS)
    return self->layer;

  if (pos_osk_widget_get_level (self) == POS_OSK_WIDGET_LAYER_SYMBOLS)
    return POS_OSK_WIDGET_LAYER_SYMBOLS2;

  if (pos_osk_widget_get_level (self) == POS_OSK_WIDGET_LAYER_SYMBOLS2)
    return POS_OSK_WIDGET_LAYER_SYMBOLS;

  return self->layer;
}


static void
pos_osk_widget_set_key_pressed (PosOskWidget *self, PosOskKey *key, gboolean pressed)
{
  const GdkRectangle *box;

  pos_osk_key_set_pressed (key, pressed);
  box = pos_osk_key_get_box (key);
  gtk_widget_queue_draw_area (GTK_WIDGET (self), box->x, box->y, box->width, box->height);
}


static void
switch_layer (PosOskWidget *self, PosOskKey *key)
{
  PosOskWidgetLayer old_layer = self->layer;
  PosOskWidgetLayer layer = pos_osk_key_get_layer (key);

  if (pos_osk_key_get_use (key) == POS_OSK_KEY_USE_TOGGLE) {
    self->layer = select_symbols2 (self, key);
    if (self->layer == old_layer) {
      switch (layer) {
      case POS_OSK_WIDGET_LAYER_CAPS:
      case POS_OSK_WIDGET_LAYER_SYMBOLS:
        if (self->layer == layer)
          self->layer = POS_OSK_WIDGET_LAYER_NORMAL;
        else
          self->layer = layer;
        break;
      case POS_OSK_WIDGET_LAYER_NORMAL:
      case POS_OSK_WIDGET_LAYER_SYMBOLS2:
      default:
        g_return_if_reached ();
        break;
      }
    }
    /* Reset caps layer on every (non toggle) key press */
  } else if (self->layer == POS_OSK_WIDGET_LAYER_CAPS) {
    self->layer = POS_OSK_WIDGET_LAYER_NORMAL;
  }

  if (self->layer >= self->layout.n_layers) {
    g_warning ("Inexistent layer %d", self->layer);
    self->layer = POS_OSK_WIDGET_LAYER_NORMAL;
  }

  if (old_layer != self->layer) {
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_LAYER]);
    gtk_widget_queue_draw (GTK_WIDGET (self));
  }

  /* Update key state for rendering */
  for (int r = 0; r < self->layout.n_rows; r++) {
    PosOskWidgetRow *row = pos_osk_widget_get_row (self, r);

    for (int k = 0; k < pos_osk_widget_row_get_num_keys (row); k++) {
      PosOskKey *akey = g_ptr_array_index (row->keys, k);
      gboolean pressed;

      if (pos_osk_key_get_use (akey) != POS_OSK_KEY_USE_TOGGLE)
        continue;

      pressed = (self->layer == pos_osk_key_get_layer (akey)) ||
                (pos_osk_widget_get_level (self) == POS_OSK_WIDGET_LAYER_SYMBOLS2);

      pos_osk_widget_set_key_pressed (self, akey, pressed);
    }
  }
}


static PosOskKey *
pos_osk_widget_locate_key (PosOskWidget *self, double x, double y)
{
  int row_num;
  PosOskWidgetRow *row;
  PosOskKey *key = NULL;
  double pos_x;
  PosOskWidgetKeyboardLayer *layer = pos_osk_widget_get_current_layer (self);

  pos_x = x - layer->offset_x;

  row_num = (int)(y / layer->key_height);
  g_return_val_if_fail (row_num < self->layout.n_rows, NULL);

  row = pos_osk_widget_get_row (self, row_num);
  pos_x -= row->offset_x * layer->key_width;
  for (int k = 0; k < pos_osk_widget_row_get_num_keys (row); k++) {
    key = pos_osk_widget_row_get_key (row, k);

    pos_x -= pos_osk_key_get_width (key) * layer->key_width;
    if (pos_x <= 0)
      break;
  }

  g_return_val_if_fail (key != NULL, NULL);

  return key;
}


static gboolean
pos_osk_widget_button_press_event (GtkWidget *widget, GdkEventButton *event)
{
  PosOskWidget *self = POS_OSK_WIDGET (widget);
  PosOskKey *key = NULL;

  g_debug ("Button press: %f, %f, button: %d, state: %d",
           event->x, event->y, event->button, event->state);


  key = pos_osk_widget_locate_key (self, event->x, event->y);
  g_return_val_if_fail (key != NULL, GDK_EVENT_PROPAGATE);

  if (self->current) {
    g_warning ("Got button press event for %s while another key %s is pressed",
               POS_OSK_KEY_DBG (key), POS_OSK_KEY_DBG (self->current));
  }
  self->current = key;
  pos_osk_widget_set_key_pressed (self, key, TRUE);

  g_signal_emit (self, signals[OSK_KEY_DOWN], 0, pos_osk_key_get_symbol (key));

  return GDK_EVENT_STOP;
}


static gboolean
pos_osk_widget_button_release_event (GtkWidget *widget, GdkEventButton *event)
{
  PosOskWidget *self = POS_OSK_WIDGET (widget);
  PosOskKey *key = NULL;

  g_debug ("Button release: %f, %f, button: %d, state: %d",
           event->x, event->y, event->button, event->state);

  pos_osk_widget_set_mode (self, POS_OSK_WIDGET_MODE_KEYBOARD);

  if (event->button != 1)
    return GDK_EVENT_PROPAGATE;

  /* Already cancelled */
  if (self->current == NULL)
    return GDK_EVENT_PROPAGATE;

  key = pos_osk_widget_locate_key (self, event->x, event->y);
  g_return_val_if_fail (key != NULL, GDK_EVENT_PROPAGATE);

  switch (pos_osk_key_get_use (key)) {
  case POS_OSK_KEY_USE_TOGGLE:
    switch_layer (self, key);
    break;

  case POS_OSK_KEY_USE_KEY:
    pos_osk_widget_set_key_pressed (self, self->current, FALSE);
    g_signal_emit (self, signals[OSK_KEY_UP], 0, pos_osk_key_get_symbol (key));
    g_signal_emit (self, signals[OSK_KEY_SYMBOL], 0, pos_osk_key_get_symbol (key));
    switch_layer (self, key);
    break;
  default:
    g_assert_not_reached ();
  }

  self->current = NULL;
  return GDK_EVENT_STOP;
}


static void
pos_osk_widget_cancel_press (PosOskWidget *self)
{
  if (self->current == NULL)
    return;

  pos_osk_widget_set_key_pressed (self, self->current, FALSE);
  g_signal_emit (self, signals[OSK_KEY_CANCELLED], 0, pos_osk_key_get_symbol (self->current));
  self->current = NULL;
}


static gboolean
pos_osk_widget_motion_notify_event (GtkWidget *widget, GdkEventMotion *event)
{
  PosOskWidget *self = POS_OSK_WIDGET (widget);
  PosOskKey *key;

  if ((event->state & GDK_BUTTON1_MASK) == 0)
    return GDK_EVENT_PROPAGATE;

  key = pos_osk_widget_locate_key (self, event->x, event->y);
  if (self->current && key != self->current) {
    g_debug ("Crossed key boundary, cancelling");
    pos_osk_widget_cancel_press (self);
  }

  return GDK_EVENT_PROPAGATE;
}


static void
on_symbol_selected (PosOskWidget *self, const char *symbol)
{
  g_debug ("Selected '%s' from popover", symbol);

  g_signal_emit (self, signals[OSK_KEY_DOWN], 0, symbol);
  g_signal_emit (self, signals[OSK_KEY_SYMBOL], 0, symbol);
  g_clear_pointer (&self->char_popup, phosh_cp_widget_destroy);
}


static void
get_popup_pos (PosOskKey *key, double x, double y, GdkRectangle *out)
{
  const GdkRectangle *box = pos_osk_key_get_box (key);

  out->x = box->x + (0.5 * box->width);
  out->y = box->y + (0.5 * box->height);
}


static void
on_long_pressed (GtkGestureLongPress *gesture, double x, double y, gpointer user_data)
{
  PosOskWidget *self = POS_OSK_WIDGET (user_data);
  PosOskKey *key = pos_osk_widget_locate_key (self, x, y);
  GStrv symbols = NULL;
  GdkRectangle rect = { 0 };

  g_debug ("Long press '%s'", pos_osk_key_get_label (key) ?: pos_osk_key_get_symbol (key));

  if (g_strcmp0 (pos_osk_key_get_symbol (key), OSK_SYMBOL_SPACE) == 0) {
    pos_osk_widget_set_mode (self, POS_OSK_WIDGET_MODE_CURSOR);
    return;
  }

  symbols = pos_osk_key_get_symbols (key);
  if (symbols == NULL || symbols[0] == NULL)
    return;

  pos_osk_widget_cancel_press (self);
  g_clear_pointer (&self->char_popup, phosh_cp_widget_destroy);
  self->char_popup = GTK_WIDGET (pos_char_popup_new (GTK_WIDGET (self), symbols));

  get_popup_pos (key, x, y, &rect);
  gtk_popover_set_pointing_to (GTK_POPOVER (self->char_popup), &rect);

  g_signal_connect_swapped (self->char_popup, "selected", G_CALLBACK (on_symbol_selected), self);
  gtk_popover_popup (GTK_POPOVER (self->char_popup));
}


static void
render_outline (cairo_t *cr, GtkStyleContext *context, const GdkRectangle *box)
{
  GtkBorder margin, border;
  double x, y, width, height;

  gtk_style_context_get_margin (context, GTK_STATE_FLAG_NORMAL, &margin);
  gtk_style_context_get_border (context, GTK_STATE_FLAG_NORMAL, &border);

  x = margin.left + border.left;
  y = margin.top + border.top;
  width = box->width - x - margin.right - border.right;
  height = box->height - y - margin.bottom - border.bottom;

  gtk_render_background (context, cr, x, y, width, height);
  gtk_render_frame (context, cr, x, y, width, height);
}


#if !PANGO_VERSION_CHECK (1, 50, 0)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PangoLayout, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PangoFontDescription, pango_font_description_free)
#endif


static void
render_label (cairo_t *cr, GtkStyleContext *context, const char *label, const GdkRectangle *box)
{
  g_autoptr (PangoLayout) layout = pango_cairo_create_layout (cr);
  g_autoptr (PangoFontDescription) font = NULL;
  PangoLayoutLine *line = NULL;
  PangoRectangle extents = { 0, };
  GdkRGBA color = {0};
  GtkStateFlags state;

  cairo_save (cr);

  state = gtk_style_context_get_state (context);
  gtk_style_context_get (context, state, "font", &font, NULL);
  pango_layout_set_font_description (layout, font);

  pango_layout_set_text (layout, label, -1);
  line = pango_layout_get_line_readonly (layout, 0);
  if (line->resolved_dir == PANGO_DIRECTION_RTL)
    pango_layout_set_alignment (layout, PANGO_ALIGN_RIGHT);

  pango_layout_set_width (layout, PANGO_SCALE * box->width);
  pango_layout_get_extents (layout, NULL, &extents);

  cairo_move_to (cr,
                 0.5 * (box->width - (double)extents.width / PANGO_SCALE),
                 0.5 * (box->height - (double)extents.height / PANGO_SCALE));
  gtk_style_context_get_color (context, state, &color);

  cairo_set_source_rgba (cr,
                         color.red,
                         color.green,
                         color.blue,
                         color.alpha);
  pango_cairo_show_layout (cr, layout);

  cairo_restore (cr);
}


static void
render_icon (cairo_t            *cr,
             GtkStyleContext    *context,
             GtkIconTheme       *icon_theme,
             const gchar        *icon,
             const GdkRectangle *box)
{
  int icon_size;

  g_autoptr (GtkIconInfo) icon_info = NULL;
  g_autoptr (GdkPixbuf) pixbuf = NULL;

  icon_size = MIN (KEY_ICON_SIZE, box->height / 2);
  icon_info = gtk_icon_theme_lookup_icon (icon_theme, icon, icon_size, 0);
  g_return_if_fail (icon_info);
  pixbuf = gtk_icon_info_load_symbolic_for_context (icon_info, context, NULL, NULL);

  gtk_render_icon (context, cr, pixbuf,
                   (box->width - icon_size) / 2,
                   (box->height - icon_size) / 2);
}


static void
draw_key (PosOskWidget *self, PosOskKey *key, cairo_t *cr, double col, double row)
{
  GdkRGBA fg_color;
  GtkStateFlags state;
  const GdkRectangle *box;
  g_autofree char *style = NULL;
  g_autofree char *icon = NULL;
  g_autofree char *label = NULL;
  g_autofree char *symbol = NULL;
  gboolean pressed;
  double width;

  state = gtk_style_context_get_state (self->key_context);
  gtk_style_context_get_color (self->key_context, state, &fg_color);

  g_object_get (key, "style", &style, "pressed", &pressed, "width", &width,
                "symbol", &symbol, "label", &label, "icon", &icon, NULL);

  if (style)
    gtk_style_context_add_class (self->key_context, style);

  if (pressed)
    gtk_style_context_add_class (self->key_context, "pressed");

  cairo_save (cr);

  box = pos_osk_key_get_box (key);
  cairo_translate (cr, box->x, box->y);
  cairo_rectangle (cr, 0.0, 0.0, box->width, box->height);
  cairo_clip (cr);

  render_outline (cr, self->key_context, box);

  if (self->mode == POS_OSK_WIDGET_MODE_KEYBOARD) {
    if (icon) {
      GdkScreen *screen = gtk_widget_get_screen (GTK_WIDGET (self));
      GtkIconTheme *icon_theme = gtk_icon_theme_get_for_screen (screen);

      render_icon (cr, self->key_context, icon_theme, icon, box);
    } else {
      render_label (cr, self->key_context, label ?: symbol, box);
    }
  }

  cairo_restore (cr);

  if (style)
    gtk_style_context_remove_class (self->key_context, style);

  if (pressed)
    gtk_style_context_remove_class (self->key_context, "pressed");
}


static void
pos_osk_widget_size_allocate (GtkWidget *widget, GdkRectangle *allocation)
{
  PosOskWidget *self = POS_OSK_WIDGET (widget);

  self->width = allocation->width;
  self->height = allocation->height;

  for (int l = 0; l <= POS_OSK_WIDGET_LAST_LAYER; l++) {
    PosOskWidgetKeyboardLayer *layer = pos_osk_widget_get_layer (self, l);
    layer->key_width = self->width / layer->width;
    layer->key_height = KEY_HEIGHT;
    layer->offset_x = 0.5 * (self->width - (layer->width * layer->key_width));

    /* Precalc all key positions */
    for (int r = 0; r < self->layout.n_rows; r++) {
      PosOskWidgetRow *row = pos_osk_widget_get_layer_row (self, l, r);
      double c = row->offset_x;

      for (int k = 0; k < pos_osk_widget_row_get_num_keys (row); k++) {
        PosOskKey *key = pos_osk_widget_row_get_key (row, k);
        GdkRectangle box;

        box.x = c * layer->key_width;
        box.y = r * layer->key_height;
        box.width = pos_osk_key_get_width (key) * layer->key_width;
        box.height = layer->key_height;
        pos_osk_key_set_box (key, &box);

        c += pos_osk_key_get_width (key);
      }
    }
  }

  GTK_WIDGET_CLASS (pos_osk_widget_parent_class)->size_allocate (widget, allocation);
}


static gboolean
pos_osk_widget_draw (GtkWidget *widget, cairo_t *cr)
{
  PosOskWidget *self = POS_OSK_WIDGET (widget);
  GtkStyleContext *context;
  PosOskWidgetKeyboardLayer *layer = pos_osk_widget_get_current_layer (self);

  cairo_save (cr);

  context = gtk_widget_get_style_context (widget);
  gtk_render_background (context, cr, 0, 0, self->width, self->height);

  cairo_translate (cr, layer->offset_x, 0);

  for (int r = 0; r < self->layout.n_rows; r++) {
    PosOskWidgetRow *row = pos_osk_widget_get_row (self, r);

    double col_num = row->offset_x;
    for (int k = 0; k < pos_osk_widget_row_get_num_keys (row); k++) {
      PosOskKey *key = pos_osk_widget_row_get_key (row, k);

      draw_key (self, key, cr, col_num, r);
      col_num += pos_osk_key_get_width (key);
    }
  }

  cairo_restore (cr);
  return FALSE;
}


static void
pos_osk_widget_finalize (GObject *object)
{
  PosOskWidget *self = POS_OSK_WIDGET (object);

  pos_osk_widget_layout_free (&self->layout);
  g_clear_object (&self->long_press);
  g_clear_pointer (&self->name, g_free);

  G_OBJECT_CLASS (pos_osk_widget_parent_class)->finalize (object);
}


static void
pos_osk_widget_class_init (PosOskWidgetClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = pos_osk_widget_get_property;
  object_class->finalize = pos_osk_widget_finalize;

  widget_class->draw = pos_osk_widget_draw;
  widget_class->size_allocate = pos_osk_widget_size_allocate;
  widget_class->button_press_event = pos_osk_widget_button_press_event;
  widget_class->button_release_event = pos_osk_widget_button_release_event;
  widget_class->motion_notify_event = pos_osk_widget_motion_notify_event;

  /**
   * PosOskWidget:layer
   *
   * The current layer used by the osk widget
   */
  props[PROP_LAYER] =
    g_param_spec_enum ("layer", "", "",
                       POS_TYPE_OSK_WIDGET_LAYER,
                       POS_OSK_WIDGET_LAYER_NORMAL,
                       G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  /**
   * PosOskWidget:name
   *
   * The name of the current layout
   */
  props[PROP_NAME] =
    g_param_spec_string ("name", "", "",
                         NULL,
                         G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, PROP_LAST_PROP, props);

  /**
   * PosOskWidget::key-down
   *
   * A key was pressed. This is mostly useful for haptic feedback
   * since it's not clear yet where the user will lift the finger.
   *
   * The event will be followed by either a "key-up" signal or
   * a "key-caneled" signal in case the press got cancelled.
   */
  signals[OSK_KEY_DOWN] = g_signal_new ("key-down",
                                        G_TYPE_FROM_CLASS (klass),
                                        G_SIGNAL_RUN_LAST,
                                        0, NULL, NULL, NULL,
                                        G_TYPE_NONE,
                                        1,
                                        G_TYPE_STRING);
  signals[OSK_KEY_UP] = g_signal_new ("key-up",
                                      G_TYPE_FROM_CLASS (klass),
                                      G_SIGNAL_RUN_LAST,
                                      0, NULL, NULL, NULL,
                                      G_TYPE_NONE,
                                      1,
                                      G_TYPE_STRING);
  signals[OSK_KEY_CANCELLED] = g_signal_new ("key-cancelled",
                                             G_TYPE_FROM_CLASS (klass),
                                             G_SIGNAL_RUN_LAST,
                                             0, NULL, NULL, NULL,
                                             G_TYPE_NONE,
                                             1,
                                             G_TYPE_STRING);
  /**
   * PosOskWidget::key-symbol
   *
   * A symbol was selected on the keyboard.
   */
  signals[OSK_KEY_SYMBOL] = g_signal_new ("key-symbol",
                                          G_TYPE_FROM_CLASS (klass),
                                          G_SIGNAL_RUN_LAST,
                                          0, NULL, NULL, NULL,
                                          G_TYPE_NONE,
                                          1,
                                          G_TYPE_STRING);

  gtk_widget_class_set_css_name (widget_class, "pos-osk-widget");
}


/* Keys are no GObject types so make up a type for CSS */
static GType
key_type (void)
{
  static GType type = 0;

  if (!type) {
    GTypeInfo info = {0};
    info.class_size = sizeof (GtkWidgetClass);
    info.instance_size = sizeof (GtkWidget);

    type = g_type_register_static (GTK_TYPE_WIDGET, "pos-key", &info, G_TYPE_FLAG_ABSTRACT);
  }

  return type;
}


static void
pos_osk_widget_init (PosOskWidget *self)
{
  /* TODO support PIN, number, etc */
  const char *purpose_class = "normal";

  g_autoptr (GtkWidgetPath) path = NULL;
  GtkStyleContext *key_context;
  GtkStyleContext *context;

  self->mode = POS_OSK_WIDGET_MODE_KEYBOARD;
  self->layer = POS_OSK_WIDGET_LAYER_NORMAL;

  gtk_widget_add_events (GTK_WIDGET (self), GDK_BUTTON_PRESS_MASK |
                         GDK_BUTTON_RELEASE_MASK |
                         GDK_POINTER_MOTION_MASK);

  context = gtk_widget_get_style_context (GTK_WIDGET (self));
  /* Create a style context for the buttons */
  path = gtk_widget_path_new ();
  /* TODO: until keys are widgets */
  gtk_widget_path_append_type (path, key_type ());
  gtk_widget_path_iter_add_class (path, -1, purpose_class);

  key_context = gtk_style_context_new ();
  gtk_style_context_set_path (key_context, path);
  gtk_style_context_set_parent (key_context, context);
  gtk_style_context_set_state (key_context, GTK_STATE_FLAG_NORMAL);
  gtk_style_context_set_screen (key_context, gdk_screen_get_default ());

  self->key_context = key_context;

  self->layer = POS_OSK_WIDGET_LAYER_NORMAL;

  self->long_press = g_object_new (GTK_TYPE_GESTURE_LONG_PRESS,
                                   "widget", self,
                                   "propagation-phase", GTK_PHASE_CAPTURE,
                                   "delay-factor", 0.5,
                                   NULL);
  g_signal_connect (self->long_press, "pressed", G_CALLBACK (on_long_pressed), self);

  self->cursor_drag = g_object_new (GTK_TYPE_GESTURE_DRAG,
                                    "widget", self,
                                    "propagation-phase", GTK_PHASE_CAPTURE,
                                    NULL);
  g_object_connect (self->cursor_drag,
                    "swapped-signal::drag-begin",
                    G_CALLBACK (on_drag_begin), self,
                    "swapped-signal::drag-update",
                    G_CALLBACK (on_drag_update), self,
                    "swapped-signal::drag-end",
                    G_CALLBACK (on_drag_end), self,
                    "swapped-signal::cancel",
                    G_CALLBACK (on_drag_cancel), self,
                    NULL);
}


PosOskWidget *
pos_osk_widget_new (void)
{
  return POS_OSK_WIDGET (g_object_new (POS_TYPE_OSK_WIDGET, NULL));
}


PosOskWidgetLayer
pos_osk_widget_get_level (PosOskWidget *self)
{
  g_return_val_if_fail (POS_IS_OSK_WIDGET (self), POS_OSK_WIDGET_LAYER_NORMAL);

  return self->layer;
}


gboolean
pos_osk_widget_set_layout (PosOskWidget *self,
                           const char   *display_name,
                           const char   *layout,
                           const char   *variant,
                           GError      **err)
{
  g_autofree char *name = NULL;
  g_autofree char *path = NULL;

  g_autoptr (GBytes) data = NULL;
  const char *json;
  gsize size;
  gboolean ret;

  if (!STR_IS_NULL_OR_EMPTY (variant))
    name = g_strdup_printf ("%s+%s", layout, variant);
  else
    name = g_strdup (layout);

  if (g_strcmp0 (self->layout.name, name) == 0)
    return TRUE;

  if (self->layout.name)
    pos_osk_widget_layout_free (&self->layout);
  g_free (self->name);
  self->name = g_strdup (display_name);

  path = g_strdup_printf ("/sm/puri/phosh/osk-stub/layouts/%s.json", name);
  data = g_resources_lookup_data (path, 0, err);
  if (data == NULL) {
    return FALSE;
  }

  json = (char*) g_bytes_get_data (data, &size);
  ret = parse_layout (self, json, size);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_NAME]);

  return ret;
}

const char *
pos_osk_widget_get_name (PosOskWidget *self)
{
  g_return_val_if_fail (POS_IS_OSK_WIDGET (self), NULL);

  return self->layout.name;
}

void
pos_osk_widget_set_mode (PosOskWidget *self, PosOskWidgetMode mode)
{
  g_return_if_fail (POS_IS_OSK_WIDGET (self));

  if (self->mode == mode)
    return;

  g_debug ("Switching to mode: %d", mode);
  self->mode = mode;

  self->last_x = self->last_y = 0.0;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}
