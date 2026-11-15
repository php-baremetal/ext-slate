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
 * Dialog (a Control, php_slate_control.c): a modal panel over a scrim, opened above a scene by Scene::open() (it
 * becomes the scene's last child) and closed by close(). The group sits at (0, 0) and measures the screen; its parts,
 * bottom to top:
 *   - scrim: a ROUNDRECT over the whole screen, black at 88 % (0xE0000000). The Dialog is a touchable (Control's
 *     default) whose area is the screen: no touch reaches what is under it, nor Scene::onTouch();
 *   - panel: a GROUP at the panel's top-left (slate_ctl_panel()), holding
 *       - pbg: a ROUNDRECT of the panel, radius 26, surface, a border of 1 in borderStrong, a shadow (0, 30) of blur 30
 *         in black at 60 % (none when shadows are not built);
 *       - body: a GROUP that clips to the panel (the shadow of pbg stays outside the clip), holding the title (a TEXT
 *         in fontTitle, text), one TEXT in font and textMuted per line of $text (split at "\n"), content (a GROUP,
 *         the parent of the PHP children of add()) and btns (a GROUP, the parent of the Buttons of buttons(), each
 *         given its width and place by the dialog: one row of equal widths, or a column with $stack).
 * The layout is made again at open(), buttons(), add() and remove() (the content's height from sl_obj_extent rules,
 * sl_obj_box of each child). Opening: the panel's caches (its shadow) are built first (sl_part_prepare: the
 * animation starts once they are, so its first frame is not a long one), then the scrim's opacity 0 -> 1 and the
 * panel up 14 px with its opacity 0 -> 1, in 300 ms (OutCubic), two internal tweens (SL_TWEEN_CTL). The panel is a
 * flat group (SLATE_PROP_FLAT, scene.h): it is not composed apart, each of its elements is drawn with its own opacity
 * times the panel's (composing a group of this size apart at every frame took about twice as long); close(): the same
 * backwards, the panel's tween with
 * SL_TWEEN_CTL_CB and a waiter (sl_ctl_wait): at its end (the watch of HoldDue) the dialog leaves its scene, then
 * onClose() and the callback of closed() run. A dialog whose scene is not shown (or that is not drawn, or whose
 * $visible is false) closes at once, without the animation; so does one that Scene::remove() takes out, and one hidden
 * while it closes (all still call onClose() and closed()). A hidden open dialog is not the top one (sl_dialog_top()):
 * it blocks no touch.
 * $dismissable: a Tap on the scrim (outside the panel) closes it.
 */
#include <string.h>

#include "php.h"
#include "zend_closures.h"
#include "zend_exceptions.h"
#include "ext/spl/spl_exceptions.h"

#include "controls.h"
#include "php_slate.h"
#include "raster.h"

#if SLATE_HAS_CONTROLS

zend_class_entry *sl_dialog_ce;
static uint32_t s_off_dismissable;
static int s_open;   /* dialogs open (Scene::add() looks for them only when there are some) */
static sl_obj_t *s_opening;   /* the dialog Scene::open() is adding (the one add() lets in) */

#define SCRIM_ARGB 0xE0000000u      /* black at 88 % */
#define SHADOW_ARGB 0x99000000u     /* black at 60 % */
#define SHADOW_BLUR 30              /* the shadow's blur: one of 80 took twice as long to build and to draw */
#define PANEL_RADIUS 26
#define LINES_MAX 100      /* lines of $text (a part each: 127 parts at most), and buttons */

typedef struct {
    zend_string *title;
    zend_string **lines;   /* the lines of $text (emalloc) */
    int nlines;
    int wgiven;
    bool stack;
    bool open;             /* opened, its close not ended */
    bool closing;          /* close() animates */
    int scrim, panel, pbg, body, title_p, line0, content, btns;   /* part indexes */
    zval buttons;          /* the Buttons of buttons() (an array; UNDEF: none) */
    uint32_t tw[2];        /* the last internal tweens of the scrim and the panel (0: none) */
    uint32_t tclose;       /* the panel's close tween, waited for (0: none) */
    bool laid;             /* p, sw, sh hold a layout sent */
    slate_ctl_panel_t p;
    int sw, sh;
} dialog_t;

static dialog_t *dlg_of(const sl_obj_t *o)
{
    return o->ctl && o->base == SL_DIALOG ? (dialog_t *)o->ctl->st : NULL;
}

static int line_height(const struct display_font_entry *f)
{
    return f ? slate_text_bbox(0, 0, " ", f, SLATE_ALIGN_LEFT, 0).y1 : 0;
}

