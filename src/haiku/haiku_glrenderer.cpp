/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the OpenGL 3 renderer.
 *
 *          A port of Qt's OpenGLRenderer (itself from PCem) to a BGLView
 *          laid over RendererView: the same GLSL/GLSLP shader pipeline
 *          (user shaders from gl3_shader_file[] with their parameters,
 *          LUT textures, previous frames, FBO scaling), the OpenGL input
 *          scale and stretch mode, VSync, the target framerate and
 *          screenshots of the shaded picture.
 *
 *          Haiku's BGLView gives a compatibility-profile context (Mesa);
 *          everything used here is core in OpenGL 3.0, which is checked at
 *          start, falling back to software like Qt does. All GL work runs
 *          on the window thread between LockGL() and UnlockGL().
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#define GL_GLEXT_PROTOTYPES
#include <Alert.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <File.h>
#include <GLView.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <NodeInfo.h>
#include <TranslationUtils.h>
#include <TranslatorFormats.h>
#include <TranslatorRoster.h>
#include <Window.h>
#include <GL/glext.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "haiku_glrenderer.hpp"
#include "haiku_renderer_backend.hpp"
#include "haiku_osd.hpp"
#include "haiku_rendererview.hpp"

extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/video.h>
#include <86box/path.h>
#include <86box/ini.h>
#include <86box/config.h>
#include <86box/renderdefs.h>
#include <86box/qt_glslp_parser.h>

/* The user shader list (config.c loads and saves it). */
char gl3_shader_file[MAX_USER_SHADERS][512];
}

#define SCALE_SOURCE   0
#define SCALE_VIEWPORT 1
#define SCALE_ABSOLUTE 2

enum : uint32 {
    MSG_GL_RENDER = 'glrd', /* target framerate tick */
};

static GLfloat matrix[] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

static const char *vertex_shader_default_tex_src = "#version 130\n"
                                                   "\n"
                                                   "in vec4 VertexCoord;\n"
                                                   "in vec2 TexCoord;\n"
                                                   "\n"
                                                   "out vec2 texCoord;\n"
                                                   "\n"
                                                   "void main()\n"
                                                   "{\n"
                                                   "       gl_Position = VertexCoord;\n"
                                                   "       texCoord = TexCoord;\n"
                                                   "}\n";

static const char *fragment_shader_default_tex_src = "#version 130\n"
                                                     "\n"
                                                     "in vec2 texCoord;\n"
                                                     "uniform sampler2D Texture;\n"
                                                     "\n"
                                                     "out vec4 color;"
                                                     "\n"
                                                     "void main()\n"
                                                     "{\n"
                                                     "       color = texture(Texture, texCoord);\n"
                                                     "       color.a = 1.0;\n"
                                                     "}\n";

static const char *vertex_shader_default_color_src = "#version 130\n"
                                                     "\n"
                                                     "in vec4 VertexCoord;\n"
                                                     "in vec4 Color;\n"
                                                     "\n"
                                                     "out vec4 color;\n"
                                                     "\n"
                                                     "void main()\n"
                                                     "{\n"
                                                     "       gl_Position = VertexCoord;\n"
                                                     "       color = Color;\n"
                                                     "}\n";

static const char *fragment_shader_default_color_src = "#version 130\n"
                                                       "\n"
                                                       "in vec4 color;\n"
                                                       "\n"
                                                       "out vec4 outColor;"
                                                       "\n"
                                                       "void main()\n"
                                                       "{\n"
                                                       "       outColor = color;\n"
                                                       "       outColor.a = 1.0;\n"
                                                       "}\n";

struct render_data {
    int                 pass;
    struct glsl_shader *shader;
    struct shader_pass *shader_pass;
    GLfloat            *output_size;
    struct shader_pass *orig_pass;
    GLint               texture;
    int                 frame_count;
};

struct int_rect {
    int x;
    int y;
    int w;
    int h;
};

