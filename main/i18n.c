// See i18n.h. Tables live in the generated i18n_tables.c.
#include "i18n.h"

#include <stdio.h>

static int s_lang = I18N_EN;

void i18n_set_lang(int lang)
{
    s_lang = (lang >= 0 && lang < I18N_LANG_N) ? lang : I18N_EN;
}

int i18n_get_lang(void) { return s_lang; }

const i18n_lang_t *i18n_lang_info(int lang)
{
    return &i18n_langs[(lang >= 0 && lang < I18N_LANG_N) ? lang : I18N_EN];
}

const char *tr(int id)
{
    if (id < 0 || id >= STR_N) return "";
    // Fall back to English rather than returning the NULL a missing designated
    // initializer leaves behind. A key added to i18n_keys.h and filled in for
    // English only used to hand every other locale a NULL, and the first label
    // to receive it took the renderer down -- so the cost of shipping a new
    // string was 21 translations or a crash, with nothing in between. English
    // in one locale is a visible gap somebody can fix; a hang is not.
    const char *s = i18n_tables[s_lang][id];
    return s ? s : (i18n_tables[I18N_EN][id] ? i18n_tables[I18N_EN][id] : "");
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
