// SPDX-License-Identifier: MIT
#pragma once

namespace wlc::apps {

// Extra commands (e.g. the survey's) get first refusal on anything the CLI does not know.
// `args` is the rest of the line after the command word. Return false if not handled.
using CliHook = bool (*)(const char *cmd, char *args);
void start_cli(CliHook hook);

}