void
haiku_gl_message(const char *title, const char *text)
{
    BAlert *alert = new BAlert(title, text, "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_STOP_ALERT);
    alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
    alert->Go(nullptr);
}

static void
gl_error(const char *title, const char *format, const char *detail)
{
    char text[4096];
    snprintf(text, sizeof(text), format, detail);
    haiku_gl_message(title, text);
}

static inline int
next_pow2(unsigned int n)
{
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n++;

    return n;
}

static void
integer_scale(double *d, double *g)
{
    double ratio;

    if (*d > *g) {
        ratio = std::floor(*d / *g);
        *d    = *g * ratio;
    } else {
        ratio = std::ceil(*d / *g);
        *d    = *g / ratio;
    }
}

/* standalone_scale() from qt_renderercommon.cpp. */
static void
standalone_scale(int_rect &destination, int width, int height, int_rect source, int scalemode)
{
    double dx;
    double dy;
    double dw;
    double dh;
    double gsr;

    double hw  = width;
    double hh  = height;
    double gw  = source.w;
    double gh  = source.h;
    double hsr = hw / hh;
    double r43 = 4.0 / 3.0;

    switch (scalemode) {
        case FULLSCR_SCALE_INT:
        case FULLSCR_SCALE_INT43:
            gsr = gw / gh;
            if (scalemode == FULLSCR_SCALE_INT43) {
                gh  = gw / r43;
                gsr = r43;
            }
            if (gsr <= hsr) {
                dw = hh * gsr;
                dh = hh;
            } else {
                dw = hw;
                dh = hw / gsr;
            }
            integer_scale(&dw, &gw);
            integer_scale(&dh, &gh);
            dx          = (hw - dw) / 2.0;
            dy          = (hh - dh) / 2.0;
            destination = { (int) dx, (int) dy, (int) dw, (int) dh };
            break;
        case FULLSCR_SCALE_43:
        case FULLSCR_SCALE_KEEPRATIO:
            gsr = (scalemode == FULLSCR_SCALE_43) ? r43 : (gw / gh);
            if (gsr <= hsr) {
                dw = hh * gsr;
                dh = hh;
            } else {
                dw = hw;
                dh = hw / gsr;
            }
            dx          = (hw - dw) / 2.0;
            dy          = (hh - dh) / 2.0;
            destination = { (int) dx, (int) dy, (int) dw, (int) dh };
            break;
        case FULLSCR_SCALE_FULL:
        default:
            destination = { 0, 0, (int) hw, (int) hh };
            break;
    }
}

static void
set_scale_mode(char *scale, int *dst)
{
    if (!strcmp(scale, "viewport"))
        *dst = SCALE_VIEWPORT;
    else if (!strcmp(scale, "absolute"))
        *dst = SCALE_ABSOLUTE;
    else
        *dst = SCALE_SOURCE;
}

static void
setup_scale(struct shader *shader, struct shader_pass *pass)
{
    set_scale_mode(shader->scale_type_x, &pass->scale.mode[0]);
    set_scale_mode(shader->scale_type_y, &pass->scale.mode[1]);
    pass->scale.value[0] = shader->scale_x;
    pass->scale.value[1] = shader->scale_y;
}

static GLint
wrap_mode(const char *mode)
{
    if (!strcmp(mode, "repeat"))
        return GL_REPEAT;
    if (!strcmp(mode, "mirrored_repeat"))
        return GL_MIRRORED_REPEAT;
    if (!strcmp(mode, "clamp_to_edge"))
        return GL_CLAMP_TO_EDGE;
    return GL_CLAMP_TO_BORDER;
}

/* Loads an image for a LUT texture through the Translation Kit, flipped
   as GL expects. The image keeps its size: OpenGL 3 has non-power-of-two
   textures (Qt scales them up to a power of two). */
static int
load_texture(const char *f, struct shader_texture *tex)
{
    BBitmap *image = BTranslationUtils::GetBitmap(f);
    if (image == nullptr)
        return 0;

    BBitmap rgba(image->Bounds(), B_RGBA32);
    if (rgba.ImportBits(image) != B_OK) {
        delete image;
        return 0;
    }
    delete image;

    const int    width  = rgba.Bounds().IntegerWidth() + 1;
    const int    height = rgba.Bounds().IntegerHeight() + 1;
    const int32  bpr    = rgba.BytesPerRow();
    uint8       *data   = static_cast<uint8 *>(malloc((size_t) width * height * 4));
    const uint8 *bits   = static_cast<const uint8 *>(rgba.Bits());
    for (int y = 0; y < height; y++)
        memcpy(data + ((size_t) y * width * 4), bits + ((size_t) (height - y - 1) * bpr), (size_t) width * 4);

    tex->width           = width;
    tex->height          = height;
    tex->internal_format = GL_RGB8;
    tex->format          = GL_BGRA;
    tex->type            = GL_UNSIGNED_BYTE;
    tex->data            = data;
    return 1;
}

/* Saves a B_RGB32 bitmap as PNG; takes ownership of it. */
static void
save_png(BBitmap *image, const char *path)
{
    BBitmapStream stream(image);
    BFile         file(path, B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);

    if ((file.InitCheck() == B_OK)
        && (BTranslatorRoster::Default()->Translate(&stream, nullptr, nullptr, &file, B_PNG_FORMAT) == B_OK)) {
        BNodeInfo info(&file);
        info.SetType("image/png");
    }

    BBitmap *detached = nullptr;
    stream.DetachBitmap(&detached);
    delete detached;
}

class GLRendererView : public BGLView, public RendererBackend {
public:
    explicit GLRendererView(RendererView *owner);
    ~GLRendererView() override;

    /* RendererBackend */
    int    VidApi() const override { return RENDERER_OPENGL3; }
    BView *ChildView() override { return this; }
    bool   Init() override;
    bool   TakesScreenshots() const override { return true; }
    void   FrameReady(bool resized) override;
    void   Refresh() override;

    /* BGLView */
    void DetachedFromWindow() override;
    void FrameResized(float width, float height) override;
    void MouseDown(BPoint where) override;
    void MouseUp(BPoint where) override;
    void MouseMoved(BPoint where, uint32 transit, const BMessage *drag) override;
    void MessageReceived(BMessage *message) override;

private:
    RendererView         *owner;
    BMessageRunner       *render_timer;
    bool                  initialized;
    int                   max_texture_size;
    int                   frame_counter;
    int                   gl_version[2];
    int                   tex_w; /* size of the scene texture */
    int                   tex_h;
    struct shader_texture scene_texture;
    glsl_t               *active_shader;

    void     Render();
    void     Finalize();
    int_rect SceneRenderRect(int w, int h) const;

    void    create_scene_shader();
    void    create_texture(struct shader_texture *tex);
    void    create_fbo(struct shader_fbo *fbo);
    void    recreate_fbo(struct shader_fbo *fbo, int width, int height);
    void    setup_fbo(struct shader *shader, struct shader_fbo *fbo);
    glsl_t *load_glslp(glsl_t *glsl, int num_shader, const char *f);
    glsl_t *load_shaders(int num, char shaders[MAX_USER_SHADERS][512]);
    int     compile_shader(GLenum shader_type, const char *prepend, const char *program, int *dst);
    int     create_default_shader_tex(struct shader_pass *pass);
    int     create_default_shader_color(struct shader_pass *pass);
    int     create_program(struct shader_program *program);
    void    find_uniforms(struct glsl_shader *glsl, int num_pass);
    void    delete_texture(struct shader_texture *tex);
    void    delete_fbo(struct shader_fbo *fbo);
    void    delete_program(struct shader_program *program);
    void    delete_vbo(struct shader_vbo *vbo);
    void    delete_pass(struct shader_pass *pass);
    void    delete_prev(struct shader_prev *prev);
    void    delete_shader(struct glsl_shader *glsl);
    void    delete_glsl(glsl_t *glsl);
    void    read_shader_config();
    void    render_pass(struct render_data *data);
};

RendererBackend *
haiku_gl_create(RendererView *owner)
{
    return new GLRendererView(owner);
}

GLRendererView::GLRendererView(RendererView *owner)
    : BGLView(owner->Bounds(), "gl renderer", B_FOLLOW_ALL, B_WILL_DRAW | B_FRAME_EVENTS, BGL_RGB | BGL_DOUBLE)
    , owner(owner)
    , render_timer(nullptr)
    , initialized(false)
    , max_texture_size(65536)
    , frame_counter(0)
    , gl_version { 0, 0 }
    , tex_w(0)
    , tex_h(0)
    , active_shader(nullptr)
{
    memset(&scene_texture, 0, sizeof(scene_texture));
}

GLRendererView::~GLRendererView()
{
    delete render_timer;
}

void
GLRendererView::DetachedFromWindow()
{
    delete render_timer;
    render_timer = nullptr;
    Finalize();
    BGLView::DetachedFromWindow();
}

/* ------------------------------------------------------------ GL helpers */

int
GLRendererView::create_program(struct shader_program *program)
{
    GLint status;
    program->id = glCreateProgram();
    glAttachShader(program->id, program->vertex_shader);
    glAttachShader(program->id, program->fragment_shader);

    glLinkProgram(program->id);

    glDeleteShader(program->vertex_shader);
    glDeleteShader(program->fragment_shader);

    program->vertex_shader = program->fragment_shader = 0;

    glGetProgramiv(program->id, GL_LINK_STATUS, &status);

    if (!status) {
        GLint maxLength = 0;
        GLint length    = 0;
        glGetProgramiv(program->id, GL_INFO_LOG_LENGTH, &maxLength);
        char *log = static_cast<char *>(calloc(1, maxLength + 1));
        glGetProgramInfoLog(program->id, maxLength, &length, log);
        gl_error("GLSL Error", "Program not linked:\n\n%s", log);
        free(log);
        return 0;
    }

    return 1;
}

int
GLRendererView::compile_shader(GLenum shader_type, const char *prepend, const char *program, int *dst)
{
    std::string source(program);
    std::string version;

    /* Take out the #version directives (Qt: ^\s*(#version\s+\w+)) and put
       the first one first; without one, use the context's GLSL version. */
    size_t line = 0;
    while (line < source.size()) {
        size_t start = line;
        while ((start < source.size()) && ((source[start] == ' ') || (source[start] == '\t')))
            start++;
        if (source.compare(start, 8, "#version") == 0) {
            size_t end = start + 8;
            while ((end < source.size()) && isspace((unsigned char) source[end]))
                end++;
            while ((end < source.size()) && (isalnum((unsigned char) source[end]) || (source[end] == '_')))
                end++;
            if (version.empty())
                version = source.substr(start, end - start) + "\n";
            source.erase(line, end - line);
        }
        line = source.find('\n', line);
        if (line == std::string::npos)
            break;
        line++;
    }
    if (version.empty()) {
        int ver = gl_version[0] * 100 + gl_version[1] * 10;
        if (ver == 300)
            ver = 130;
        else if (ver == 310)
            ver = 140;
        else if (ver == 320)
            ver = 150;
        version = "#version " + std::to_string(ver) + "\n";
    }

    /* Remove parameter lines. */
    line = 0;
    while (line < source.size()) {
        size_t start = line;
        while ((start < source.size()) && ((source[start] == ' ') || (source[start] == '\t')))
            start++;
        size_t end = source.find('\n', line);
        if (source.compare(start, 17, "#pragma parameter") == 0) {
            source.erase(line, (end == std::string::npos) ? std::string::npos : (end + 1 - line));
            continue;
        }
        if (end == std::string::npos)
            break;
        line = end + 1;
    }

    const char *sources[5];
    sources[0] = version.c_str();
    sources[1] = "\n#extension GL_ARB_shading_language_420pack : enable\n";
    sources[2] = prepend ? prepend : "";
    sources[3] = "\n#line 1\n";
    sources[4] = source.c_str();

    GLuint shader = glCreateShader(shader_type);
    glShaderSource(shader, 5, sources, NULL);
    glCompileShader(shader);

    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        char *log = static_cast<char *>(calloc(1, length + 1));
        glGetShaderInfoLog(shader, length, &length, log);
        gl_error("GLSL Error", "Could not compile shader:\n\n%s", log);
        free(log);
        glDeleteShader(shader);
        return 0;
    }

    *dst = shader;

    return 1;
}

