#include "gp32_host.h"
#include "gp32_menu.h"
#include "gp32_resample.h"
#include "s3c2400_bus.h"
#if M0_GP32_PC_TOOLS
#include "gp32_shot.h"
#endif
#ifdef M0_GP32_BUNDLE
#include "gp32_bundle.h"
#endif
#include "gp32_state.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#define GL_GLEXT_PROTOTYPES 1
#endif
#include <SDL.h>
#include <SDL_opengl.h>
#include <ctype.h>
#if M0_GP32_PC_TOOLS
#include <dirent.h>
#include <sys/stat.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    HOST_AUDIO_RATE = 48000,
    HOST_AUDIO_FRAME = 2 * sizeof(int16_t),
    HOST_AUDIO_BLOCK = 2 * HOST_AUDIO_RATE / 100,   /* samples */
    HOST_AUDIO_PREROLL = HOST_AUDIO_RATE * HOST_AUDIO_FRAME / 16,
    HOST_AUDIO_LIMIT = HOST_AUDIO_RATE * HOST_AUDIO_FRAME / 8,
    HOST_AUDIO_TARGET = HOST_AUDIO_RATE * HOST_AUDIO_FRAME / 12,
    /* Adaptive CPI range: the emulated core is as fast as the host allows. */
    HOST_CPI_MIN = 2,
    HOST_CPI_MAX = 16,
    HOST_SLICE_STEPS = 20000
};

typedef struct {
    SDL_AudioDeviceID device;
    Gp32Resample resample;      /* from the game's rate to the host's */
    unsigned rate, count;
    bool playing;
    int16_t samples[2048];      /* at the host's rate, not yet queued */
    SDL_atomic_t volume;    /* 0 to 8 */
    /* Interruptions of the output, blocks thrown away, rate changes. */
    unsigned underruns, dropped, streams;
} HostAudio;

static void queue_audio(HostAudio *audio)
{
    int volume = SDL_AtomicGet(&audio->volume);
    if (volume < 8)
        for (unsigned i = 0; i < audio->count; ++i)
            audio->samples[i] = (int16_t)(audio->samples[i] * volume / 8);
    if (audio->count && SDL_GetQueuedAudioSize(audio->device) >= 2 * HOST_AUDIO_LIMIT)
        ++audio->dropped;
    else if (audio->count)
        SDL_QueueAudio(audio->device, audio->samples,
                       (Uint32)(audio->count * sizeof(*audio->samples)));
    audio->count = 0;
}

static void audio_sample(void *context, int16_t left, int16_t right, unsigned rate)
{
    HostAudio *audio = context;
    if (!audio->device || rate < 1000 || rate > 384000)
        return;
    if (audio->rate != rate) {
        gp32_resample_init(&audio->resample, rate, HOST_AUDIO_RATE);
        audio->rate = rate;
        ++audio->streams;
    }
    audio->count += 2 * gp32_resample_frame(&audio->resample, left, right,
                                            audio->samples + audio->count);
    /* Queue about every 10 ms, always leaving room for one more input frame. */
    if (audio->count >= HOST_AUDIO_BLOCK)
        queue_audio(audio);
}

static SDL_GameController *open_controller(void)
{
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i)) {
            SDL_GameController *controller = SDL_GameControllerOpen(i);
            if (controller)
                return controller;
        }
    return NULL;
}

static uint16_t read_buttons(SDL_GameController *controller)
{
    static const SDL_Scancode keys[] = {
        SDL_SCANCODE_LEFT, SDL_SCANCODE_DOWN, SDL_SCANCODE_RIGHT, SDL_SCANCODE_UP,
        SDL_SCANCODE_Q, SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_W,
        SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE
    };
    static const SDL_GameControllerButton pad[] = {
        SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_UP,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_B,
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_BACK
    };
    uint16_t buttons = 0;
    const uint8_t *pressed = SDL_GetKeyboardFocus() ? SDL_GetKeyboardState(NULL) : NULL;
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        if ((pressed && pressed[keys[i]]) || (controller && SDL_GameControllerGetButton(controller, pad[i])))
            buttons |= 1u << i;
    if (controller) {
        int x = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
        int y = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);
        if (x < -16000) buttons |= GP32_LEFT;
        if (x > 16000) buttons |= GP32_RIGHT;
        if (y < -16000) buttons |= GP32_UP;
        if (y > 16000) buttons |= GP32_DOWN;
    }
    return buttons;
}

typedef struct {
    SDL_Window *window;
    SDL_GLContext context;
    GLuint program, lines, palettes, vao, vbo, uploads[2];
    GLint transform;
    unsigned upload;
    /* The option menu, drawn over the picture while visible. */
    GLuint menu_program, menu;
    GLint menu_transform;
    bool menu_visible;
} GpuDisplay;

#ifdef _WIN32
/* opengl32.dll only exports OpenGL 1.1: the rest is resolved at run time. */
#define GL_FUNCTIONS(X) \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) \
    X(PFNGLDELETESHADERPROC, glDeleteShader) \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLMAPBUFFERRANGEPROC, glMapBufferRange) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv) \
    X(PFNGLUNMAPBUFFERPROC, glUnmapBuffer) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)
#define DECLARE(type, name) static type name##_pointer;
GL_FUNCTIONS(DECLARE)
#undef DECLARE

static bool load_gl(void)
{
    bool loaded = true;
#define LOAD(type, name) \
    loaded &= (name##_pointer = (type)SDL_GL_GetProcAddress(#name)) != NULL;
    GL_FUNCTIONS(LOAD)
#undef LOAD
    return loaded;
}

#define glActiveTexture glActiveTexture_pointer
#define glAttachShader glAttachShader_pointer
#define glBindBuffer glBindBuffer_pointer
#define glBindVertexArray glBindVertexArray_pointer
#define glBufferData glBufferData_pointer
#define glCompileShader glCompileShader_pointer
#define glCreateProgram glCreateProgram_pointer
#define glCreateShader glCreateShader_pointer
#define glDeleteBuffers glDeleteBuffers_pointer
#define glDeleteProgram glDeleteProgram_pointer
#define glDeleteShader glDeleteShader_pointer
#define glDeleteVertexArrays glDeleteVertexArrays_pointer
#define glEnableVertexAttribArray glEnableVertexAttribArray_pointer
#define glGenBuffers glGenBuffers_pointer
#define glGenVertexArrays glGenVertexArrays_pointer
#define glGetProgramiv glGetProgramiv_pointer
#define glGetShaderInfoLog glGetShaderInfoLog_pointer
#define glGetShaderiv glGetShaderiv_pointer
#define glGetUniformLocation glGetUniformLocation_pointer
#define glLinkProgram glLinkProgram_pointer
#define glMapBufferRange glMapBufferRange_pointer
#define glShaderSource glShaderSource_pointer
#define glUniform1i glUniform1i_pointer
#define glUniformMatrix4fv glUniformMatrix4fv_pointer
#define glUnmapBuffer glUnmapBuffer_pointer
#define glUseProgram glUseProgram_pointer
#define glVertexAttribPointer glVertexAttribPointer_pointer
#else
static bool load_gl(void) { return true; }
#endif

static GLuint compile_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled)
        return shader;
    char message[1024];
    glGetShaderInfoLog(shader, sizeof(message), NULL, message);
    fprintf(stderr, "OpenGL shader: %s\n", message);
    glDeleteShader(shader);
    return 0;
}