static bool is_button(const dialog_t *d, const sl_obj_t *c)
{
    if (Z_TYPE(d->buttons) != IS_ARRAY) {
        return false;
    }
    zval *zv;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL(d->buttons), zv) {
        if (Z_OBJ_P(zv) == &c->std) {
            return true;
        }
    } ZEND_HASH_FOREACH_END();
    return false;
}

/* The size laid out: ctl->w / h, the $w / $h slots and, realized and not at the realization, the group's W and H. */
static bool set_size(sl_obj_t *o, int w, int h, bool all)
{
    o->ctl->w = (int16_t)w;
    o->ctl->h = (int16_t)h;
    zval *zw = sl_slot(o, "w"), *zh = sl_slot(o, "h");
    if (!zw || !zh || (Z_TYPE_P(zw) == IS_LONG && Z_LVAL_P(zw) == w && Z_TYPE_P(zh) == IS_LONG && Z_LVAL_P(zh) == h)) {
        return true;
    }
    zval_ptr_dtor(zw);
    ZVAL_LONG(zw, w);
    zval_ptr_dtor(zh);
    ZVAL_LONG(zh, h);
    if (!all && sl_realized(o)) {
        slate_cmd_t cw = { .kind = SLATE_CMD_SET, .id = o->id, .prop = SLATE_PROP_W, .v = slate_v_num((float)w) };
        slate_cmd_t ch = { .kind = SLATE_CMD_SET, .id = o->id, .prop = SLATE_PROP_H, .v = slate_v_num((float)h) };
        return sl_push(&cw) && sl_push(&ch);
    }
    return true;
}

static bool num(sl_obj_t *o, int part, slate_prop_t prop, float v)
{
    return sl_part_set(o, part, prop, slate_v_num(v));
}

/* The look of an open dialog at rest: the scrim and the panel shown, the panel in place. */
static bool shown_look(sl_obj_t *o, dialog_t *d)
{
    return num(o, d->scrim, SLATE_PROP_OPACITY, 1) && num(o, d->panel, SLATE_PROP_Y, (float)d->p.y)
           && num(o, d->panel, SLATE_PROP_OPACITY, 1);
}

/* Lays the dialog out (the screen, the content, the buttons) and sends the geometry when it changed (all: everything,
   the realization: the PHP children are placed already). false with an exception. */
