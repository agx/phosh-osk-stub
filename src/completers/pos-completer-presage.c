/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#define G_LOG_DOMAIN "pos-completer-presage"

#include "pos-config.h"

#include "pos-completer-priv.h"
#include "pos-completer-presage.h"

#include "util.h"

#include <presage.h>

#include <gio/gio.h>

#include <locale.h>

#define MAX_COMPLETIONS 3
#define CONFIG_NGRM_PREDICTOR_DBFILE "Presage.Predictors.DefaultSmoothedNgramPredictor.DBFILENAME"
#define CONFIG_NGRM_PREDICTOR_USER_DBFILE "Presage.Predictors.UserSmoothedNgramPredictor.DBFILENAME"

enum {
  PROP_0,
  PROP_NAME,
  PROP_PREEDIT,
  PROP_BEFORE_TEXT,
  PROP_AFTER_TEXT,
  PROP_COMPLETIONS,
  PROP_LAST_PROP
};
static GParamSpec *props[PROP_LAST_PROP];

/**
 * PosCompleterPresage:
 *
 * A completer using presage.
 *
 * Uses [presage](https://presage.sourceforge.io/) for completions
 */
struct _PosCompleterPresage {
  GObject               parent;

  char                 *name;
  char                 *before_text;
  char                 *after_text;
  GString              *preedit;
  GStrv                 completions;
  guint                 max_completions;

  presage_t             presage;
  char                 *presage_past;
  char                 *presage_future;

  char                 *lang;
};


static void pos_completer_presage_interface_init (PosCompleterInterface *iface);
static void pos_completer_presage_initable_interface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (PosCompleterPresage, pos_completer_presage, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (POS_TYPE_COMPLETER,
                                                pos_completer_presage_interface_init)
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                pos_completer_presage_initable_interface_init)
  )

static void
pos_completer_presage_set_completions (PosCompleter *iface, GStrv completions)
{
  PosCompleterPresage *self = POS_COMPLETER_PRESAGE (iface);

  g_strfreev (self->completions);
  self->completions = pos_completer_capitalize_by_template (self->preedit->str,
                                                            completions);

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_COMPLETIONS]);
}


static void
pos_completer_presage_predict (PosCompleterPresage *self)
{
  presage_error_code_t result;
  g_auto (GStrv) completions = NULL;

  result = presage_predict (self->presage, &completions);
  if (result == PRESAGE_OK) {
    pos_completer_presage_set_completions (POS_COMPLETER (self), completions);
  } else {
    g_warning ("Failed to complete %s", self->preedit->str);
    pos_completer_presage_set_completions (POS_COMPLETER (self), NULL);
  }
}


static const char *
pos_completer_presage_get_preedit (PosCompleter *iface)
{
  PosCompleterPresage *self = POS_COMPLETER_PRESAGE (iface);

  return self->preedit->str;
}


static void
pos_completer_presage_set_preedit (PosCompleter *iface, const char *preedit)
{
  PosCompleterPresage *self = POS_COMPLETER_PRESAGE (iface);

  if (g_strcmp0 (self->preedit->str, preedit) == 0)
    return;

  g_string_truncate (self->preedit, 0);
  if (preedit)
    g_string_append (self->preedit, preedit);
  else {
    /* No string: reset completions */
    pos_completer_presage_set_completions (POS_COMPLETER (self), NULL);
  }

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PREEDIT]);
}


static const char *
pos_completer_presage_get_before_text (PosCompleter *iface)
{
  PosCompleterPresage *self = POS_COMPLETER_PRESAGE (iface);

  return self->before_text;
}


static const char *
pos_completer_presage_get_after_text (PosCompleter *iface)
{
  PosCompleterPresage *self = POS_COMPLETER_PRESAGE (iface);

  return self->after_text;
}


static void
pos_completer_presage_set_surrounding_text (PosCompleter *iface,
                                            const char   *before_text,
                                            const char   *after_text)
{
  PosCompleterPresage *self = POS_COMPLETER_PRESAGE (iface);
  g_autofree char *word = NULL;
  g_autofree char *new_before = NULL;

  if (g_strcmp0 (self->after_text, after_text) == 0 &&
      g_strcmp0 (self->before_text, before_text) == 0)
    return;

  g_free (self->after_text);
  self->after_text = g_strdup (after_text);

  g_free (self->before_text);
  if (pos_completer_grab_last_word (before_text, &new_before, &word)) {
    self->before_text = g_steal_pointer (&new_before);
    g_string_prepend (self->preedit, word);

    g_debug ("Updating preedit:  b:'%s' p:'%s' a:'%s'",
             self->before_text, self->preedit->str, self->after_text);
    g_signal_emit_by_name (self, "update", self->preedit->str, strlen (word), 0);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PREEDIT]);
  } else {
    self->before_text = g_strdup (before_text);
  }
  pos_completer_presage_predict (self);

  g_debug ("Updating:  b:'%s' p:'%s' a:'%s'",
           self->before_text, self->preedit->str, self->after_text);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_BEFORE_TEXT]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_AFTER_TEXT]);
}