static GLuint integer_texture(GLint format, int width, int height, GLenum type)
{
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, GL_RED_INTEGER, type, NULL);
    return texture;
}

static GLuint link_program(const char *vertex_source, const char *fragment_source)
{
    GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
    GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
    if (!vertex || !fragment)
        return 0;
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    return linked ? program : 0;
}

static bool gpu_display_open(GpuDisplay *display, SDL_Window *window)
{
    static const char *vertex_source =
        "#version 330 core\n"
        "layout(location=0) in vec2 position;\n"
        "layout(location=1) in vec2 uv;\n"
        "uniform mat4 transform; out vec2 texcoord;\n"
        "void main(){ gl_Position=transform*vec4(position,0,1); texcoord=uv; }\n";
    /* Decodes a Gp32LcdFrame: same rules as the CPU scanout in s3c2400.c,
       then the panel response curve. */
    static const char *fragment_source =
        "#version 330 core\n"
        "in vec2 texcoord; out vec4 color;\n"
        "uniform usampler2D lines, palettes;\n"
        "uint header(int i,int y){ return texelFetch(lines,ivec2(i,y),0).r; }\n"
        "void main(){\n"
        "  ivec2 p=clamp(ivec2(texcoord*vec2(240,320)),ivec2(0),ivec2(239,319));\n"
        "  uint format=header(0,p.y), value=0u;\n"
        "  if(uint(p.x)<header(1,p.y)){\n"
        "    if((format&0x10000u)!=0u){ value=header(3,p.y); }\n"
        "    else {\n"
        "      uint bits=format&255u, lanes=(format>>8)&3u, offset=uint(p.x)*bits;\n"
        "      uint data=header(4+int(offset>>5),p.y), swapped=0u;\n"
        "      for(uint i=0u;i<4u;++i) swapped|=((data>>((i^lanes)*8u))&255u)<<(i*8u);\n"
        "      value=(swapped>>(32u-bits-(offset&31u)))&((1u<<bits)-1u);\n"
        "      if(bits!=16u) value=texelFetch(palettes,ivec2(int(value),int(header(2,p.y))),0).r;\n"
        "    }\n"
        "  }\n"
        "  uvec3 c=((uvec3(value)>>uvec3(10u,5u,0u))&62u)|(value&1u);\n"
        "  vec3 rgb=vec3((c<<2u)|(c>>4u))/255.0;\n"
        "  color=vec4(0.9215686*pow(rgb,vec3(1.22)),1);\n"
        "}\n";
    /* The same quad: texcoord runs along the rotated panel. */
    static const char *menu_source =
        "#version 330 core\n"
        "in vec2 texcoord; out vec4 color;\n"
        "uniform sampler2D menu;\n"
        "void main(){\n"
        "  ivec2 p=clamp(ivec2(vec2(texcoord.y,1.0-texcoord.x)*vec2(160,120)),ivec2(0),ivec2(159,119));\n"
        "  color=texelFetch(menu,p,0);\n"
        "}\n";
    static const float vertices[] = {
        -1,-1, 0,0,  1,-1, 0,1,  1,1, 1,1,
        -1,-1, 0,0,  1,1, 1,1,  -1,1, 1,0
    };
    memset(display, 0, sizeof(*display));
    display->window = window;
    display->context = SDL_GL_CreateContext(window);
    if (!display->context || !load_gl())
        return false;
    SDL_GL_SetSwapInterval(1);
    display->program = link_program(vertex_source, fragment_source);
    display->menu_program = link_program(vertex_source, menu_source);
    if (!display->program || !display->menu_program)
        return false;

    glGenTextures(1, &display->menu);
    glBindTexture(GL_TEXTURE_2D, display->menu);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, GP32_MENU_WIDTH, GP32_MENU_HEIGHT, 0,
                 GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
    glUseProgram(display->menu_program);
    glUniform1i(glGetUniformLocation(display->menu_program, "menu"), 0);
    display->menu_transform = glGetUniformLocation(display->menu_program, "transform");

    display->lines = integer_texture(GL_R32UI, GP32_LCD_LINE_WORDS, GP32_LCD_HEIGHT,
                                     GL_UNSIGNED_INT);
    display->palettes = integer_texture(GL_R16UI, 256, GP32_LCD_HEIGHT, GL_UNSIGNED_SHORT);
    glUseProgram(display->program);
    glUniform1i(glGetUniformLocation(display->program, "lines"), 0);
    glUniform1i(glGetUniformLocation(display->program, "palettes"), 1);

    glGenBuffers(2, display->uploads);
    glGenVertexArrays(1, &display->vao);
    glGenBuffers(1, &display->vbo);
    glBindVertexArray(display->vao);
    glBindBuffer(GL_ARRAY_BUFFER, display->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));
    display->transform = glGetUniformLocation(display->program, "transform");
    return glGetError() == GL_NO_ERROR;
}

/* Texture updates are sourced from alternating pixel buffers, so the driver
   copies them without stalling on the previous draw. */