static bool layout(sl_obj_t *o, bool all)
{
    dialog_t *d = dlg_of(o);
    sl_theme_t *th = sl_ctl_theme(o);
    if (!d || !th) {
        return true;
    }
    int sw = d->sw, sh = d->sh;
    sl_screen(&sw, &sh);
    int title_lh = ZSTR_LEN(d->title) ? line_height(th->font_title) : 0;
    int line_h = line_height(th->font);
    int content_h = 0, n = 0;
    int bh[LINES_MAX];
    zval *zv;
    if (o->has_children) {
        ZEND_HASH_FOREACH_VAL(&o->children, zv) {
            sl_obj_t *c = sl_obj(Z_OBJ_P(zv));
            int x0, y0, x1, y1;
            if (!is_button(d, c) && sl_obj_box(c, &x0, &y0, &x1, &y1) && y1 > content_h) {
                content_h = y1;
            }
        } ZEND_HASH_FOREACH_END();
    }
    if (Z_TYPE(d->buttons) == IS_ARRAY) {
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL(d->buttons), zv) {
            sl_obj_t *b = sl_obj(Z_OBJ_P(zv));
            if (n < LINES_MAX) {
                bh[n++] = SL_CTL(b) ? b->ctl->h : 0;
            }
        } ZEND_HASH_FOREACH_END();
    }
    slate_ctl_panel_t P = slate_ctl_panel(d->wgiven, sw, sh, title_lh, d->nlines, line_h, content_h, n, bh, d->stack);
    if (!set_size(o, sw, sh, all)) {
        return false;
    }
    if (!all && n > 0) {
        /* the buttons: their width and place in btns (writes, as the code would make them) */
        int inner = P.w - 2 * SLATE_CTL_DIALOG_PAD;
        slate_ctl_box_t boxes[LINES_MAX];
        slate_ctl_dialog_buttons(inner < 1 ? 1 : inner, n, bh, d->stack, boxes);
        int i = 0;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL(d->buttons), zv) {
            sl_obj_t *b = sl_obj(Z_OBJ_P(zv));
            if (i < n && (!sl_button_width(b, boxes[i].w) || !sl_write_num(b, "x", boxes[i].x)
                          || !sl_write_num(b, "y", boxes[i].y))) {
                return false;
            }
            i++;
        } ZEND_HASH_FOREACH_END();
    }
    bool moved = all || !d->laid || memcmp(&P, &d->p, sizeof P) != 0 || sw != d->sw || sh != d->sh;
    d->p = P;
    d->sw = sw;
    d->sh = sh;
    d->laid = true;
    if (!moved || !sl_realized(o)) {
        return true;
    }
    const uint32_t *t = th->c;
    const float pad = SLATE_CTL_DIALOG_PAD;
    if (all) {
        if (!sl_part_set(o, d->scrim, SLATE_PROP_FILL, slate_v_color(SCRIM_ARGB))
            || !num(o, d->pbg, SLATE_PROP_RADIUS, PANEL_RADIUS)
            || !sl_part_set(o, d->pbg, SLATE_PROP_FILL, slate_v_color(sl_rgb(t[SL_T_SURFACE])))
            || !num(o, d->pbg, SLATE_PROP_BORDER, 1)
            || !sl_part_set(o, d->pbg, SLATE_PROP_BORDER_COLOR, slate_v_color(sl_rgb(t[SL_T_BORDER_STRONG])))
#if SLATE_HAS_SHADOWS
            || !sl_part_set(o, d->pbg, SLATE_PROP_SHADOW, slate_v_shadow(0, 30, sl_blur_radius(SHADOW_BLUR), SHADOW_ARGB))
#endif
            || !sl_part_set(o, d->body, SLATE_PROP_CLIP, slate_v_bool(true))
            || !sl_part_set(o, d->panel, SLATE_PROP_FLAT, slate_v_bool(true))
            || !num(o, d->title_p, SLATE_PROP_X, pad)
            || !sl_part_set(o, d->title_p, SLATE_PROP_TEXT, slate_v_text(ZSTR_VAL(d->title)))
            || !sl_part_set(o, d->title_p, SLATE_PROP_FONT, slate_v_font(th->font_title))
            || !sl_part_set(o, d->title_p, SLATE_PROP_COLOR, slate_v_color(sl_rgb(t[SL_T_TEXT])))
            || !num(o, d->content, SLATE_PROP_X, pad) || !num(o, d->btns, SLATE_PROP_X, pad)) {
            return false;
        }
        for (int i = 0; i < d->nlines; i++) {
            int p = d->line0 + i;
            if (!num(o, p, SLATE_PROP_X, pad)
                || !sl_part_set(o, p, SLATE_PROP_TEXT, slate_v_text(ZSTR_VAL(d->lines[i])))
                || !sl_part_set(o, p, SLATE_PROP_FONT, slate_v_font(th->font))
                || !sl_part_set(o, p, SLATE_PROP_COLOR, slate_v_color(sl_rgb(t[SL_T_TEXT_MUTED])))) {
                return false;
            }
        }
    }
    if (!num(o, d->scrim, SLATE_PROP_W, (float)sw) || !num(o, d->scrim, SLATE_PROP_H, (float)sh)
        || !num(o, d->panel, SLATE_PROP_X, (float)P.x) || !num(o, d->pbg, SLATE_PROP_W, (float)P.w)
        || !num(o, d->pbg, SLATE_PROP_H, (float)P.h) || !num(o, d->body, SLATE_PROP_W, (float)P.w)
        || !num(o, d->body, SLATE_PROP_H, (float)P.h) || !num(o, d->title_p, SLATE_PROP_Y, (float)P.title_y)
        || !num(o, d->content, SLATE_PROP_Y, (float)P.content_y)
        || !num(o, d->btns, SLATE_PROP_Y, (float)P.buttons_y)) {
        return false;
    }
    for (int i = 0; i < d->nlines; i++) {
        if (!num(o, d->line0 + i, SLATE_PROP_Y, (float)(P.text_y + i * line_h))) {
            return false;
        }
    }
    /* the panel's place and the opacities: at the realization the look at rest; later only while no animation runs
       (a SET would stop it) */
    if (all || !d->closing) {
        return shown_look(o, d);
    }
    return true;
}

/* ---- open and close ---- */

/* The internal tweens of the dialog stop (they would wait, paused, in a dialog taken out of its scene). */
static void stop_tweens(dialog_t *d)
{
    for (int i = 0; i < 2; i++) {
        sl_part_tween_stop(d->tw[i], false);   /* an exception (no memory for it) is the caller's */
        d->tw[i] = 0;
    }
    if (d->tclose) {
        sl_ctl_unwait(d->tclose);
        d->tclose = 0;
    }
}

