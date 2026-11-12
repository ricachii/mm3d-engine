/* ControllerTest: lists the game controllers SDL sees (same SDL2.dll and same
 * resources/gamecontrollerdb.txt as the game), reports hot-plug events and shows each button
 * press with the 3DS button it drives in Majora's Mask 3D.
 *
 *   ControllerTest.exe [seconds]      (default 60; run it before or after plugging the pad)
 *
 * Part of the MM3D adaptation built on Volpe34's mm3d-pc (GPL-3.0-or-later). */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

static FILE *gLog;

static void out(const char *fmt, ...) {
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    fputs(text, stdout);
    fflush(stdout);
    if (gLog) { fputs(text, gLog); fflush(gLog); }
}

static const char *button3ds(SDL_GameControllerButton b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return "A";
    case SDL_CONTROLLER_BUTTON_B: return "B";
    case SDL_CONTROLLER_BUTTON_X: return "3DS Y";
    case SDL_CONTROLLER_BUTTON_Y: return "3DS X";
    case SDL_CONTROLLER_BUTTON_BACK: return "Select";
    case SDL_CONTROLLER_BUTTON_START: return "Start";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "(left stick click, unused)";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "view / first person (R3)";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "item slot I (LB)";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "item slot II (RB)";
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "ocarina (D-pad up)";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "masks / map (D-pad down)";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "minimap (D-pad left)";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-pad right";
    default: return "?";
    }
}

static const char *axis3ds(SDL_GameControllerAxis a) {
    switch (a) {
    case SDL_CONTROLLER_AXIS_LEFTX: case SDL_CONTROLLER_AXIS_LEFTY: return "move (left stick)";
    case SDL_CONTROLLER_AXIS_RIGHTX: case SDL_CONTROLLER_AXIS_RIGHTY: return "camera (right stick)";
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return "L / target (LT)";
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return "R / shield (RT)";
    default: return "?";
    }
}

static void describe(int index) {
    char guid[33];
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof(guid));
    const char *joyName = SDL_JoystickNameForIndex(index);
    if (SDL_IsGameController(index)) {
        char *mapping = SDL_GameControllerMappingForDeviceIndex(index);
        out("  [%d] \"%s\"  GUID %s  -> gamepad: YES (%s)\n", index, SDL_GameControllerNameForIndex(index), guid,
            mapping ? "mapping found" : "no mapping text");
        if (mapping) SDL_free(mapping);
    } else {
        out("  [%d] \"%s\"  GUID %s  -> gamepad: NO. SDL does not know this device as a gamepad: its mapping "
            "is missing from resources\\gamecontrollerdb.txt (the game will not use it).\n",
            index, joyName ? joyName : "?", guid);
    }
}

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 60;
    if (seconds <= 0) seconds = 60;
    gLog = fopen("logs\\controller_test.log", "w");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        out("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_version v;
    SDL_GetVersion(&v);
    out("SDL %d.%d.%d (SDL2.dll of this folder)\n", v.major, v.minor, v.patch);
    int added = SDL_GameControllerAddMappingsFromFile("resources\\gamecontrollerdb.txt");
    out("gamecontrollerdb.txt: %d mappings loaded%s\n", added, added < 0 ? " (FILE NOT FOUND: run from the game folder)" : "");
    int n = SDL_NumJoysticks();
    out("Devices seen right now: %d\n", n);
    for (int i = 0; i < n; i++) describe(i);
    if (n == 0) out("  none yet: plug the pad in now (hot-plug is tested while this runs).\n");
    out("Listening for %d s: press buttons, move the sticks, plug/unplug the pad. Ctrl+C to stop.\n", seconds);

    SDL_GameController *open[16] = {0};
    for (int i = 0; i < n && i < 16; i++)
        if (SDL_IsGameController(i)) open[i] = SDL_GameControllerOpen(i);
    Uint32 end = SDL_GetTicks() + (Uint32)seconds * 1000U;
    int lastAxis[16][6] = {{0}};
    while (SDL_GetTicks() < end) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_CONTROLLERDEVICEADDED: {
                int i = e.cdevice.which;
                out("EVENT: pad connected\n");
                describe(i);
                if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
                break;
            }
            case SDL_CONTROLLERDEVICEREMOVED: {
                SDL_GameController *c = SDL_GameControllerFromInstanceID(e.cdevice.which);
                out("EVENT: pad disconnected (instance %d)\n", e.cdevice.which);
                if (c) SDL_GameControllerClose(c);
                break;
            }
            case SDL_JOYDEVICEADDED:
                out("EVENT: joystick device added (index %d)\n", e.jdevice.which);
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                out("  button %-14s -> %s\n", SDL_GameControllerGetStringForButton((SDL_GameControllerButton)e.cbutton.button),
                    button3ds((SDL_GameControllerButton)e.cbutton.button));
                break;
            case SDL_CONTROLLERAXISMOTION: {
                int a = e.caxis.axis, value = e.caxis.value, big = value > 16000 || value < -16000;
                if (a >= 0 && a < 6 && big != lastAxis[0][a]) {
                    lastAxis[0][a] = big;
                    if (big) out("  axis   %-14s -> %s (value %d)\n", SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)a),
                                 axis3ds((SDL_GameControllerAxis)a), value);
                }
                break;
            }
            case SDL_QUIT: end = 0; break;
            }
        }
        SDL_Delay(5);
    }
    out("Done.\n");
    SDL_Quit();
    return 0;
}