static void gpu_display_upload(GpuDisplay *display, const Gp32LcdFrame *frame)
{
    size_t lines = sizeof(frame->lines);
    size_t palettes = frame->palette_count * sizeof(frame->palettes[0]);
    display->upload ^= 1u;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, display->uploads[display->upload]);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)(lines + sizeof(frame->palettes)),
                 NULL, GL_STREAM_DRAW);
    uint8_t *mapped = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0,
                                       (GLsizeiptr)(lines + palettes),
                                       GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    bool copied = false;
    if (mapped) {
        memcpy(mapped, frame->lines, lines);
        memcpy(mapped + lines, frame->palettes, palettes);
        copied = glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER) == GL_TRUE;
    }
    const uint8_t *line_source = NULL;
    const uint8_t *palette_source = (const uint8_t *)NULL + lines;
    if (!copied) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        line_source = (const uint8_t *)frame->lines;
        palette_source = (const uint8_t *)frame->palettes;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, display->lines);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GP32_LCD_LINE_WORDS, GP32_LCD_HEIGHT,
                    GL_RED_INTEGER, GL_UNSIGNED_INT, line_source);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, display->palettes);
    if (frame->palette_count)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, (GLsizei)frame->palette_count,
                        GL_RED_INTEGER, GL_UNSIGNED_SHORT, palette_source);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

static void gpu_display_menu(GpuDisplay *display, const Gp32Menu *menu)
{
    uint16_t pixels[GP32_MENU_WIDTH * GP32_MENU_HEIGHT];
    gp32_menu_render(menu, pixels);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, display->menu);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GP32_MENU_WIDTH, GP32_MENU_HEIGHT,
                    GL_RGB, GL_UNSIGNED_SHORT_5_6_5, pixels);
}

static void gpu_display_draw(GpuDisplay *display)
{
    int width, height;
    SDL_GL_GetDrawableSize(display->window, &width, &height);
    if (width <= 0 || height <= 0)
        return;
    float scale = fminf((float)width / GP32_LCD_HEIGHT, (float)height / GP32_LCD_WIDTH);
    float sx = scale * GP32_LCD_HEIGHT / width;
    float sy = scale * GP32_LCD_WIDTH / height;
    /* The quad's texture coordinates already rotate the column-major panel
       buffer to landscape: only letterbox here. */
    const float transform[16] = {
        sx, 0, 0, 0,
        0, sy, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(display->program);
    glUniformMatrix4fv(display->transform, 1, GL_FALSE, transform);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, display->lines);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, display->palettes);
    glBindVertexArray(display->vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    if (display->menu_visible) {
        glUseProgram(display->menu_program);
        glUniformMatrix4fv(display->menu_transform, 1, GL_FALSE, transform);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, display->menu);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    SDL_GL_SwapWindow(display->window);
}

static void gpu_display_close(GpuDisplay *display)
{
    if (display->vbo) glDeleteBuffers(1, &display->vbo);
    if (display->vao) glDeleteVertexArrays(1, &display->vao);
    if (display->uploads[0]) glDeleteBuffers(2, display->uploads);
    if (display->lines) glDeleteTextures(1, &display->lines);
    if (display->palettes) glDeleteTextures(1, &display->palettes);
    if (display->program) glDeleteProgram(display->program);
    if (display->menu) glDeleteTextures(1, &display->menu);
    if (display->menu_program) glDeleteProgram(display->menu_program);
    if (display->context) SDL_GL_DeleteContext(display->context);
}

/* The emulation thread owns the machine; the main thread owns the window.
   They only share the atomics and the frame mailbox below. */
typedef struct {
    Gp32Host *host;
    S3c2400 *soc;
    HostAudio *audio;
    unsigned fixed_cpi;     /* zero adapts */
    SDL_atomic_t quit, buttons, paused, turbo, speed, cpi, failed;
    /* How many times faster than the console the host emulates when it
       does not wait for it. */
    SDL_atomic_t capacity;
    /* Latest completed frame: write is filled outside the lock, then
       exchanged with ready, which the display exchanges with read. */
    SDL_mutex *lock;
    Gp32LcdFrame *frames;
    unsigned write, ready, read;
    bool fresh;
    bool statistics;
    /* Save states and restart: the display thread posts a command, the
       emulation thread runs it between two slices and posts the outcome. */
    SDL_atomic_t menu, command, outcome;
    const S3c2400 *boot;
    const char *state_base;
    uint64_t identity;
#if M0_GP32_PC_TOOLS
    uint64_t program;
    /* Where shots go, and the application's name which starts theirs. */
    char folder[1100], name[64];
    /* A shot to load, written by the display thread before it posts
       COMMAND_SHOT. */
    char shot[1100];
    SDL_Thread *writer; /* the shot being written, if any */
#endif
    struct { uint32_t offset, size; uint8_t bytes[8]; } freeze[16];    /* --freeze */
    unsigned freezes;
} Emulation;

enum { COMMAND_NONE, COMMAND_SAVE, COMMAND_LOAD, COMMAND_RESTART, COMMAND_SNAP, COMMAND_SHOT, COMMAND_LAST };

static const char *const outcomes[] = {
    NULL, "Etat sauve", "Echec sauvegarde", "Etat charge", "Echec chargement",
    "Redemarre", "Echec redemarrage", "Shot enregistre", "Echec shot",
    "Jeu plante : F1 puis Charger"};

/* A UTF-8 name from SDL, as the C file functions want it. */
static bool host_path(const char *utf8, char *path, size_t size)
{
#ifdef _WIN32
    wchar_t wide[1100];
    return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 1100) &&
           WideCharToMultiByte(CP_ACP, 0, wide, -1, path, (int)size, NULL, NULL);
#else
    int length = snprintf(path, size, "%s", utf8);
    return length >= 0 && (size_t)length < size;
#endif
}

/* A save state slot kept as the bare machine state: what the builds without
   shots write, and what earlier builds wrote. */
static void old_state_path(const Emulation *emulation, unsigned slot, char *path, size_t size)
{
    snprintf(path, size, "%s.state%u", emulation->state_base, slot + 1);
}

static bool file_exists(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file)
        fclose(file);
    return file != NULL;
}

#if M0_GP32_PC_TOOLS
static void make_folder(const char *path)
{
#ifdef _WIN32
    CreateDirectoryA(path, NULL);
#else
    mkdir(path, 0777);
#endif
}

/* Shots go to "shots" next to the executable, or beside the application's
   saves when that directory is read only. */
static void shot_folder(Emulation *emulation)
{
    const char *name = emulation->state_base;
    for (const char *at = name; *at; ++at)
        if (*at == '/' || *at == '\\')
            name = at + 1;
    const char *dot = strrchr(name, '.');
    int length = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
    snprintf(emulation->name, sizeof(emulation->name), "%.*s", length, name);

    char *base = SDL_GetBasePath();
    char directory[1024], probe[1100];
    if (base && host_path(base, directory, sizeof(directory))) {
        snprintf(emulation->folder, sizeof(emulation->folder), "%sshots", directory);
        make_folder(emulation->folder);
        snprintf(probe, sizeof(probe), "%s/.probe", emulation->folder);
        FILE *file = fopen(probe, "wb");
        if (file) {
            fclose(file);
            remove(probe);
            SDL_free(base);
            return;
        }
    }
    SDL_free(base);
    snprintf(emulation->folder, sizeof(emulation->folder), "%s.shots", emulation->state_base);
    make_folder(emulation->folder);
}