/* The dialog is closed: out of its scene (remove), then onClose() and the callback of closed(). */
static void finish(sl_obj_t *o, bool remove)
{
    dialog_t *d = dlg_of(o);
    if (!d || !d->open) {
        return;
    }
    d->open = d->closing = false;
    s_open--;
    stop_tweens(d);
    GC_ADDREF(&o->std);
    if (remove && o->parent && !EG(exception)) {
        zval zv;
        ZVAL_OBJ(&zv, &o->std);
        sl_children_remove(o->parent, &zv, 1);
    }
    if (!EG(exception)) {
        zend_function *fn = zend_hash_str_find_ptr(&o->std.ce->function_table, "onclose", sizeof("onclose") - 1);
        if (fn && !(fn->type == ZEND_INTERNAL_FUNCTION && fn->common.scope == sl_dialog_ce)) {
            zend_call_known_instance_method_with_0_params(fn, &o->std, NULL);
        }
    }
    if (!EG(exception) && o->ctl && Z_TYPE(o->ctl->cb2) == IS_OBJECT) {
        zval cb, rv;
        ZVAL_COPY(&cb, &o->ctl->cb2);   /* kept alive through the call (closed(null) inside it) */
        ZVAL_UNDEF(&rv);
        call_user_function(NULL, NULL, &cb, &rv, 0, NULL);
        zval_ptr_dtor(&rv);
        zval_ptr_dtor(&cb);
    }
    OBJ_RELEASE(&o->std);
}

/* The end of the close tween (sl_ctl_due_run), or the Slate stopped (stopped: no PHP call, the dialog leaves its scene
   quietly). */
static void dialog_due(zend_object *obj, uint32_t id, bool stopped)
{
    sl_obj_t *o = sl_obj(obj);
    dialog_t *d = dlg_of(o);
    if (!d || !d->closing || d->tclose != id) {
        return;
    }
    d->tclose = 0;   /* its waiter is running */
    if (!stopped) {
        finish(o, true);
        return;
    }
    d->open = d->closing = false;
    s_open--;
    d->tw[0] = d->tw[1] = 0;   /* gone with the engine */
    if (o->parent) {
        zval zv;
        ZVAL_OBJ(&zv, &o->std);
        sl_children_remove(o->parent, &zv, 1);   /* no engine: no command */
    }
}

static bool tween(sl_obj_t *o, int part, float y, float opacity, bool with_y, bool cb, uint32_t *id)
{
    slate_tween_prop_t props[2] = {
        { .prop = SLATE_PROP_OPACITY, .type = SLATE_VAL_NUM, .to.num = opacity },
        { .prop = SLATE_PROP_Y, .type = SLATE_VAL_NUM, .to.num = y },
    };
    return sl_part_tween(o, part, props, with_y ? 2 : 1, SLATE_CTL_DIALOG_MS, 0, SLATE_EASE_OUT_CUBIC, 0, 0, cb, id);
}

static void dialog_close(sl_obj_t *o)
{
    dialog_t *d = dlg_of(o);
    if (!d || !d->open || d->closing) {
        return;   /* not open, or closing already */
    }
    zend_object *shown = sl_shown_scene();
    if (!o->parent || !sl_realized(o) || !shown || &o->parent->std != shown || !sl_slot_true(o, "visible")) {
        finish(o, true);   /* its scene is not shown, it is not drawn or it is hidden: at once, no animation */
        return;
    }
    if (!sl_ctl_due_ready()) {
        return;   /* no memory for the wait: nothing sent, the dialog stays open */
    }
    const float rise = SLATE_CTL_DIALOG_RISE;
    uint32_t id;
    if (!tween(o, d->scrim, 0, 0, false, false, &d->tw[0])
        || !tween(o, d->panel, (float)d->p.y + rise, 0, true, true, &id)) {
        return;
    }
    if (!id) {
        finish(o, true);
        return;
    }
    d->tw[1] = id;
    d->tclose = id;
    d->closing = true;
    sl_ctl_wait(id, &o->std, dialog_due, SLATE_CTL_DIALOG_MS);   /* cannot fail: sl_ctl_due_ready() above */
    sl_touch_recheck();   /* nothing in it takes a touch now: a finger held on its content gets Cancel */
}