void
GLRendererView::find_uniforms(struct glsl_shader *glsl, int num_pass)
{
    char                s[50];
    struct shader_pass *pass = &glsl->passes[num_pass];
    int                 p    = pass->program.id;
    glUseProgram(p);

    struct shader_uniforms *u = &pass->uniforms;

    u->mvp_matrix   = glGetUniformLocation(p, "MVPMatrix");
    u->vertex_coord = glGetAttribLocation(p, "VertexCoord");
    u->tex_coord    = glGetAttribLocation(p, "TexCoord");
    u->color        = glGetAttribLocation(p, "Color");

    u->frame_count     = glGetUniformLocation(p, "FrameCount");
    u->frame_direction = glGetUniformLocation(p, "FrameDirection");

    u->texture      = glGetUniformLocation(p, "Texture");
    u->input_size   = glGetUniformLocation(p, "InputSize");
    u->texture_size = glGetUniformLocation(p, "TextureSize");
    u->output_size  = glGetUniformLocation(p, "OutputSize");

    u->orig.texture      = glGetUniformLocation(p, "OrigTexture");
    u->orig.input_size   = glGetUniformLocation(p, "OrigInputSize");
    u->orig.texture_size = glGetUniformLocation(p, "OrigTextureSize");

    for (int i = 0; i < glsl->num_passes; ++i) {
        snprintf(s, sizeof(s) - 1, "Pass%dTexture", (i + 1));
        u->pass[i].texture = glGetUniformLocation(p, s);
        snprintf(s, sizeof(s) - 1, "Pass%dInputSize", (i + 1));
        u->pass[i].input_size = glGetUniformLocation(p, s);
        snprintf(s, sizeof(s) - 1, "Pass%dTextureSize", (i + 1));
        u->pass[i].texture_size = glGetUniformLocation(p, s);

        snprintf(s, sizeof(s) - 1, "PassPrev%dTexture", num_pass - i);
        u->prev_pass[i].texture = glGetUniformLocation(p, s);
        snprintf(s, sizeof(s) - 1, "PassPrev%dInputSize", num_pass - i);
        u->prev_pass[i].input_size = glGetUniformLocation(p, s);
        snprintf(s, sizeof(s) - 1, "PassPrev%dTextureSize", num_pass - i);
        u->prev_pass[i].texture_size = glGetUniformLocation(p, s);
    }

    u->prev[0].texture   = glGetUniformLocation(p, "PrevTexture");
    u->prev[0].tex_coord = glGetAttribLocation(p, "PrevTexCoord");
    for (int i = 1; i < MAX_PREV; ++i) {
        snprintf(s, sizeof(s) - 1, "Prev%dTexture", i);
        u->prev[i].texture = glGetUniformLocation(p, s);
        snprintf(s, sizeof(s) - 1, "Prev%dTexCoord", i);
        u->prev[i].tex_coord = glGetAttribLocation(p, s);
    }
    for (int i = 0; i < MAX_PREV; ++i)
        if (u->prev[i].texture >= 0)
            glsl->has_prev = 1;

    for (int i = 0; i < glsl->num_lut_textures; ++i)
        u->lut_textures[i] = glGetUniformLocation(p, glsl->lut_textures[i].name);

    for (int i = 0; i < glsl->num_parameters; ++i)
        u->parameters[i] = glGetUniformLocation(p, glsl->parameters[i].id);

    glUseProgram(0);
}