static gboolean
pos_completer_presage_set_language (PosCompleter *completer,
                                    const char   *lang,
                                    const char   *region,
                                    GError      **error)
{
  PosCompleterPresage *self = POS_COMPLETER_PRESAGE (completer);
  g_autofree char *dbdir = NULL;
  g_autofree char *dbfile = NULL;
  g_autofree char *dbpath = NULL;
  gboolean ret;
  presage_error_code_t result;

  g_return_val_if_fail (POS_IS_COMPLETER_PRESAGE (self), FALSE);

  /* TODO: handle region */
  if (g_strcmp0 (self->lang, lang) == 0)
    return TRUE;

  g_debug ("Switching to language '%s'", lang);

  dbfile = g_strdup_printf ("database_%s.db", lang);
  dbpath = g_build_path (G_DIR_SEPARATOR_S, PRESAGE_DICT_DIR, dbfile, NULL);

  if (g_file_test (dbpath, G_FILE_TEST_EXISTS) == FALSE) {
    g_set_error (error,
                 POS_COMPLETER_ERROR, POS_COMPLETER_ERROR_LANG_INIT,
                 "No db %s for %s - please fix", dbpath, lang);
    return FALSE;
  }

  result = presage_config_set (self->presage, CONFIG_NGRM_PREDICTOR_DBFILE, dbpath);
  if (result != PRESAGE_OK) {
    g_set_error (error,
                 POS_COMPLETER_ERROR, POS_COMPLETER_ERROR_LANG_INIT,
                 "Failed to set db %s", dbpath);
    return FALSE;
  }

  g_clear_pointer (&dbfile, g_free);
  g_clear_pointer (&dbpath, g_free);

  /* presage example uses a single file, we use one file per language */
  dbdir = g_build_path ("/", g_get_user_data_dir (), "phosh-osk-stub", NULL);
  dbpath = g_strdup_printf ("%s/lm_%s.db", dbdir, lang);
  ret = g_mkdir_with_parents (dbdir, 0755);
  if (ret != 0) {
    g_set_error (error,
                 G_IO_ERROR, g_io_error_from_errno (ret),
                 "Failed to set user db %s: %s", dbpath, g_strerror (ret));
  }

  result = presage_config_set (self->presage, CONFIG_NGRM_PREDICTOR_USER_DBFILE, dbpath);
  if (result != PRESAGE_OK) {
    g_set_error (error,
                 POS_COMPLETER_ERROR, POS_COMPLETER_ERROR_LANG_INIT,
                 "Failed to set user db %s", dbpath);
    return FALSE;
  }

  g_free (self->lang);
  self->lang = g_strdup (lang);

  return TRUE;
}


