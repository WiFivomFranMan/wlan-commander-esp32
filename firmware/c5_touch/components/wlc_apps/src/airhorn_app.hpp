// SPDX-License-Identifier: MIT
#pragma once
#include "tool_app.hpp"
#include "wlc_generator.hpp"
namespace wlc::apps {
generator::Config generator_config();
bool save_generator_config(const generator::Config &config);
bool boot_generator_image(const generator::Config &config, bool shielded_ack = false);
class AirhornApp : public ToolApp {
public:
    AirhornApp();
protected:
    void build(lv_obj_t *body) override;
    void teardown() override;
private:
    void changed();
    generator::Config config_;
    lv_obj_t *fields_[11]{};
    lv_obj_t *rows_[11]{};
    lv_obj_t *note_ = nullptr, *ack_ = nullptr, *go_ = nullptr;
    bool approved_ = false;
};
}
