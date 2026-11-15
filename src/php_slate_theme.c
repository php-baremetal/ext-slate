/*
   +----------------------------------------------------------------------+
   | php-baremetal: ext-slate                                             |
   +----------------------------------------------------------------------+
   | Copyright (c) 2026 Gianfrancesco Aurecchia                           |
   +----------------------------------------------------------------------+
   | This source file is subject to the MIT license, that is bundled with |
   | this package in the file LICENSE.                                    |
   +----------------------------------------------------------------------+
   | Authors: Gianfrancesco Aurecchia <gianfri@php-baremetal.com>         |
   +----------------------------------------------------------------------+
*/

/*
 * The theme of the controls: the final class Theme (its tokens as public readonly properties, and as C fields for the
 * controls: sl_theme_t) and the enums Size, Variant, Shape and ChoiceStyle. A Slate starts with a Theme of the
 * defaults (sl_theme_default(), no check of the fonts: a firmware without the 'default' font gets no font, and its
 * texts draw nothing); Slate::theme() replaces it before the first show() (php_slate.c). A control reads the theme
 * of the running Slate when it is made (php_slate_control.c).
 *
 * The constructor checks every token in the order of its parameters and throws the \ValueError of the first one out
 * of range: a colour is 0xRRGGBB (0..0xFFFFFF), $radius 0..64, $small / $medium / $large 24..128, a font a name of
 * the firmware's fonts (extra.baremetal.fonts), as Text::$font.
 */
#include <string.h>

#include "php.h"
#include "zend_enum.h"
#include "zend_exceptions.h"

#include "php_slate.h"

#if SLATE_HAS_CONTROLS

zend_class_entry *sl_theme_ce, *sl_size_ce, *sl_variant_ce, *sl_shape_ce, *sl_choice_style_ce;

static zend_object_handlers theme_handlers;

#define N_COLORS 14
#define N_NUMS 18   /* the colours, radius, small, medium, large */
#define N_TOKENS 23

/* The tokens in the order of the constructor's parameters (and of the properties). */
static const char *const s_tok[N_TOKENS] = {
    "bg", "surface", "raised", "well", "border", "borderStrong", "text", "textMuted", "textDim", "accent", "onAccent",
    "danger", "info", "success", "radius", "small", "medium", "large", "font", "fontSmall", "fontTitle", "fontMono",
    "fontDisplay",
};
static const zend_long s_def[N_NUMS] = {
    0x0F1115, 0x161A20, 0x1C2129, 0x111419, 0x262C35, 0x343B46, 0xE8EAED, 0xA0A7B2, 0x5C6370, 0x4C8DFF, 0xFFFFFF,
    0xFF5D5D, 0x5CC8FF, 0x4ADE80, 12, 40, 48, 56,
};
static uint32_t s_off[N_TOKENS];

/* ---- enums ---- */

static const char *const size_names[] = { "Small", "Medium", "Large" };   /* slate_ctl_size_t order */

int sl_enum_case(const zval *v, const char *const *names, int n)
{
    if (Z_TYPE_P(v) != IS_OBJECT || !(Z_OBJCE_P(v)->ce_flags & ZEND_ACC_ENUM)) {
        return 0;
    }
    zval *name = zend_enum_fetch_case_name(Z_OBJ_P(v));
    for (int i = 0; i < n; i++) {
        if (Z_TYPE_P(name) == IS_STRING && strcmp(Z_STRVAL_P(name), names[i]) == 0) {
            return i;
        }
    }
    return 0;
}

int sl_size_of(const zval *v)
{
    return sl_enum_case(v, size_names, 3);
}

static zend_class_entry *pure_enum(const char *name, const char *const *cases, int n)
{
    zend_class_entry *ce = zend_register_internal_enum(name, IS_UNDEF, NULL);
    for (int i = 0; i < n; i++) {
        zend_enum_add_case_cstr(ce, cases[i], NULL);
    }
    return ce;
}

/* ---- Theme ---- */

/* Checks (check) and writes the tokens: nums (the colours, radius, small, medium, large) and fonts (5 names). false
   with the \ValueError of the first token out of range, nothing written. */
