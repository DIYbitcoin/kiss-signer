// Wallet UI translations. tr(STR_*) looks the key up in the active language's
// table (generated into i18n_tables.c from i18n/*.json by tools/gen_i18n.py).
// Scope: wallet screens and live home-tile labels. The game and baked KISS
// wordmark/tagline stay English by design (baked artwork + decoy story).
#pragma once
#include "i18n_keys.h"

// Order = i18n_tables[] = the NVS "lang" value. APPEND ONLY (see gen_i18n.py).
enum {
    I18N_EN = 0,
    I18N_DE,
    I18N_ES,
    I18N_FR,
    I18N_IT,
    I18N_JA,
    I18N_KO,
    I18N_NL,
    I18N_PL,
    I18N_PT,
    I18N_RU,
    I18N_TR,
    I18N_VI,
    I18N_ZH,
    I18N_ES_ES,   // appended (enum order = stored NVS value: append only)
    I18N_PT_PT,
    I18N_NB,
    I18N_SV,
    I18N_DA,
    I18N_CS,
    I18N_HR,
    I18N_LANG_N
};

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
