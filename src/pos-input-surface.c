/*
 * Copyright (C) 2021 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#define G_LOG_DOMAIN "pos-input-surface"

#include "pos-config.h"

#include "phosh-osk-enums.h"
#include "pos-debug-widget.h"
#include "pos-emoji-picker.h"
#include "pos-input-method.h"
#include "pos-completer.h"
#include "pos-completer-manager.h"
#include "pos-completion-bar.h"
#include "pos-input-surface.h"
#include "pos-main.h"
#include "pos-osk-widget.h"
#include "pos-shortcuts-bar.h"
#include "pos-vk-driver.h"
#include "pos-virtual-keyboard.h"
#include "pos-vk-driver.h"
#include "util.h"

#include <handy.h>
#define LIBFEEDBACK_USE_UNSTABLE_API
#include <libfeedback.h>

#define GNOME_DESKTOP_USE_UNSTABLE_API
#include <libgnome-desktop/gnome-xkb-info.h>

#include <glib/gi18n-lib.h>

enum {
  PROP_0,
  PROP_INPUT_METHOD,
  PROP_COMPLETER,
  PROP_COMPLETER_MANAGER,
  PROP_SCREEN_KEYBOARD_ENABLED,
  PROP_KEYBOARD_DRIVER,
  PROP_SURFACE_VISIBLE,
  PROP_COMPLETER_ACTIVE,
  PROP_COMPLETION_ENABLED,
  PROP_LAST_PROP
};
static GParamSpec *props[PROP_LAST_PROP];

typedef struct {
  gboolean show;
  double   progress;
  gint64   last_frame;
} PosInputSurfaceAnimation;

/**
 * PosInputSurface:
 *
 * Main surface that has all the widgets. Should not bother
 * how the OSK is driven.
 *
 * As toplevel widget it also implements #GActionMap so one
 * can easily add and remove actions.
 */
struct _PosInputSurface {
  PhoshLayerSurface        parent;

  gboolean                 surface_visible;
  PosInputSurfaceAnimation animation;
  int                      height;

  /* GNOME settings */
  gboolean                 screen_keyboard_enabled;
  GSettings               *a11y_settings;
  GSettings               *input_settings;
  GSettings               *osk_settings;
  GnomeXkbInfo            *xkbinfo;

  /* wayland input-method */
  PosInputMethod          *input_method;

  /* OSK */
  GHashTable              *osks;
  HdyDeck                 *deck;
  GtkWidget               *osk_terminal;
  GtkWidget               *emoji_picker;
  GtkWidget               *last_layout;
  PosShortcutsBar         *shortcuts_bar;

  /* The debug surface */
  PosDebugWidget          *debug_widget;

  /* TODO: this should be an interface for different keyboard drivers */
  PosVkDriver             *keyboard_driver;

  GtkCssProvider          *css_provider;
  char                    *theme_name;

  /* menu popover */
  GtkBox                  *menu_box_layouts;
  GtkPopover              *menu_popup;
  GSimpleActionGroup      *action_map;

  /* word completion */
  GtkWidget               *word_completion_btn;
  PosCompleter            *completer;
  PosCompleterManager     *completer_manager;
  GtkWidget               *completion_bar;
  gboolean                 completion_enabled;
  PhoshOskCompletionModeFlags completion_mode;

  /* emission hook for clicks */
  gulong                   clicked_id;
};


static void pos_input_surface_action_group_iface_init (GActionGroupInterface *iface);
static void pos_input_surface_action_map_iface_init (GActionMapInterface *iface);

G_DEFINE_TYPE_WITH_CODE (PosInputSurface, pos_input_surface, PHOSH_TYPE_LAYER_SURFACE,
                         G_IMPLEMENT_INTERFACE (G_TYPE_ACTION_GROUP, pos_input_surface_action_group_iface_init)
                         G_IMPLEMENT_INTERFACE (G_TYPE_ACTION_MAP, pos_input_surface_action_map_iface_init)
  )


static void
on_shortcut_activated (PosInputSurface *self, PosShortcut *shortcut, PosShortcutsBar *bar)
{
  g_return_if_fail (POS_IS_INPUT_SURFACE (self));
  g_return_if_fail (POS_IS_SHORTCUTS_BAR (bar));

  pos_vk_driver_key_press_gdk (self->keyboard_driver,
                               pos_shortcut_get_key (shortcut),
                               pos_shortcut_get_modifiers (shortcut));
}


static void
pos_input_surface_toggle_shortcuts_bar (PosInputSurface *self)
{
  GtkWidget *child;
  gboolean shortcuts_visible = FALSE;

  child = hdy_deck_get_visible_child (self->deck);

  /* shortcuts bar is only for terminal and when we have shortcuts defined */
  if (child == self->osk_terminal)
    shortcuts_visible = !!pos_shortcuts_bar_get_num_shortcuts (self->shortcuts_bar);

  gtk_widget_set_visible (GTK_WIDGET (self->shortcuts_bar), shortcuts_visible);
}


static void
on_num_shortcuts_changed (PosInputSurface *self)
{
  pos_input_surface_toggle_shortcuts_bar (self);
}



static void
pos_input_surface_notify_key_press (PosInputSurface *self)
{
  g_autoptr (LfbEvent) event = NULL;

  g_assert (POS_IS_INPUT_SURFACE (self));

  event = lfb_event_new ("button-pressed");
  lfb_event_trigger_feedback_async (event, NULL, NULL, NULL);
}


static gboolean
on_click_hook (GSignalInvocationHint *ihint,
               guint                  n_param_values,
               const GValue          *param_values,
               gpointer               user_data)
{
  PosInputSurface *self = POS_INPUT_SURFACE (user_data);

  pos_input_surface_notify_key_press (self);
  return TRUE;
}


/* This is a bit more strict than is_completer_active so it can be used
   with active completer but also takes the OSK's mode into account */
static gboolean
pos_input_surface_is_completion_mode (PosInputSurface *self)
{
  GtkWidget *osk_widget;

  if (pos_input_surface_is_completer_active(self) == FALSE)
    return FALSE;

  /* no completion in cursor mode */
  osk_widget = hdy_deck_get_visible_child (self->deck);
  if (POS_IS_OSK_WIDGET (osk_widget) == FALSE)
    return FALSE;

  return pos_osk_widget_get_mode (POS_OSK_WIDGET (osk_widget)) == POS_OSK_WIDGET_MODE_KEYBOARD;
}


