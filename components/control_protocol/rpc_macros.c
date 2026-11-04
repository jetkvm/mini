/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "control_protocol_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const s_keyboard_macro_key_names[] = {
#include "keyboard_key_names.inc"
};

static const char *const s_keyboard_macro_modifier_names[] = {
    "ControlLeft",
    "ControlRight",
    "ShiftLeft",
    "ShiftRight",
    "AltLeft",
    "AltRight",
    "MetaLeft",
    "MetaRight",
};

_Static_assert(
    sizeof(s_keyboard_macro_key_names) /
            sizeof(s_keyboard_macro_key_names[0]) ==
        JETKVM_CONTROL_KEYBOARD_MACRO_KEY_NAME_COUNT,
    "saved-macro key table must match the frontend contract");
_Static_assert(
    sizeof(s_keyboard_macro_modifier_names) /
            sizeof(s_keyboard_macro_modifier_names[0]) ==
        JETKVM_CONTROL_KEYBOARD_MACRO_MAX_MODIFIERS,
    "saved-macro modifier table must match the frontend contract");

static bool bounded_string_valid(const char *value, size_t capacity,
                                 bool allow_empty)
{
    if (value == NULL || capacity == 0U ||
        memchr(value, '\0', capacity) == NULL) {
        return false;
    }
    return allow_empty || value[0] != '\0';
}

