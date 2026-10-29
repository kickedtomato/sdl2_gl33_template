#include <glad/glad.h>   // include before SDL headers; do not include SDL_opengl.h
#include <SDL.h>
#include <SDL_main.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_EASY_FONT_IMPLEMENTATION
#include "stb_easy_font.h"
#pragma GCC diagnostic pop

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>

static const int WINDOW_WIDTH  = 800;
static const int WINDOW_HEIGHT = 600;

static const float MOVE_SPEED = 0.8f;   // NDC units per second
static const float MOVE_LIMIT = 0.4f;

static const char* VERT_SRC = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec3 aColor;
uniform vec2 uOffset;
out vec3 vColor;
void main() {
    gl_Position = vec4(aPos + uOffset, 0.0, 1.0);
    vColor = aColor;
}
)";

static const char* FRAG_SRC = R"(#version 330 core
in vec3 vColor;
out vec4 fragColor;
void main() {
    fragColor = vec4(vColor, 1.0);
}
)";

// Text uses pixel coordinates (top-left origin) converted to NDC in the shader
static const char* TEXT_VERT_SRC = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
uniform vec2 uScreen;
out vec4 vColor;
void main() {
    vec2 ndc = vec2(aPos.x / uScreen.x * 2.0 - 1.0,
                    1.0 - aPos.y / uScreen.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vColor = aColor;
}
)";

static const char* TEXT_FRAG_SRC = R"(#version 330 core
in vec4 vColor;
out vec4 fragColor;
void main() {
    fragColor = vColor;
}
)";

// Matches stb_easy_font's 16-byte vertex layout
struct TextVertex {
    float x, y, z;
    unsigned char r, g, b, a;
};

struct App {
    SDL_Window*   window  = nullptr;
    SDL_GLContext context = nullptr;
    bool          ready   = false;   // true once GL objects exist; gates the resize redraw
    bool          running = true;

    GLuint program = 0, vao = 0, vbo = 0;
    GLint  offsetLoc = -1;

    GLuint textProgram = 0, textVao = 0, textVbo = 0;
    GLint  textScreenLoc = -1;
    std::vector<TextVertex> textVerts;

    float posX = 0.0f, posY = 0.0f;
    bool  vsync = true;

    Uint64 lastCounter = 0;
    Uint64 perfFreq    = 0;
    double fpsTimer    = 0.0;
    int    fpsFrames   = 0;
    double fps         = 0.0;
    double frameMs     = 0.0;
};

static App g;

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "Shader compile error: %s\n", log);
        std::exit(EXIT_FAILURE);
    }
    return shader;
}

static GLuint create_program(const char* vs, const char* fs) {
    GLuint v = compile_shader(GL_VERTEX_SHADER, vs);
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs);

    GLuint program = glCreateProgram();
    glAttachShader(program, v);
    glAttachShader(program, f);
    glLinkProgram(program);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "Program link error: %s\n", log);
        std::exit(EXIT_FAILURE);
    }

    glDeleteShader(v);
    glDeleteShader(f);
    return program;
}

// Appends text to the overlay batch. Coordinates are in pixels from the top-left.
static void add_text(float x, float y, const char* text) {
    static unsigned char raw[256 * 1024];
    unsigned char color[4] = { 255, 255, 255, 255 };

    int quads = stb_easy_font_print(x, y, const_cast<char*>(text), color, raw, sizeof(raw));
    const TextVertex* src = reinterpret_cast<const TextVertex*>(raw);

    // stb emits quads (4 verts each); expand to triangles (6 verts each)
    for (int q = 0; q < quads; ++q) {
        const TextVertex* v = src + q * 4;
        g.textVerts.push_back(v[0]);
        g.textVerts.push_back(v[1]);
        g.textVerts.push_back(v[2]);
        g.textVerts.push_back(v[0]);
        g.textVerts.push_back(v[2]);
        g.textVerts.push_back(v[3]);
    }
}

static void draw_overlay(int w, int h) {
    g.textVerts.clear();

    char line[128];
    std::snprintf(line, sizeof(line), "FPS: %.0f   frame: %.2f ms", g.fps, g.frameMs);
    add_text(8, 8, line);

    std::snprintf(line, sizeof(line), "vsync: %s   pos: %.2f, %.2f",
                  g.vsync ? "on" : "off", g.posX, g.posY);
    add_text(8, 22, line);

    add_text(8, (float)h - 16.0f, "WASD/arrows: move   Space: reset   V: vsync   Esc: quit");

    if (g.textVerts.empty()) return;

    glUseProgram(g.textProgram);
    glUniform2f(g.textScreenLoc, (float)w, (float)h);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(g.textVao);
    glBindBuffer(GL_ARRAY_BUFFER, g.textVbo);
    glBufferData(GL_ARRAY_BUFFER,
                 g.textVerts.size() * sizeof(TextVertex),
                 g.textVerts.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g.textVerts.size());
    glBindVertexArray(0);

    glDisable(GL_BLEND);
}

