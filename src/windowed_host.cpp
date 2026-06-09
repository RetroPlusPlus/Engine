#include "gbcpp/windowed_host.h"

namespace gbcpp {

void WindowedHost::run() {
    while (!platform_.quitRequested()) {
        platform_.pumpEvents();
        loop_.setRawInput(platform_.buttons());
        loop_.advance();  // the render callback presents inside advance()
    }
}

}  // namespace gbcpp