void
GLRendererView::create_texture(struct shader_texture *tex)
{
    if (tex->width > max_texture_size)
        tex->width = max_texture_size;
    if (tex->height > max_texture_size)
        tex->height = max_texture_size;
    glGenTextures(1, (GLuint *) &tex->id);
    glBindTexture(GL_TEXTURE_2D, tex->id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, tex->wrap_mode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, tex->wrap_mode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, tex->min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, tex->mag_filter);
    glTexImage2D(GL_TEXTURE_2D, 0, tex->internal_format, tex->width, tex->height, 0, tex->format, tex->type, tex->data);
    if (tex->mipmap)
        glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void
GLRendererView::delete_texture(struct shader_texture *tex)
{
    if (tex->id > 0)
        glDeleteTextures(1, (GLuint *) &tex->id);
    tex->id = 0;
}

void
GLRendererView::delete_fbo(struct shader_fbo *fbo)
{
    if (fbo->id >= 0) {
        glDeleteFramebuffers(1, (GLuint *) &fbo->id);
        delete_texture(&fbo->texture);
    }
}

void
GLRendererView::delete_program(struct shader_program *program)
{
    if (program->vertex_shader)
        glDeleteShader(program->vertex_shader);
    if (program->fragment_shader)
        glDeleteShader(program->fragment_shader);
    glDeleteProgram(program->id);
}

void
GLRendererView::delete_vbo(struct shader_vbo *vbo)
{
    if (vbo->color >= 0)
        glDeleteBuffers(1, (GLuint *) &vbo->color);
    glDeleteBuffers(1, (GLuint *) &vbo->vertex_coord);
    glDeleteBuffers(1, (GLuint *) &vbo->tex_coord);
}

void
GLRendererView::delete_pass(struct shader_pass *pass)
{
    delete_fbo(&pass->fbo);
    delete_vbo(&pass->vbo);
    delete_program(&pass->program);
    glDeleteVertexArrays(1, (GLuint *) &pass->vertex_array);
}

void
GLRendererView::delete_prev(struct shader_prev *prev)
{
    delete_fbo(&prev->fbo);
    delete_vbo(&prev->vbo);
}

void
GLRendererView::delete_shader(struct glsl_shader *glsl)
{
    for (int i = 0; i < glsl->num_passes; ++i)
        delete_pass(&glsl->passes[i]);
    if (glsl->has_prev) {
        delete_pass(&glsl->prev_scene);
        for (int i = 0; i < MAX_PREV; ++i)
            delete_prev(&glsl->prev[i]);
    }
    for (int i = 0; i < glsl->num_lut_textures; ++i)
        delete_texture(&glsl->lut_textures[i].texture);
}

void
GLRendererView::delete_glsl(glsl_t *glsl)
{
    for (int i = 0; i < glsl->num_shaders; ++i)
        delete_shader(&glsl->shaders[i]);
    delete_pass(&glsl->scene);
    delete_pass(&glsl->fs_color);
    delete_pass(&glsl->final_pass);
}

void
GLRendererView::create_fbo(struct shader_fbo *fbo)
{
    create_texture(&fbo->texture);

    glGenFramebuffers(1, (GLuint *) &fbo->id);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo->id);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo->texture.id, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        pclog("OpenGL: could not create framebuffer\n");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void
GLRendererView::setup_fbo(struct shader *shader, struct shader_fbo *fbo)
{
    fbo->texture.internal_format = GL_RGB8;
    fbo->texture.format          = GL_RGB;
    /* filter_linear is -1 when the preset doesn't set it, use nearest then. */
    fbo->texture.min_filter = fbo->texture.mag_filter = (shader->filter_linear > 0) ? GL_LINEAR : GL_NEAREST;
    fbo->texture.width                                = 2048;
    fbo->texture.height                               = 2048;
    fbo->texture.type                                 = GL_UNSIGNED_BYTE;
    fbo->texture.wrap_mode                            = wrap_mode(shader->wrap_mode);
    fbo->srgb                                         = 0;
    if (shader->srgb_framebuffer) {
        fbo->texture.internal_format = GL_SRGB8;
        fbo->srgb                    = 1;
    } else if (shader->float_framebuffer) {
        fbo->texture.internal_format = GL_RGB32F;
        fbo->texture.type            = GL_FLOAT;
    }

    if (fbo->texture.mipmap)
        fbo->texture.min_filter = (shader->filter_linear > 0) ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;

    create_fbo(fbo);
}

void
GLRendererView::recreate_fbo(struct shader_fbo *fbo, int width, int height)
{
    if ((width != fbo->texture.width) || (height != fbo->texture.height)) {
        glDeleteFramebuffers(1, (GLuint *) &fbo->id);
        glDeleteTextures(1, (GLuint *) &fbo->texture.id);
        fbo->texture.width  = width;
        fbo->texture.height = height;
        create_fbo(fbo);
    }
}

int
GLRendererView::create_default_shader_tex(struct shader_pass *pass)
{
    if (!compile_shader(GL_VERTEX_SHADER, 0, vertex_shader_default_tex_src, &pass->program.vertex_shader)
        || !compile_shader(GL_FRAGMENT_SHADER, 0, fragment_shader_default_tex_src, &pass->program.fragment_shader)
        || !create_program(&pass->program))
        return 0;
    glGenVertexArrays(1, (GLuint *) &pass->vertex_array);

    struct shader_uniforms *u = &pass->uniforms;
    int                     p = pass->program.id;
    memset(u, -1, sizeof(struct shader_uniforms));
    u->vertex_coord      = glGetAttribLocation(p, "VertexCoord");
    u->tex_coord         = glGetAttribLocation(p, "TexCoord");
    u->texture           = glGetUniformLocation(p, "Texture");
    pass->scale.mode[0]  = pass->scale.mode[1] = SCALE_SOURCE;
    pass->scale.value[0] = pass->scale.value[1] = 1.0f;
    pass->fbo.id                                = -1;
    pass->active                                = 1;
    return 1;
}

int
GLRendererView::create_default_shader_color(struct shader_pass *pass)
{
    if (!compile_shader(GL_VERTEX_SHADER, 0, vertex_shader_default_color_src, &pass->program.vertex_shader)
        || !compile_shader(GL_FRAGMENT_SHADER, 0, fragment_shader_default_color_src, &pass->program.fragment_shader)
        || !create_program(&pass->program))
        return 0;
    glGenVertexArrays(1, (GLuint *) &pass->vertex_array);

    struct shader_uniforms *u = &pass->uniforms;
    int                     p = pass->program.id;
    memset(u, -1, sizeof(struct shader_uniforms));
    u->vertex_coord      = glGetAttribLocation(p, "VertexCoord");
    u->color             = glGetAttribLocation(p, "Color");
    pass->scale.mode[0]  = pass->scale.mode[1] = SCALE_SOURCE;
    pass->scale.value[0] = pass->scale.value[1] = 1.0f;
    pass->fbo.id                                = -1;
    pass->active                                = 1;
    return 1;
}

/* The default scene shader. */
void
GLRendererView::create_scene_shader()
{
    struct shader scene_shader_conf;
    memset(&scene_shader_conf, 0, sizeof(struct shader));
    create_default_shader_tex(&active_shader->scene);
    scene_shader_conf.filter_linear = video_filter_method;
    if ((active_shader->num_shaders > 0) && (active_shader->shaders[0].input_filter_linear >= 0))
        scene_shader_conf.filter_linear = active_shader->shaders[0].input_filter_linear;
    setup_fbo(&scene_shader_conf, &active_shader->scene.fbo);

    memset(&scene_shader_conf, 0, sizeof(struct shader));
    create_default_shader_color(&active_shader->fs_color);
    setup_fbo(&scene_shader_conf, &active_shader->fs_color.fbo);
}

glsl_t *
GLRendererView::load_glslp(glsl_t *glsl, int num_shader, const char *f)
{
    glslp_t *p = glslp_parse(f);

    if (p == nullptr)
        return nullptr;

    char path[512];
    char file[1024];
    int  failed = 0;
    snprintf(path, sizeof(path), "%s", f);
    char *filename = path_get_filename(path);

    struct glsl_shader *gshader = &glsl->shaders[num_shader];

    strcpy(gshader->name, p->name);
    *filename = 0;

    gshader->num_lut_textures = p->num_textures;

    for (int i = 0; i < p->num_textures; ++i) {
        struct texture *texture = &p->textures[i];

        snprintf(file, sizeof(file) - 1, "%s%s", path, texture->path);

        struct shader_lut_texture *tex = &gshader->lut_textures[i];
        strcpy(tex->name, texture->name);

        if (!load_texture(file, &tex->texture)) {
            gl_error("GLSL Error", "Could not load texture: %s", file);
            failed = 1;
            break;
        }

        tex->texture.wrap_mode = wrap_mode(texture->wrap_mode);
        tex->texture.mipmap    = texture->mipmap;

        tex->texture.min_filter = tex->texture.mag_filter = texture->linear ? GL_LINEAR : GL_NEAREST;
        if (tex->texture.mipmap)
            tex->texture.min_filter = texture->linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;

        create_texture(&tex->texture);
        free(tex->texture.data);
        tex->texture.data = 0;
    }

    if (!failed) {
        gshader->input_filter_linear = p->input_filter_linear;

        gshader->num_parameters = p->num_parameters;
        for (int j = 0; j < gshader->num_parameters; ++j)
            memcpy(&gshader->parameters[j], &p->parameters[j], sizeof(struct shader_parameter));

        gshader->num_passes = p->num_shaders;

        for (int i = 0; i < p->num_shaders; ++i) {
            struct shader      *shader = &p->shaders[i];
            struct shader_pass *pass   = &gshader->passes[i];

            /* filter_linear0 is the filtering of the preset's input,
               filter_linearN that of the output of pass N-1, which is the
               input of pass N. */
            pass->input_filter_linear = (i == 0) ? p->input_filter_linear : p->shaders[i - 1].filter_linear;

            strcpy(pass->alias, shader->alias);
            if (!strlen(pass->alias))
                snprintf(pass->alias, sizeof(pass->alias) - 1, "Pass %u", (i + 1));

            if (!shader->shader_program) {
                gl_error("GLSL Error", "Could not load shader: %s", shader->shader_fn);
                failed = 1;
                break;
            }
            failed = !compile_shader(GL_VERTEX_SHADER, "#define VERTEX\n#define PARAMETER_UNIFORM\n",
                                     shader->shader_program, &pass->program.vertex_shader)
                || !compile_shader(GL_FRAGMENT_SHADER, "#define FRAGMENT\n#define PARAMETER_UNIFORM\n",
                                   shader->shader_program, &pass->program.fragment_shader);
            if (failed)
                break;

            if (!create_program(&pass->program)) {
                failed = 1;
                break;
            }
            pass->frame_count_mod  = shader->frame_count_mod;
            pass->fbo.mipmap_input = shader->mipmap_input;

            glGenVertexArrays(1, (GLuint *) &pass->vertex_array);
            find_uniforms(gshader, i);
            setup_scale(shader, pass);
            if (i == p->num_shaders - 1) {
                /* The last pass may or may not be an FBO, depending on its
                   scale. */
                if (num_shader == glsl->num_shaders - 1) {
                    pass->fbo.id = -1;

                    for (uint8_t j = 0; j < 2; ++j) {
                        if ((pass->scale.mode[j] != SCALE_SOURCE) || (pass->scale.value[j] != 1)) {
                            setup_fbo(shader, &pass->fbo);
                            break;
                        }
                    }
                } else {
                    /* Whether the next shader's first pass wants its input
                       mipmapped or filtered. */
                    pass->fbo.texture.mipmap = (glsl->shaders[num_shader + 1].num_passes > 0)
                        && glsl->shaders[num_shader + 1].passes[0].fbo.mipmap_input;
                    if ((glsl->shaders[num_shader + 1].num_passes > 0)
                        && (glsl->shaders[num_shader + 1].input_filter_linear >= 0))
                        shader->filter_linear = glsl->shaders[num_shader + 1].input_filter_linear;
                    setup_fbo(shader, &pass->fbo);
                }
            } else {
                /* Mipmaps of this pass if the next pass wants its input
                   mipmapped. */
                pass->fbo.texture.mipmap = ((i + 1) < p->num_shaders) && p->shaders[i + 1].mipmap_input;
                setup_fbo(shader, &pass->fbo);
            }
            if (pass->fbo.srgb)
                glsl->srgb = 1;
            pass->active = 1;
        }
        if (!failed && gshader->has_prev) {
            struct shader scene_shader_conf;
            memset(&scene_shader_conf, 0, sizeof(struct shader));
            for (int i = 0; i < MAX_PREV; ++i)
                setup_fbo(&scene_shader_conf, &gshader->prev[i].fbo);
        }
    }

    glslp_free(p);

    return failed ? nullptr : glsl;
}

glsl_t *
GLRendererView::load_shaders(int num, char shaders[MAX_USER_SHADERS][512])
{
    glsl_t *glsl = static_cast<glsl_t *>(calloc(1, sizeof(glsl_t)));

    glsl->num_shaders = num;
    int failed        = 0;
    for (int i = num - 1; i >= 0; --i) {
        const char *f = shaders[i];
        if (f && strlen(f)) {
            if (!load_glslp(glsl, i, f)) {
                failed = 1;
                break;
            }
        }
    }
    if (failed) {
        delete_glsl(glsl);
        memset(glsl, 0, sizeof(glsl_t));
    }
    return glsl;
}

void
GLRendererView::read_shader_config()
{
    char s[512];
    for (int i = 0; i < active_shader->num_shaders; ++i) {
        struct glsl_shader *shader = &active_shader->shaders[i];
        snprintf(s, sizeof(s) - 1, "GL3 Shaders - %s", shader->name);
        for (int j = 0; j < shader->num_parameters; ++j) {
            struct shader_parameter *param = &shader->parameters[j];
            param->value                   = config_get_double(s, param->id, param->default_value);
        }
    }
}

/* Sets up a pass's vertex and texture-coordinate buffers. */
static void
setup_pass_buffers(struct shader_pass *pass, const GLfloat *vertex, size_t vertex_size, const GLfloat *tex_coords,
                   size_t tex_size)
{
    struct shader_vbo *vbo = &pass->vbo;

    glBindVertexArray(pass->vertex_array);

    glGenBuffers(1, (GLuint *) &vbo->vertex_coord);
    glBindBuffer(GL_ARRAY_BUFFER, vbo->vertex_coord);
    glBufferData(GL_ARRAY_BUFFER, vertex_size, vertex, GL_STATIC_DRAW);
    glVertexAttribPointer(pass->uniforms.vertex_coord, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), (GLvoid *) 0);

    glGenBuffers(1, (GLuint *) &vbo->tex_coord);
    glBindBuffer(GL_ARRAY_BUFFER, vbo->tex_coord);
    glBufferData(GL_ARRAY_BUFFER, tex_size, tex_coords, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(pass->uniforms.tex_coord, 2, GL_FLOAT, GL_TRUE, 2 * sizeof(GLfloat), (GLvoid *) 0);
}

/* OpenGLRenderer::initialize(). */
bool
GLRendererView::Init()
{
    LockGL();

    const char *version_string = reinterpret_cast<const char *>(glGetString(GL_VERSION));
    if ((version_string == nullptr) || (sscanf(version_string, "%d.%d", &gl_version[0], &gl_version[1]) != 2))
        gl_version[0] = gl_version[1] = 0;

    if (gl_version[0] < 3) {
        UnlockGL();
        char text[512];
        if (version_string == nullptr)
            snprintf(text, sizeof(text), "Error initializing OpenGL.\nCouldn't create OpenGL context.\nFalling back to software rendering.");
        else
            snprintf(text, sizeof(text),
                     "Error initializing OpenGL.\nOpenGL version 3.0 or greater is required. Current version is %d.%d\n"
                     "Falling back to software rendering.",
                     gl_version[0], gl_version[1]);
        haiku_gl_message("OpenGL", text);
        return false;
    }

    pclog("OpenGL: %s, %s, %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), version_string);

    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);

    scene_texture.data            = NULL;
    scene_texture.width           = 2048;
    scene_texture.height          = 2048;
    scene_texture.internal_format = GL_RGBA8;
    scene_texture.format          = GL_RGBA;
    scene_texture.type            = GL_UNSIGNED_INT_8_8_8_8_REV;
    scene_texture.wrap_mode       = GL_CLAMP_TO_BORDER;
    scene_texture.min_filter = scene_texture.mag_filter = video_filter_method ? GL_LINEAR : GL_NEAREST;
    scene_texture.mipmap                                = 0;

    create_texture(&scene_texture);
    tex_w = tex_h = 0;

    int num_shaders = 0;
    for (int i = 0; i < MAX_USER_SHADERS; ++i) {
        if (strlen(gl3_shader_file[i]))
            ++num_shaders;
        else
            break;
    }
    active_shader = load_shaders(num_shaders, gl3_shader_file);

    create_scene_shader();

    read_shader_config();

    static const GLfloat vertex[]     = { -1.0f, -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f };
    static const GLfloat inv_vertex[] = { -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f, -1.0f };
    static const GLfloat tex_coords[] = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f };
    static const GLfloat colors[]     = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                          1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

    /* The scene shader's buffers. */
    setup_pass_buffers(&active_shader->scene, inv_vertex, sizeof(inv_vertex), tex_coords, sizeof(tex_coords));

    /* The buffers of all passes. */
    for (int j = 0; j < active_shader->num_shaders; ++j) {
        struct glsl_shader *shader = &active_shader->shaders[j];
        for (int i = 0; i < shader->num_passes; ++i) {
            struct shader_uniforms *u = &shader->passes[i].uniforms;

            setup_pass_buffers(&shader->passes[i], vertex, sizeof(vertex), tex_coords, sizeof(tex_coords));

            if (u->color >= 0) {
                struct shader_vbo *vbo = &shader->passes[i].vbo;
                glGenBuffers(1, (GLuint *) &vbo->color);
                glBindBuffer(GL_ARRAY_BUFFER, vbo->color);
                glBufferData(GL_ARRAY_BUFFER, sizeof(colors), colors, GL_STATIC_DRAW);
                glVertexAttribPointer(u->color, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (GLvoid *) 0);
            }
        }
    }

    for (int i = 0; i < active_shader->num_shaders; ++i) {
        struct glsl_shader *shader = &active_shader->shaders[i];
        if (shader->has_prev) {
            struct shader_pass *prev_pass = &shader->prev_scene;
            create_default_shader_tex(prev_pass);
            setup_pass_buffers(prev_pass, vertex, sizeof(vertex), tex_coords, sizeof(tex_coords));

            for (int j = 0; j < MAX_PREV; ++j) {
                struct shader_vbo *prev_vbo = &shader->prev[j].vbo;

                glGenBuffers(1, (GLuint *) &prev_vbo->vertex_coord);
                glBindBuffer(GL_ARRAY_BUFFER, prev_vbo->vertex_coord);
                glBufferData(GL_ARRAY_BUFFER, sizeof(vertex), vertex, GL_STATIC_DRAW);

                glGenBuffers(1, (GLuint *) &prev_vbo->tex_coord);
                glBindBuffer(GL_ARRAY_BUFFER, prev_vbo->tex_coord);
                glBufferData(GL_ARRAY_BUFFER, sizeof(tex_coords), tex_coords, GL_DYNAMIC_DRAW);
            }
        }
    }

    /* The final pass, unless the last shader pass draws to the window. */
    if ((active_shader->num_shaders == 0)
        || (active_shader->shaders[active_shader->num_shaders - 1]
                .passes[active_shader->shaders[active_shader->num_shaders - 1].num_passes - 1]
                .fbo.id
            >= 0)) {
        create_default_shader_tex(&active_shader->final_pass);
        setup_pass_buffers(&active_shader->final_pass, vertex, sizeof(vertex), tex_coords, sizeof(tex_coords));
    }

    {
        struct shader_pass *color_pass = &active_shader->fs_color;
        create_default_shader_color(color_pass);

        glBindVertexArray(color_pass->vertex_array);

        struct shader_vbo *vbo = &color_pass->vbo;

        glGenBuffers(1, (GLuint *) &vbo->vertex_coord);
        glBindBuffer(GL_ARRAY_BUFFER, vbo->vertex_coord);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertex), vertex, GL_STATIC_DRAW);
        glVertexAttribPointer(color_pass->uniforms.vertex_coord, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), (GLvoid *) 0);

        glGenBuffers(1, (GLuint *) &vbo->color);
        glBindBuffer(GL_ARRAY_BUFFER, vbo->color);
        glBufferData(GL_ARRAY_BUFFER, sizeof(colors), colors, GL_DYNAMIC_DRAW);
        glVertexAttribPointer(color_pass->uniforms.color, 4, GL_FLOAT, GL_TRUE, 4 * sizeof(GLfloat), (GLvoid *) 0);
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    initialized = true;

    SwapBuffers();
    UnlockGL();

    /* "Use target framerate" draws on a timer instead of on every frame. */
    if (video_framerate != -1) {
        BMessage tick(MSG_GL_RENDER);
        render_timer = new BMessageRunner(BMessenger(this), &tick,
                                          (bigtime_t) ceilf(1000.f / (float) video_framerate) * 1000);
    }

    return true;
}