// Draws one full frame. Safe to call from the event watch during window resize.
static void draw_frame() {
    if (!g.ready) return;

    int w, h;
    SDL_GL_GetDrawableSize(g.window, &w, &h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);

    glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(g.program);
    glUniform2f(g.offsetLoc, g.posX, g.posY);
    glBindVertexArray(g.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    draw_overlay(w, h);

    SDL_GL_SwapWindow(g.window);
}

// SDL calls this synchronously during events, including while the user drags a window edge.
// Redrawing here keeps the window updated during resize instead of freezing.
static int on_event_watch(void*, SDL_Event* e) {
    if (e->type == SDL_WINDOWEVENT &&
        (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
         e->window.event == SDL_WINDOWEVENT_EXPOSED)) {
        draw_frame();
    }
    return 0;
}

static void update(double dt) {
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    float dx = 0.0f, dy = 0.0f;
    if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT])  dx -= 1.0f;
    if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) dx += 1.0f;
    if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP])    dy += 1.0f;
    if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN])  dy -= 1.0f;

    g.posX += dx * MOVE_SPEED * (float)dt;
    g.posY += dy * MOVE_SPEED * (float)dt;
    g.posX = std::clamp(g.posX, -MOVE_LIMIT, MOVE_LIMIT);
    g.posY = std::clamp(g.posY, -MOVE_LIMIT, MOVE_LIMIT);

    // FPS is averaged over half-second windows
    g.fpsFrames++;
    g.fpsTimer += dt;
    if (g.fpsTimer >= 0.5) {
        g.fps     = g.fpsFrames / g.fpsTimer;
        g.frameMs = (g.fpsTimer / g.fpsFrames) * 1000.0;

        char title[128];
        std::snprintf(title, sizeof(title),
                      "SDL2 + OpenGL 3.3 | %.0f FPS | vsync %s",
                      g.fps, g.vsync ? "on" : "off");
        SDL_SetWindowTitle(g.window, title);

        g.fpsFrames = 0;
        g.fpsTimer  = 0.0;
    }
}

int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    g.window = SDL_CreateWindow(
        "SDL2 + OpenGL 3.3",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!g.window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return EXIT_FAILURE;
    }

    g.context = SDL_GL_CreateContext(g.window);
    if (!g.context) {
        std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g.window);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    SDL_GL_SetSwapInterval(g.vsync ? 1 : 0);

    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        std::fprintf(stderr, "Failed to load OpenGL functions\n");
        SDL_GL_DeleteContext(g.context);
        SDL_DestroyWindow(g.window);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    std::printf("GL_VERSION:  %s\n", glGetString(GL_VERSION));
    std::printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));

    // Triangle
    g.program   = create_program(VERT_SRC, FRAG_SRC);
    g.offsetLoc = glGetUniformLocation(g.program, "uOffset");

    const GLfloat vertices[] = {
         0.0f,  0.6f,   1.0f, 0.0f, 0.0f,
        -0.6f, -0.6f,   0.0f, 1.0f, 0.0f,
         0.6f, -0.6f,   0.0f, 0.0f, 1.0f,
    };
    const GLsizei stride = 5 * sizeof(GLfloat);

    glGenVertexArrays(1, &g.vao);
    glGenBuffers(1, &g.vbo);
    glBindVertexArray(g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(2 * sizeof(GLfloat)));
    glBindVertexArray(0);

    // Overlay text
    g.textProgram     = create_program(TEXT_VERT_SRC, TEXT_FRAG_SRC);
    g.textScreenLoc   = glGetUniformLocation(g.textProgram, "uScreen");

    glGenVertexArrays(1, &g.textVao);
    glGenBuffers(1, &g.textVbo);
    glBindVertexArray(g.textVao);
    glBindBuffer(GL_ARRAY_BUFFER, g.textVbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(TextVertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(TextVertex), (void*)12);
    glBindVertexArray(0);

    g.ready = true;
    SDL_AddEventWatch(on_event_watch, nullptr);

    // Timing
    g.perfFreq    = SDL_GetPerformanceFrequency();
    g.lastCounter = SDL_GetPerformanceCounter();

    while (g.running) {
        Uint64 now = SDL_GetPerformanceCounter();
        double dt = (double)(now - g.lastCounter) / (double)g.perfFreq;
        g.lastCounter = now;
        if (dt > 0.1) dt = 0.1;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT:
                    g.running = false;
                    break;
                case SDL_KEYDOWN:
                    if (e.key.repeat) break;
                    switch (e.key.keysym.sym) {
                        case SDLK_ESCAPE:
                            g.running = false;
                            break;
                        case SDLK_SPACE:
                            g.posX = 0.0f;
                            g.posY = 0.0f;
                            break;
                        case SDLK_v:
                            g.vsync = !g.vsync;
                            SDL_GL_SetSwapInterval(g.vsync ? 1 : 0);
                            break;
                    }
                    break;
            }
        }

        update(dt);
        draw_frame();
    }

    SDL_DelEventWatch(on_event_watch, nullptr);
    glDeleteBuffers(1, &g.vbo);
    glDeleteVertexArrays(1, &g.vao);
    glDeleteProgram(g.program);
    glDeleteBuffers(1, &g.textVbo);
    glDeleteVertexArrays(1, &g.textVao);
    glDeleteProgram(g.textProgram);
    SDL_GL_DeleteContext(g.context);
    SDL_DestroyWindow(g.window);
    SDL_Quit();
    return EXIT_SUCCESS;
}