bool sl_dialog_open(sl_obj_t *scene, sl_obj_t *o)
{
    dialog_t *d = dlg_of(o);
    if (!d) {
        zend_throw_error(NULL, "%s is not constructed", ZSTR_VAL(o->std.ce->name));
        return false;
    }
    if (d->open && !d->closing && !o->parent) {
        /* its scene was freed while it was open: it is closed (quietly: nothing was shown of it) */
        d->open = false;
        s_open--;
        stop_tweens(d);
    }
    if (d->open || o->parent) {
        zend_throw_error(NULL, "%s is already open", ZSTR_VAL(o->std.ce->name));
        return false;
    }
    zval zv;
    ZVAL_OBJ(&zv, &o->std);
    s_opening = o;
    sl_children_add(scene, &zv, 1);
    s_opening = NULL;
    if (EG(exception)) {
        return false;
    }
    d->open = true;
    d->closing = false;
    s_open++;
    if (!layout(o, false)) {
        return false;
    }
    if (!sl_realized(o)) {
        return true;   /* drawn with its scene, at rest */
    }
    stop_tweens(d);
    if (!sl_realized(scene)) {
        return shown_look(o, d);   /* (never: a realized dialog is in a realized scene) */
    }
    /* from the start values, in the same batch: the scrim and the panel clear, the panel 14 px lower; its shadow built,
       then the tweens (they start once it is) */
    const float rise = SLATE_CTL_DIALOG_RISE;
    if (!num(o, d->scrim, SLATE_PROP_OPACITY, 0) || !num(o, d->panel, SLATE_PROP_Y, (float)d->p.y + rise)
        || !num(o, d->panel, SLATE_PROP_OPACITY, 0) || !sl_part_prepare(o, d->panel)
        || !tween(o, d->scrim, 0, 1, false, false, &d->tw[0])
        || !tween(o, d->panel, (float)d->p.y, 1, true, false, &d->tw[1])) {
        return false;
    }
    if (sl_shown_scene() == &scene->std) {
        sl_touch_cancel_held();   /* the scrim covers whatever held the finger: it gets Cancel */
    }
    return !EG(exception);
}

void sl_dialog_raise(sl_obj_t *scene)
{
    if (s_open <= 0 || !scene->has_children) {
        return;
    }
    uint32_t n = 0, cap = (uint32_t)s_open;
    sl_obj_t **open = emalloc(cap * sizeof *open);
    zval *zv;
    ZEND_HASH_FOREACH_VAL(&scene->children, zv) {
        sl_obj_t *c = sl_obj(Z_OBJ_P(zv));
        if (n < cap && sl_dialog_is_open(c)) {
            open[n++] = c;
        }
    } ZEND_HASH_FOREACH_END();
    for (uint32_t i = 0; i < n && !EG(exception); i++) {
        sl_children_raise(scene, open[i]);   /* in their order: the last opened stays on top */
    }
    efree(open);
}

bool sl_dialog_add_ok(sl_obj_t *c)
{
    if (c == s_opening) {
        return true;
    }
    zend_throw_error(NULL, "%s is opened with Scene::open(), not added", ZSTR_VAL(c->std.ce->name));
    return false;
}

bool sl_dialog_leave(sl_obj_t *scene)
{
    if (s_open <= 0 || !scene->has_children) {
        return true;
    }
    uint32_t n = 0, cap = (uint32_t)s_open;
    sl_obj_t **closing = emalloc(cap * sizeof *closing);
    zval *zv;
    ZEND_HASH_FOREACH_VAL(&scene->children, zv) {
        sl_obj_t *c = sl_obj(Z_OBJ_P(zv));
        dialog_t *d = dlg_of(c);
        if (n < cap && d && d->open && d->closing) {
            GC_ADDREF(&c->std);
            closing[n++] = c;
        }
    } ZEND_HASH_FOREACH_END();
    for (uint32_t i = 0; i < n; i++) {
        if (!EG(exception)) {
            finish(closing[i], true);   /* at once: removed, onClose(), closed() */
        }
        OBJ_RELEASE(&closing[i]->std);
    }
    efree(closing);
    return !EG(exception);
}

bool sl_dialog_is_open(const sl_obj_t *o)
{
    const dialog_t *d = dlg_of(o);
    return d && d->open;
}

bool sl_dialog_closing(const sl_obj_t *o)
{
    const dialog_t *d = dlg_of(o);
    return d && d->open && d->closing;
}

sl_obj_t *sl_dialog_top(zend_object *scene)
{
    if (s_open <= 0 || !scene) {
        return NULL;
    }
    sl_obj_t *so = sl_obj(scene);
    if (!so->has_children) {
        return NULL;
    }
    zval *zv;
    ZEND_HASH_REVERSE_FOREACH_VAL(&so->children, zv) {
        sl_obj_t *c = sl_obj(Z_OBJ_P(zv));
        if (sl_dialog_is_open(c) && sl_slot_true(c, "visible")) {
            return c;   /* open dialogs are kept last (sl_dialog_raise): usually the first one looked at */
        }
    } ZEND_HASH_FOREACH_END();
    return NULL;
}

bool sl_dialog_hidden(sl_obj_t *o)
{
    if (sl_dialog_closing(o)) {
        finish(o, true);   /* its close tween would wait, paused, for ever: it ends now */
    }
    return !EG(exception);
}

bool sl_dialog_removed(sl_obj_t *o)
{
    finish(o, false);
    return !EG(exception);
}

/* ---- ops ---- */

static bool dialog_realize(sl_obj_t *o)
{
    return layout(o, true);
}