/* OpenGLRenderer::finalize(). */
void
GLRendererView::Finalize()
{
    if (!initialized)
        return;

    LockGL();

    delete_texture(&scene_texture);

    /* The OSD's OpenGL objects go with the context (Qt shuts the OSD down
       with each renderer). */
    if (owner->Monitor() == 0)
        haiku_osd_shutdown();

    if (active_shader) {
        delete_glsl(active_shader);
        free(active_shader);
    }
    active_shader = nullptr;

    UnlockGL();

    initialized = false;
}

/* OpenGLRenderer::onBlit(): uploads the frame from RendererView's bitmap. */
void
GLRendererView::FrameReady(bool)
{
    if (!initialized)
        return;

    LockGL();

    {
        std::lock_guard<std::mutex> guard(owner->FrameLock());
        const BBitmap *frame = owner->Frame();
        const BRect    src   = owner->Source();
        const int      x     = (int) src.left;
        const int      y     = (int) src.top;
        const int      w     = src.IntegerWidth() + 1;
        const int      h     = src.IntegerHeight() + 1;

        if (frame != nullptr) {
            glBindTexture(GL_TEXTURE_2D, scene_texture.id);
            if ((tex_w != w) || (tex_h != h)) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
                tex_w = w;
                tex_h = h;
            }
            const int32 bpr = frame->BytesPerRow();
            glPixelStorei(GL_UNPACK_ROW_LENGTH, bpr / 4);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                            static_cast<const uint8 *>(frame->Bits()) + ((size_t) y * bpr) + ((size_t) x * 4));
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }

    UnlockGL();

    if (video_framerate == -1)
        Render();
}