static bool theme_fill(sl_theme_t *t, const zend_long *nums, zend_string *const *fonts, bool check)
{
    const struct display_font_entry *f[5];
    for (int i = 0; i < N_NUMS && check; i++) {
        zend_long v = nums[i];
        if (i < N_COLORS && (v < 0 || v > 0xFFFFFF)) {
            zend_value_error("Baremetal\\Slate\\Theme::$%s must be a color, 0xRRGGBB", s_tok[i]);
            return false;
        }
        if (i == N_COLORS && (v < 0 || v > 64)) {
            zend_value_error("Baremetal\\Slate\\Theme::$radius must be between 0 and 64");
            return false;
        }
        if (i > N_COLORS && (v < 24 || v > 128)) {
            zend_value_error("Baremetal\\Slate\\Theme::$%s must be between 24 and 128", s_tok[i]);
            return false;
        }
    }
    for (int i = 0; i < 5; i++) {
        f[i] = sl_font_find(ZSTR_VAL(fonts[i]), ZSTR_LEN(fonts[i]));
        if (!f[i] && check) {
            sl_font_error("Baremetal\\Slate\\Theme", s_tok[N_NUMS + i], fonts[i]);
            return false;
        }
    }
    for (int i = 0; i < N_NUMS; i++) {
        zval *slot = OBJ_PROP(&t->std, s_off[i]);
        zval_ptr_dtor(slot);
        ZVAL_LONG(slot, nums[i]);
    }
    for (int i = 0; i < 5; i++) {
        zval *slot = OBJ_PROP(&t->std, s_off[N_NUMS + i]);
        zval_ptr_dtor(slot);
        ZVAL_STR_COPY(slot, fonts[i]);
    }
    for (int i = 0; i < N_COLORS; i++) {
        t->c[i] = (uint32_t)nums[i] & 0xFFFFFFu;
    }
    t->radius = (int)nums[N_COLORS];
    for (int i = 0; i < 3; i++) {
        t->size_h[i] = (int)nums[N_COLORS + 1 + i];
    }
    t->font = f[0];
    t->font_small = f[1];
    t->font_title = f[2];
    t->font_mono = f[3];
    t->font_display = f[4];
    return true;
}

static zend_string *s_default_font;

/* The C fields of the defaults, from the object's creation (the constructor writes the tokens given). */
static void theme_c_defaults(sl_theme_t *t)
{
    for (int i = 0; i < N_COLORS; i++) {
        t->c[i] = (uint32_t)s_def[i];
    }
    t->radius = (int)s_def[N_COLORS];
    for (int i = 0; i < 3; i++) {
        t->size_h[i] = (int)s_def[N_COLORS + 1 + i];
    }
    const struct display_font_entry *f = sl_font_find("default", 7);
    t->font = t->font_small = t->font_title = t->font_mono = t->font_display = f;
}

static zend_object *theme_create(zend_class_entry *ce)
{
    sl_theme_t *t = zend_object_alloc(sizeof(sl_theme_t), ce);
    zend_object_std_init(&t->std, ce);
    object_properties_init(&t->std, ce);
    t->std.handlers = &theme_handlers;
    theme_c_defaults(t);
    return &t->std;
}

void sl_theme_default(zval *rv)
{
    object_init_ex(rv, sl_theme_ce);
    zend_string *fonts[5] = { s_default_font, s_default_font, s_default_font, s_default_font, s_default_font };
    theme_fill(sl_theme_of(Z_OBJ_P(rv)), s_def, fonts, false);
}