/* A save state slot is a shot: a PNG of the screen carrying the machine. */
static void state_path(const Emulation *emulation, unsigned slot, char *path, size_t size)
{
    snprintf(path, size, "%s/%s-slot%u.png", emulation->folder, emulation->name, slot + 1);
}

/* A new name in the shot folder, which nothing overwrites. */
static bool collection_path(const Emulation *emulation, char *path, size_t size)
{
    char stamp[32];
    time_t now = time(NULL);
    const struct tm *local = localtime(&now);
    if (!local || !strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", local))
        stamp[0] = 0;
    for (unsigned i = 0; i < 10000; ++i) {
        int length = snprintf(path, size, "%s/%s-%s-f%llu-%u.png", emulation->folder,
                              emulation->name, stamp,
                              (unsigned long long)emulation->soc->frames, i);
        if (length < 0 || (size_t)length >= size)
            return false;
        if (!file_exists(path))
            return true;
    }
    return false;
}

/* The shot of this program written last, slots included. */
static bool latest_shot(const Emulation *emulation, char *path, size_t size)
{
    DIR *directory = opendir(emulation->folder);
    if (!directory)
        return false;
    size_t prefix = strlen(emulation->name), longest = 0;
    char found[1100], latest[300] = "";
    time_t newest = 0;
    for (struct dirent *entry; (entry = readdir(directory));) {
        const char *name = entry->d_name;
        size_t length = strlen(name);
        if (length <= prefix + 5 || strncmp(name, emulation->name, prefix) ||
            name[prefix] != '-' || strcmp(name + length - 4, ".png") || length >= sizeof(latest))
            continue;
        struct stat status;
        int written = snprintf(found, sizeof(found), "%s/%s", emulation->folder, name);
        if (written < 0 || (size_t)written >= sizeof(found) || stat(found, &status))
            continue;
        /* Within one second, the later frame has the longer or greater name. */
        if (!latest[0] || status.st_mtime > newest ||
            (status.st_mtime == newest &&
             (length > longest || (length == longest && strcmp(name, latest) > 0)))) {
            newest = status.st_mtime;
            longest = length;
            memcpy(latest, name, length + 1);
        }
    }
    closedir(directory);
    int written = snprintf(path, size, "%s/%s", emulation->folder, latest);
    return latest[0] && written >= 0 && (size_t)written < size;
}

/* The card holds the player's saves and a shot brings its own: keep a copy of
   the card as it was before the first shot of a session. */
static void keep_card(const Emulation *emulation)
{
    static bool kept;
    const Gp32Host *host = emulation->host;
    char path[1100];
    if (kept || !host->image || !host->card_path || host->readonly)
        return;
    int length = snprintf(path, sizeof(path), "%s.before-shot", host->card_path);
    kept = length > 0 && (size_t)length < sizeof(path) &&
           gp32_host_save_file(path, host->image, host->image_size);
}

/* The coverage played so far goes to disk with each shot, not only at exit. */
static void flush_coverage(void)
{
#ifdef M0_GP32_AOT
    arm920t_aot_flush();
#endif
}
#endif /* M0_GP32_PC_TOOLS */

#if M0_GP32_PC_TOOLS
/* A shot takes a third of a second to compress and write: longer than the audio
   queued, so another thread does it. */
typedef struct {
    Emulation *emulation;
    Gp32Shot *shot;
    char path[1100];
} ShotWrite;

static int write_shot(void *context)
{
    ShotWrite *work = context;
    if (gp32_shot_store(work->shot, work->path)) {
        fprintf(stderr, "Shot: %s\n", work->path);
    } else {
        fprintf(stderr, "%s: unable to write the shot\n", work->path);
        SDL_AtomicSet(&work->emulation->outcome, 8);
    }
    free(work);
    return 0;
}

static void finish_shot(Emulation *emulation)
{
    if (emulation->writer)
        SDL_WaitThread(emulation->writer, NULL);
    emulation->writer = NULL;
}

/* A new shot in the collection; false when none could be started. */
static bool snap(Emulation *emulation)
{
    ShotWrite *work = malloc(sizeof(*work));
    if (!work || !collection_path(emulation, work->path, sizeof(work->path))) {
        free(work);
        return false;
    }
    work->emulation = emulation;
    work->shot = gp32_shot_take(emulation->soc, emulation->host, emulation->program);
    if (!work->shot) {
        free(work);
        return false;
    }
    emulation->writer = SDL_CreateThread(write_shot, "gp32-shot", work);
    if (!emulation->writer)
        write_shot(work);
    return true;
}
#endif

/* True when the machine was replaced. */
static bool run_command(Emulation *emulation, int command)
{
    S3c2400 *soc = emulation->soc;
    Gp32Host *host = emulation->host;
    unsigned slot = (unsigned)command >> 8;
    char path[1100];
#if M0_GP32_PC_TOOLS
    char kept[1100];
#endif
    bool done, replaced = true;
    int outcome;
#if M0_GP32_PC_TOOLS
    /* Every command sees the shot folder with the last shot complete. */
    finish_shot(emulation);
#endif
    switch (command & 0xff) {
#if M0_GP32_PC_TOOLS
    case COMMAND_SAVE:
        state_path(emulation, slot, path, sizeof(path));
        /* The shot the slot held joins the collection. */
        done = !file_exists(path) || (collection_path(emulation, kept, sizeof(kept)) &&
                                      !rename(path, kept));
        done = done && gp32_shot_save(soc, host, emulation->program, path);
        flush_coverage();
        outcome = done ? 1 : 2;
        replaced = false;
        break;
    case COMMAND_LOAD:
        state_path(emulation, slot, path, sizeof(path));
        if (file_exists(path)) {
            done = gp32_shot_load(soc, host, emulation->program, path);
        } else {
            old_state_path(emulation, slot, path, sizeof(path));
            done = gp32_state_load(soc, host, emulation->identity, path);
        }
        outcome = done ? 3 : 4;
        break;
    case COMMAND_SNAP:
        done = snap(emulation);
        flush_coverage();
        outcome = done ? 7 : 8;
        replaced = false;
        break;
    case COMMAND_SHOT:
        keep_card(emulation);
        SDL_LockMutex(emulation->lock);
        memcpy(path, emulation->shot, sizeof(path));
        SDL_UnlockMutex(emulation->lock);
        done = gp32_shot_load(soc, host, emulation->program, path);
        if (!done)
            fprintf(stderr, "%s: not a shot of this program\n", path);
        outcome = done ? 3 : 4;
        break;
    case COMMAND_LAST:
        done = latest_shot(emulation, path, sizeof(path));
        if (done) {
            keep_card(emulation);
            done = gp32_shot_load(soc, host, emulation->program, path);
            fprintf(stderr, done ? "Shot loaded: %s\n" : "%s: not a shot of this program\n", path);
        } else {
            fprintf(stderr, "No shot in %s\n", emulation->folder);
        }
        outcome = done ? 3 : 4;
        break;
#else
    case COMMAND_SAVE:
        old_state_path(emulation, slot, path, sizeof(path));
        done = gp32_state_save(soc, host, emulation->identity, path);
        outcome = done ? 1 : 2;
        replaced = false;
        break;
    case COMMAND_LOAD:
        old_state_path(emulation, slot, path, sizeof(path));
        done = gp32_state_load(soc, host, emulation->identity, path);
        outcome = done ? 3 : 4;
        break;
#endif
    default:
        done = gp32_state_restart(soc, emulation->boot);
        outcome = done ? 5 : 6;
        break;
    }
    SDL_AtomicSet(&emulation->outcome, outcome);
    if (!done || !replaced)
        return false;
    HostAudio *audio = emulation->audio;
    audio->count = 0;
    gp32_resample_clear(&audio->resample);
    if (audio->device)
        SDL_ClearQueuedAudio(audio->device);
    return true;
}

static void publish_frame(Emulation *emulation, const Gp32LcdFrame *frame)
{
    Gp32LcdFrame *copy = &emulation->frames[emulation->write];
    memcpy(copy->lines, frame->lines, sizeof(frame->lines));
    memcpy(copy->palettes, frame->palettes,
           frame->palette_count * sizeof(frame->palettes[0]));
    copy->palette_count = frame->palette_count;
    SDL_LockMutex(emulation->lock);
    unsigned previous = emulation->ready;
    emulation->ready = emulation->write;
    emulation->write = previous;
    emulation->fresh = true;
    SDL_UnlockMutex(emulation->lock);
}

static const Gp32LcdFrame *take_frame(Emulation *emulation)
{
    const Gp32LcdFrame *frame = NULL;
    SDL_LockMutex(emulation->lock);
    if (emulation->fresh) {
        unsigned previous = emulation->read;
        emulation->read = emulation->ready;
        emulation->ready = previous;
        emulation->fresh = false;
        frame = &emulation->frames[emulation->read];
    }
    SDL_UnlockMutex(emulation->lock);
    return frame;
}

static double cpu_clock(const S3c2400 *soc)
{
    uint32_t pll = soc->clock[1];
    return (((pll >> 12) & 255) + 8) * 12e6 /
           ((((pll >> 4) & 63) + 2) << (pll & 3));
}

static void drive_audio(HostAudio *audio, bool paused)
{
    if (!audio->device)
        return;
    Uint32 queued = SDL_GetQueuedAudioSize(audio->device);
    if (audio->playing && !paused && !queued)
        ++audio->underruns;
    if (audio->playing && (paused || !queued)) {
        SDL_PauseAudioDevice(audio->device, 1);
        audio->playing = false;
    }
    if (!paused && !audio->playing && queued >= HOST_AUDIO_PREROLL) {
        SDL_PauseAudioDevice(audio->device, 0);
        audio->playing = true;
    }
}

/* The battery backed clock of a console starts at the host's local time. */
static void set_host_time(S3c2400 *soc)
{
    time_t now = time(NULL);
    const struct tm *local = localtime(&now);
    if (local)
        s3c2400_set_time(soc, (S3c2400Time){
            .year = (unsigned)local->tm_year % 100, .month = (unsigned)local->tm_mon + 1,
            .day = (unsigned)local->tm_mday, .weekday = (unsigned)local->tm_wday + 1,
            .hour = (unsigned)local->tm_hour, .minute = (unsigned)local->tm_min,
            .second = local->tm_sec > 59 ? 59u : (unsigned)local->tm_sec});
}

/* Runs the machine at wall clock speed, nudged by up to two percent toward
   the audio device clock. */
static int emulate(void *context)
{
    Emulation *emulation = context;
    S3c2400 *soc = emulation->soc;
    HostAudio *audio = emulation->audio;
    const double tick = 1.0 / (double)SDL_GetPerformanceFrequency();
    Uint64 origin = SDL_GetPerformanceCounter();
    double emulated = 0, previous = 0;      /* seconds since origin */
    double window_start = 0, window_busy = 0, window_emulated = 0;
    bool late = false, running = true;
    uint64_t exceptions = 0, frame = soc->frames;
    unsigned cpi = emulation->fixed_cpi ? emulation->fixed_cpi : HOST_CPI_MIN;
    soc->cycles_per_instruction = cpi;
    SDL_AtomicSet(&emulation->cpi, (int)cpi);

    while (!SDL_AtomicGet(&emulation->quit)) {
        int command = SDL_AtomicSet(&emulation->command, COMMAND_NONE);
        if (command && run_command(emulation, command)) {
            running = true;
            frame = soc->frames;
            exceptions = 0;
            soc->cycles_per_instruction = cpi;
            SDL_AtomicSet(&emulation->failed, 0);
        }
        bool paused = !running || SDL_AtomicGet(&emulation->paused) ||
                      SDL_AtomicGet(&emulation->menu);
        bool turbo = SDL_AtomicGet(&emulation->turbo);
        double now = (double)(SDL_GetPerformanceCounter() - origin) * tick;
        drive_audio(audio, paused);
        if (paused) {
            emulated = previous = now;
            window_start = now;
            window_busy = window_emulated = 0;
            SDL_Delay(10);
            continue;
        }
        if (audio->playing) {
            double missing = 1.0 - (double)SDL_GetQueuedAudioSize(audio->device) /
                                   HOST_AUDIO_TARGET;
            emulated -= 0.02 * fmax(missing, -1.0) * (now - previous);
        }
        previous = now;
        if (turbo) {
            emulated = now;
        } else if (emulated - now > 0.002) {
            SDL_Delay(1);
            continue;
        } else if (now - emulated > 0.1) {
            emulated = now;     /* too slow: do not try to catch up */
            late = true;
        }

        s3c2400_set_buttons(soc, (uint16_t)SDL_AtomicGet(&emulation->buttons));
        uint64_t cycles = soc->cycles;
        Uint64 start = SDL_GetPerformanceCounter();
        for (unsigned i = 0; i < emulation->freezes; ++i)
            memcpy(soc->ram + emulation->freeze[i].offset, emulation->freeze[i].bytes,
                   emulation->freeze[i].size);
        running = gp32_host_run(emulation->host, soc, HOST_SLICE_STEPS,
                                emulation->host->hle ? &exceptions : NULL);
        window_busy += (double)(SDL_GetPerformanceCounter() - start) * tick;
        double slice = (double)(soc->cycles - cycles) / cpu_clock(soc);
        emulated += slice;
        window_emulated += slice;
        if (frame != soc->frames) {
            frame = soc->frames;
            publish_frame(emulation, &soc->lcd_capture->frames[soc->lcd_capture->back ^ 1u]);
        }
        if (!running && exceptions) {
            SDL_AtomicSet(&emulation->failed, 1);
            SDL_AtomicSet(&emulation->outcome, 9);
        }

        double span = now - window_start;
        if (span >= 0.5) {
            SDL_AtomicSet(&emulation->speed, (int)(100 * window_emulated / span + 0.5));
            if (window_busy > 0)
                SDL_AtomicSet(&emulation->capacity,
                              (int)(10 * window_emulated / window_busy + 0.5));
            if (!emulation->fixed_cpi && !turbo) {
                double load = window_busy / span;
                if ((late || load > 0.85) && cpi < HOST_CPI_MAX)
                    cpi += late && cpi + 2 <= HOST_CPI_MAX ? 2 : 1;
                else if (load < 0.45 && cpi > HOST_CPI_MIN)
                    --cpi;
                soc->cycles_per_instruction = cpi;
                SDL_AtomicSet(&emulation->cpi, (int)cpi);
            }
            window_start = now;
            window_busy = window_emulated = 0;
            late = false;
        }
    }
    return 0;
}

static void set_fullscreen(SDL_Window *window, bool full)
{
    SDL_SetWindowFullscreen(window, full ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    SDL_ShowCursor(full ? SDL_DISABLE : SDL_ENABLE);
}

/* What the menu shows from the outside world when it opens. */
static void prepare_menu(Emulation *emulation, GpuDisplay *display, Gp32Menu *menu)
{
    menu->used = 0;
    for (unsigned slot = 0; slot < GP32_MENU_SLOTS; ++slot) {
#if M0_GP32_PC_TOOLS
        char path[1100];
#endif
        char old[1100];
        old_state_path(emulation, slot, old, sizeof(old));
        bool used = file_exists(old);
#if M0_GP32_PC_TOOLS
        state_path(emulation, slot, path, sizeof(path));
        used = used || file_exists(path);
#endif
        if (used)
            menu->used |= 1u << slot;
    }
    menu->fullscreen = (SDL_GetWindowFlags(display->window) & SDL_WINDOW_FULLSCREEN) != 0;
    menu->info = outcomes[SDL_AtomicSet(&emulation->outcome, 0)];
    SDL_AtomicSet(&emulation->menu, 1);
}

static void apply_menu(Emulation *emulation, GpuDisplay *display, Gp32Menu *menu,
                       Gp32MenuAction action, bool *open)
{
    SDL_AtomicSet(&emulation->audio->volume, menu->volume);
    bool full = (SDL_GetWindowFlags(display->window) & SDL_WINDOW_FULLSCREEN) != 0;
    if (full != (menu->fullscreen != 0))
        set_fullscreen(display->window, menu->fullscreen);
    if (action == GP32_MENU_STAY)
        return;
    int command = action == GP32_MENU_SAVE ? COMMAND_SAVE
                : action == GP32_MENU_LOAD ? COMMAND_LOAD
                : action == GP32_MENU_RESTART ? COMMAND_RESTART : COMMAND_NONE;
    if (command)
        SDL_AtomicSet(&emulation->command, command | menu->slot << 8);
    if (action == GP32_MENU_QUIT)
        *open = false;
    else
        SDL_AtomicSet(&emulation->menu, 0);
}

static void play(Emulation *emulation, GpuDisplay *display)
{
    Gp32Menu menu = {.volume = 8};
    SDL_GameController *controller = NULL;
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0)
        controller = open_controller();
    SDL_Thread *thread = SDL_CreateThread(emulate, "gp32", emulation);
    if (!thread) {
        fprintf(stderr, "%s\n", SDL_GetError());
        SDL_AtomicSet(&emulation->failed, 1);
    }
    bool open = thread != NULL, redraw = true;
    Uint32 titled = 0;
    while (open) {
        SDL_Event event;
        for (int pending = SDL_WaitEventTimeout(&event, 2); pending;
             pending = SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                open = false;
            } else if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                SDL_Keycode key = event.key.keysym.sym;
                if (key == SDLK_F1 || (key == SDLK_ESCAPE && menu.open)) {
                    if (menu.open) {
                        menu.open = false;
                        apply_menu(emulation, display, &menu, GP32_MENU_RESUME, &open);
                        redraw = true;
                    } else {
                        gp32_menu_open(&menu, read_buttons(controller));
                        prepare_menu(emulation, display, &menu);
                    }
                } else if (key == SDLK_ESCAPE) {
                    open = false;
#if M0_GP32_PC_TOOLS
                } else if (key == SDLK_F5 && !menu.open) {
                    SDL_AtomicSet(&emulation->command, COMMAND_SNAP);
                } else if (key == SDLK_F9 && !menu.open) {
                    SDL_AtomicSet(&emulation->command, COMMAND_LAST);
#endif
                } else if (key == SDLK_p) {
                    SDL_AtomicSet(&emulation->paused, !SDL_AtomicGet(&emulation->paused));
                } else if (key == SDLK_F11) {
                    bool full = SDL_GetWindowFlags(display->window) & SDL_WINDOW_FULLSCREEN;
                    set_fullscreen(display->window, !full);
                    menu.fullscreen = !full;
                    menu.changed = true;
                }
#if M0_GP32_PC_TOOLS
            } else if (event.type == SDL_DROPFILE) {
                /* The emulation thread reads the name while a load is pending. */
                SDL_LockMutex(emulation->lock);
                bool named = host_path(event.drop.file, emulation->shot, sizeof(emulation->shot));
                SDL_UnlockMutex(emulation->lock);
                if (named)
                    SDL_AtomicSet(&emulation->command, COMMAND_SHOT);
                SDL_free(event.drop.file);
#endif
            } else if (event.type == SDL_WINDOWEVENT) {
                redraw = true;
            }
            if (controller && !SDL_GameControllerGetAttached(controller)) {
                SDL_GameControllerClose(controller);
                controller = NULL;
            }
            if (!controller && event.type == SDL_CONTROLLERDEVICEADDED)
                controller = open_controller();
        }
        uint16_t buttons = read_buttons(controller);
        if (menu.open) {
            Gp32MenuAction action = gp32_menu_update(&menu, buttons);
            apply_menu(emulation, display, &menu, action, &open);
            redraw |= !menu.open;
            buttons = 0;
        } else {
            buttons = gp32_menu_filter(&menu, buttons, SDL_GetTicks());
            if (menu.open)
                prepare_menu(emulation, display, &menu);
        }
        SDL_AtomicSet(&emulation->buttons, buttons);
        SDL_AtomicSet(&emulation->turbo, menu.fast ||
                      (SDL_GetKeyboardFocus() && SDL_GetKeyboardState(NULL)[SDL_SCANCODE_TAB]));
        display->menu_visible = menu.open;
        if (menu.open && menu.changed) {
            gpu_display_menu(display, &menu);
            menu.changed = false;
            redraw = true;
        }
        const Gp32LcdFrame *frame = take_frame(emulation);
        if (frame)
            gpu_display_upload(display, frame);
        if (frame || redraw)
            gpu_display_draw(display);
        redraw = false;
        if (SDL_GetTicks() - titled >= 500) {
            titled = SDL_GetTicks();
            char title[128];
            int capacity = SDL_AtomicGet(&emulation->capacity);
            snprintf(title, sizeof(title), "m0boy GP32 - %d%% (%d,%dx r\u00e9el) - CPI %d%s",
                     SDL_AtomicGet(&emulation->speed), capacity / 10, capacity % 10,
                     SDL_AtomicGet(&emulation->cpi),
                     SDL_AtomicGet(&emulation->paused) ? " - pause" : "");
            int outcome = SDL_AtomicGet(&emulation->outcome);
            if (outcome) {
                size_t length = strlen(title);
                snprintf(title + length, sizeof(title) - length, " - %s", outcomes[outcome]);
            }
            SDL_SetWindowTitle(display->window, title);
            if (emulation->statistics)
                fprintf(stderr, "%s - audio %u Hz, %u ms queued, %u underruns, %u dropped, %u streams\n",
                        title, emulation->audio->rate,
                        emulation->audio->device ? SDL_GetQueuedAudioSize(emulation->audio->device) /
                            (HOST_AUDIO_RATE * HOST_AUDIO_FRAME / 1000) : 0,
                        emulation->audio->underruns, emulation->audio->dropped,
                        emulation->audio->streams);
        }
    }
    SDL_AtomicSet(&emulation->quit, 1);
    if (thread)
        SDL_WaitThread(thread, NULL);
