/*
 * HID report engine: double-buffered report state + a periodic Zephyr thread
 * that notifies the active connection. Ported from upstream hid_controller.c:
 * FreeRTOS task -> Zephyr thread, and the direct GATT/subscription calls go
 * through ble/ble_iface.h (ble_hid_ready / ble_hid_notify).
 */

#include "controller/hid_controller.h"
#include "controller/hid_controller_pro2.h"
#include "device.h"
#include "utils.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(jb_hid_controller, CONFIG_JOY_BRIDGE_LOG_LEVEL);
#include "app_log.h"

#define CTRL_TASK_STACK_SIZE 4096
#define CTRL_TASK_PRIO       5

K_THREAD_STACK_DEFINE(ctrl_task_stack, CTRL_TASK_STACK_SIZE);
static struct k_thread ctrl_task_data;

controller_handle_t g_hid_controller = {
    .ops = &controller_ops,
    .hid_ops = NULL,
    .type = CONTROLLER_TYPE_PRO2,
    .buffer = {
        .front_buffer = NULL,
        .back_buffer = NULL,
        .swap_request = 0
    },
    .task_handle = NULL,
    .task_running = false,
    .ns2_notification_handle = NS2_NOTIFICATION_HANDLE
};

static void controller_task(void *p1, void *p2, void *p3) {
    controller_handle_t *ctrl = (controller_handle_t *)p1;
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    ESP_LOGI(LOG_HID, "controller report task start, interval: %dms", HID_REPORT_INTERVAL);

    while (ctrl->task_running) {
        /* Connected + HID characteristic subscribed + DEV_READY? */
        if (!ble_hid_ready()) {
            k_msleep(100);
            continue;
        }

        if (ctrl->buffer.front_buffer != NULL) {
            if (ctrl->hid_ops == NULL || ctrl->hid_ops->next_report == NULL ||
                ctrl->hid_ops->report_size == NULL) {
                ESP_LOGE(LOG_HID, "No valid hid device operations for type %d", ctrl->type);
                k_msleep(HID_REPORT_INTERVAL);
                continue;
            }

            /* Swap front/back buffers if a commit was requested. */
            if (ctrl->buffer.swap_request) {
                MEMORY_BARRIER();
                controller_hid_report_t *temp = ctrl->buffer.front_buffer;
                ctrl->buffer.front_buffer = ctrl->buffer.back_buffer;
                ctrl->buffer.back_buffer = temp;
                MEMORY_BARRIER();
                ctrl->buffer.swap_request = 0;
            }

            size_t report_size = ctrl->hid_ops->report_size();

            /* Copy out before a potential swap to keep the report consistent. */
            uint8_t report_buffer[report_size];
            uint8_t *next_report = ctrl->hid_ops->next_report(ctrl->buffer.front_buffer);
            memcpy(report_buffer, next_report, report_size);

            int rc = ble_hid_notify(report_buffer, report_size);
            if (rc != 0) {
                ESP_LOGD(LOG_HID, "controller report send failed, rc: %d", rc);
                if (rc == -ENOTCONN || rc == -ENOMEM) {
                    /* let buffers free up */
                    k_msleep(1);
                }
            }
        }

        /* A fixed sleep is adequate here; for jitter-free cadence switch to an
         * absolute periodic schedule (k_timer / k_uptime delta). */
        k_msleep(HID_REPORT_INTERVAL);
    }
}

static int controller_init_impl(controller_handle_t *ctrl, controller_type_t type) {
    if (ctrl == NULL) {
        return -1;
    }

    if (ctrl->ops != NULL && ctrl->ops->deinit != NULL) {
        ctrl->ops->deinit(ctrl);
    }

    /* TODO JoyCon support */
    ctrl->hid_ops = type == CONTROLLER_TYPE_PRO2 ? &controller_pro2_ops : NULL;
    if (ctrl->hid_ops == NULL) {
        ESP_LOGE(LOG_HID, "No valid hid device operations for type %d", type);
        return -1;
    }
    ctrl->type = type;

    ctrl->buffer.front_buffer = (controller_hid_report_t *)malloc(sizeof(controller_hid_report_t));
    ctrl->buffer.back_buffer = (controller_hid_report_t *)malloc(sizeof(controller_hid_report_t));
    ctrl->buffer.swap_request = 0;

    if (ctrl->buffer.front_buffer == NULL || ctrl->buffer.back_buffer == NULL) {
        ESP_LOGE(LOG_HID, "malloc controller report double buffer failed");
        goto error;
    }

    memset(ctrl->buffer.front_buffer, 0, sizeof(controller_hid_report_t));
    memset(ctrl->buffer.back_buffer, 0, sizeof(controller_hid_report_t));

    ctrl->hid_ops->report_init(ctrl->buffer.front_buffer);
    ctrl->hid_ops->report_init(ctrl->buffer.back_buffer);

    if (ctrl->buffer.front_buffer->report == NULL || ctrl->buffer.back_buffer->report == NULL) {
        ESP_LOGE(LOG_HID, "controller report init failed");
        goto error;
    }

    return 0;

error:
    if (ctrl->ops != NULL && ctrl->ops->deinit != NULL) {
        ctrl->ops->deinit(ctrl);
    }
    return -1;
}

