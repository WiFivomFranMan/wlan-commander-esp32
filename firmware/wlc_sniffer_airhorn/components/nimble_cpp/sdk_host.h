// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// NimBLE-Arduino's config always selects its bundled headers. This component
// links ESP-IDF's host, so both declarations and controller ownership must use
// the SDK path. Include the config once before clearing its selection marker.
#include "nimconfig.h"
#undef USING_NIMBLE_ARDUINO_HEADERS
