/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:TV2"
#include "esp_lib_utils.h"
#include "systems/phone/assets/esp_brookesia_phone_assets.h"
#include "esp_brookesia_app_tv2.hpp"

#define APP_NAME "TV2"

using namespace std;
using namespace esp_brookesia::systems;

namespace esp_brookesia::apps {

Tv2App *Tv2App::_instance = nullptr;

Tv2App *Tv2App::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (_instance == nullptr) {
        _instance = new Tv2App(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

Tv2App::Tv2App(bool use_status_bar, bool use_navigation_bar):
    App(APP_NAME, &esp_brookesia_image_middle_app_launcher_default_112_112, true, use_status_bar, use_navigation_bar)
{
}

Tv2App::~Tv2App()
{
}

bool Tv2App::run(void)
{
    ESP_UTILS_LOGD("Run");

    lv_obj_t *screen = lv_scr_act();
    const lv_area_t &visual_area = getVisualArea();
    const lv_coord_t visual_width = lv_area_get_width(&visual_area);
    const lv_coord_t visual_height = lv_area_get_height(&visual_area);

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B1220), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_set_size(card, visual_width - 32, visual_height - 32);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 28, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_left(card, 24, 0);
    lv_obj_set_style_pad_right(card, 24, 0);
    lv_obj_set_style_pad_top(card, 24, 0);
    lv_obj_set_style_pad_bottom(card, 24, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x111C31), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);

    lv_obj_t *badge = lv_label_create(card);
    lv_label_set_text(badge, "CUSTOM APP");
    lv_obj_set_style_text_color(badge, lv_color_hex(0x7DD3FC), 0);
    lv_obj_set_style_text_font(badge, &lv_font_montserrat_16, 0);
    lv_obj_align(badge, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "TV2");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF8FAFC), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_34, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 36);

    lv_obj_t *subtitle = lv_label_create(card);
    lv_label_set_text(subtitle, "Dedicated watch app baseline");
    lv_obj_set_width(subtitle, visual_width - 80);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0xCBD5E1), 0);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_18, 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 0, 92);

    lv_obj_t *panel = lv_obj_create(card);
    lv_obj_set_size(panel, visual_width - 80, 124);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, -4);
    lv_obj_set_style_radius(panel, 22, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    lv_obj_t *panel_title = lv_label_create(panel);
    lv_label_set_text(panel_title, "Ready for expansion");
    lv_obj_set_style_text_color(panel_title, lv_color_hex(0xF8FAFC), 0);
    lv_obj_set_style_text_font(panel_title, &lv_font_montserrat_20, 0);
    lv_obj_align(panel_title, LV_ALIGN_TOP_LEFT, 16, 16);

    lv_obj_t *panel_text = lv_label_create(panel);
    lv_label_set_text(panel_text, "Next we can add Wi-Fi, device controls, or a TV remote workflow here.");
    lv_obj_set_width(panel_text, visual_width - 120);
    lv_label_set_long_mode(panel_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(panel_text, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(panel_text, &lv_font_montserrat_16, 0);
    lv_obj_align(panel_text, LV_ALIGN_TOP_LEFT, 16, 52);

    lv_obj_t *close_button = lv_btn_create(card);
    lv_obj_set_size(close_button, visual_width - 80, 56);
    lv_obj_align(close_button, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(close_button, 18, 0);
    lv_obj_set_style_border_width(close_button, 0, 0);
    lv_obj_set_style_bg_color(close_button, lv_color_hex(0x38BDF8), 0);
    lv_obj_add_event_cb(close_button, onCloseButtonPressed, LV_EVENT_CLICKED, this);

    lv_obj_t *close_label = lv_label_create(close_button);
    lv_label_set_text(close_label, "Close");
    lv_obj_set_style_text_color(close_label, lv_color_hex(0x082F49), 0);
    lv_obj_set_style_text_font(close_label, &lv_font_montserrat_20, 0);
    lv_obj_center(close_label);

    return true;
}

bool Tv2App::back(void)
{
    ESP_UTILS_LOGD("Back");

    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");

    return true;
}

void Tv2App::onCloseButtonPressed(lv_event_t *event)
{
    Tv2App *app = static_cast<Tv2App *>(lv_event_get_user_data(event));
    if (app == nullptr) {
        return;
    }

    ESP_UTILS_CHECK_FALSE_EXIT(app->notifyCoreClosed(), "Notify core closed failed");
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, Tv2App, APP_NAME, []()
{
    return std::shared_ptr<Tv2App>(Tv2App::requestInstance(), [](Tv2App *p) {});
})

} // namespace esp_brookesia::apps