static void
pos_completer_presage_set_property (GObject      *object,
                                    guint         property_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  PosCompleterPresage *self = POS_COMPLETER_PRESAGE (object);

  switch (property_id) {
  case PROP_PREEDIT:
    pos_completer_presage_set_preedit (POS_COMPLETER (self), g_value_get_string (value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
pos_completer_presage_get_property (GObject    *object,
                                    guint       property_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  PosCompleterPresage *self = POS_COMPLETER_PRESAGE (object);

  switch (property_id) {
  case PROP_NAME:
    g_value_set_string (value, self->name);
    break;
  case PROP_PREEDIT:
    g_value_set_string (value, self->preedit->str);
    break;
  case PROP_BEFORE_TEXT:
    g_value_set_string (value, self->before_text);
    break;
  case PROP_AFTER_TEXT:
    g_value_set_string (value, self->after_text);
    break;
  case PROP_COMPLETIONS:
    g_value_set_boxed (value, self->completions);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
pos_completer_presage_finalize (GObject *object)
{
  PosCompleterPresage *self = POS_COMPLETER_PRESAGE (object);

  g_clear_pointer (&self->completions, g_strfreev);
  g_string_free (self->preedit, TRUE);
  g_clear_pointer (&self->before_text, g_free);
  g_clear_pointer (&self->after_text, g_free);
  g_clear_pointer (&self->presage_past, g_free);
  g_clear_pointer (&self->presage_future, g_free);
  g_clear_pointer (&self->lang, g_free);
  presage_free (self->presage);

  G_OBJECT_CLASS (pos_completer_presage_parent_class)->finalize (object);
}


static void
pos_completer_presage_class_init (PosCompleterPresageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = pos_completer_presage_get_property;
  object_class->set_property = pos_completer_presage_set_property;
  object_class->finalize = pos_completer_presage_finalize;

  g_object_class_override_property (object_class, PROP_NAME, "name");
  props[PROP_NAME] = g_object_class_find_property (object_class, "name");

  g_object_class_override_property (object_class, PROP_PREEDIT, "preedit");
  props[PROP_PREEDIT] = g_object_class_find_property (object_class, "preedit");

  g_object_class_override_property (object_class, PROP_BEFORE_TEXT, "before-text");
  props[PROP_BEFORE_TEXT] = g_object_class_find_property (object_class, "before-text");

  g_object_class_override_property (object_class, PROP_AFTER_TEXT, "after-text");
  props[PROP_AFTER_TEXT] = g_object_class_find_property (object_class, "after-text");

  g_object_class_override_property (object_class, PROP_COMPLETIONS, "completions");
  props[PROP_COMPLETIONS] = g_object_class_find_property (object_class, "completions");
}


static const char*
pos_completer_presage_get_past_stream (void *data)
{
  PosCompleterPresage *self = POS_COMPLETER_PRESAGE (data);

  g_free (self->presage_past);
  self->presage_past = g_strdup_printf ("%s%s", self->before_text ?: "", self->preedit->str);

  g_debug ("Past: %s", self->presage_past);
  return self->presage_past;
}


static const char*
pos_completer_presage_get_future_stream (void *data)
{
  return "";
}


static gboolean
pos_completer_presage_initable_init (GInitable    *initable,
                                     GCancellable *cancelable,
                                     GError      **error)
{
  PosCompleterPresage *self = POS_COMPLETER_PRESAGE (initable);
  presage_error_code_t result;
  g_autofree char *max = NULL;

  /* FIXME: presage gets confused otherwise and doesn't predict */
  setlocale (LC_NUMERIC, "C.UTF-8");
  result = presage_new (pos_completer_presage_get_past_stream,
                        self,
                        pos_completer_presage_get_future_stream,
                        self,
                        &self->presage);

  if (result != PRESAGE_OK) {
    g_set_error (error,
                 POS_COMPLETER_ERROR, POS_COMPLETER_ERROR_ENGINE_INIT,
                 "Failed to init presage engine");
    return FALSE;
  }

  max = g_strdup_printf ("%d", self->max_completions);
  presage_config_set (self->presage, "Presage.Selector.SUGGESTIONS", max);
  presage_config_set (self->presage, "Presage.Selector.REPEAT_SUGGESTIONS", "yes");

  /* Set up default language */
  if (pos_completer_presage_set_language (POS_COMPLETER (self),
                                          POS_COMPLETER_DEFAULT_LANG,
                                          POS_COMPLETER_DEFAULT_REGION,
                                          error) == FALSE)
    return FALSE;

  g_debug ("Presage completer inited with lang '%s'", self->lang);

  return TRUE;
}


static void
pos_completer_presage_initable_interface_init (GInitableIface *iface)
{
  iface->init = pos_completer_presage_initable_init;
}


static const char *
pos_completer_presage_get_name (PosCompleter *iface)
{
  PosCompleterPresage *self = POS_COMPLETER_PRESAGE (iface);

  return self->name;
}


static gboolean
pos_completer_presage_feed_symbol (PosCompleter *iface, const char *symbol)
{
  PosCompleterPresage *self = POS_COMPLETER_PRESAGE (iface);
  g_autofree char *preedit = g_strdup (self->preedit->str);

  if (pos_completer_add_preedit (POS_COMPLETER (self), self->preedit, symbol)) {
    g_signal_emit_by_name (self, "commit-string", self->preedit->str);
    pos_completer_presage_set_preedit (POS_COMPLETER (self), NULL);

    /* Make sure enter is processed as raw keystroke */
    if (g_strcmp0 (symbol, "KEY_ENTER") == 0)
      return FALSE;

    return TRUE;
  }

  /* preedit didn't change and wasn't committed so we didn't handle it */
  if (g_strcmp0 (self->preedit->str, preedit) == 0)
    return FALSE;

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PREEDIT]);

  pos_completer_presage_predict (self);
  return TRUE;
}


static void
pos_completer_presage_interface_init (PosCompleterInterface *iface)
{
  iface->get_name = pos_completer_presage_get_name;
  iface->feed_symbol = pos_completer_presage_feed_symbol;
  iface->get_preedit = pos_completer_presage_get_preedit;
  iface->set_preedit = pos_completer_presage_set_preedit;
  iface->get_before_text = pos_completer_presage_get_before_text;
  iface->get_after_text = pos_completer_presage_get_after_text;
  iface->set_surrounding_text = pos_completer_presage_set_surrounding_text;
  iface->set_language = pos_completer_presage_set_language;
}


static void
pos_completer_presage_init (PosCompleterPresage *self)
{
  self->max_completions = MAX_COMPLETIONS;
  self->preedit = g_string_new (NULL);
  self->name = "presage";
}

/**
 * pos_completer_presage_new:
 * @err:(nullable): a GError location to store the error occurring, or NULL to ignore.
 *
 * Returns:(transfer full): A new presage based completer.
 */
PosCompleter *
pos_completer_presage_new (GError **err)
{
  return POS_COMPLETER (g_initable_new (POS_TYPE_COMPLETER_PRESAGE, NULL, err, NULL));
}
