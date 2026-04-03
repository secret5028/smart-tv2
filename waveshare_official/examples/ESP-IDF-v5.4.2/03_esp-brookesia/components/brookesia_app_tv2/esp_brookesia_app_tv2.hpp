/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class Tv2App: public systems::phone::App {
public:
    static Tv2App *requestInstance(bool use_status_bar = true, bool use_navigation_bar = true);

    ~Tv2App();

protected:
    Tv2App(bool use_status_bar, bool use_navigation_bar);

    bool run(void) override;
    bool back(void) override;

private:
    static void onCloseButtonPressed(lv_event_t *event);

    static Tv2App *_instance;
};

} // namespace esp_brookesia::apps
