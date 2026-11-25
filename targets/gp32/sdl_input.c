#include "platform/platform_internal.h"
#include "platform/sdl3/common.h"
#include "input/gp32_sdl3_input_shared.h"

#include <stdlib.h>

typedef struct sdl3_input_backend {
    gp32_input_backend_t base;
    gp32_sdl3_input_state_t shared;
    SDL_Gamepad *pad;
    int gamepads_initialized;
} sdl3_input_backend_t;

static uint32_t sdl3_input_take_actions(gp32_input_backend_t *backend) {
    sdl3_input_backend_t *in = (sdl3_input_backend_t *)backend;
    return in ? gp32_sdl3_input_take_actions(&in->shared) : 0u;
}

static uint32_t gamepad_buttons(SDL_Gamepad *pad, int enable_axis) {
    static const struct {
        SDL_GamepadButton button;
        uint32_t mask;
    } mapping[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, GP32_BUTTON_UP},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, GP32_BUTTON_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, GP32_BUTTON_LEFT},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, GP32_BUTTON_RIGHT},
        {SDL_GAMEPAD_BUTTON_SOUTH, GP32_BUTTON_A},
        {SDL_GAMEPAD_BUTTON_EAST, GP32_BUTTON_B},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, GP32_BUTTON_L},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, GP32_BUTTON_R},
        {SDL_GAMEPAD_BUTTON_START, GP32_BUTTON_START},
        {SDL_GAMEPAD_BUTTON_BACK, GP32_BUTTON_SELECT},
    };
    uint32_t mask = 0;
    for (size_t i = 0; i < sizeof(mapping) / sizeof(mapping[0]); ++i) {
        if (SDL_GetGamepadButton(pad, mapping[i].button)) mask |= mapping[i].mask;
    }
    if (enable_axis) {
        const int deadzone = 16000;
        int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        int y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        if (x < -deadzone) mask |= GP32_BUTTON_LEFT;
        if (x > deadzone) mask |= GP32_BUTTON_RIGHT;
        if (y < -deadzone) mask |= GP32_BUTTON_UP;
        if (y > deadzone) mask |= GP32_BUTTON_DOWN;
    }
    return mask;
}

static void open_gamepad(sdl3_input_backend_t *in) {
    if (!in->gamepads_initialized || in->pad) return;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (ids && count > 0) in->pad = SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
}

static gp32_status_t sdl3_input_poll(gp32_input_backend_t *backend, uint32_t *out_buttons, int *out_quit) {
    sdl3_input_backend_t *in = (sdl3_input_backend_t *)backend;
    if (!in || !out_buttons || !out_quit) return GP32_ERR_INVALID_ARGUMENT;

    SDL_Event event;
    *out_quit = 0;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) *out_quit = 1;
        gp32_sdl3_input_handle_event(&in->shared, &event, out_quit);
        if (event.type == SDL_EVENT_GAMEPAD_REMOVED && in->pad &&
            event.gdevice.which == SDL_GetGamepadID(in->pad)) {
            SDL_CloseGamepad(in->pad);
            in->pad = NULL;
        }
        if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED)
            open_gamepad(in);
        if (event.type == SDL_EVENT_JOYSTICK_REMOVED)
            gp32_sdl3_input_open_first_joystick(&in->shared);
    }

    if (in->pad || in->shared.joy) SDL_UpdateJoysticks();
    *out_buttons = SDL_GetKeyboardFocus() ? gp32_sdl3_input_keyboard_buttons() : 0;
    *out_buttons |= in->pad ? gamepad_buttons(in->pad, in->shared.enable_joystick_axis)
                           : gp32_sdl3_input_joystick_buttons(&in->shared);
    return GP32_OK;
}

static const char *sdl3_input_error(const gp32_input_backend_t *backend) {
    const sdl3_input_backend_t *in = (const sdl3_input_backend_t *)backend;
    return in ? gp32_sdl3_input_error(&in->shared) : "";
}

static void sdl3_input_destroy(gp32_input_backend_t *backend) {
    sdl3_input_backend_t *in = (sdl3_input_backend_t *)backend;
    if (!in) return;
    if (in->pad) SDL_CloseGamepad(in->pad);
    if (in->gamepads_initialized) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    gp32_sdl3_input_state_shutdown(&in->shared);
    if (in->shared.enable_joystick) gp32_sdl3_quit_subsystem(SDL_INIT_JOYSTICK);
    gp32_sdl3_quit_subsystem(SDL_INIT_VIDEO);
    free(in);
}

gp32_input_backend_t *gp32_input_sdl3_create(const gp32_input_options_t *options) {
    SDL_InitFlags flags = SDL_INIT_VIDEO;
    int use_joy = options ? options->enable_joystick : 0;
    int use_joy_axis = options ? options->enable_joystick_axis : 0;
    if (use_joy) flags |= SDL_INIT_JOYSTICK;
    if (gp32_sdl3_init_subsystem(flags) != 0) return NULL;
    sdl3_input_backend_t *in = (sdl3_input_backend_t *)calloc(1, sizeof(*in));
    if (!in) {
        if (use_joy) gp32_sdl3_quit_subsystem(SDL_INIT_JOYSTICK);
        gp32_sdl3_quit_subsystem(SDL_INIT_VIDEO);
        return NULL;
    }
    in->base.destroy = sdl3_input_destroy;
    in->base.poll = sdl3_input_poll;
    in->base.take_actions = sdl3_input_take_actions;
    in->base.error = sdl3_input_error;
    in->gamepads_initialized = use_joy && SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    gp32_sdl3_input_state_init(&in->shared, use_joy, use_joy_axis);
    if (use_joy) gp32_sdl3_input_open_first_joystick(&in->shared);
    open_gamepad(in);
    return &in->base;
}
