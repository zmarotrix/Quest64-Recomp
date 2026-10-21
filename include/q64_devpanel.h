#ifndef __Q64_DEVPANEL_H__
#define __Q64_DEVPANEL_H__

#include <cstdint>

// Developer panel: a small web server on 127.0.0.1 that serves a page for editing game data
// (spells, Brian's stats, save slots) while the game runs. Only started when requested
// (--dev-panel on the command line or Q64_DEV_PANEL=1 in the environment).
namespace q64::devpanel {
    // Avoid 6463-6472, which Discord uses for its local RPC server.
    // Override with Q64_DEV_PANEL_PORT in the environment.
    constexpr int default_port = 43064;

    bool requested(int argc, char** argv);
    // Controller input held by the panel's POST /api/input; false when none is active.
    bool injected_input(uint16_t* buttons, float* x, float* y);
    void start(uint8_t* rdram);
    void stop();
}

#endif
