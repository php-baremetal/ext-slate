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
 * Scene: what Slate shows, one at a time. Its elements hang from a root group of its own (the scene is not an
 * Element: it cannot go inside a group). A page of an application extends it: build() adds the components
 * (called once, before the first show), onEnter() and onLeave() run when it is shown and replaced (with a
 * transition: onEnter() at its start, onLeave() at its end), onTouch() gets the raw touch events of the touches no
 * element takes while it is the active scene (not during a transition; php_slate_touch.c routes them). All four are
 * empty here.
 *
 * Dialogs (slate-controls). open() adds a Dialog as the scene's last child, with its opening animation; while dialogs
 * are open, add() puts them back on top of what it added (in their order), and remove() of an open one closes it at
 * once (onClose(), closed()). A dialog stays with its scene when another one is shown (php_slate_ctl_dialog.c).
 *
 * protected bool $keepCache = true: when the scene is left, its element caches stay for a return to it while there
 * is room (Slate::$keepScenes, Slate::$keptBudget; engine.h, Kept scenes); false: they go when it is left. Read by
 * the show() that leaves the scene.
 */
#include "php.h"
#include "zend_exceptions.h"

#include "php_slate.h"

zend_class_entry *sl_scene_ce;

PHP_METHOD(SlateScene, add)
{
    zval *args = NULL;
    uint32_t argc = 0;
    ZEND_PARSE_PARAMETERS_START(0, -1)
        Z_PARAM_VARIADIC('*', args, argc)
    ZEND_PARSE_PARAMETERS_END();
    for (uint32_t i = 0; i < argc; i++) {
        if (Z_TYPE(args[i]) != IS_OBJECT || !instanceof_function(Z_OBJCE(args[i]), sl_element_ce)) {
            zend_argument_type_error(i + 1, "must be of type Baremetal\\Slate\\Element, %s given",
                                     zend_zval_value_name(&args[i]));
            RETURN_THROWS();
        }
    }
    sl_children_add(sl_obj(Z_OBJ_P(ZEND_THIS)), args, argc);
#if SLATE_HAS_CONTROLS
    if (!EG(exception)) {
        sl_dialog_raise(sl_obj(Z_OBJ_P(ZEND_THIS)));   /* the open dialogs stay on top (nothing when none is open) */
    }
#endif
    if (EG(exception)) {
        RETURN_THROWS();
    }
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

PHP_METHOD(SlateScene, remove)
{
    zval *args = NULL;
    uint32_t argc = 0;
    ZEND_PARSE_PARAMETERS_START(0, -1)
        Z_PARAM_VARIADIC('*', args, argc)
    ZEND_PARSE_PARAMETERS_END();
    for (uint32_t i = 0; i < argc; i++) {
        if (Z_TYPE(args[i]) != IS_OBJECT || !instanceof_function(Z_OBJCE(args[i]), sl_element_ce)) {
            zend_argument_type_error(i + 1, "must be of type Baremetal\\Slate\\Element, %s given",
                                     zend_zval_value_name(&args[i]));
            RETURN_THROWS();
        }
    }
    sl_obj_t *so = sl_obj(Z_OBJ_P(ZEND_THIS));
#if SLATE_HAS_CONTROLS
    /* the open dialogs among them: closed at once after the removal (onClose(), closed()) */
    uint32_t nd = 0;
    sl_obj_t **dialogs = NULL;
    for (uint32_t i = 0; i < argc; i++) {
        sl_obj_t *c = sl_obj(Z_OBJ(args[i]));
        if (c->parent == so && sl_dialog_is_open(c)) {
            if (!dialogs) {
                dialogs = emalloc(argc * sizeof *dialogs);
            }
            dialogs[nd++] = c;   /* alive: the caller's arguments hold them */
        }
    }
#endif
    sl_children_remove(so, args, argc);
#if SLATE_HAS_CONTROLS
    for (uint32_t i = 0; i < nd && !EG(exception); i++) {
        sl_dialog_removed(dialogs[i]);
    }
    if (dialogs) {
        efree(dialogs);
    }
#endif
    if (EG(exception)) {
        RETURN_THROWS();
    }
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

#if SLATE_HAS_CONTROLS
PHP_METHOD(SlateScene, open)
{
    zend_object *d;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJ_OF_CLASS(d, sl_dialog_ce)
    ZEND_PARSE_PARAMETERS_END();
    sl_anim_flush();
    if (!sl_dialog_open(sl_obj(Z_OBJ_P(ZEND_THIS)), sl_obj(d))) {
        RETURN_THROWS();
    }
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
#endif

PHP_METHOD(SlateScene, build)
{
    ZEND_PARSE_PARAMETERS_NONE();
}

PHP_METHOD(SlateScene, onEnter)
{
    ZEND_PARSE_PARAMETERS_NONE();
}

PHP_METHOD(SlateScene, onLeave)
{
    ZEND_PARSE_PARAMETERS_NONE();
}

PHP_METHOD(SlateScene, onTouch)
{
    zval *e;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT(e)
    ZEND_PARSE_PARAMETERS_END();
    (void)e;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_scene_children, 0, 0, IS_STATIC, 0)
    ZEND_ARG_VARIADIC_OBJ_INFO(0, e, Baremetal\\Slate\\Element, 0)
ZEND_END_ARG_INFO()

#if SLATE_HAS_CONTROLS
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_scene_open, 0, 1, IS_STATIC, 0)
    ZEND_ARG_OBJ_INFO(0, d, Baremetal\\Slate\\Dialog, 0)
ZEND_END_ARG_INFO()
#endif

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_scene_void, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_scene_touch, 0, 1, IS_VOID, 0)
    ZEND_ARG_OBJ_INFO(0, e, Baremetal\\Input\\TouchEvent, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry scene_methods[] = {
    PHP_ME(SlateScene, add, arginfo_scene_children, ZEND_ACC_PUBLIC)
    PHP_ME(SlateScene, remove, arginfo_scene_children, ZEND_ACC_PUBLIC)
#if SLATE_HAS_CONTROLS
    PHP_ME(SlateScene, open, arginfo_scene_open, ZEND_ACC_PUBLIC)
#endif
    PHP_ME(SlateScene, build, arginfo_scene_void, ZEND_ACC_PROTECTED)
    PHP_ME(SlateScene, onEnter, arginfo_scene_void, ZEND_ACC_PUBLIC)
    PHP_ME(SlateScene, onLeave, arginfo_scene_void, ZEND_ACC_PUBLIC)
    PHP_ME(SlateScene, onTouch, arginfo_scene_touch, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

void sl_scene_register(void)
{
    zend_class_entry ce;
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "Scene", scene_methods);
    sl_scene_ce = zend_register_internal_class(&ce);
    sl_scene_ce->ce_flags |= ZEND_ACC_NOT_SERIALIZABLE;
    sl_elements_scene(sl_scene_ce);
    zval dv;
    ZVAL_TRUE(&dv);
    zend_declare_typed_property(sl_scene_ce, zend_string_init_interned("keepCache", sizeof("keepCache") - 1, 1), &dv,
                                ZEND_ACC_PROTECTED, NULL, (zend_type)ZEND_TYPE_INIT_CODE(_IS_BOOL, 0, 0));
}