static int keyboard_macro_name_id(
    const char *name, const char *const names[], size_t count)
{
    if (name == NULL) {
        return -1;
    }
    for (size_t i = 0U; i < count; ++i) {
        if (strcmp(name, names[i]) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static uint32_t fnv1a_update(uint32_t hash, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    for (size_t i = 0U; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }
    return hash;
}

static void keyboard_macro_generate_id(
    jetkvm_control_keyboard_macro_t *macro, size_t index)
{
    uint32_t hash = fnv1a_update(2166136261U, &index, sizeof(index));
    hash = fnv1a_update(hash, macro->name, strlen(macro->name));
    hash = fnv1a_update(hash, &macro->step_count,
                        sizeof(macro->step_count));
    hash = fnv1a_update(hash, macro->steps,
                        (size_t)macro->step_count * sizeof(macro->steps[0]));
    (void)snprintf(macro->id, sizeof(macro->id), "macro-%08lx-%02u",
                   (unsigned long)hash, (unsigned)index);
}

static bool keyboard_macro_list_valid(
    const jetkvm_control_keyboard_macro_list_t *list)
{
    if (list == NULL ||
        list->count > JETKVM_CONTROL_KEYBOARD_MACRO_MAX_COUNT) {
        return false;
    }
    for (size_t i = 0U; i < list->count; ++i) {
        const jetkvm_control_keyboard_macro_t *macro = &list->macros[i];
        if (!bounded_string_valid(macro->id, sizeof(macro->id), false) ||
            !bounded_string_valid(macro->name, sizeof(macro->name), false) ||
            macro->step_count == 0U ||
            macro->step_count > JETKVM_CONTROL_KEYBOARD_MACRO_MAX_STEPS) {
            return false;
        }
        for (size_t j = 0U; j < macro->step_count; ++j) {
            const jetkvm_control_keyboard_macro_step_t *step =
                &macro->steps[j];
            if (step->key_count > JETKVM_CONTROL_KEYBOARD_MACRO_MAX_KEYS ||
                step->modifier_count >
                    JETKVM_CONTROL_KEYBOARD_MACRO_MAX_MODIFIERS ||
                step->delay_ms < 50U || step->delay_ms > 2000U) {
                return false;
            }
            for (size_t k = 0U; k < step->key_count; ++k) {
                if (step->key_ids[k] >=
                    JETKVM_CONTROL_KEYBOARD_MACRO_KEY_NAME_COUNT) {
                    return false;
                }
            }
            for (size_t k = 0U; k < step->modifier_count; ++k) {
                if (step->modifier_ids[k] >=
                    JETKVM_CONTROL_KEYBOARD_MACRO_MAX_MODIFIERS) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool add_keyboard_macro_json(
    cJSON *array, const jetkvm_control_keyboard_macro_t *macro)
{
    cJSON *item = cJSON_CreateObject();
    cJSON *steps = NULL;
    if (!item || !cJSON_AddStringToObject(item, "id", macro->id) ||
        !cJSON_AddStringToObject(item, "name", macro->name) ||
        !cJSON_AddNumberToObject(item, "sortOrder", macro->sort_order) ||
        !(steps = cJSON_AddArrayToObject(item, "steps"))) {
        cJSON_Delete(item);
        return false;
    }
    for (size_t i = 0U; i < macro->step_count; ++i) {
        const jetkvm_control_keyboard_macro_step_t *step = &macro->steps[i];
        cJSON *step_item = cJSON_CreateObject();
        cJSON *keys = NULL;
        cJSON *modifiers = NULL;
        if (!step_item || !(keys = cJSON_AddArrayToObject(step_item, "keys")) ||
            !(modifiers = cJSON_AddArrayToObject(step_item, "modifiers")) ||
            !cJSON_AddNumberToObject(step_item, "delay", step->delay_ms)) {
            cJSON_Delete(step_item);
            cJSON_Delete(item);
            return false;
        }
        for (size_t j = 0U; j < step->key_count; ++j) {
            cJSON *name = cJSON_CreateString(
                s_keyboard_macro_key_names[step->key_ids[j]]);
            if (!name || !cJSON_AddItemToArray(keys, name)) {
                cJSON_Delete(name);
                cJSON_Delete(step_item);
                cJSON_Delete(item);
                return false;
            }
        }
        for (size_t j = 0U; j < step->modifier_count; ++j) {
            cJSON *name = cJSON_CreateString(
                s_keyboard_macro_modifier_names[step->modifier_ids[j]]);
            if (!name || !cJSON_AddItemToArray(modifiers, name)) {
                cJSON_Delete(name);
                cJSON_Delete(step_item);
                cJSON_Delete(item);
                return false;
            }
        }
        if (!cJSON_AddItemToArray(steps, step_item)) {
            cJSON_Delete(step_item);
            cJSON_Delete(item);
            return false;
        }
    }
    if (!cJSON_AddItemToArray(array, item)) {
        cJSON_Delete(item);
        return false;
    }
    return true;
}

bool rpc_get_keyboard_macros(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    jetkvm_control_keyboard_macro_list_t *list = calloc(1U, sizeof(*list));
    if (list == NULL) {
        return false;
    }
    if (!context->keyboard_macros.get ||
        !context->keyboard_macros.get(list, context->keyboard_macros.ctx) ||
        !keyboard_macro_list_valid(list)) {
        free(list);
        return set_hid_unavailable(dispatch,
                                   "Keyboard macros unavailable");
    }
    cJSON *result = cJSON_CreateArray();
    if (!result) {
        free(list);
        return false;
    }
    for (size_t i = 0U; i < list->count; ++i) {
        if (!add_keyboard_macro_json(result, &list->macros[i])) {
            cJSON_Delete(result);
            free(list);
            return false;
        }
    }
    free(list);
    dispatch->result = result;
    return true;
}

static bool parse_keyboard_macro_step(
    const cJSON *item, jetkvm_control_keyboard_macro_step_t *step)
{
    const cJSON *keys = rpc_param(item, "keys");
    const cJSON *modifiers = rpc_param(item, "modifiers");
    const cJSON *delay = rpc_param(item, "delay");
    if (!cJSON_IsObject(item) || !cJSON_IsArray(keys) ||
        !cJSON_IsArray(modifiers) ||
        cJSON_GetArraySize(keys) >
            (int)JETKVM_CONTROL_KEYBOARD_MACRO_MAX_KEYS ||
        cJSON_GetArraySize(modifiers) >
            (int)JETKVM_CONTROL_KEYBOARD_MACRO_MAX_MODIFIERS) {
        return false;
    }
    int64_t delay_value = 0;
    if (!json_integer_in_range(delay, INT32_MIN, INT32_MAX, &delay_value)) {
        return false;
    }
    if (delay_value < 50) {
        delay_value = 50;
    } else if (delay_value > 2000) {
        delay_value = 2000;
    }
    step->delay_ms = (uint16_t)delay_value;

    cJSON *value = NULL;
    cJSON_ArrayForEach(value, keys) {
        if (!cJSON_IsString(value) || value->valuestring == NULL) {
            return false;
        }
        int id = keyboard_macro_name_id(
            value->valuestring, s_keyboard_macro_key_names,
            JETKVM_CONTROL_KEYBOARD_MACRO_KEY_NAME_COUNT);
        if (id < 0) {
            return false;
        }
        step->key_ids[step->key_count++] = (uint8_t)id;
    }
    cJSON_ArrayForEach(value, modifiers) {
        if (!cJSON_IsString(value) || value->valuestring == NULL) {
            return false;
        }
        int id = keyboard_macro_name_id(
            value->valuestring, s_keyboard_macro_modifier_names,
            JETKVM_CONTROL_KEYBOARD_MACRO_MAX_MODIFIERS);
        if (id < 0) {
            return false;
        }
        step->modifier_ids[step->modifier_count++] = (uint8_t)id;
    }
    return true;
}

bool rpc_set_keyboard_macros(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    const cJSON *wrapper = rpc_param(params, "params");
    const cJSON *macros = rpc_param(wrapper, "macros");
    if (!cJSON_IsObject(params) || !cJSON_IsObject(wrapper) ||
        !cJSON_IsArray(macros) ||
        cJSON_GetArraySize(macros) >
            (int)JETKVM_CONTROL_KEYBOARD_MACRO_MAX_COUNT) {
        return set_invalid_hid_params(
            dispatch, "params.macros must contain at most 25 macros");
    }

    jetkvm_control_keyboard_macro_list_t *list = calloc(1U, sizeof(*list));
    if (list == NULL) {
        return false;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, macros) {
        jetkvm_control_keyboard_macro_t *macro =
            &list->macros[list->count];
        const cJSON *id = rpc_param(item, "id");
        const cJSON *name = rpc_param(item, "name");
        const cJSON *steps = rpc_param(item, "steps");
        const cJSON *sort_order = rpc_param(item, "sortOrder");
        if (!cJSON_IsObject(item) ||
            !copy_json_string(name, macro->name, sizeof(macro->name), false) ||
            !cJSON_IsArray(steps) || cJSON_GetArraySize(steps) < 1 ||
            cJSON_GetArraySize(steps) >
                (int)JETKVM_CONTROL_KEYBOARD_MACRO_MAX_STEPS) {
            free(list);
            return set_invalid_hid_params(dispatch,
                                           "invalid keyboard macro");
        }
        if (id != NULL &&
            !copy_json_string(id, macro->id, sizeof(macro->id), true)) {
            free(list);
            return set_invalid_hid_params(dispatch,
                                           "invalid keyboard macro id");
        }
        int64_t order = (int64_t)list->count + 1;
        if (sort_order != NULL &&
            !json_integer_in_range(sort_order, INT32_MIN, INT32_MAX,
                                   &order)) {
            free(list);
            return set_invalid_hid_params(dispatch,
                                           "invalid keyboard macro order");
        }
        macro->sort_order = (int32_t)order;
        cJSON *step_item = NULL;
        cJSON_ArrayForEach(step_item, steps) {
            if (!parse_keyboard_macro_step(
                    step_item, &macro->steps[macro->step_count])) {
                free(list);
                return set_invalid_hid_params(
                    dispatch, "invalid keyboard macro step");
            }
            macro->step_count++;
        }
        if (macro->id[0] == '\0') {
            keyboard_macro_generate_id(macro, list->count);
        }
        list->count++;
    }
    if (!keyboard_macro_list_valid(list)) {
        free(list);
        return set_invalid_hid_params(dispatch, "invalid keyboard macros");
    }
    if (!context->keyboard_macros.set) {
        free(list);
        return set_hid_unavailable(dispatch, "Keyboard macros unavailable");
    }
    bool saved = context->keyboard_macros.set(
        list, context->keyboard_macros.ctx);
    free(list);
    if (!saved) {
        return set_hid_unavailable(
            dispatch, "failed to persist keyboard macros");
    }
    dispatch->result = cJSON_CreateNull();
    return dispatch->result != NULL;
}

bool rpc_get_jiggler_state(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    bool enabled = false;
    if (!context->jiggler.get_state ||
        !context->jiggler.get_state(&enabled, context->jiggler.ctx)) {
        return set_hid_unavailable(dispatch, "Mouse jiggler unavailable");
    }
    dispatch->result = cJSON_CreateBool(enabled);
    return dispatch->result != NULL;
}

bool rpc_set_jiggler_state(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    const cJSON *enabled = rpc_param(params, "enabled");
    if (!cJSON_IsObject(params) || !cJSON_IsBool(enabled)) {
        return set_invalid_hid_params(dispatch, "enabled must be boolean");
    }
    if (!context->jiggler.set_state ||
        !context->jiggler.set_state(cJSON_IsTrue(enabled),
                                    context->jiggler.ctx)) {
        return set_hid_unavailable(
            dispatch, "failed to persist mouse jiggler state");
    }
    dispatch->result = cJSON_CreateNull();
    return dispatch->result != NULL;
}

bool rpc_get_jiggler_config(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    jetkvm_control_jiggler_config_t config = {0};
    if (!context->jiggler.get_config ||
        !context->jiggler.get_config(&config, context->jiggler.ctx) ||
        !bounded_string_valid(config.schedule_cron_tab,
                              sizeof(config.schedule_cron_tab), false) ||
        !bounded_string_valid(config.timezone, sizeof(config.timezone),
                              true)) {
        return set_hid_unavailable(dispatch, "Mouse jiggler unavailable");
    }
    cJSON *result = cJSON_CreateObject();
    if (!result ||
        !cJSON_AddNumberToObject(result, "inactivity_limit_seconds",
                                 config.inactivity_limit_seconds) ||
        !cJSON_AddNumberToObject(result, "jitter_percentage",
                                 config.jitter_percentage) ||
        !cJSON_AddStringToObject(result, "schedule_cron_tab",
                                 config.schedule_cron_tab) ||
        !cJSON_AddStringToObject(result, "timezone", config.timezone)) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

bool rpc_set_jiggler_config(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    const cJSON *wrapper = rpc_param(params, "jigglerConfig");
    const cJSON *inactivity = rpc_param(wrapper, "inactivity_limit_seconds");
    const cJSON *jitter = rpc_param(wrapper, "jitter_percentage");
    const cJSON *cron = rpc_param(wrapper, "schedule_cron_tab");
    const cJSON *timezone = rpc_param(wrapper, "timezone");
    int64_t inactivity_value = 0;
    int64_t jitter_value = 0;
    jetkvm_control_jiggler_config_t config = {0};
    if (!cJSON_IsObject(params) || !cJSON_IsObject(wrapper) ||
        !json_integer_in_range(inactivity, 1, 86400, &inactivity_value) ||
        !json_integer_in_range(jitter, 0, 100, &jitter_value) ||
        !copy_json_string(cron, config.schedule_cron_tab,
                          sizeof(config.schedule_cron_tab), false) ||
        (timezone != NULL &&
         !copy_json_string(timezone, config.timezone,
                           sizeof(config.timezone), true))) {
        return set_invalid_hid_params(dispatch, "invalid jiggler config");
    }
    if (config.timezone[0] == '\0') {
        memcpy(config.timezone, JETKVM_CONTROL_JIGGLER_DEFAULT_TIMEZONE,
               sizeof(JETKVM_CONTROL_JIGGLER_DEFAULT_TIMEZONE));
    }
    config.inactivity_limit_seconds = (uint32_t)inactivity_value;
    config.jitter_percentage = (uint32_t)jitter_value;
    if (!context->jiggler.set_config) {
        return set_hid_unavailable(dispatch, "Mouse jiggler unavailable");
    }
    switch (context->jiggler.set_config(&config, context->jiggler.ctx)) {
    case JETKVM_JIGGLER_CONFIG_CHANGE_ACCEPTED:
        dispatch->result = cJSON_CreateNull();
        return dispatch->result != NULL;
    case JETKVM_JIGGLER_CONFIG_CHANGE_INVALID:
        return set_invalid_hid_params(dispatch,
                                      "invalid cron or timezone");
    case JETKVM_JIGGLER_CONFIG_CHANGE_FAILED:
    default:
        return set_hid_unavailable(
            dispatch, "failed to persist mouse jiggler config");
    }
}