#if M0_GP32_PC_TOOLS
    finish_shot(emulation);
#endif
    if (controller)
        SDL_GameControllerClose(controller);
}

/* ADDR=HEXBYTES: a RAM range rewritten while the game runs (a cheat). */
static bool parse_freeze(Emulation *emulation, const char *text)
{
    char *end;
    unsigned long address = strtoul(text, &end, 16);
    unsigned n = emulation->freezes, size = 0, byte;
    if (n == sizeof(emulation->freeze) / sizeof(emulation->freeze[0]) || *end++ != '=')
        return false;
    for (; end[0] && end[1] && size < sizeof(emulation->freeze[0].bytes); end += 2) {
        if (!isxdigit((unsigned char)end[0]) || !isxdigit((unsigned char)end[1]) ||
            sscanf(end, "%2x", &byte) != 1)
            return false;
        emulation->freeze[n].bytes[size++] = (uint8_t)byte;
    }
    if (*end || !size || address < S3C2400_SDRAM_BASE || address - S3C2400_SDRAM_BASE > GP32_RAM_SIZE - size)
        return false;
    emulation->freeze[n].offset = (uint32_t)(address - S3C2400_SDRAM_BASE);
    emulation->freeze[n].size = size;
    ++emulation->freezes;
    return true;
}