static void
on_completion_selected (PosInputSurface *self, const char *completion)
{
  g_autofree gchar *send = NULL;

  g_return_if_fail (POS_IS_INPUT_SURFACE (self));
  g_return_if_fail (completion != NULL);

  g_debug ("completion: %s", completion);
  send = g_strdup_printf ("%s ", completion);

  pos_input_method_send_string (self->input_method, send, TRUE);

  if (pos_input_surface_is_completer_active (self))
    pos_completer_set_preedit (self->completer, NULL);
}


static void
on_completer_preedit_changed (PosInputSurface *self)
{
  const char *preedit = NULL;
  int pos;

  /* Only update preedit when in cursor mode as this updates preedit too */
  if (pos_input_surface_is_completion_mode (self) == FALSE)
    return;

  preedit = pos_completer_get_preedit (self->completer);
  pos = preedit ? strlen (preedit) : 0;

  pos_input_method_send_preedit (self->input_method, preedit, pos, pos, TRUE);
}


static void
on_completer_completions_changed (PosInputSurface *self)
{
  pos_completion_bar_set_completions (POS_COMPLETION_BAR (self->completion_bar),
                                      pos_completer_get_completions (self->completer));
}


static void
on_completer_commit_string (PosInputSurface *self, const char *text)
{
  g_debug ("%s: %s", __func__, text);
  pos_input_method_send_string (self->input_method, text, TRUE);
}


static void
on_completer_update (PosInputSurface *self, const char *preedit, guint before, guint after)
{
  guint pos = strlen (preedit);

  /* In cursor mode we reset preedit, make sure to break the cycle */
  if (pos_input_surface_is_completion_mode (self) == FALSE)
    return;

  pos_input_method_delete_surrounding_text (self->input_method, before, after, FALSE);
  pos_input_method_send_preedit (self->input_method, preedit, pos, pos, TRUE);
}


static void
pos_input_surface_submit_current_preedit (PosInputSurface *self)
{
  g_autofree char *preedit = NULL;

  if (pos_input_surface_is_completer_active (self) == FALSE)
    return;

  preedit = g_strdup (pos_completer_get_preedit (self->completer));
  g_debug ("%s: Submitting %s", __func__, preedit);
  pos_completer_set_preedit (self->completer, NULL);
  pos_input_method_send_preedit (self->input_method, "", 0, 0, FALSE);
  pos_input_method_send_string (self->input_method, preedit, TRUE);
}