static void controller_deinit_impl(controller_handle_t *ctrl) {
    if (ctrl == NULL) {
        return;
    }

    if (ctrl->ops != NULL && ctrl->ops->stop_task != NULL) {
        ctrl->ops->stop_task(ctrl);
    }

    if (ctrl->buffer.front_buffer != NULL) {
        if (ctrl->buffer.front_buffer->report != NULL) {
            free(ctrl->buffer.front_buffer->report);
        }
        free(ctrl->buffer.front_buffer);
        ctrl->buffer.front_buffer = NULL;
    }
    if (ctrl->buffer.back_buffer != NULL) {
        if (ctrl->buffer.back_buffer->report != NULL) {
            free(ctrl->buffer.back_buffer->report);
        }
        free(ctrl->buffer.back_buffer);
        ctrl->buffer.back_buffer = NULL;
    }

    ctrl->buffer.swap_request = 0;
    ctrl->hid_ops = NULL;
}

static int controller_start_task_impl(controller_handle_t *ctrl) {
    if (ctrl == NULL) {
        return -1;
    }
    if (ctrl->task_handle != NULL) {
        ESP_LOGW(LOG_HID, "controller report task already started");
        return 0;
    }
    if (ctrl->buffer.front_buffer == NULL || ctrl->buffer.back_buffer == NULL) {
        ESP_LOGE(LOG_HID, "controller not initialized");
        return -1;
    }

    ctrl->task_running = true;
    ctrl->task_handle = k_thread_create(&ctrl_task_data, ctrl_task_stack,
                                        K_THREAD_STACK_SIZEOF(ctrl_task_stack),
                                        controller_task, ctrl, NULL, NULL,
                                        CTRL_TASK_PRIO, 0, K_NO_WAIT);
    if (ctrl->task_handle == NULL) {
        ESP_LOGE(LOG_HID, "create controller report task failed");
        ctrl->task_running = false;
        return -1;
    }
    k_thread_name_set(ctrl->task_handle, "controller_task");
    return 0;
}

static void controller_stop_task_impl(controller_handle_t *ctrl) {
    if (ctrl == NULL) {
        return;
    }
    if (ctrl->task_handle != NULL) {
        ctrl->task_running = false;
        k_thread_abort(ctrl->task_handle);
        ctrl->task_handle = NULL;
    }
}

static controller_hid_report_t *controller_get_back_buffer_impl(controller_handle_t *ctrl) {
    if (ctrl == NULL) {
        return NULL;
    }
    return ctrl->buffer.back_buffer;
}

static void controller_hid_commit_impl(controller_handle_t *ctrl) {
    if (ctrl == NULL) {
        return;
    }
    MEMORY_BARRIER();
    ctrl->buffer.swap_request = 1;
}

static void controller_hid_reset_impl(controller_handle_t *ctrl) {
    if (ctrl == NULL || ctrl->hid_ops == NULL || ctrl->hid_ops->report_init == NULL) {
        return;
    }
    ctrl->hid_ops->report_init(ctrl->buffer.front_buffer);
    ctrl->hid_ops->report_init(ctrl->buffer.back_buffer);
}

const controller_ops_t controller_ops = {
    .name            = "controller",
    .init            = controller_init_impl,
    .deinit          = controller_deinit_impl,
    .start_task      = controller_start_task_impl,
    .stop_task       = controller_stop_task_impl,
    .get_back_buffer = controller_get_back_buffer_impl,
    .hid_commit      = controller_hid_commit_impl,
    .hid_reset       = controller_hid_reset_impl,
};
