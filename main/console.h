// USB Serial/JTAG console: diagnostics, screenshots, touch injection, desk
// protocol testing and credentials setup. Type `help` in a serial terminal.
#pragma once

namespace app {
class App;
}

namespace console {

void start(app::App *app);

}  // namespace console
