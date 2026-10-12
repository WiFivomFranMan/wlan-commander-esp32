// SPDX-License-Identifier: MIT
#include "wlc_apps.hpp"

#include "survey_apps.hpp"
#include "airhorn_app.hpp"
#include "switch_app.hpp"

namespace wlc::apps {

std::vector<esp_brookesia::systems::phone::App *> all()
{
    static SurveyApp scan(SurveyApp::Kind::Scan);
    static SurveyApp clients(SurveyApp::Kind::Clients);
    static SurveyApp passpoint(SurveyApp::Kind::Passpoint);
    static SwitchApp spectrum("Spectrum",
        "Restarts into the sensor image and draws the spectrum on this screen. Pick a view:", true);
    static SwitchApp sensor("Sensor",
        "Runs the WLAN Commander C5 sensor firmware: the phone app connects over Bluetooth "
        "and does the parsing, exactly as with the other C5 boards.", false);
    static AirhornApp airhorn;
    return {&scan, &clients, &passpoint, &spectrum, &sensor, &airhorn};
}

}