/* The Windows build has no console: report in a dialog too. */
static void show_error(const char *message)
{
    fprintf(stderr, "%s\n", message);
#ifdef _WIN32
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "m0boy GP32", message, NULL);
#endif
}

static void show_usage(const char *program)
{
    char message[1024];
    snprintf(message, sizeof(message),
             "Usage: %s [--bios BIOS.bin] [--hle] ROM [--smc card] [--eeprom file] "
             "[--readonly] [--cpi N] [--fullscreen] [--stats] [--freeze ADDR=HEX]"
#if M0_GP32_PC_TOOLS
             " [SHOT.png]\n"
             "F5 takes a shot into the shots folder, F9 resumes from the last one; a shot dropped on the window or given here resumes from it."
#endif
             , program);
    show_error(message);
}

int main(int argc, char **argv)
{
    SDL_SetMainReady();
#ifdef _WIN32
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
    if (argc == 3 && !strcmp(argv[1], "--identify"))
        return gp32_host_identify(argv[2]) ? EXIT_SUCCESS : EXIT_FAILURE;
    const char *program = argv[0];
#define SHOW_USAGE() do { show_usage(program); return EXIT_FAILURE; } while (0)
    bool hle = false, fullscreen = false, statistics = false;
    unsigned cpi = 0;
    const char *bios_path = NULL;
    const char *rom_path = NULL;
#if M0_GP32_PC_TOOLS
    const char *shot_path = NULL;
    char shot[1100];
#endif
    Gp32Host host = {0};
    Emulation cheats = {0};      /* only its --freeze list */
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--stats")) {
            statistics = true;
        } else if (!strcmp(argv[i], "--fullscreen")) {
            fullscreen = true;
        } else if (!strcmp(argv[i], "--cpi") && i + 1 < argc) {
            char *end;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (*end || !value || value > 64) SHOW_USAGE();
            cpi = (unsigned)value;
        } else if (!strcmp(argv[i], "--freeze") && i + 1 < argc) {
            if (!parse_freeze(&cheats, argv[++i])) SHOW_USAGE();
        } else if (!strcmp(argv[i], "--hle")) {
            hle = true;
        } else if (!strcmp(argv[i], "--bios") && i + 1 < argc && !bios_path) {
            bios_path = argv[++i];
        } else if (argv[i][0] == '-') {
            if (!strcmp(argv[i], "--readonly") || !strcmp(argv[i], "--ids")) {
                char *option[] = {argv[i]};
                if (!gp32_host_options(&host, 1, option)) SHOW_USAGE();
            } else if ((!strcmp(argv[i], "--smc") || !strcmp(argv[i], "--eeprom") ||
                        !strcmp(argv[i], "--exec") || !strcmp(argv[i], "--smcid") ||
                        !strcmp(argv[i], "--pduid") || !strcmp(argv[i], "--id-file")) && i + 1 < argc) {
                char *option[] = {argv[i], argv[i + 1]};
                if (!gp32_host_options(&host, 2, option)) SHOW_USAGE();
                ++i;
            } else {
                SHOW_USAGE();
            }
#if M0_GP32_PC_TOOLS
        } else if (!shot_path && strlen(argv[i]) > 4 &&
                   !SDL_strcasecmp(argv[i] + strlen(argv[i]) - 4, ".png")) {
            shot_path = argv[i];
#endif
        } else if (!rom_path) {
            rom_path = argv[i];
        } else {
            SHOW_USAGE();
        }
    }
    if (rom_path && !host.exec_path)
        host.exec_path = rom_path;