static bool dialog_touch(sl_obj_t *o, int kind, const sl_touch_t *p, int lx, int ly)
{
    (void)p;
    dialog_t *d = dlg_of(o);
    if (!d || kind != SL_TOUCH_TAP || Z_TYPE_P(OBJ_PROP(&o->std, s_off_dismissable)) != IS_TRUE) {
        return true;
    }
    const slate_ctl_panel_t *P = &d->p;
    if (lx >= P->x && lx < P->x + P->w && ly >= P->y && ly < P->y + P->h) {
        return true;   /* on the panel */
    }
    dialog_close(o);
    return !EG(exception);
}

static bool dialog_write(sl_obj_t *o, const char *name, zval *slot, zval *old)
{
    (void)o;
    (void)name;
    (void)slot;
    (void)old;
    return true;   /* $dismissable: read at the next Tap */
}

static uint32_t dialog_child_parent(sl_obj_t *o, sl_obj_t *child)
{
    dialog_t *d = dlg_of(o);
    if (!d) {
        return 0;
    }
    return o->ctl->parts[is_button(d, child) ? d->btns : d->content].id;
}

static void dialog_gc(sl_obj_t *o, zend_get_gc_buffer *buf)
{
    dialog_t *d = dlg_of(o);
    if (d && Z_TYPE(d->buttons) == IS_ARRAY) {
        zend_get_gc_buffer_add_zval(buf, &d->buttons);
    }
}

static void dialog_free(sl_obj_t *o)
{
    dialog_t *d = o->ctl->st;
    if (d->open) {
        s_open--;
    }
    if (d->tclose) {
        sl_ctl_unwait(d->tclose);   /* never: the waiter holds the dialog */
    }
    zend_string_release(d->title);
    for (int i = 0; i < d->nlines; i++) {
        zend_string_release(d->lines[i]);
    }
    if (d->lines) {
        efree(d->lines);
    }
    zval_ptr_dtor(&d->buttons);
    efree(d);
}

static const sl_ctl_ops_t dialog_ops = {
    .realize = dialog_realize,
    .touch = dialog_touch,
    .write = dialog_write,
    .child_parent = dialog_child_parent,
    .gc = dialog_gc,
    .free = dialog_free,
};

/* ---- Dialog ---- */

PHP_METHOD(SlateDialog, __construct)
{
    zend_string *title = NULL, *text = NULL;
    zend_long w = 460;
    bool stack = false;
    ZEND_PARSE_PARAMETERS_START(0, 4)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR(title)
        Z_PARAM_STR(text)
        Z_PARAM_LONG(w)
        Z_PARAM_BOOL(stack)
    ZEND_PARSE_PARAMETERS_END();
    zend_object *obj = Z_OBJ_P(ZEND_THIS);
    sl_obj_t *o = sl_obj(obj);
    const char *cls = ZSTR_VAL(obj->ce->name);
    if (!sl_ctl_need_slate(o)) {
        RETURN_THROWS();
    }
    if (!o->ctl || o->ctl->st) {
        zend_throw_error(NULL, "%s is already constructed", cls);
        RETURN_THROWS();
    }
    if (w < 1 || w > 32767) {
        zend_value_error("%s::$w must be between 1 and 32767", cls);
        RETURN_THROWS();
    }
    dialog_t *d = ecalloc(1, sizeof *d);
    ZVAL_UNDEF(&d->buttons);
    d->title = title ? zend_string_copy(title) : ZSTR_EMPTY_ALLOC();
    d->wgiven = (int)w;
    d->stack = stack;
    if (text && ZSTR_LEN(text)) {
        const char *s = ZSTR_VAL(text), *end = s + ZSTR_LEN(text);
        int n = 1;
        for (const char *c = s; c < end; c++) {
            n += *c == '\n';
        }
        if (n > LINES_MAX) {
            zend_string_release(d->title);
            efree(d);
            zend_value_error("%s::$text must hold at most %d lines", cls, LINES_MAX);
            RETURN_THROWS();
        }
        d->lines = emalloc((size_t)n * sizeof *d->lines);
        for (const char *a = s;; ) {
            const char *nl = memchr(a, '\n', (size_t)(end - a));
            const char *e = nl ? nl : end;
            d->lines[d->nlines++] = zend_string_init(a, (size_t)(e - a), 0);
            if (!nl) {
                break;
            }
            a = nl + 1;
        }
    }
    o->ctl->st = d;
    d->scrim = sl_part_add(o, SLATE_EL_ROUNDRECT, -1);
    d->panel = sl_part_add(o, SLATE_EL_GROUP, -1);
    d->pbg = sl_part_add(o, SLATE_EL_ROUNDRECT, d->panel);
    d->body = sl_part_add(o, SLATE_EL_GROUP, d->panel);
    d->title_p = sl_part_add(o, SLATE_EL_TEXT, d->body);
    d->line0 = d->title_p + 1;
    for (int i = 0; i < d->nlines && !EG(exception); i++) {
        sl_part_add(o, SLATE_EL_TEXT, d->body);
    }
    d->content = sl_part_add(o, SLATE_EL_GROUP, d->body);
    d->btns = sl_part_add(o, SLATE_EL_GROUP, d->body);
    if (EG(exception) || !layout(o, false)) {
        RETURN_THROWS();
    }
}