void
GLRendererView::Refresh()
{
    Render();
}

void
GLRendererView::FrameResized(float width, float height)
{
    BGLView::FrameResized(width, height);
    Render();
}

/* Input goes to RendererView (which also gets it, while capturing, as a
   pointer listener; it drops the second copy). */
void
GLRendererView::MouseDown(BPoint where)
{
    owner->MouseDown(ConvertToParent(where));
}

void
GLRendererView::MouseUp(BPoint where)
{
    owner->MouseUp(ConvertToParent(where));
}

void
GLRendererView::MouseMoved(BPoint where, uint32 transit, const BMessage *drag)
{
    owner->MouseMoved(ConvertToParent(where), transit, drag);
}

void
GLRendererView::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case B_MOUSE_WHEEL_CHANGED:
            owner->MessageReceived(message);
            break;
        case MSG_GL_RENDER:
            Render();
            break;
        default:
            BGLView::MessageReceived(message);
            break;
    }
}

int_rect
GLRendererView::SceneRenderRect(int w, int h) const
{
    int_rect rect = { 0, 0, (int) (w * video_gl_input_scale), (int) (h * video_gl_input_scale) };
    standalone_scale(rect, w, h, rect, video_gl_input_scale_mode);
    return rect;
}

void
GLRendererView::render_pass(struct render_data *data)
{
    GLuint texture_unit = 0;

    glBindVertexArray(data->shader_pass->vertex_array);

    GLint                   p = data->shader_pass->program.id;
    struct shader_uniforms *u = &data->shader_pass->uniforms;

    glUseProgram(p);

    if (data->texture) {
        glActiveTexture(GL_TEXTURE0 + texture_unit);
        glBindTexture(GL_TEXTURE_2D, data->texture);
        glUniform1i(u->texture, texture_unit);
        texture_unit++;
    }

    if (u->color >= 0)
        glEnableVertexAttribArray(u->color);

    if (u->mvp_matrix >= 0)
        glUniformMatrix4fv(u->mvp_matrix, 1, 0, matrix);
    if (u->frame_direction >= 0)
        glUniform1i(u->frame_direction, 1);

    int framecnt = data->frame_count;
    if (data->shader_pass->frame_count_mod > 0)
        framecnt = framecnt % data->shader_pass->frame_count_mod;
    if (u->frame_count >= 0)
        glUniform1i(u->frame_count, framecnt);

    if (u->input_size >= 0)
        glUniform2fv(u->input_size, 1, data->shader_pass->state.input_size);
    if (u->texture_size >= 0)
        glUniform2fv(u->texture_size, 1, data->shader_pass->state.input_texture_size);
    if (u->output_size >= 0)
        glUniform2fv(u->output_size, 1, data->output_size);

    if (data->shader) {
        for (int i = 0; i < data->shader->num_parameters; ++i)
            if (u->parameters[i] >= 0)
                glUniform1f(u->parameters[i], data->shader->parameters[i].value);

        if (data->pass > 0) {
            struct shader_pass *passes = data->shader->passes;
            struct shader_pass *orig   = data->orig_pass;
            if (u->orig.texture >= 0) {
                glActiveTexture(GL_TEXTURE0 + texture_unit);
                glBindTexture(GL_TEXTURE_2D, orig->fbo.texture.id);
                glUniform1i(u->orig.texture, texture_unit);
                texture_unit++;
            }
            if (u->orig.input_size >= 0)
                glUniform2fv(u->orig.input_size, 1, orig->state.input_size);
            if (u->orig.texture_size >= 0)
                glUniform2fv(u->orig.texture_size, 1, orig->state.input_texture_size);

            for (int i = 0; i < data->pass; ++i) {
                if (u->pass[i].texture >= 0) {
                    glActiveTexture(GL_TEXTURE0 + texture_unit);
                    glBindTexture(GL_TEXTURE_2D, passes[i].fbo.texture.id);
                    glUniform1i(u->pass[i].texture, texture_unit);
                    texture_unit++;
                }
                if (u->pass[i].texture_size >= 0)
                    glUniform2fv(u->pass[i].texture_size, 1, passes[i].state.input_texture_size);
                if (u->pass[i].input_size >= 0)
                    glUniform2fv(u->pass[i].input_size, 1, passes[i].state.input_size);

                if (u->prev_pass[i].texture >= 0) {
                    glActiveTexture(GL_TEXTURE0 + texture_unit);
                    glBindTexture(GL_TEXTURE_2D, passes[i].fbo.texture.id);
                    glUniform1i(u->prev_pass[i].texture, texture_unit);
                    texture_unit++;
                }
                if (u->prev_pass[i].texture_size >= 0)
                    glUniform2fv(u->prev_pass[i].texture_size, 1, passes[i].state.input_texture_size);
                if (u->prev_pass[i].input_size >= 0)
                    glUniform2fv(u->prev_pass[i].input_size, 1, passes[i].state.input_size);
            }
        }

        if (data->shader->has_prev) {
            /* Each previous frame. */
            for (int i = 0; i < MAX_PREV; ++i) {
                if (u->prev[i].texture >= 0) {
                    glActiveTexture(GL_TEXTURE0 + texture_unit);
                    glBindTexture(GL_TEXTURE_2D, data->shader->prev[i].fbo.texture.id);
                    glUniform1i(u->prev[i].texture, texture_unit);
                    texture_unit++;
                }
                if (u->prev[i].tex_coord >= 0) {
                    glBindBuffer(GL_ARRAY_BUFFER, data->shader->prev[i].vbo.tex_coord);
                    glVertexAttribPointer(u->prev[i].tex_coord, 2, GL_FLOAT, GL_TRUE, 2 * sizeof(GLfloat), (GLvoid *) 0);
                    glEnableVertexAttribArray(u->prev[i].tex_coord);
                    glBindBuffer(GL_ARRAY_BUFFER, 0);
                }
            }
        }

        for (int i = 0; i < data->shader->num_lut_textures; ++i) {
            if (u->lut_textures[i] >= 0) {
                glActiveTexture(GL_TEXTURE0 + texture_unit);
                glBindTexture(GL_TEXTURE_2D, data->shader->lut_textures[i].texture.id);
                glUniform1i(u->lut_textures[i], texture_unit);
                texture_unit++;
            }
        }
    }

    glEnableVertexAttribArray(u->vertex_coord);
    glEnableVertexAttribArray(u->tex_coord);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    glDisableVertexAttribArray(data->shader_pass->uniforms.vertex_coord);
    glDisableVertexAttribArray(data->shader_pass->uniforms.tex_coord);
    if (data->shader_pass->uniforms.color >= 0)
        glDisableVertexAttribArray(data->shader_pass->uniforms.color);

    if (data->shader && data->shader->has_prev) {
        for (int i = 0; i < MAX_PREV; ++i) {
            if (u->prev[i].tex_coord >= 0)
                glDisableVertexAttribArray(u->prev[i].tex_coord);
        }
    }

    glBindVertexArray(0);

    glUseProgram(0);
}

