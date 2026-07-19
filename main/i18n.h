// Wallet UI translations. tr(STR_*) looks the key up in the active language's
// table (generated into i18n_tables.c from i18n/*.json by tools/gen_i18n.py).
// Scope: wallet screens and live home-tile labels. The game and baked KISS
// wordmark/tagline stay English by design (baked artwork + decoy story).
#pragma once
#include <stdint.h>
#include "i18n_keys.h"   // STR_* keys + the I18N_* locale enum (generated;
                         // the locale manifest lives in tools/gen_i18n.py)

// Which generated font family renders this locale.
enum { I18N_FC_LAT = 0, I18N_FC_JA, I18N_FC_KO, I18N_FC_ZH };

typedef struct {
    const char *code;    // "de", "es-MX", ... (matches i18n/<code>.json)
    const char *native;  // the name shown in the picker, in its own language
    int font_class;      // I18N_FC_*
} i18n_lang_t;

const char *tr(int id);                    // never NULL; clamps bad ids
const char *tr_sym(const char *sym, int id);  // "SYM  text" (LV_SYMBOL_* prefix)
void i18n_set_lang(int lang);              // clamps; caller persists to NVS
int  i18n_get_lang(void);
const i18n_lang_t *i18n_lang_info(int lang);

extern const char *const *const i18n_tables[I18N_LANG_N];  // generated
extern const i18n_lang_t i18n_langs[I18N_LANG_N];          // generated
extern const uint8_t i18n_pick_order[I18N_LANG_N];         // generated: picker
                                  // display order (alphabetical, variants
                                  // adjacent), decoupled from the NVS enum