/* Element ...$e of buttons(), add(), remove(): Buttons for buttons() (only), Elements for the others. */
static bool check_args(zval *args, uint32_t argc, zend_class_entry *ce, const char *type)
{
    for (uint32_t i = 0; i < argc; i++) {
        if (Z_TYPE(args[i]) != IS_OBJECT || !instanceof_function(Z_OBJCE(args[i]), ce)) {
            zend_argument_type_error(i + 1, "must be of type %s, %s given", type, zend_zval_value_name(&args[i]));
            return false;
        }
    }
    return true;
}

PHP_METHOD(SlateDialog, buttons)
{
    zval *args = NULL;
    uint32_t argc = 0;
    ZEND_PARSE_PARAMETERS_START(0, -1)
        Z_PARAM_VARIADIC('*', args, argc)
    ZEND_PARSE_PARAMETERS_END();
    sl_obj_t *o = sl_obj(Z_OBJ_P(ZEND_THIS));
    dialog_t *d = dlg_of(o);
    zend_class_entry *bce = zend_hash_str_find_ptr(CG(class_table), "baremetal\\slate\\button",
                                                   sizeof("baremetal\\slate\\button") - 1);
    if (!check_args(args, argc, bce, "Baremetal\\Slate\\Button")) {
        RETURN_THROWS();
    }
    if (!d) {
        zend_throw_error(NULL, "%s is not constructed", ZSTR_VAL(o->std.ce->name));
        RETURN_THROWS();
    }
    if (argc > LINES_MAX) {
        zend_value_error("%s::buttons(): at most %d buttons", ZSTR_VAL(o->std.ce->name), LINES_MAX);
        RETURN_THROWS();
    }
    for (uint32_t i = 0; i < argc; i++) {
        sl_obj_t *b = sl_obj(Z_OBJ(args[i]));
        if (b->parent && !(b->parent == o && is_button(d, b))) {
            zend_throw_exception_ex(spl_ce_LogicException, 0,
                                    "%s already belongs to a group: remove() it from there first",
                                    ZSTR_VAL(b->std.ce->name));
            RETURN_THROWS();
        }
    }
    /* the old buttons go (those given again come back below, in the new order) */
    zval old;
    ZVAL_COPY_VALUE(&old, &d->buttons);
    ZVAL_UNDEF(&d->buttons);
    if (Z_TYPE(old) == IS_ARRAY) {
        zval *zv;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL(old), zv) {
            sl_children_remove(o, zv, 1);
        } ZEND_HASH_FOREACH_END();
    }
    zval_ptr_dtor(&old);
    if (EG(exception)) {
        RETURN_THROWS();
    }
    array_init_size(&d->buttons, argc);
    for (uint32_t i = 0; i < argc; i++) {
        Z_ADDREF(args[i]);
        add_next_index_zval(&d->buttons, &args[i]);
    }
    sl_children_add(o, args, argc);   /* each to btns (dialog_child_parent) */
    if (EG(exception) || !layout(o, false)) {
        RETURN_THROWS();
    }
    RETURN_OBJ_COPY(&o->std);
}

PHP_METHOD(SlateDialog, add)
{
    zval *args = NULL;
    uint32_t argc = 0;
    ZEND_PARSE_PARAMETERS_START(0, -1)
        Z_PARAM_VARIADIC('*', args, argc)
    ZEND_PARSE_PARAMETERS_END();
    sl_obj_t *o = sl_obj(Z_OBJ_P(ZEND_THIS));
    if (!check_args(args, argc, sl_element_ce, "Baremetal\\Slate\\Element")) {
        RETURN_THROWS();
    }
    sl_children_add(o, args, argc);
    if (EG(exception) || !layout(o, false)) {
        RETURN_THROWS();
    }
    RETURN_OBJ_COPY(&o->std);
}