#if M0_GP32_PC_TOOLS
    if (shot_path) {
        /* Unpacking may change the current directory: make the name absolute. */
#ifdef _WIN32
        if (!_fullpath(shot, shot_path, sizeof(shot)))
            SHOW_USAGE();
#else
        char *absolute = realpath(shot_path, NULL);
        if (!absolute || strlen(absolute) >= sizeof(shot)) {
            show_error("Unable to read the shot");
            return EXIT_FAILURE;
        }
        strcpy(shot, absolute);
        free(absolute);
#endif
        shot_path = shot;
    }
#endif
#ifdef M0_GP32_BUNDLE
    if (!host.exec_path && !gp32_bundle_install(&host)) {
        show_error("Unable to unpack the embedded application: check that the "
                   "executable's directory or the user directory is writable");
        return EXIT_FAILURE;
    }
#endif
    host.hle = hle || !bios_path;
    if (!host.exec_path && host.hle)
        SHOW_USAGE();
    uint8_t *bios = host.hle ? NULL : malloc(GP32_BIOS_SIZE);
    S3c2400 *soc = malloc(sizeof(*soc));
    Gp32LcdCapture *capture = calloc(1, sizeof(*capture));
    FILE *file = host.hle ? NULL : fopen(bios_path, "rb");
    bool loaded = host.hle || (file && bios &&
                  fread(bios, 1, GP32_BIOS_SIZE, file) == GP32_BIOS_SIZE &&
                  fgetc(file) == EOF && !ferror(file));
    if (file)
        fclose(file);
    int status = EXIT_FAILURE;
    bool mounted = false;
    if (!loaded || !soc || !capture) {
        show_error("Expected a raw 512 KiB GP32 BIOS image and available memory");
    } else if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "%s\n", SDL_GetError());
    } else {
        SDL_Window *window = NULL;
        GpuDisplay display = {0};
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        window = SDL_CreateWindow("m0boy GP32", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  960, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL |
                                  (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
        if (window && gpu_display_open(&display, window) &&
            gp32_host_boot(&host, soc, &bios, (Arm920tConfig){0},
                                  (S3c2400Hooks){0}, (S3c2400Video){0})) {
            mounted = true;
            soc->lcd_capture = capture;
            set_host_time(soc);
            if (fullscreen)
                SDL_ShowCursor(SDL_DISABLE);
            HostAudio audio = {0};
            SDL_AtomicSet(&audio.volume, 8);
            /* The machine as booted, for the menu's restart. */
            S3c2400 *boot = malloc(sizeof(*boot));
            if (boot)
                memcpy(boot, soc, sizeof(*boot));
            SDL_AudioSpec spec = {.freq = HOST_AUDIO_RATE, .format = AUDIO_S16SYS, .channels = 2, .samples = 1024};
            if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0)
                audio.device = SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0);
            if (audio.device) {
                soc->audio = (S3c2400Audio){audio_sample, &audio};
            } else {
                fprintf(stderr, "Audio: %s\n", SDL_GetError());
            }
            Emulation emulation = {.host = &host, .soc = soc, .audio = &audio,
                                   .fixed_cpi = cpi, .ready = 1, .read = 2,
                                   .statistics = statistics};
            emulation.boot = boot;
            emulation.state_base = host.exec_path ? host.exec_path : bios_path;
            emulation.identity = gp32_state_identity(emulation.state_base);
#if M0_GP32_PC_TOOLS
            emulation.program = gp32_shot_program(emulation.state_base);
            shot_folder(&emulation);
#ifdef M0_GP32_AOT
            char found[1200];
            snprintf(found, sizeof(found), "%s/%s-found.code", emulation.folder, emulation.name);
            arm920t_aot_found(found);
#endif
#endif
            memcpy(emulation.freeze, cheats.freeze, sizeof(emulation.freeze));
            emulation.freezes = cheats.freezes;
#if M0_GP32_PC_TOOLS
            if (shot_path) {
                strcpy(emulation.shot, shot_path);
                SDL_AtomicSet(&emulation.command, COMMAND_SHOT);
            }
#endif
            emulation.lock = SDL_CreateMutex();
            emulation.frames = calloc(3, sizeof(*emulation.frames));
            if (boot && emulation.lock && emulation.frames)
                play(&emulation, &display);
            status = boot && emulation.lock && emulation.frames &&
                     !SDL_AtomicGet(&emulation.failed) ? EXIT_SUCCESS : EXIT_FAILURE;
            free(emulation.frames);
            free(boot);
            if (emulation.lock)
                SDL_DestroyMutex(emulation.lock);
            if (audio.device)
                SDL_CloseAudioDevice(audio.device);
        } else {
            char message[512];
            snprintf(message, sizeof(message), "GPU display or GP32 boot failed: %s", SDL_GetError());
            show_error(message);
        }
        gpu_display_close(&display);
        if (window)
            SDL_DestroyWindow(window);
        SDL_Quit();
    }
    if (!gp32_host_close(&host, mounted ? soc : NULL))
        status = EXIT_FAILURE;
    free(soc);
    free(capture);
    free(bios);
    return status;
}