/* Select proper style sheet in case of high contrast */
static void
on_gtk_theme_name_changed (PosInputSurface *self, GParamSpec *pspec, GtkSettings *settings)
{
  const char *style;
  g_autofree char *name = NULL;

  g_autoptr (GtkCssProvider) provider = gtk_css_provider_new ();

  g_object_get (settings, "gtk-theme-name", &name, NULL);

  if (g_strcmp0 (self->theme_name, name) == 0)
    return;

  self->theme_name = g_steal_pointer (&name);
  g_debug ("GTK theme: %s", self->theme_name);

  if (self->css_provider) {
    gtk_style_context_remove_provider_for_screen (gdk_screen_get_default (),
                                                  GTK_STYLE_PROVIDER (self->css_provider));
  }

  if (g_strcmp0 (self->theme_name, "HighContrast") == 0)
    style = "/sm/puri/phosh/osk-stub/stylesheet/adwaita-hc-light.css";
  else
    style = "/sm/puri/phosh/osk-stub/stylesheet/adwaita-dark.css";

  gtk_css_provider_load_from_resource (provider, style);
  gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
                                             GTK_STYLE_PROVIDER (provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_set_object (&self->css_provider, provider);
}


static void
on_osk_key_down (PosInputSurface *self, const char *symbol, GtkWidget *osk_widget)
{
  g_return_if_fail (POS_IS_INPUT_SURFACE (self));
  g_return_if_fail (POS_IS_OSK_WIDGET (osk_widget));

  pos_input_surface_notify_key_press (self);
}


static void
on_osk_key_symbol (PosInputSurface *self, const char *symbol, GtkWidget *osk_widget)
{
  gboolean handled;

  g_return_if_fail (POS_IS_INPUT_SURFACE (self));
  g_return_if_fail (osk_widget == NULL || POS_IS_OSK_WIDGET (osk_widget));

  g_debug ("Key: '%s' symbol", symbol);

  /* virtual-keyboard, no input method */
  if (!pos_input_method_get_active (self->input_method)) {
    pos_vk_driver_key_down (self->keyboard_driver, symbol);
    pos_vk_driver_key_up (self->keyboard_driver, symbol);
    return;
  }

  if (pos_input_surface_is_completion_mode (self)) {
    handled = pos_completer_feed_symbol (self->completer, symbol);
    if (handled)
      return;
  }

  if (g_str_has_prefix (symbol, "KEY_")) {
    pos_vk_driver_key_down (self->keyboard_driver, symbol);
    pos_vk_driver_key_up (self->keyboard_driver, symbol);
  } else {
    pos_input_method_send_string (self->input_method, symbol, TRUE);
  }

  if (pos_input_surface_is_completer_active (self))
    pos_completer_set_preedit (self->completer, NULL);
}


static void
on_osk_mode_changed (PosInputSurface *self, GParamSpec *pspec, GtkWidget *osk_widget)
{
  g_return_if_fail (POS_IS_INPUT_SURFACE (self));
  g_return_if_fail (POS_IS_OSK_WIDGET (osk_widget));

  /* We only want to clear preedit when entering cursor mode */
  if (pos_input_surface_is_completion_mode (self) == TRUE)
    return;

  pos_input_surface_submit_current_preedit (self);
}


static void
clipboard_copy_activated (GSimpleAction *action,
                          GVariant      *parameter,
                          gpointer       data)
{
  PosInputSurface *self = POS_INPUT_SURFACE (data);

  pos_vk_driver_key_down (self->keyboard_driver, "KEY_COPY");
  pos_vk_driver_key_up (self->keyboard_driver, "KEY_COPY");
}


static void
clipboard_paste_activated (GSimpleAction *action,
                           GVariant      *parameter,
                           gpointer       data)
{
  PosInputSurface *self = POS_INPUT_SURFACE (data);

  pos_vk_driver_key_down (self->keyboard_driver, "KEY_PASTE");
  pos_vk_driver_key_up (self->keyboard_driver, "KEY_PASTE");
}

/* Emoji picker */

static void
on_emoji_picked (PosInputSurface *self, const char *emoji, PosEmojiPicker *emoji_picker)
{
  g_assert (POS_IS_INPUT_SURFACE (self));
  g_assert (POS_IS_EMOJI_PICKER (emoji_picker));

  pos_input_surface_submit_current_preedit (self);
  pos_input_method_send_string (self->input_method, emoji, TRUE);

  pos_input_surface_notify_key_press (self);
}


static void
on_emoji_picker_done (PosInputSurface *self)
{
  hdy_deck_set_visible_child (self->deck, self->last_layout);
}


static void
on_emoji_picker_delete_last (PosInputSurface *self)
{
  g_debug ("%s", __func__);
  on_osk_key_symbol (self, "KEY_BACKSPACE", NULL);
}

/* menu button */

static void
menu_add_layout (gpointer key, gpointer value, gpointer data)
{
  GtkWidget *button;
  const char *name = key;
  PosOskWidget *osk_widget = POS_OSK_WIDGET (value);
  PosInputSurface *self = POS_INPUT_SURFACE (data);
  const char *display_name = pos_osk_widget_get_display_name (osk_widget);

  button = g_object_new (GTK_TYPE_MODEL_BUTTON,
                         "visible", TRUE,
                         "label", display_name,
                         "action-name", "win.select-layout",
                         "action-target", g_variant_new_string (name),
                         NULL);
  gtk_box_pack_start (self->menu_box_layouts, button, FALSE, FALSE, 0);
}


static void
menu_add_emoji_picker (PosInputSurface *self)
{
  GtkWidget *button;

  button = g_object_new (GTK_TYPE_MODEL_BUTTON,
                         "visible", TRUE,
                         "label", _("Emoji"),
                         "action-name", "win.select-layout",
                         "action-target", g_variant_new_string ("emoji"),
                         NULL);
  gtk_box_pack_start (self->menu_box_layouts, button, FALSE, FALSE, 0);
}


static void
menu_activated (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  PosInputSurface *self = POS_INPUT_SURFACE (data);
  GdkRectangle rect = {};
  GtkWidget *osk_widget;
  GAction *layout_action;
  const char *osk_name;

  osk_widget = hdy_deck_get_visible_child (self->deck);
  osk_name = pos_osk_widget_get_name (POS_OSK_WIDGET (osk_widget));
  g_variant_get (parameter, "(ii)", &rect.x, &rect.y);
  g_debug ("Menu popoup activated at %d %d, current: '%s'", rect.x, rect.y, osk_name);

  layout_action = g_action_map_lookup_action (G_ACTION_MAP (self->action_map), "select-layout");
  g_simple_action_set_state (G_SIMPLE_ACTION (layout_action),
                             g_variant_new ("s", osk_name));

  gtk_container_foreach (GTK_CONTAINER (self->menu_box_layouts),
                         (GtkCallback) gtk_widget_destroy,
                         NULL);

  g_hash_table_foreach (self->osks, menu_add_layout, self);
  menu_add_layout ((gpointer)pos_osk_widget_get_name (POS_OSK_WIDGET (self->osk_terminal)),
                   self->osk_terminal,
                   self);
  menu_add_emoji_picker (self);

  gtk_widget_set_visible (self->word_completion_btn,
                          self->completer &&
                          !(self->completion_mode == PHOSH_OSK_COMPLETION_MODE_NONE));

  gtk_popover_set_relative_to (self->menu_popup, osk_widget);
  gtk_popover_set_pointing_to (self->menu_popup, &rect);

  gtk_popover_popup (self->menu_popup);
}


static void
select_layout_change_state (GSimpleAction *action,
                            GVariant      *parameter,
                            gpointer       data)
{
  PosInputSurface *self = POS_INPUT_SURFACE (data);
  const char *layout = NULL;
  GtkWidget *osk_widget;

  /* popdown popover right away to avoid flicker when switching layouts */
  gtk_popover_popdown (self->menu_popup);

  g_variant_get (parameter, "&s", &layout);
  g_debug ("Layout '%s' selected", layout);

  osk_widget = g_hash_table_lookup (self->osks, layout);
  if (osk_widget == NULL) {
    if (g_str_equal (layout, "terminal")) {
      osk_widget = self->osk_terminal;
    } else if (g_str_equal (layout, "emoji")) {
      osk_widget = self->emoji_picker;
    } else {
      g_warning ("Failed to find layout '%s'", layout);
      return;
    }
  }

  hdy_deck_set_visible_child (self->deck, GTK_WIDGET (osk_widget));
  g_simple_action_set_state (action, parameter);
}


static void
switch_language (PosInputSurface *self, const char *locale)
{
  gboolean success;
  g_autoptr (GError) err = NULL;

  if (self->completer == NULL)
    return;

  g_debug ("Switching language, locale: '%s'", locale);
  success = pos_completer_set_language (self->completer, locale, &err);
  if (success == FALSE) {
    g_warning ("Failed to set language: %s, switching to '%s' instead",
               err->message, POS_COMPLETER_DEFAULT_LANG);
    pos_completer_set_language (self->completer, POS_COMPLETER_DEFAULT_LANG, NULL);
  }

  pos_completion_bar_set_completions (POS_COMPLETION_BAR (self->completion_bar), NULL);
}


static void
on_visible_child_changed (PosInputSurface *self)
{
  GtkWidget *child;
  PosOskWidget *osk;

  child = hdy_deck_get_visible_child (self->deck);

  pos_input_surface_toggle_shortcuts_bar (self);

  if (!POS_IS_OSK_WIDGET (child))
    return;

  osk = POS_OSK_WIDGET (child);
  g_debug ("Switched to layout '%s'", pos_osk_widget_get_display_name (osk));
  pos_osk_widget_set_layer (osk, POS_OSK_WIDGET_LAYER_NORMAL);

  /* Remember last layout */
  self->last_layout = GTK_WIDGET (osk);

  switch_language (self, pos_osk_widget_get_locale (osk));

  /* Recheck completion bar visibility */
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_COMPLETER_ACTIVE]);
}

static void
pos_screen_keyboard_set_enabled (PosInputSurface *self, gboolean enable)
{
  const char *msg = enable ? "enabled" : "disabled";

  g_debug ("Screen keyboard enable: %s", msg);

  if (enable == self->screen_keyboard_enabled)
    return;

  self->screen_keyboard_enabled = enable;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SCREEN_KEYBOARD_ENABLED]);
}


