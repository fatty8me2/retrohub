// Console test: the controller name shown on the Controllers page.
#include <cstdio>

#include "pad_name_logic.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace hubpadname;

int main() {
    CHECK(label("Xbox Wireless Controller", true) == "Xbox Wireless Controller");
    CHECK(label("Xbox Wireless Controller", false) == "Xbox Wireless Controller");
    CHECK(label("DualSense Wireless Controller", false) == "DualSense Wireless Controller");
    CHECK(label("Nintendo Switch Pro Controller", false) == "Nintendo Switch Pro Controller");
    CHECK(label("Generic USB Joystick", false) == "Generic USB Joystick");
    CHECK(label("Generic USB Joystick", true) == "Generic USB Joystick (Xbox)");   // Xbox-style but not named so: say it once
    CHECK(label("  Controller   (XBOX 360 For Windows) ", true) == "Controller (XBOX 360 For Windows)");
    CHECK(label("", false) == "Controller");
    CHECK(label("A  B", false) == "A B");
    if (fails == 0) std::printf("pad name: all passed\n");
    return fails ? 1 : 0;
}
