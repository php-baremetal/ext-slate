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
 * (called once, before the first show), onEnter() and onLeave() run when it is shown and replaced, onTouch()
 * gets the touch events while it is the active scene. All four are empty here.
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
    sl_children_remove(sl_obj(Z_OBJ_P(ZEND_THIS)), args, argc);
    if (EG(exception)) {
        RETURN_THROWS();
    }
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

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

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_scene_void, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_scene_touch, 0, 1, IS_VOID, 0)
    ZEND_ARG_OBJ_INFO(0, e, Baremetal\\Input\\TouchEvent, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry scene_methods[] = {
    PHP_ME(SlateScene, add, arginfo_scene_children, ZEND_ACC_PUBLIC)
    PHP_ME(SlateScene, remove, arginfo_scene_children, ZEND_ACC_PUBLIC)
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
}