static void
pos_input_surface_move (PosInputSurface *self)
{
  int margin;
  int height;
  double progress = hdy_ease_out_cubic (self->animation.progress);

  if (self->animation.show)
    progress = 1.0 - progress;

  g_object_get (self, "configured-height", &height, NULL);
  margin = -height * progress;

  phosh_layer_surface_set_margins (PHOSH_LAYER_SURFACE (self), 0, 0, margin, 0);

  if (self->animation.progress >= 1.0 &&  self->animation.show) {
    /* On unfold adjust the exclusive zone at the very end to avoid flickering */
    phosh_layer_surface_set_exclusive_zone (PHOSH_LAYER_SURFACE (self), height);
  } else if (self->animation.progress < 1.0 && !self->animation.show) {
    /* On fold adjust the exclusive zone at the start to avoid flickering */
    phosh_layer_surface_set_exclusive_zone (PHOSH_LAYER_SURFACE (self), 0);
  }

  if (self->animation.show) {
    gtk_widget_show (GTK_WIDGET (self));
  } else if (self->animation.progress >= 1.0 && !self->animation.show) {
    GtkWidget *widget;

    gtk_widget_hide (GTK_WIDGET (self));
    widget = hdy_deck_get_visible_child (self->deck);
    if (POS_IS_OSK_WIDGET (widget))
      pos_osk_widget_set_layer (POS_OSK_WIDGET (widget), POS_OSK_WIDGET_LAYER_NORMAL);
  }

  phosh_layer_surface_wl_surface_commit (PHOSH_LAYER_SURFACE (self));
}


static gboolean
animate_cb (GtkWidget     *widget,
            GdkFrameClock *frame_clock,
            gpointer       user_data)
{
  PosInputSurface *self = POS_INPUT_SURFACE (widget);
  gint64 time;
  gboolean finished = FALSE;

  time = gdk_frame_clock_get_frame_time (frame_clock) - self->animation.last_frame;
  if (self->animation.last_frame < 0)
    time = 0;

  self->animation.progress += 0.06666 * time / 16666.00;
  self->animation.last_frame = gdk_frame_clock_get_frame_time (frame_clock);

  if (self->animation.progress >= 1.0) {
    finished = TRUE;
    self->animation.progress = 1.0;
  }

  pos_input_surface_move (self);

  if (finished)
    return G_SOURCE_REMOVE;

  return G_SOURCE_CONTINUE;
}


static void
pos_input_surface_set_completer (PosInputSurface *self, PosCompleter *completer)
{
  if (self->completer == completer)
    return;

  if (self->completer)
    g_signal_handlers_disconnect_by_data (self->completer, self);

  g_set_object (&self->completer, completer);

  if (self->completer != NULL) {
    g_debug ("Adding completer '%s'", G_OBJECT_CLASS_NAME (G_OBJECT_GET_CLASS (self->completer)));
    g_object_connect (self->completer,
                      "swapped-signal::notify::completions",
                      G_CALLBACK (on_completer_completions_changed), self,
                      "swapped-signal::notify::preedit",
                      G_CALLBACK (on_completer_preedit_changed), self,
                      "swapped-signal::commit-string",
                      G_CALLBACK (on_completer_commit_string), self,
                      "swapped-signal::update",
                      G_CALLBACK (on_completer_update), self,
                      NULL);
    switch_language (self, pos_osk_widget_get_locale (POS_OSK_WIDGET (self->last_layout)));
  } else {
    g_debug ("Removing completer");
  }
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_COMPLETER]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_COMPLETER_ACTIVE]);
}


static void
pos_input_surface_set_completer_manager (PosInputSurface     *self,
                                         PosCompleterManager *completer_manager)
{
  PosCompleter *default_completer;

  if (self->completer_manager == completer_manager)
    return;

  g_set_object (&self->completer_manager, completer_manager);

  /* Update the current completer */
  default_completer = pos_completer_manager_get_default_completer (self->completer_manager);
  pos_input_surface_set_completer (self, default_completer);
}


static double
reverse_ease_out_cubic (double t)
{
  return cbrt (t - 1) + 1;
}


static void
pos_input_surface_set_completion_enabled (PosInputSurface *self, gboolean enable)
{
  if (self->completion_enabled == enable)
    return;

  /* popdown popover right away */
  gtk_popover_popdown (self->menu_popup);

  self->completion_enabled = enable;

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_COMPLETION_ENABLED]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_COMPLETER_ACTIVE]);
}