/* Output size and texture coordinates of a pass drawing at its scale. */
static void
scale_pass(struct shader_pass *pass, struct shader_pass *input, const GLfloat *orig_output_size)
{
    memcpy(pass->state.input_size, input->state.output_size, 2 * sizeof(GLfloat));
    memcpy(pass->state.input_texture_size, input->state.output_texture_size, 2 * sizeof(GLfloat));

    for (uint8_t j = 0; j < 2; ++j) {
        if (pass->scale.mode[j] == SCALE_VIEWPORT)
            pass->state.output_size[j] = orig_output_size[j] * pass->scale.value[j];
        else if (pass->scale.mode[j] == SCALE_ABSOLUTE)
            pass->state.output_size[j] = pass->scale.value[j];
        else
            pass->state.output_size[j] = pass->state.input_size[j] * pass->scale.value[j];

        pass->state.output_texture_size[j] = next_pow2(pass->state.output_size[j]);
    }
}

static void
set_tex_coords(struct shader_pass *pass)
{
    GLfloat maxx = pass->state.output_size[0] / (GLfloat) pass->state.output_texture_size[0];
    GLfloat maxy = pass->state.output_size[1] / (GLfloat) pass->state.output_texture_size[1];

    pass->state.tex_coords[0] = 0;
    pass->state.tex_coords[1] = 0;
    pass->state.tex_coords[2] = 0;
    pass->state.tex_coords[3] = maxy;
    pass->state.tex_coords[4] = maxx;
    pass->state.tex_coords[5] = 0;
    pass->state.tex_coords[6] = maxx;
    pass->state.tex_coords[7] = maxy;
}

static void
set_filter(GLuint texture, GLint min_filter, GLint mag_filter)
{
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter);
    glBindTexture(GL_TEXTURE_2D, 0);
}

