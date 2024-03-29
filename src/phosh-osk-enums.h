/*
 * Copyright (C) 2022-2023 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Enums and flags that are also used in GSettings. Hence the `Phosh` prefix.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * PhoshOskCompletionModeFlags:
 * @PHOSH_OSK_COMPLETION_MODE_NONE: no completion
 * @PHOSH_OSK_COMPLETION_MODE_MANUAL: completion is triggered manually
 * @PHOSH_OSK_COMPLETION_MODE_HINT: opportunistic completion (enable when text-input hints us)
 *
 * When to turn on text completion.
 */
typedef enum {
  PHOSH_OSK_COMPLETION_MODE_NONE   = 0,
  PHOSH_OSK_COMPLETION_MODE_MANUAL = (1 << 0),
  PHOSH_OSK_COMPLETION_MODE_HINT   = (1 << 1),
} PhoshOskCompletionModeFlags;

/**
 * PhoshOskFeatures:
 *
 * PHOSH_OSK_FEATURE_DEFAULT: no special features
 * PHOSH_OSK_FEATURE_KEY_DRAG: When set crossing a key boundary
 *   by dragging the finger across the keyboard sends a
 *   [signal@PosOskWiddget:key-up] for the old key and a
 *   [signal@PosOskWiddget:key-down] for the newly touched key. Without
 *   this flags the key press is canceled.
 */
typedef enum {
  PHOSH_OSK_FEATURE_DEFAULT  = 0,        /*< skip >*/
  PHOSH_OSK_FEATURE_KEY_DRAG = (1 << 0), /*< nick=key-drag >*/
} PhoshOskFeatures;

G_END_DECLS