static void
pos_input_surface_set_property (GObject      *object,
                                guint         property_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  PosInputSurface *self = POS_INPUT_SURFACE (object);

  switch (property_id) {
  case PROP_INPUT_METHOD:
    self->input_method = g_value_dup_object (value);
    pos_debug_widget_set_input_method (self->debug_widget, self->input_method);
    break;
  case PROP_COMPLETER_MANAGER:
    pos_input_surface_set_completer_manager (self, g_value_get_object (value));
    break;
  case PROP_SCREEN_KEYBOARD_ENABLED:
    pos_screen_keyboard_set_enabled (self, g_value_get_boolean (value));
    break;
  case PROP_KEYBOARD_DRIVER:
    self->keyboard_driver = g_value_dup_object (value);
    break;
  case PROP_SURFACE_VISIBLE:
    pos_input_surface_set_visible (self, g_value_get_boolean (value));
    break;
  case PROP_COMPLETION_ENABLED:
    pos_input_surface_set_completion_enabled (self, g_value_get_boolean (value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
pos_input_surface_get_property (GObject    *object,
                                guint       property_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  PosInputSurface *self = POS_INPUT_SURFACE (object);

  switch (property_id) {
  case PROP_COMPLETER:
    g_value_set_object (value, self->completer);
    break;
  case PROP_COMPLETER_MANAGER:
    g_value_set_object (value, self->completer_manager);
    break;
  case PROP_SCREEN_KEYBOARD_ENABLED:
    g_value_set_boolean (value, pos_input_surface_get_screen_keyboard_enabled (self));
    break;
  case PROP_SURFACE_VISIBLE:
    g_value_set_boolean (value, pos_input_surface_get_visible (self));
    break;
  case PROP_COMPLETER_ACTIVE:
    g_value_set_boolean (value, pos_input_surface_is_completer_active (self));
    break;
  case PROP_COMPLETION_ENABLED:
    g_value_set_boolean (value, self->completion_enabled);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
on_im_purpose_changed (PosInputSurface *self, GParamSpec *pspec, PosInputMethod *im)
{
  GtkWidget *osk_widget = NULL;
  PosOskWidgetLayer layer = POS_OSK_WIDGET_LAYER_NORMAL;

  g_assert (POS_IS_INPUT_SURFACE (self));
  g_assert (POS_IS_INPUT_METHOD (im));

  /* We only have completer active on `normal` input purpose */
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_COMPLETER_ACTIVE]);

  switch (pos_input_method_get_purpose (im)) {
  case POS_INPUT_METHOD_PURPOSE_ALPHA:
  case POS_INPUT_METHOD_PURPOSE_EMAIL:
  case POS_INPUT_METHOD_PURPOSE_NAME:
  case POS_INPUT_METHOD_PURPOSE_NORMAL:
  case POS_INPUT_METHOD_PURPOSE_PASSWORD:
  case POS_INPUT_METHOD_PURPOSE_URL:
    layer = POS_OSK_WIDGET_LAYER_NORMAL;
    break;
  case POS_INPUT_METHOD_PURPOSE_DATE:
  case POS_INPUT_METHOD_PURPOSE_DATETIME:
  case POS_INPUT_METHOD_PURPOSE_DIGITS:
  case POS_INPUT_METHOD_PURPOSE_NUMBER:
  case POS_INPUT_METHOD_PURPOSE_PHONE:
  case POS_INPUT_METHOD_PURPOSE_PIN:
  case POS_INPUT_METHOD_PURPOSE_TIME:
    layer = POS_OSK_WIDGET_LAYER_SYMBOLS;
    break;
  case POS_INPUT_METHOD_PURPOSE_TERMINAL:
    osk_widget = self->osk_terminal;
    break;
  default:
    g_return_if_reached ();
  }

  if (osk_widget == NULL) {
    osk_widget = hdy_deck_get_visible_child (self->deck);
    /* Debug surface and emoji don't have layers */
    if (POS_IS_OSK_WIDGET (osk_widget) == FALSE)
      osk_widget = self->last_layout;
  }
  hdy_deck_set_visible_child (self->deck, osk_widget);

  pos_osk_widget_set_layer (POS_OSK_WIDGET (osk_widget), layer);
}


static void
on_im_text_change_cause_changed (PosInputSurface *self, GParamSpec *pspec, PosInputMethod *im)
{
  g_assert (POS_IS_INPUT_SURFACE (self));
  g_assert (POS_IS_INPUT_METHOD (im));

  if (!pos_input_surface_is_completer_active (self))
    return;

  if (pos_input_method_get_text_change_cause (im) != POS_INPUT_METHOD_TEXT_CHANGE_CAUSE_IM)
    pos_completer_set_preedit (self->completer, NULL);
}


static void
on_im_hint_changed (PosInputSurface *self, GParamSpec *pspec, PosInputMethod *im)
{
  gboolean enable;

  g_assert (POS_IS_INPUT_SURFACE (self));
  g_assert (POS_IS_INPUT_METHOD (im));

  g_debug ("Hint changed: 0x%.2x", pos_input_method_get_hint (im));
  if ((self->completion_mode & PHOSH_OSK_COMPLETION_MODE_HINT) == 0)
    return;

  enable = !!(pos_input_method_get_hint (im) & POS_INPUT_METHOD_HINT_COMPLETION);

  pos_input_surface_set_completion_enabled (self, enable);
}


static void
on_im_surrounding_text_changed (PosInputSurface *self, GParamSpec *pspec, PosInputMethod *im)
{
  const char *text;
  guint anchor, cursor;
  g_autofree char *before = NULL;
  g_autofree char *after = NULL;

  g_assert (POS_IS_INPUT_SURFACE (self));
  g_assert (POS_IS_INPUT_METHOD (im));

  text = pos_input_method_get_surrounding_text (im, &anchor, &cursor);
  if (!pos_input_surface_is_completer_active (self))
    return;

  before = g_strndup (text, cursor);

  if (text)
    after = g_strdup (&(text[cursor]));

  pos_completer_set_surrounding_text (POS_COMPLETER (self->completer), before, after);
}


static void
on_im_active_changed (PosInputSurface *self, GParamSpec *pspec, PosInputMethod *im)
{
  gboolean active;

  g_assert (POS_IS_INPUT_SURFACE (self));
  g_assert (POS_IS_INPUT_METHOD (im));

  active = pos_input_method_get_active (im);

  if (active) {
    /* TODO: Reset buffered commit_string, delete_surrounding_text */
    if (pos_input_surface_is_completer_active (self)) {
      pos_completer_set_preedit (self->completer, NULL);
    }
  }

  /* Completer can only be active with input method, not vk */
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_COMPLETER_ACTIVE]);
}


static void
pos_input_surface_constructed (GObject *object)
{
  PosInputSurface *self = POS_INPUT_SURFACE (object);

  G_OBJECT_CLASS (pos_input_surface_parent_class)->constructed (object);

  g_assert (POS_IS_INPUT_METHOD (self->input_method));
  g_object_connect (self->input_method,
                    "swapped-object-signal::notify::active", on_im_active_changed, self,
                    "swapped-object-signal::notify::purpose", on_im_purpose_changed, self,
                    "swapped-object-signal::notify::hint", on_im_hint_changed, self,
                    "swapped-object-signal::notify::text-change-cause",
                    on_im_text_change_cause_changed, self,
                    "swapped-object-signal::notify::surrounding-text",
                    on_im_surrounding_text_changed, self,
                    NULL);
}


static void
pos_input_surface_destroy (GtkWidget *widget)
{
  PosInputSurface *self = POS_INPUT_SURFACE (widget);

  g_clear_object (&self->action_map);
  /* Remove hash table early since this also destroys the osks in the deck */
  g_clear_pointer (&self->osks, g_hash_table_destroy);

  GTK_WIDGET_CLASS (pos_input_surface_parent_class)->destroy (widget);
}


static void
pos_input_surface_finalize (GObject *object)
{
  PosInputSurface *self = POS_INPUT_SURFACE (object);

  g_signal_remove_emission_hook (g_signal_lookup ("clicked", GTK_TYPE_BUTTON), self->clicked_id);
  self->clicked_id = 0;

  g_clear_object (&self->input_method);
  g_clear_object (&self->a11y_settings);
  g_clear_object (&self->input_settings);
  g_clear_object (&self->osk_settings);
  g_clear_object (&self->xkbinfo);
  g_clear_object (&self->css_provider);
  g_clear_object (&self->completer);
  g_clear_object (&self->completer_manager);
  g_clear_pointer (&self->theme_name, g_free);
  g_clear_pointer (&self->osks, g_hash_table_destroy);

  G_OBJECT_CLASS (pos_input_surface_parent_class)->finalize (object);
}


static gchar **
pos_input_surface_list_actions (GActionGroup *group)
{
  PosInputSurface *self = POS_INPUT_SURFACE (group);

  /* may be NULL after dispose has run */
  if (!self->action_map)
    return g_new0 (char *, 0 + 1);

  return g_action_group_list_actions (G_ACTION_GROUP (self->action_map));
}

static gboolean
pos_input_surface_query_action (GActionGroup        *group,
                                const gchar         *action_name,
                                gboolean            *enabled,
                                const GVariantType **parameter_type,
                                const GVariantType **state_type,
                                GVariant           **state_hint,
                                GVariant           **state)
{
  PosInputSurface *self = POS_INPUT_SURFACE (group);

  if (!self->action_map)
    return FALSE;

  return g_action_group_query_action (G_ACTION_GROUP (self->action_map),
                                      action_name, enabled, parameter_type, state_type, state_hint, state);
}


static void
pos_input_surface_activate_action (GActionGroup *group,
                                   const gchar  *action_name,
                                   GVariant     *parameter)
{
  PosInputSurface *self = POS_INPUT_SURFACE (group);

  if (!self->action_map)
    return;

  g_action_group_activate_action (G_ACTION_GROUP (self->action_map), action_name, parameter);
}


static void
pos_input_surface_change_action_state (GActionGroup *group,
                                       const gchar  *action_name,
                                       GVariant     *state)
{
  PosInputSurface *self = POS_INPUT_SURFACE (group);

  if (!self->action_map)
    return;

  g_action_group_change_action_state (G_ACTION_GROUP (self->action_map), action_name, state);
}


static void
pos_input_surface_action_group_iface_init (GActionGroupInterface *iface)
{
  iface->list_actions = pos_input_surface_list_actions;
  iface->query_action = pos_input_surface_query_action;
  iface->activate_action = pos_input_surface_activate_action;
  iface->change_action_state = pos_input_surface_change_action_state;
}


static GAction *
pos_input_surface_lookup_action (GActionMap  *action_map, const gchar *action_name)
{
  PosInputSurface *self = POS_INPUT_SURFACE (action_map);

  if (!self->action_map)
    return NULL;

  return g_action_map_lookup_action (G_ACTION_MAP (self->action_map), action_name);
}

static void
pos_input_surface_add_action (GActionMap *action_map, GAction *action)
{
  PosInputSurface *self = POS_INPUT_SURFACE (action_map);

  if (!self->action_map)
    return;

  g_action_map_add_action (G_ACTION_MAP (self->action_map), action);
}

static void
pos_input_surface_remove_action (GActionMap *action_map, const gchar *action_name)
{
  PosInputSurface *self = POS_INPUT_SURFACE (action_map);

  if (!self->action_map)
    return;

  g_action_map_remove_action (G_ACTION_MAP (self->action_map), action_name);
}


static void pos_input_surface_action_map_iface_init (GActionMapInterface *iface)
{
  iface->lookup_action = pos_input_surface_lookup_action;
  iface->add_action = pos_input_surface_add_action;
  iface->remove_action = pos_input_surface_remove_action;
}


static void
pos_input_surface_check_resize (GtkContainer *container)
{
  PosInputSurface *self = POS_INPUT_SURFACE (container);
  GtkRequisition min, nat;
  int height;

  g_return_if_fail (GTK_IS_CONTAINER (container));

  gtk_widget_get_preferred_size (GTK_WIDGET (self), &min, &nat);
  g_object_get (self, "height", &height, NULL);

  if (gtk_widget_get_mapped (GTK_WIDGET (self)) && min.height != self->height) {
    phosh_layer_surface_set_size (PHOSH_LAYER_SURFACE (self), -1, min.height);
    /* Don't interfere with animation */
    if (self->animation.progress >= 1.0) {
      phosh_layer_surface_set_exclusive_zone (PHOSH_LAYER_SURFACE (self), min.height);
    }
  }

  GTK_CONTAINER_CLASS (pos_input_surface_parent_class)->check_resize (container);
}



static void
pos_input_surface_class_init (PosInputSurfaceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GtkContainerClass *container_class = GTK_CONTAINER_CLASS (klass);

  object_class->get_property = pos_input_surface_get_property;
  object_class->set_property = pos_input_surface_set_property;
  object_class->constructed = pos_input_surface_constructed;
  object_class->finalize = pos_input_surface_finalize;

  widget_class->destroy = pos_input_surface_destroy;

  container_class->check_resize = pos_input_surface_check_resize;

  /* Make sure resources are available when building gir */
  pos_init ();

  g_type_ensure (POS_TYPE_COMPLETION_BAR);
  g_type_ensure (POS_TYPE_DEBUG_WIDGET);
  g_type_ensure (POS_TYPE_EMOJI_PICKER);
  g_type_ensure (POS_TYPE_OSK_WIDGET);
  g_type_ensure (POS_TYPE_SHORTCUTS_BAR);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/sm/puri/phosh/osk-stub/ui/input-surface.ui");
  gtk_widget_class_bind_template_child (widget_class, PosInputSurface, completion_bar);
  gtk_widget_class_bind_template_child (widget_class, PosInputSurface, debug_widget);
  gtk_widget_class_bind_template_child (widget_class, PosInputSurface, deck);
  gtk_widget_class_bind_template_child (widget_class, PosInputSurface, emoji_picker);
  gtk_widget_class_bind_template_child (widget_class, PosInputSurface, osk_terminal);
  gtk_widget_class_bind_template_child (widget_class, PosInputSurface, menu_box_layouts);
  gtk_widget_class_bind_template_child (widget_class, PosInputSurface, menu_popup);
  gtk_widget_class_bind_template_child (widget_class, PosInputSurface, shortcuts_bar);
  gtk_widget_class_bind_template_child (widget_class, PosInputSurface, word_completion_btn);
  gtk_widget_class_bind_template_callback (widget_class, on_completion_selected);
  gtk_widget_class_bind_template_callback (widget_class, on_emoji_picked);
  gtk_widget_class_bind_template_callback (widget_class, on_emoji_picker_done);
  gtk_widget_class_bind_template_callback (widget_class, on_emoji_picker_delete_last);
  gtk_widget_class_bind_template_callback (widget_class, on_num_shortcuts_changed);
  gtk_widget_class_bind_template_callback (widget_class, on_osk_key_down);
  gtk_widget_class_bind_template_callback (widget_class, on_osk_key_symbol);
  gtk_widget_class_bind_template_callback (widget_class, on_osk_mode_changed);
  gtk_widget_class_bind_template_callback (widget_class, on_shortcut_activated);
  gtk_widget_class_bind_template_callback (widget_class, on_visible_child_changed);

  /**
   * PosInputSurface:input-method:
   *
   * A zwp_input_method_v2
   */
  props[PROP_INPUT_METHOD] =
    g_param_spec_object ("input-method", "", "",
                         POS_TYPE_INPUT_METHOD,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
   /**
    * PosInputSurface:completer:
    *
    * A completer implementing the #PosCompleter interface.
    *
    */
   props[PROP_COMPLETER] =
     g_param_spec_object ("completer", "", "",
                          POS_TYPE_COMPLETER,
                          G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  /**
   * PosInputSurface:completer-manager:
   *
   * A completer manager to use. It allows to query which implementation of
   * [iface@Completer] to use.
   */
  props[PROP_COMPLETER_MANAGER] =
    g_param_spec_object ("completer-manager", "", "",
                         POS_TYPE_COMPLETER_MANAGER,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  /**
   * PosInputSurface:enable
   *
   * Whether an on screen keyboard should be enabled. This is the
   * global toggle that enables screen keyboards and maps the a11y
   * settings.
   */
  props[PROP_SCREEN_KEYBOARD_ENABLED] =
    g_param_spec_boolean ("screen-keyboard-enabled", "", "", FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  /**
   * PosInputSurface:surface-visible
   *
   * If this is %TRUE the input surface will be shown, otherwise hidden.
   */
  props[PROP_SURFACE_VISIBLE] =
    g_param_spec_boolean ("surface-visible", "", "", FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  /**
   * PosInputSurface:completer-active
   *
   * %TRUE if the there is a completer set and active
   */
  props[PROP_COMPLETER_ACTIVE] =
    g_param_spec_boolean ("completer-active", "", "", FALSE,
                          G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  /**
   * PosInputSurface:completion-enabled
   *
   * %TRUE if the user enabled completion. That does imply that completion is actually active
   * as this also depends on an input-method being present, a completer configured, etc. This
   * setting merely reflects the users intent.
   */
  props[PROP_COMPLETION_ENABLED] =
    g_param_spec_boolean ("completion-enabled", "", "", FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  props[PROP_KEYBOARD_DRIVER] =
    g_param_spec_object ("keyboard-driver", "", "",
                         /* TODO: should be an interface */
                         POS_TYPE_VK_DRIVER,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST_PROP, props);
}


static PosOskWidget *
insert_osk (PosInputSurface *self,
            const char      *name,
            const char      *display_name,
            const char      *layout,
            const char      *variant)
{
  g_autoptr (GError) err = NULL;
  PosOskWidget *osk_widget;

  osk_widget = g_hash_table_lookup (self->osks, name);
  if (osk_widget)
    return osk_widget;

  osk_widget = pos_osk_widget_new ();
  if (!pos_osk_widget_set_layout (POS_OSK_WIDGET (osk_widget),
                                  display_name, layout, variant, &err)) {
    g_warning ("Failed to load osk layout for %s: %s", name, err->message);

    gtk_widget_destroy (g_object_ref_sink (GTK_WIDGET (osk_widget)));
    return NULL;
  }

  g_debug ("Adding osk for layout '%s'", name);
  gtk_widget_set_visible (GTK_WIDGET (osk_widget), TRUE);
  g_object_connect (osk_widget,
                    "swapped-signal::key-down", G_CALLBACK (on_osk_key_down), self,
                    "swapped-signal::key-symbol", G_CALLBACK (on_osk_key_symbol), self,
                    "swapped-signal::notify::mode", G_CALLBACK (on_osk_mode_changed), self,
                    NULL);

  hdy_deck_insert_child_after (self->deck, GTK_WIDGET (osk_widget), NULL);
  g_hash_table_insert (self->osks, g_strdup (name), osk_widget);

  if (self->last_layout == NULL)
    self->last_layout = GTK_WIDGET (osk_widget);

  return osk_widget;
}


static PosOskWidget *
insert_layout (PosInputSurface *self, const char *type, const char *id)
{
  g_autofree char *name = NULL;
  const gchar *layout = NULL;
  const gchar *variant = NULL;
  const gchar *display_name = NULL;

  if (g_strcmp0 (type, "xkb")) {
    g_debug ("Not a xkb layout: '%s' - ignoring", id);
    return NULL;
  }

  if (!gnome_xkb_info_get_layout_info (self->xkbinfo, id, &display_name, NULL,
                                       &layout, &variant)) {
    g_warning ("Failed to get layout info for %s", id);
    return NULL;
  }
  if (STR_IS_NULL_OR_EMPTY (variant))
    name = g_strdup (layout);
  else
    name = g_strdup_printf ("%s+%s", layout, variant);

  return insert_osk (self, name, display_name, layout, variant);
}


static void
on_input_setting_changed (PosInputSurface *self, const char *key, GSettings *settings)
{
  g_autoptr (GVariant) sources = NULL;
  g_autoptr (GHashTable) new = NULL;
  g_autofree GStrv old = NULL;
  g_autofree GStrv old_keys = NULL;
  GVariantIter iter;
  const char *id = NULL;
  const char *type = NULL;
  gboolean first_set = FALSE;

  g_debug ("Setting changed, reloading input settings");

  sources = g_settings_get_value (settings, "sources");
  g_variant_iter_init (&iter, sources);

  /* Get us a copy of the keys since we remove elements while iterating */
  old_keys = (GStrv)g_hash_table_get_keys_as_array (self->osks, NULL);
  old = g_strdupv (old_keys);
  new = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  self->last_layout = NULL;
  while (g_variant_iter_next (&iter, "(&s&s)", &type, &id)) {
    PosOskWidget *osk_widget;

    osk_widget = insert_layout (self, type, id);
    if (osk_widget == NULL)
      continue;

    g_hash_table_add (new, g_strdup (pos_osk_widget_get_name (osk_widget)));
    if (!first_set) {
      first_set = TRUE;
      hdy_deck_set_visible_child (self->deck, GTK_WIDGET (osk_widget));
    }
  }

  if (old) {
    /* Drop removed layouts */
    for (int i = 0; old[i]; i++) {
      if (!g_hash_table_contains (new, old[i])) {
        g_debug ("Removing layout %s", old[i]);
        g_hash_table_remove (self->osks, old[i]);
      }
    }
  }

  /* If nothing is left add a default */
  if (g_hash_table_size (self->osks) == 0) {
    insert_osk (self, "us", "English (USA)", "us", NULL);
  }
}


static void
on_completion_mode_changed (PosInputSurface *self, const char *key, GSettings *settings)
{
  PhoshOskCompletionModeFlags mode;

  mode = g_settings_get_flags (settings, "completion-mode");

  if (mode == self->completion_mode)
    return;

  self->completion_mode = mode;

  /* In manual mode we don't interfere with the user's choice */
  if (self->completion_mode & PHOSH_OSK_COMPLETION_MODE_MANUAL)
    return;

  /* In hint mode catch up with the input method */
  if ((self->completion_mode & PHOSH_OSK_COMPLETION_MODE_HINT) && self->input_method) {
    gboolean enable;

    enable = pos_input_method_get_hint (self->input_method) & POS_INPUT_METHOD_HINT_COMPLETION;
    pos_input_surface_set_completion_enabled (self, enable);
    return;
  }

  /* no completion wanted */
  pos_input_surface_set_completion_enabled (self, FALSE);
}


static GActionEntry entries[] =
{
  { .name = "clipboard-copy", .activate = clipboard_copy_activated },
  { .name = "clipboard-paste", .activate = clipboard_paste_activated },
  { .name = "select-layout", .parameter_type = "s", .state = "\"terminal\"",
    .change_state = select_layout_change_state },
  { .name = "menu", .parameter_type = "(ii)", .activate = menu_activated },
};


static void
pos_input_surface_init (PosInputSurface *self)
{
  GtkSettings *gtk_settings;
  const char *test_layout = g_getenv ("POS_TEST_LAYOUT");
  g_autoptr (GPropertyAction) completion_action = NULL;

  gtk_widget_init_template (GTK_WIDGET (self));

  self->action_map = g_simple_action_group_new ();
  g_action_map_add_action_entries (G_ACTION_MAP (self->action_map),
                                   entries,
                                   G_N_ELEMENTS (entries),
                                   self);
  gtk_widget_insert_action_group (GTK_WIDGET (self),
                                  "win",
                                  G_ACTION_GROUP (self->action_map));
  completion_action = g_property_action_new ("word-completion", self, "completion-enabled");
  g_action_map_add_action (G_ACTION_MAP (self), G_ACTION (completion_action));

  /* Ensure initial sync */
  self->screen_keyboard_enabled = -1;
  self->surface_visible = -1;
  self->animation.progress = 1.0;

  self->a11y_settings = g_settings_new ("org.gnome.desktop.a11y.applications");
  g_settings_bind (self->a11y_settings, "screen-keyboard-enabled",
                   self, "screen-keyboard-enabled", G_SETTINGS_BIND_GET);

  self->osks = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)gtk_widget_destroy);
  self->xkbinfo = gnome_xkb_info_new ();
  self->input_settings = g_settings_new ("org.gnome.desktop.input-sources");
  self->osk_settings = g_settings_new ("sm.puri.phosh.osk");
  g_signal_connect_swapped (self->osk_settings, "changed::completion-mode",
                            G_CALLBACK (on_completion_mode_changed),
                            self);
  on_completion_mode_changed (self, NULL, self->osk_settings);

  pos_osk_widget_set_layout (POS_OSK_WIDGET (self->osk_terminal),
                             _("Terminal"),
                             "terminal",
                             NULL,
                             NULL);
  if (test_layout) {
    PosOskWidget *osk_widget = insert_layout (self, "xkb", test_layout);
    hdy_deck_set_visible_child (self->deck, GTK_WIDGET (osk_widget));
  } else {
    g_object_connect (self->input_settings,
                      "swapped-signal::changed::sources",
                      G_CALLBACK (on_input_setting_changed), self,
                      "swapped-signal::changed::xkb-options",
                      G_CALLBACK (on_input_setting_changed), self,
                      NULL);
    on_input_setting_changed (self, NULL, self->input_settings);
  }

  gtk_settings = gtk_settings_get_default ();
  g_object_set (G_OBJECT (gtk_settings), "gtk-application-prefer-dark-theme", TRUE, NULL);

  g_signal_connect_swapped (gtk_settings, "notify::gtk-theme-name",
                            G_CALLBACK (on_gtk_theme_name_changed), self);
  on_gtk_theme_name_changed (self, NULL, gtk_settings);

  self->clicked_id = g_signal_add_emission_hook (g_signal_lookup ("clicked", GTK_TYPE_BUTTON), 0,
                                                 on_click_hook, self, NULL);

  /* Disable swipe gestures to change layouts by default */
  pos_input_surface_set_layout_swipe (self, FALSE);
}


gboolean
pos_input_surface_get_active (PosInputSurface *self)
{
  g_return_val_if_fail (POS_IS_INPUT_SURFACE (self), FALSE);

  return pos_input_method_get_active (self->input_method) && self->completer;
}


void
pos_input_surface_set_visible (PosInputSurface *self, gboolean visible)
{
  g_return_if_fail (POS_IS_INPUT_SURFACE (self));

  if (visible == self->surface_visible)
    return;

  self->surface_visible = visible;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SURFACE_VISIBLE]);

  self->animation.show = visible;
  self->animation.last_frame = -1;
  self->animation.progress =
    reverse_ease_out_cubic (1.0 - hdy_ease_out_cubic (self->animation.progress));

  gtk_widget_add_tick_callback (GTK_WIDGET (self), animate_cb, NULL, NULL);
}


gboolean
pos_input_surface_get_visible (PosInputSurface *self)
{
  g_return_val_if_fail (POS_IS_INPUT_SURFACE (self), FALSE);

  return self->surface_visible;
}


gboolean
pos_input_surface_get_screen_keyboard_enabled (PosInputSurface *self)
{
  g_return_val_if_fail (POS_IS_INPUT_SURFACE (self), FALSE);

  return self->screen_keyboard_enabled;
}

gboolean
pos_input_surface_is_completer_active (PosInputSurface *self)
{
  GtkWidget *child;

  g_return_val_if_fail (POS_IS_INPUT_SURFACE (self), FALSE);

  if (self->completer == NULL)
    return FALSE;

  if (pos_input_method_get_active (self->input_method) == FALSE)
    return FALSE;

  if (self->completion_enabled == FALSE)
    return FALSE;

  /* Completion should only be used on "regular" layouts */
  child = hdy_deck_get_visible_child (self->deck);
  if (POS_IS_OSK_WIDGET (child) == FALSE || child == self->osk_terminal)
    return FALSE;

  /* We only complete input purpose `normal` */
  return (pos_input_method_get_purpose (self->input_method) == POS_INPUT_METHOD_PURPOSE_NORMAL);
}

/**
 * pos_input_surface_set_layout_swipe:
 * @self: The input surface
 * @enable: Passing %TRUE will enable layout change via swipe gestures.
 *
 * Controls whether layout changes are possible using swipe gestures.
 */
void
pos_input_surface_set_layout_swipe (PosInputSurface *self, gboolean enable)
{
  g_return_if_fail (POS_IS_INPUT_SURFACE (self));

  hdy_deck_set_can_swipe_forward (self->deck, enable);
  hdy_deck_set_can_swipe_back (self->deck, enable);
}

/**
 * pos_input_surface_get_layout_swipe:
 * @self: The input surface
 *
 * Whether layouts can be changed using a swipe gesture.
 *
 * Returns %TRUE is layout swiping is enabled, otherwise %FALSE.
 */
gboolean
pos_input_surface_get_layout_swipe (PosInputSurface *self)
{
  g_return_val_if_fail (POS_IS_INPUT_SURFACE (self), FALSE);

  return hdy_deck_get_can_swipe_forward (self->deck);
}