PHP_METHOD(SlateDialog, remove)
{
    zval *args = NULL;
    uint32_t argc = 0;
    ZEND_PARSE_PARAMETERS_START(0, -1)
        Z_PARAM_VARIADIC('*', args, argc)
    ZEND_PARSE_PARAMETERS_END();
    sl_obj_t *o = sl_obj(Z_OBJ_P(ZEND_THIS));
    dialog_t *d = dlg_of(o);
    if (!check_args(args, argc, sl_element_ce, "Baremetal\\Slate\\Element")) {
        RETURN_THROWS();
    }
    if (d && Z_TYPE(d->buttons) == IS_ARRAY) {
        /* a Button of buttons() removed: out of the list too */
        for (uint32_t i = 0; i < argc; i++) {
            zend_ulong idx;
            zval *zv;
            ZEND_HASH_FOREACH_NUM_KEY_VAL(Z_ARRVAL(d->buttons), idx, zv) {
                if (Z_OBJ_P(zv) == Z_OBJ(args[i])) {
                    zend_hash_index_del(Z_ARRVAL(d->buttons), idx);
                    break;
                }
            } ZEND_HASH_FOREACH_END();
        }
    }
    sl_children_remove(o, args, argc);
    if (EG(exception) || !layout(o, false)) {
        RETURN_THROWS();
    }
    RETURN_OBJ_COPY(&o->std);
}

PHP_METHOD(SlateDialog, close)
{
    ZEND_PARSE_PARAMETERS_NONE();
    sl_anim_flush();
    dialog_close(sl_obj(Z_OBJ_P(ZEND_THIS)));
    if (EG(exception)) {
        RETURN_THROWS();
    }
}

PHP_METHOD(SlateDialog, onClose)
{
    ZEND_PARSE_PARAMETERS_NONE();
}

PHP_METHOD(SlateDialog, closed)
{
    zval *fn = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS_OR_NULL(fn, zend_ce_closure)
    ZEND_PARSE_PARAMETERS_END();
    sl_obj_t *o = sl_obj(Z_OBJ_P(ZEND_THIS));
    if (o->ctl) {
        zval old;
        ZVAL_COPY_VALUE(&old, &o->ctl->cb2);
        if (fn) {
            ZVAL_COPY(&o->ctl->cb2, fn);
        } else {
            ZVAL_UNDEF(&o->ctl->cb2);
        }
        zval_ptr_dtor(&old);   /* after: it may be running */
    }
    RETURN_OBJ_COPY(&o->std);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_dialog_ctor, 0, 0, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, title, IS_STRING, 0, "''")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, text, IS_STRING, 0, "''")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, w, IS_LONG, 0, "460")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, stack, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_dialog_buttons, 0, 0, IS_STATIC, 0)
    ZEND_ARG_VARIADIC_OBJ_INFO(0, b, Baremetal\\Slate\\Button, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_dialog_children, 0, 0, IS_STATIC, 0)
    ZEND_ARG_VARIADIC_OBJ_INFO(0, e, Baremetal\\Slate\\Element, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_dialog_void, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_dialog_closed, 0, 1, IS_STATIC, 0)
    ZEND_ARG_OBJ_INFO(0, fn, Closure, 1)
ZEND_END_ARG_INFO()

static const zend_function_entry dialog_methods[] = {
    PHP_ME(SlateDialog, __construct, arginfo_dialog_ctor, ZEND_ACC_PUBLIC)
    PHP_ME(SlateDialog, buttons, arginfo_dialog_buttons, ZEND_ACC_PUBLIC)
    PHP_ME(SlateDialog, add, arginfo_dialog_children, ZEND_ACC_PUBLIC)
    PHP_ME(SlateDialog, remove, arginfo_dialog_children, ZEND_ACC_PUBLIC)
    PHP_ME(SlateDialog, close, arginfo_dialog_void, ZEND_ACC_PUBLIC)
    PHP_ME(SlateDialog, onClose, arginfo_dialog_void, ZEND_ACC_PUBLIC)
    PHP_ME(SlateDialog, closed, arginfo_dialog_closed, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

void sl_dialog_register(void)
{
    zend_class_entry ce;
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "Dialog", dialog_methods);
    sl_dialog_ce = zend_register_internal_class_ex(&ce, sl_control_ce);   /* not final: an app may extend it */
    sl_dialog_ce->ce_flags |= ZEND_ACC_NOT_SERIALIZABLE;
    zval dv;
    ZVAL_FALSE(&dv);
    zend_property_info *pi = zend_declare_typed_property(
        sl_dialog_ce, zend_string_init_interned("dismissable", sizeof("dismissable") - 1, 1), &dv, ZEND_ACC_PUBLIC,
        NULL, (zend_type)ZEND_TYPE_INIT_CODE(_IS_BOOL, 0, 0));
    s_off_dismissable = pi->offset;
    sl_ctl_class(sl_dialog_ce, SL_DIALOG, &dialog_ops);
}

#endif