/* OpenGLRenderer::render(). */
void
GLRendererView::Render()
{
    if (!initialized || (tex_w <= 0) || (tex_h <= 0))
        return;

    LockGL();

    const BRect bounds   = Bounds();
    const int   output_w = bounds.IntegerWidth() + 1;
    const int   output_h = bounds.IntegerHeight() + 1;
    const BRect dest     = owner->Destination();

    /* GL measures the viewport from the bottom; the picture can extend
       past the window edges, so the offsets can be negative. */
    int_rect window_rect;
    window_rect.x = (int) dest.left;
    window_rect.w = dest.IntegerWidth() + 1;
    window_rect.h = dest.IntegerHeight() + 1;
    window_rect.y = output_h - (int) dest.top - window_rect.h;

    /* Black around the picture. */
    glViewport(0, 0, output_w, output_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    const GLint filter = video_filter_method ? GL_LINEAR : GL_NEAREST;
    scene_texture.min_filter = scene_texture.mag_filter = filter;
    active_shader->scene.fbo.texture.min_filter = active_shader->scene.fbo.texture.mag_filter = filter;
    set_filter(scene_texture.id, filter, filter);
    set_filter(active_shader->scene.fbo.texture.id, filter, filter);

    GLfloat orig_output_size[] = { (GLfloat) window_rect.w, (GLfloat) window_rect.h };

    if (active_shader->srgb)
        glEnable(GL_FRAMEBUFFER_SRGB);

    struct render_data data;

    /* Render the scene to a texture. */
    {
        struct shader_pass *pass = &active_shader->scene;
        int_rect            rect = SceneRenderRect(tex_w, tex_h);

        pass->state.input_size[0] = pass->state.output_size[0] = rect.w;
        pass->state.input_size[1] = pass->state.output_size[1] = rect.h;

        pass->state.input_texture_size[0] = pass->state.output_texture_size[0] = next_pow2(pass->state.output_size[0]);
        pass->state.input_texture_size[1] = pass->state.output_texture_size[1] = next_pow2(pass->state.output_size[1]);

        recreate_fbo(&active_shader->scene.fbo, pass->state.output_texture_size[0], pass->state.output_texture_size[1]);

        glBindFramebuffer(GL_FRAMEBUFFER, active_shader->scene.fbo.id);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        glViewport(0, 0, pass->state.output_size[0], pass->state.output_size[1]);

        set_tex_coords(pass);

        GLfloat tex_coords[] = { 0, 0, 0, 1, 1, 0, 1, 1 };

        glBindVertexArray(pass->vertex_array);

        glBindBuffer(GL_ARRAY_BUFFER, pass->vbo.tex_coord);
        glBufferSubData(GL_ARRAY_BUFFER, 0, 8 * sizeof(GLfloat), tex_coords);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        memset(&data, 0, sizeof(struct render_data));
        data.pass        = -1;
        data.shader_pass = &active_shader->scene;
        data.texture     = scene_texture.id;
        data.output_size = orig_output_size;
        render_pass(&data);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    struct shader_pass *orig  = &active_shader->scene;
    struct shader_pass *input = &active_shader->scene;

    for (int s = 0; s < active_shader->num_shaders; ++s) {
        struct glsl_shader *shader = &active_shader->shaders[s];

        int frame_count = frame_counter;

        for (int i = 0; i < shader->num_passes; ++i) {
            bool                reset_filtering = false;
            struct shader_pass *pass            = &shader->passes[i];

            /* The preset's filtering for this pass's input if it sets one,
               otherwise the filter method chosen in the menu. */
            const int input_linear = (pass->input_filter_linear >= 0) ? pass->input_filter_linear : video_filter_method;

            /* The first pass of a shader reads the scene or the previous
               shader's output. */
            if (i == 0) {
                input->fbo.texture.min_filter = input->fbo.texture.mag_filter = input_linear ? GL_LINEAR : GL_NEAREST;
                if (input->fbo.texture.mipmap)
                    input->fbo.texture.min_filter = input_linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;
                set_filter(input->fbo.texture.id, input->fbo.texture.min_filter, input->fbo.texture.mag_filter);
            }

            scale_pass(pass, input, orig_output_size);

            if (pass->fbo.id >= 0) {
                recreate_fbo(&pass->fbo, pass->state.output_texture_size[0], pass->state.output_texture_size[1]);

                glBindFramebuffer(GL_FRAMEBUFFER, pass->fbo.id);
                glViewport(0, 0, pass->state.output_size[0], pass->state.output_size[1]);
                glClearColor(0, 0, 0, 1);
                glClear(GL_COLOR_BUFFER_BIT);
            } else {
                reset_filtering = true;
                set_filter(input->fbo.texture.id, input_linear ? GL_LINEAR : GL_NEAREST,
                           input_linear ? GL_LINEAR : GL_NEAREST);
                glViewport(window_rect.x, window_rect.y, window_rect.w, window_rect.h);
            }

            set_tex_coords(pass);

            glBindVertexArray(pass->vertex_array);

            glBindBuffer(GL_ARRAY_BUFFER, pass->vbo.tex_coord);
            glBufferSubData(GL_ARRAY_BUFFER, 0, 8 * sizeof(GLfloat), input->state.tex_coords);
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            memset(&data, 0, sizeof(struct render_data));
            data.shader      = shader;
            data.pass        = i;
            data.shader_pass = pass;
            data.texture     = input->fbo.texture.id;
            data.output_size = orig_output_size;
            data.orig_pass   = orig;
            data.frame_count = frame_count;

            render_pass(&data);

            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            if (pass->fbo.texture.mipmap) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, pass->fbo.texture.id);
                glGenerateMipmap(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, 0);
            }

            if (reset_filtering)
                set_filter(input->fbo.texture.id, input->fbo.texture.min_filter, input->fbo.texture.mag_filter);

            input = pass;
        }

        if (shader->has_prev) {
            /* Shift the previous frames. */
            memmove(&shader->prev[1], &shader->prev[0], MAX_PREV * sizeof(struct shader_prev));
            memcpy(&shader->prev[0], &shader->prev[MAX_PREV], sizeof(struct shader_prev));

            struct shader_pass *pass      = orig;
            struct shader_pass *prev_pass = &shader->prev_scene;
            struct shader_prev *prev      = &shader->prev[0];

            memcpy(&prev_pass->state, &pass->state, sizeof(struct shader_state));

            recreate_fbo(&prev->fbo, prev_pass->state.output_texture_size[0], prev_pass->state.output_texture_size[1]);

            memcpy(&prev_pass->fbo, &prev->fbo, sizeof(struct shader_fbo));

            glBindFramebuffer(GL_FRAMEBUFFER, prev->fbo.id);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);

            glViewport(0, 0, pass->state.output_size[0], pass->state.output_size[1]);

            glBindVertexArray(prev_pass->vertex_array);

            glBindBuffer(GL_ARRAY_BUFFER, prev->vbo.tex_coord);
            glBufferSubData(GL_ARRAY_BUFFER, 0, 8 * sizeof(GLfloat), pass->state.tex_coords);
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            glBindBuffer(GL_ARRAY_BUFFER, prev_pass->vbo.tex_coord);
            glBufferSubData(GL_ARRAY_BUFFER, 0, 8 * sizeof(GLfloat), pass->state.tex_coords);
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            memset(&data, 0, sizeof(struct render_data));
            data.shader      = shader;
            data.pass        = -10;
            data.shader_pass = prev_pass;
            data.texture     = pass->fbo.texture.id;
            data.output_size = orig_output_size;

            render_pass(&data);

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        orig = input;
    }

    if (active_shader->final_pass.active) {
        struct shader_pass *pass = &active_shader->final_pass;

        scale_pass(pass, input, orig_output_size);

        set_filter(input->fbo.texture.id, filter, filter);

        glViewport(window_rect.x, window_rect.y, window_rect.w, window_rect.h);

        set_tex_coords(pass);

        glBindVertexArray(pass->vertex_array);

        glBindBuffer(GL_ARRAY_BUFFER, pass->vbo.tex_coord);
        glBufferSubData(GL_ARRAY_BUFFER, 0, 8 * sizeof(GLfloat), input->state.tex_coords);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        memset(&data, 0, sizeof(struct render_data));
        data.pass        = -2;
        data.shader_pass = pass;
        data.texture     = input->fbo.texture.id;
        data.output_size = orig_output_size;
        data.orig_pass   = orig;

        render_pass(&data);
    }

    /* Screenshots of the shaded picture, as far as it is inside the view. */
    if ((monitors[owner->Monitor()].mon_screenshots > 0) || (monitors[owner->Monitor()].mon_screenshots_clipboard > 0)) {
        const int rx = (window_rect.x < 0) ? 0 : window_rect.x;
        const int ry = (window_rect.y < 0) ? 0 : window_rect.y;
        const int rr = ((window_rect.x + window_rect.w) > output_w) ? output_w : (window_rect.x + window_rect.w);
        const int rt = ((window_rect.y + window_rect.h) > output_h) ? output_h : (window_rect.y + window_rect.h);
        const int width  = rr - rx;
        const int height = rt - ry;

        if ((width > 0) && (height > 0)) {
            uint8 *pixels = static_cast<uint8 *>(malloc((size_t) width * height * 4));
            glFinish();
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(rx, ry, width, height, GL_BGRA, GL_UNSIGNED_BYTE, pixels);

            BBitmap *image = new BBitmap(BRect(0, 0, width - 1, height - 1), B_RGB32);
            for (int y = 0; y < height; y++)
                memcpy(static_cast<uint8 *>(image->Bits()) + ((size_t) y * image->BytesPerRow()),
                       pixels + ((size_t) (height - y - 1) * width * 4), (size_t) width * 4);
            free(pixels);

            if (monitors[owner->Monitor()].mon_screenshots_clipboard > 0) {
                haiku_copy_to_clipboard(image, image->Bounds());
                monitors[owner->Monitor()].mon_screenshots_clipboard--;
            }
            if (monitors[owner->Monitor()].mon_screenshots > 0) {
                char path[1024];
                char fn[256];

                memset(fn, 0, sizeof(fn));
                memset(path, 0, sizeof(path));

                path_append_filename(path, usr_path, SCREENSHOT_PATH);

                if (!plat_dir_check(path))
                    plat_dir_create(path);

                path_slash(path);
                strcat(path, "Monitor_1_");

                plat_tempfile(fn, NULL, (char *) ".png");
                strcat(path, fn);

                save_png(image, path);
                image = nullptr;
                monitors[owner->Monitor()].mon_screenshots--;
            }
            delete image;
        }
    }

    glDisable(GL_FRAMEBUFFER_SRGB);

    /* The OSD crisp on top of the shaded image, in the default framebuffer
       at the view's resolution (Qt OpenGLRenderer::render()). */
    if ((owner->Monitor() == 0) && haiku_osd_needs_render()) {
        const BRect view = Bounds();
        const BRect dest = owner->Destination();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, view.IntegerWidth() + 1, view.IntegerHeight() + 1);
        haiku_osd_set_layout_scale_for(dest.IntegerWidth() + 1, dest.IntegerHeight() + 1);
        haiku_osd_render_gl(view.IntegerWidth() + 1, view.IntegerHeight() + 1, gl_version[0], gl_version[1]);
    }

    frame_counter++;
    SwapBuffers(video_vsync != 0);
    UnlockGL();
}