PHP_METHOD(SlateTheme, __construct)
{
    zend_long n[N_NUMS];
    memcpy(n, s_def, sizeof n);
    zend_string *f[5] = { s_default_font, s_default_font, s_default_font, s_default_font, s_default_font };
    ZEND_PARSE_PARAMETERS_START(0, N_TOKENS)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(n[0]) Z_PARAM_LONG(n[1]) Z_PARAM_LONG(n[2]) Z_PARAM_LONG(n[3]) Z_PARAM_LONG(n[4])
        Z_PARAM_LONG(n[5]) Z_PARAM_LONG(n[6]) Z_PARAM_LONG(n[7]) Z_PARAM_LONG(n[8]) Z_PARAM_LONG(n[9])
        Z_PARAM_LONG(n[10]) Z_PARAM_LONG(n[11]) Z_PARAM_LONG(n[12]) Z_PARAM_LONG(n[13]) Z_PARAM_LONG(n[14])
        Z_PARAM_LONG(n[15]) Z_PARAM_LONG(n[16]) Z_PARAM_LONG(n[17])
        Z_PARAM_STR(f[0]) Z_PARAM_STR(f[1]) Z_PARAM_STR(f[2]) Z_PARAM_STR(f[3]) Z_PARAM_STR(f[4])
    ZEND_PARSE_PARAMETERS_END();
    zend_object *obj = Z_OBJ_P(ZEND_THIS);
    if (!Z_ISUNDEF_P(OBJ_PROP(obj, s_off[0]))) {
        zend_throw_error(NULL, "Cannot modify readonly property %s::$bg", ZSTR_VAL(obj->ce->name));
        RETURN_THROWS();
    }
    if (!theme_fill(sl_theme_of(obj), n, f, true)) {
        RETURN_THROWS();
    }
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_theme_ctor, 0, 0, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, bg, IS_LONG, 0, "0x0F1115")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, surface, IS_LONG, 0, "0x161A20")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, raised, IS_LONG, 0, "0x1C2129")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, well, IS_LONG, 0, "0x111419")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, border, IS_LONG, 0, "0x262C35")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, borderStrong, IS_LONG, 0, "0x343B46")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, text, IS_LONG, 0, "0xE8EAED")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, textMuted, IS_LONG, 0, "0xA0A7B2")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, textDim, IS_LONG, 0, "0x5C6370")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, accent, IS_LONG, 0, "0x4C8DFF")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, onAccent, IS_LONG, 0, "0xFFFFFF")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, danger, IS_LONG, 0, "0xFF5D5D")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, info, IS_LONG, 0, "0x5CC8FF")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, success, IS_LONG, 0, "0x4ADE80")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, radius, IS_LONG, 0, "12")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, small, IS_LONG, 0, "40")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, medium, IS_LONG, 0, "48")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, large, IS_LONG, 0, "56")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, font, IS_STRING, 0, "'default'")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, fontSmall, IS_STRING, 0, "'default'")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, fontTitle, IS_STRING, 0, "'default'")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, fontMono, IS_STRING, 0, "'default'")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, fontDisplay, IS_STRING, 0, "'default'")
ZEND_END_ARG_INFO()

static const zend_function_entry theme_methods[] = {
    PHP_ME(SlateTheme, __construct, arginfo_theme_ctor, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

void sl_theme_register(void)
{
    static const char *const variant_names[] = { "Primary", "Secondary", "Ghost", "Danger", "Outline" };
    static const char *const shape_names[] = { "Pill", "Rounded" };
    static const char *const style_names[] = { "Segmented", "Chips", "List", "Nav" };
    sl_size_ce = pure_enum("Baremetal\\Slate\\Size", size_names, 3);
    sl_variant_ce = pure_enum("Baremetal\\Slate\\Variant", variant_names, 5);
    sl_shape_ce = pure_enum("Baremetal\\Slate\\Shape", shape_names, 2);
    sl_choice_style_ce = pure_enum("Baremetal\\Slate\\ChoiceStyle", style_names, 4);

    s_default_font = zend_string_init_interned("default", 7, 1);
    memcpy(&theme_handlers, zend_get_std_object_handlers(), sizeof theme_handlers);
    theme_handlers.offset = XtOffsetOf(sl_theme_t, std);
    theme_handlers.clone_obj = NULL;
    zend_class_entry ce;
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "Theme", theme_methods);
    sl_theme_ce = zend_register_internal_class(&ce);
    sl_theme_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    sl_theme_ce->create_object = theme_create;
    for (int i = 0; i < N_TOKENS; i++) {
        zval undef;
        ZVAL_UNDEF(&undef);
        zend_type t = i < N_NUMS ? (zend_type)ZEND_TYPE_INIT_CODE(IS_LONG, 0, 0)
                                 : (zend_type)ZEND_TYPE_INIT_CODE(IS_STRING, 0, 0);
        zend_property_info *pi = zend_declare_typed_property(
            sl_theme_ce, zend_string_init_interned(s_tok[i], strlen(s_tok[i]), 1), &undef,
            ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL, t);
        s_off[i] = pi->offset;
    }
}

#endif
