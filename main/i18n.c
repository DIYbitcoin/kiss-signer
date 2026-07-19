// See i18n.h. Tables live in the generated i18n_tables.c.
#include "i18n.h"

#include <stdio.h>

static int s_lang = I18N_EN;

// native names deliberately in their own language/script: a user who switched
// to a language they can't read must still find their way back
static const i18n_lang_t LANGS[I18N_LANG_N] = {
    [I18N_EN] = {"en",    "ENGLISH",    I18N_FC_LAT},
    [I18N_DE] = {"de",    "DEUTSCH",    I18N_FC_LAT},
    [I18N_ES] = {"es-MX", "ESPAÑOL (MÉXICO)", I18N_FC_LAT},
    [I18N_FR] = {"fr",    "FRANÇAIS",   I18N_FC_LAT},
    [I18N_IT] = {"it",    "ITALIANO",   I18N_FC_LAT},
    [I18N_JA] = {"ja",    "日本語",      I18N_FC_JA},
    [I18N_KO] = {"ko",    "한국어",      I18N_FC_KO},
    [I18N_NL] = {"nl",    "NEDERLANDS", I18N_FC_LAT},
    [I18N_PL] = {"pl",    "POLSKI",     I18N_FC_LAT},
    [I18N_PT] = {"pt-BR", "PORTUGUÊS (BRASIL)", I18N_FC_LAT},
    [I18N_RU] = {"ru",    "РУССКИЙ",    I18N_FC_LAT},
    [I18N_TR] = {"tr",    "TÜRKÇE",     I18N_FC_LAT},
    [I18N_VI] = {"vi",    "TIẾNG VIỆT", I18N_FC_LAT},
    [I18N_ZH] = {"zh-CN", "中文",        I18N_FC_ZH},
    [I18N_ES_ES] = {"es-ES", "ESPAÑOL (ESPAÑA)",    I18N_FC_LAT},
    [I18N_PT_PT] = {"pt-PT", "PORTUGUÊS (PORTUGAL)", I18N_FC_LAT},
    [I18N_NB] = {"nb-NO", "NORSK BOKMÅL", I18N_FC_LAT},
    [I18N_SV] = {"sv-SE", "SVENSKA",      I18N_FC_LAT},
    [I18N_DA] = {"da-DK", "DANSK",        I18N_FC_LAT},
    [I18N_CS] = {"cs-CZ", "ČEŠTINA",      I18N_FC_LAT},
    [I18N_HR] = {"hr-HR", "HRVATSKI",     I18N_FC_LAT},
};

void i18n_set_lang(int lang)
{
    s_lang = (lang >= 0 && lang < I18N_LANG_N) ? lang : I18N_EN;
}

int i18n_get_lang(void) { return s_lang; }

const i18n_lang_t *i18n_lang_info(int lang)
{
    return &LANGS[(lang >= 0 && lang < I18N_LANG_N) ? lang : I18N_EN];
}

const char *tr(int id)
{
    if (id < 0 || id >= STR_N) return "";
    return i18n_tables[s_lang][id];
}

// LV_SYMBOL_* + translated text in one string. Rotating buffers: labels copy
// the text on set, so each result only has to live until the next 4 calls.
const char *tr_sym(const char *sym, int id)
{
    static char buf[4][192];
    static int i;
    i = (i + 1) & 3;
    snprintf(buf[i], sizeof buf[i], "%s  %s", sym, tr(id));
    return buf[i];
}
