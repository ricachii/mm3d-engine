#ifdef ENABLE_OPENGL

#include "fast/backends/gfx_opengl.h"
#if defined(__SWITCH__)
#include "fast/backends/gfx_sdl.h"
#endif

#if defined(__SWITCH__)
#include <EGL/egl.h>
#include <switch.h>
#include <optional>
#endif


#include "fast/backends/gfx_opengl_pica_post.h"
#include "fast/backends/gfx_opengl_pica_gpu_lean.h"
#include "fast/backends/gl_frame_profiler.h"
#include "fast/backends/gfx_opengl_pica_residency.h"
#include "fast/backends/gfx_opengl_pica_scale.h"
#include "fast/backends/gl_pica_dynamic_scale.h"
#include "fast/backends/gfx_opengl_pica_uniform_trim.h"
#include "fast/backends/gfx_opengl_pica_state_cache.h"
#include "fast/oot3d/gl_widescreen_menu_policy.h"
#include "fast/oot3d/gl_widescreen_policy.h"
#include "fast/oot3d/pica_geometry_registry.h"
#include "fast/oot3d/pica_uniform_layout.h"
#include "fast/renderer3ds/pica_composition.h"
#include "fast/renderer3ds/pica_draw_coverage.h"
#include "fast/oot3d/pica_shadow2d.h"
#include "fast/oot3d/smaa_lookup_data.h"
#include "fast/renderer3ds/pica_composition_schedule.h"
#include "fast/renderer3ds/pica_gl_shader_dialect.h"
#include "oot3d/renderer/pica_texture_decode.h"
#include "fast/oot3d/pica_texture_predecode.h"
#include "fast/oot3d/pica_etc1_transcode.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <chrono>
#include <memory>
#include <mutex>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Fast {
namespace {

void SetPicaGlError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

#if !defined(USE_OPENGLES) && !defined(__APPLE__)

uint64_t HashBytes(std::span<const uint8_t> bytes) {
    uint64_t hash = 1469598103934665603ULL;
    for (const uint8_t value : bytes) {
        hash = (hash ^ value) * 1099511628211ULL;
    }
    return hash == 0U ? 1U : hash;
}

// Internal resolution scale (like Citra/Azahar's "resolution scale"). Every
// PICA render target -- color, depth/stencil and the Shadow2D image -- and
// every display-transfer image is allocated N times larger per axis;
// viewport, scissor and transfer blits are multiplied by N and the scanout
// samples the large image. Keys, guest addresses and logical extents stay
// native, so N=1 issues exactly the same GL calls as before. The GL backend
// never reads PICA pixels back into guest memory (sampled textures are always
// decoded from guest bytes; only the live Shadow2D image is shared, and its
// consumer filters it through textureSize()), so no downsample path is
// needed.
//
// N may be fractional (2.5: 1000x600, or 1067x600 widened, upscaled to the
// 720p screen by FSR EASU with ~69% of the pixel work of 3x). Every host edge
// is then round(native edge * N) (PicaGlScale::ScaledEdge), so rectangles
// that tile natively still tile; at an integer N that is exactly native * N.
//
// Read once: on the Switch from sdmc:/switch/oot3dre/config/resolution_scale
// (text "1", "2", "3" or a decimal such as "2.5"); elsewhere from
// TRIAEVUM_GL_RESOLUTION_SCALE. Missing or invalid means 1.
float ReadPicaGlResolutionScale() {
    static const float scale = [] {
#if defined(__SWITCH__)
        // Chosen at boot like the window size (gfx_sdl2.cpp): docking later
        // keeps the boot mode's scale and 1280x720 window.
        const auto readFirstLine = [](const char* path) -> std::optional<std::string> {
            std::ifstream file(path);
            std::string text;
            if (!file || !std::getline(file, text)) return std::nullopt;
            return text;
        };
        const auto docked = readFirstLine("sdmc:/switch/oot3dre/config/resolution_scale_docked");
        const auto common = readFirstLine("sdmc:/switch/oot3dre/config/resolution_scale");
        return PicaGlScale::SelectResolutionScale(
            docked ? &*docked : nullptr, common ? &*common : nullptr,
            appletGetOperationMode() == AppletOperationMode_Console);
#else
        const char* value = std::getenv("TRIAEVUM_GL_RESOLUTION_SCALE");
        return value != nullptr ? PicaGlScale::ParseResolutionScale(value)
                                : 1.0F;
#endif
    }();
    return scale;
}

// config/gl_lean (see PicaGlScale::ParseLeanSetting): on by default.
bool ReadPicaGlLean() {
    static const bool lean = [] {
#if defined(__SWITCH__)
        std::ifstream file("sdmc:/switch/oot3dre/config/gl_lean");
        std::string text;
        if (!file || !std::getline(file, text)) {
            return true;
        }
        return PicaGlScale::ParseLeanSetting(text, true);
#else
        const char* value = std::getenv("TRIAEVUM_GL_LEAN");
        return value == nullptr || PicaGlScale::ParseLeanSetting(value, true);
#endif
    }();
    return lean;
}

static_assert(PicaGlGpuLean::kGlZero == GL_ZERO);
static_assert(PicaGlGpuLean::kGlOne == GL_ONE);
static_assert(PicaGlGpuLean::kGlFuncAdd == GL_FUNC_ADD);
static_assert(PicaGlGpuLean::kGlCopy == GL_COPY);
static_assert(PicaGlGpuLean::kGlNoop == GL_NOOP);
static_assert(PicaGlGpuLean::kGlKeep == GL_KEEP);

// config/gl_gpu_lean (see gfx_opengl_pica_gpu_lean.h): on by default.
bool ReadPicaGlGpuLean() {
    static const bool lean = [] {
#if defined(__SWITCH__)
        std::ifstream file("sdmc:/switch/oot3dre/config/gl_gpu_lean");
        std::string text;
        if (!file || !std::getline(file, text)) {
            return true;
        }
        return PicaGlScale::ParseLeanSetting(text, true);
#else
        const char* value = std::getenv("TRIAEVUM_GL_GPU_LEAN");
        return value == nullptr || PicaGlScale::ParseLeanSetting(value, true);
#endif
    }();
    return lean;
}

// config/gl_texture_budget_mb (PicaGlResidency::ParseBudgetBytes).
uint64_t ReadPicaGlTextureBudget() {
    static const uint64_t budget = [] {
        const uint64_t fallback = PicaGlResidency::Policy{}.BudgetBytes;
#if defined(__SWITCH__)
        std::ifstream file("sdmc:/switch/oot3dre/config/gl_texture_budget_mb");
        std::string text;
        if (!file || !std::getline(file, text)) {
            return fallback;
        }
        return PicaGlResidency::ParseBudgetBytes(text, fallback);
#else
        const char* value = std::getenv("TRIAEVUM_GL_TEXTURE_BUDGET_MB");
        return value == nullptr
                   ? fallback
                   : PicaGlResidency::ParseBudgetBytes(value, fallback);
#endif
    }();
    return budget;
}

// Optional scanout post-processing and texture filtering (all off unless a
// file exists), read once like the resolution scale: on the Switch from
// sdmc:/switch/oot3dre/config/{antialiasing,anisotropy,upscaler,sharpen};
// elsewhere from TRIAEVUM_GL_{ANTIALIASING,ANISOTROPY,UPSCALER,SHARPEN}.
// See gfx_opengl_pica_post.h for the accepted values.
std::optional<std::string> ReadPicaGlConfigText(const char* file,
                                                const char* environment) {
#if defined(__SWITCH__)
    (void)environment;
    std::ifstream stream(std::string("sdmc:/switch/oot3dre/config/") + file);
    std::string text;
    if (!stream || !std::getline(stream, text)) {
        return std::nullopt;
    }
    return text;
#else
    (void)file;
    const char* value = std::getenv(environment);
    return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
#endif
}

// On/off switches read once: on the Switch from sdmc:/switch/oot3dre/config/
// <file> (missing: `absent`; present: PicaGlStateCache::ParseSwitch with an
// empty file meaning `empty`), elsewhere from the environment variable.
//   gl_state_cache  TRIAEVUM_GL_STATE_CACHE  on unless "0"
//   gl_stats        TRIAEVUM_GL_STATS        off unless the file exists
bool ReadPicaGlSwitch(const char* file, const char* environment, bool absent,
                      bool empty) {
#if defined(__SWITCH__)
    (void)environment;
    std::ifstream stream(std::string("sdmc:/switch/oot3dre/config/") + file);
    if (!stream) {
        return absent;
    }
    std::string text;
    std::getline(stream, text);
    return PicaGlStateCache::ParseSwitch(text.c_str(), empty);
#else
    (void)file;
    const char* value = std::getenv(environment);
    return value == nullptr ? absent
                            : PicaGlStateCache::ParseSwitch(value, empty);
#endif
}

uint64_t PicaGlNowNanoseconds() {
#if defined(__SWITCH__)
    // CNTPCT (19.2 MHz): a register read, no syscall.
    return armTicksToNs(armGetSystemTick());
#else
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
#endif
}

const PicaGlPost::Config& ReadPicaGlPostConfig() {
    static const PicaGlPost::Config config = PicaGlPost::ResolveConfig(
        ReadPicaGlConfigText("antialiasing", "TRIAEVUM_GL_ANTIALIASING"),
        ReadPicaGlConfigText("anisotropy", "TRIAEVUM_GL_ANISOTROPY"),
        ReadPicaGlConfigText("upscaler", "TRIAEVUM_GL_UPSCALER"),
        ReadPicaGlConfigText("sharpen", "TRIAEVUM_GL_SHARPEN"));
    return config;
}

struct PicaGlTargetKey {
    uint64_t Namespace = 0U;
    uint32_t ColorAddress = 0U;
    uint32_t DepthAddress = 0U;
    uint16_t Width = 0U;
    uint16_t Height = 0U;
    uint8_t ColorFormat = 0U;
    uint8_t DepthFormat = 0U;

    bool operator<(const PicaGlTargetKey& other) const noexcept {
        return std::tie(Namespace, ColorAddress, DepthAddress, Width, Height,
                        ColorFormat, DepthFormat) <
               std::tie(other.Namespace, other.ColorAddress,
                        other.DepthAddress, other.Width, other.Height,
                        other.ColorFormat, other.DepthFormat);
    }
};

struct PicaGlShaderKey {
    Renderer3ds::PicaShaderSourceIdentity Vertex;
    Renderer3ds::PicaShaderSourceIdentity Fragment;

    bool operator<(const PicaGlShaderKey& other) const noexcept {
        return std::tie(Vertex.Id, Vertex.SecondaryHash, Vertex.Size,
                        Fragment.Id, Fragment.SecondaryHash, Fragment.Size) <
               std::tie(other.Vertex.Id, other.Vertex.SecondaryHash,
                        other.Vertex.Size, other.Fragment.Id,
                        other.Fragment.SecondaryHash, other.Fragment.Size);
    }
};

struct PicaGlTextureKey {
    uint64_t ContentHash = 0U;
    uint32_t PhysicalAddress = 0U;
    uint16_t Width = 0U;
    uint16_t Height = 0U;
    uint8_t Format = 0U;
    uint8_t Type = 0U;
    uint8_t WrapS = 0U;
    uint8_t WrapT = 0U;
    bool MinLinear = false;
    bool MagLinear = false;
    bool MipLinear = false;
    int16_t LodBiasRaw = 0;
    uint8_t MinMipLevel = 0U;
    uint8_t MaxMipLevel = 0U;

    bool operator<(const PicaGlTextureKey& other) const noexcept {
        return std::tie(ContentHash, PhysicalAddress, Width, Height, Format,
                        Type, WrapS, WrapT, MinLinear, MagLinear, MipLinear,
                        LodBiasRaw, MinMipLevel, MaxMipLevel) <
               std::tie(other.ContentHash, other.PhysicalAddress,
                        other.Width, other.Height, other.Format, other.Type,
                        other.WrapS, other.WrapT, other.MinLinear,
                        other.MagLinear, other.MipLinear, other.LodBiasRaw,
                        other.MinMipLevel, other.MaxMipLevel);
    }
};

struct PicaGlProgram {
    GLuint Handle = 0U;
    // PicaGlGpuLean::FragmentFeature bits of the fragment source.
    uint8_t FragmentFeatures = 0U;
    // Prefix of each uniform block the program can read
    // (gfx_opengl_pica_uniform_trim.h; kAll: the whole block).
    size_t VertexUniformUsed = PicaGlUniformTrim::kAll;
    size_t FragmentUniformUsed = PicaGlUniformTrim::kAll;
};

struct PicaGlTarget {
    // Drawn extent: the key's native extent times Scale (and, for a
    // widescreen top-screen target, its height times the widescreen factor
    // as well). The allocated extent unless dynamic resolution
    // (gl_pica_dynamic_scale.h) draws the bottom-left sub-rectangle of it.
    uint32_t Width = 0U;
    uint32_t Height = 0U;
    // Allocated extent and the scale it was allocated at (never change).
    uint32_t AllocWidth = 0U;
    uint32_t AllocHeight = 0U;
    float AllocScale = 1.0F;
    float AllocScaleY = 1.0F;
    float WideFactor = 1.0F;
    // Dynamic resolution: the top-screen scene target of a namespace without
    // a scale of its own; Exempt once it took part in a Shadow2D pass (the
    // consumer filters the image through textureSize()).
    bool DynamicEligible = false;
    bool DynamicExempt = false;
    // FrameSerial the scale was last looked at (gl_pica_dynamic_scale.h).
    uint32_t ScaleSerial = 0U;
    // Host pixels per native pixel along storage Y (the presented horizontal
    // axis of the rotated top screen). ResolutionScale unless Wide.
    float ScaleY = 1.0F;
    // Host pixels per native pixel along storage X: the resolution scale of
    // the target's namespace (SetPicaNamespaceResolutionScale; the global
    // ResolutionScale unless a namespace was given its own).
    float Scale = 1.0F;
    // Widescreen top-screen target: the scene stretches over the whole
    // Height while placed UI keeps a centred native canvas of
    // FramebufferHeight * ResolutionScale pixels.
    bool Wide = false;
    GLuint Framebuffer = 0U;
    GLuint Color = 0U;
    GLuint DepthStencil = 0U;
    GLuint ShadowFramebuffer = 0U;
    GLuint Shadow = 0U;
    // Unique per created target (never reused) and bumped by every GL write
    // to Color: together they identify the colour content a display copy
    // read (PicaGlScale::DisplayCopyIdentity).
    uint64_t Id = 0U;
    uint64_t Version = 0U;
    // Lean mode: the Shadow2D image is cleared only when a shadow producer
    // draws into it or a consumer samples it. Its contents are visible to no
    // one else (only producer programs declare the image), so a deferred
    // clear writes the same values the eager one would.
    bool ShadowClearPending = false;
    uint32_t ShadowClearValue = 0U;
};

// GL 4.3 / ARB_ES3_compatibility; GM20B samples it in hardware (nvc0).
constexpr GLenum kGlCompressedRgb8Etc2 = 0x9274;

struct PicaGlTexture {
    GLuint Handle = 0U;
    GLenum Format = GL_RGBA8;
    // Residency (PicaGlResidency): decoded size and last frame a draw used it.
    uint64_t Bytes = 0U;
    uint64_t LastUsedFrame = 0U;
};

struct PicaGlDisplay {
    GLuint Framebuffer = 0U;
    GLuint Color = 0U;
    // Native (guest) extent; the texture is Width/Height times the scale.
    uint32_t Width = 0U;
    uint32_t Height = 0U;
    // Allocated texture extent: Width/Height times the scale, except that a
    // widescreen top-screen transfer has a wider Height.
    uint32_t AllocWidth = 0U;
    uint32_t AllocHeight = 0U;
    // Pixels of the last copy (bottom-left sub-rectangle; the allocated
    // extent unless the source was drawn at a dynamic scale).
    uint32_t ContentWidth = 0U;
    uint32_t ContentHeight = 0U;
    // Scale of the source that copy came from (perf.txt drs_scale_avg).
    float ContentScale = 1.0F;
    bool Initialized = false;
    // The copy that produced Color (lean mode skips an identical repeat:
    // a presentation re-submits the transfer the sample already executed).
    PicaGlScale::DisplayCopyIdentity LastCopy;
};

// Intermediate image of the scanout post-processing chain. Reallocated only
// when its extent changes (first frame, dock/undock), never per frame.
struct PicaGlPostImage {
    GLuint Framebuffer = 0U;
    GLuint Texture = 0U;
    uint32_t Width = 0U;
    uint32_t Height = 0U;
};

struct PicaGlPostProgram {
    GLuint Handle = 0U;
    GLint Orient = -1;
    GLint FlipY = -1;
    GLint OutputSize = -1;
    std::array<GLint, 4> Params{-1, -1, -1, -1};
    // Last uploaded values: uniforms are re-sent only when they change, so
    // a steady frame issues no glUniform calls for the chain.
    int LastOrient = -1;
    int LastFlipY = -1;
    uint64_t LastOutputKey = 0U;
    uint64_t LastInputKey = 0U;
    int LastResample = -1;
    // EASU/RCAS: texture extent of the last source (its content may be a
    // dynamic-resolution sub-rectangle, LastInputKey).
    uint64_t LastTextureKey = 0U;
};

struct PicaGlPostState {
    // Effective configuration (effects whose setup failed are turned off).
    PicaGlPost::Config Config;
    PicaGlPostProgram Fxaa;
    PicaGlPostProgram SmaaEdges;
    PicaGlPostProgram SmaaWeights;
    PicaGlPostProgram SmaaBlend;
    PicaGlPostProgram Easu;
    PicaGlPostProgram Rcas;
    GLuint SmaaArea = 0U;
    GLuint SmaaSearch = 0U;
    // Anti-aliased image, source (render x scale) extent.
    PicaGlPostImage Color;
    // SMAA edges and blend weights, source extent.
    PicaGlPostImage Edges;
    PicaGlPostImage Weights;
    // EASU output, portrait extent of the window viewport (only when RCAS
    // follows; otherwise EASU draws the window directly).
    PicaGlPostImage Upscaled;
    // Lean: what Upscaled holds (display image, the copy that filled it,
    // flip). A repeated scanout of an unchanged image (a dropped in-between
    // frame, a presentation without a new guest frame) skips EASU.
    GLuint UpscaledSource = 0U;
    PicaGlScale::DisplayCopyIdentity UpscaledCopy;
    bool UpscaledFlip = false;
};

struct PicaGlAttributeState {
    GLuint Buffer = 0U;
    GLsizei ByteStride = 0;
    uintptr_t ByteOffset = 0U;
    GLenum Type = GL_FLOAT;
    uint8_t ComponentCount = 0U;
    bool Configured = false;
};

struct PicaGlGeometryBuffer {
    GLuint Handle = 0U;
    uint64_t ContentVersion = 0U;
    uint64_t StructuralSignature = 0U;
    uint64_t Bytes = 0U;
};

// One stream ring (PicaGlResidency::StreamRing) and its buffer object.
//
// Persistent mode (Switch, with config/gl_thread): Mesa's GL thread runs a
// glMapBufferRange synchronously (it returns a pointer), so mapping per draw
// would drain the GL thread three times a draw. The ring is then kSlots
// buffers with immutable storage (glBufferStorage), each mapped once,
// persistent and coherent: a write is a memcpy. A new generation moves to the
// next slot instead of orphaning (immutable storage cannot be reallocated),
// after a fence shows the GPU is done with that slot's previous generation.
struct PicaGlStreamBuffer {
    static constexpr size_t kSlots = 4U;
    GLenum Target = GL_ARRAY_BUFFER;
    GLuint Handle = 0U;
    PicaGlResidency::StreamRing Ring;
    bool Persistent = false;
    size_t Slot = 0U;
    std::array<GLuint, kSlots> Slots{};
    std::array<uint8_t*, kSlots> Mapped{};
    std::array<GLsync, kSlots> Fences{};
};

#if defined(__SWITCH__)
// glad here is GL 4.3 core: glBufferStorage (GL 4.4 / ARB_buffer_storage)
// comes from the driver when it advertises the extension.
using PicaGlBufferStorageProc = void (*)(GLenum, GLsizeiptr, const void*, GLbitfield);
constexpr GLbitfield kPicaGlMapPersistent = 0x0040U;
constexpr GLbitfield kPicaGlMapCoherent = 0x0080U;

PicaGlBufferStorageProc PicaGlPersistentStreamStorage() {
    std::error_code error;
    if (!std::filesystem::exists("sdmc:/switch/oot3dre/config/gl_thread", error)) {
        return nullptr;
    }
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    bool advertised = false;
    for (GLint i = 0; i < count && !advertised; ++i) {
        const auto* name = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        advertised = name != nullptr && std::strcmp(name, "GL_ARB_buffer_storage") == 0;
    }
    return advertised ? reinterpret_cast<PicaGlBufferStorageProc>(eglGetProcAddress("glBufferStorage"))
                      : nullptr;
}
#endif

// Persistent slots of `stream` (see PicaGlStreamBuffer); false leaves the
// ring as one ordinary buffer.
bool CreatePicaGlPersistentStream(PicaGlStreamBuffer& stream) {
#if defined(__SWITCH__)
    static const PicaGlBufferStorageProc storage = PicaGlPersistentStreamStorage();
    if (storage == nullptr) return false;
    const GLbitfield flags = GL_MAP_WRITE_BIT | kPicaGlMapPersistent | kPicaGlMapCoherent;
    glGenBuffers(static_cast<GLsizei>(stream.Slots.size()), stream.Slots.data());
    for (size_t i = 0; i < stream.Slots.size(); ++i) {
        glBindBuffer(stream.Target, stream.Slots[i]);
        storage(stream.Target, static_cast<GLsizeiptr>(stream.Ring.Capacity()), nullptr, flags);
        stream.Mapped[i] = static_cast<uint8_t*>(glMapBufferRange(
            stream.Target, 0, static_cast<GLsizeiptr>(stream.Ring.Capacity()), flags));
        if (stream.Mapped[i] == nullptr) {
            glDeleteBuffers(static_cast<GLsizei>(stream.Slots.size()), stream.Slots.data());
            stream.Slots = {};
            stream.Mapped = {};
            return false;
        }
    }
    stream.Persistent = true;
    stream.Slot = 0U;
    stream.Handle = stream.Slots[0];
    return true;
#else
    (void)stream;
    return false;
#endif
}

void DeletePicaGlStream(PicaGlStreamBuffer& stream) {
    if (!stream.Persistent) {
        glDeleteBuffers(1, &stream.Handle);
        return;
    }
    for (GLsync& fence : stream.Fences) {
        if (fence != nullptr) glDeleteSync(fence);
        fence = nullptr;
    }
    glDeleteBuffers(static_cast<GLsizei>(stream.Slots.size()), stream.Slots.data());
    stream.Persistent = false;
}

// Live GPU objects and bytes, published at each frame's first draw for the
// stats reader (another thread).
struct PicaGlResidencyCounters {
    std::atomic<uint64_t> TexturesLive{0};
    std::atomic<uint64_t> TextureBytes{0};
    std::atomic<uint64_t> LutsLive{0};
    std::atomic<uint64_t> LutBytes{0};
    std::atomic<uint64_t> ProgramsLive{0};
    std::atomic<uint64_t> TargetsLive{0};
    std::atomic<uint64_t> DisplaysLive{0};
    std::atomic<uint64_t> GeometryBuffersLive{0};
    std::atomic<uint64_t> GeometryBufferBytes{0};
    std::atomic<uint64_t> TexturesCreated{0};
    std::atomic<uint64_t> TexturesEvicted{0};
    std::atomic<uint64_t> RingOrphans{0};
    std::atomic<uint64_t> OutOfMemoryEvents{0};
    std::atomic<uint64_t> SkippedDraws{0};
};

// The driver could not allocate or map memory for a draw (GL_OUT_OF_MEMORY
// or a failed buffer map): the draw is skipped after a trim, not fatal.
struct PicaGlOutOfMemory : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Drains the GL error flags; true when one of them was GL_OUT_OF_MEMORY.
// With Mesa's GL thread each glGetError waits for it to run everything
// queued (two per new texture: ~26 waits in a scene frame); the pre-swap
// drain takes the errors instead (BeginPicaGlFrame).
bool TakePicaGlOutOfMemory() {
#if defined(__SWITCH__)
    if (SwitchMesaGlThreadRunning()) return false;
#endif
    bool outOfMemory = false;
    for (int guard = 0; guard < 8; ++guard) {
        const GLenum error = glGetError();
        if (error == GL_NO_ERROR) break;
        outOfMemory = outOfMemory || error == GL_OUT_OF_MEMORY;
    }
    return outOfMemory;
}

GLuint CompilePicaGlShader(GLenum stage, std::string_view source) {
    const GLuint shader = glCreateShader(stage);
    const GLchar* text = source.data();
    const GLint length = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &text, &length);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) {
        return shader;
    }
    GLint logLength = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
    std::string log(static_cast<size_t>(std::max(logLength, 1)), '\0');
    glGetShaderInfoLog(shader, logLength, nullptr, log.data());
    log.resize(std::strlen(log.c_str()));
    const GLenum error = glGetError();
    glDeleteShader(shader);
    const char* stageName = stage == GL_VERTEX_SHADER ? "vertex" : "fragment";
    // An empty driver log says little; record the context and the source so
    // a failure on a new driver (e.g. Switch Mesa) can be reproduced offline.
    const auto glString = [](GLenum name) {
        const auto* value = reinterpret_cast<const char*>(glGetString(name));
        return std::string(value != nullptr ? value : "(null)");
    };
    std::ofstream(std::string("logs/failed_pica_") + stageName + ".glsl", std::ios::trunc) << source;
    throw std::runtime_error(
        std::string(stageName) + " PICA OpenGL shader failed: " + log +
        " [shader=" + std::to_string(shader) + " glError=" + std::to_string(error) +
        " GL_VERSION=" + glString(GL_VERSION) +
        " GLSL=" + glString(GL_SHADING_LANGUAGE_VERSION) +
        " source_bytes=" + std::to_string(source.size()) +
#if defined(__SWITCH__)
        " thread=" + std::to_string(threadGetCurHandle()) +
        " egl_ctx=" + std::to_string(reinterpret_cast<uintptr_t>(eglGetCurrentContext())) +
#endif
        "]");
}

void BindPicaGlProgramSamplers(GLuint program) {
    for (GLint slot = 0; slot < 3; ++slot) {
        const std::string name = "pica_texture" + std::to_string(slot);
        const GLint location = glGetUniformLocation(program, name.c_str());
        if (location >= 0) {
            glUseProgram(program);
            glUniform1i(location, slot + 1);
        }
    }
    const GLint lighting = glGetUniformLocation(program, "pica_lighting_lut");
    if (lighting >= 0) {
        glUseProgram(program);
        glUniform1i(lighting, 13);
    }
}

GLuint LinkPicaGlProgram(std::string_view vertexSource, std::string_view fragmentSource) {
    const GLuint vertex = CompilePicaGlShader(GL_VERTEX_SHADER, vertexSource);
    GLuint fragment = 0U;
    GLuint program = 0U;
    try {
        fragment = CompilePicaGlShader(GL_FRAGMENT_SHADER, fragmentSource);
        program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            GLint logLength = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
            std::string log(
                static_cast<size_t>(std::max(logLength, 1)), '\0');
            glGetProgramInfoLog(program, logLength, nullptr, log.data());
            throw std::runtime_error(
                "PICA OpenGL program link failed: " + log);
        }
    } catch (...) {
        if (program != 0U) {
            glDeleteProgram(program);
        }
        glDeleteShader(fragment);
        glDeleteShader(vertex);
        throw;
    }
    // Detach before deleting: an attached shader is only marked for deletion
    // and lives as long as the program. Mesa 26 keeps the NIR of every
    // compiled shader in it (compile-time glsl_to_nir), ~2 MB per program
    // with these sources, never used again after the link.
    glDetachShader(program, fragment);
    glDetachShader(program, vertex);
    glDeleteShader(fragment);
    glDeleteShader(vertex);
    return program;
}

#if defined(__SWITCH__)
// Shader warm-up. The console's Mesa driver exposes no program binary
// formats, so linked programs cannot be stored. Instead every program the
// game links records its translated sources on the SD card, and the next
// launch links them all before the first draw (StartPicaGlWarmup) instead of
// stalling for ~0.1-0.4 s at each first use during play.
constexpr const char* kPicaGlProgramCacheDirectory = "sdmc:/switch/oot3dre/shadercache";
constexpr uint32_t kPicaGlProgramSourceMagic = 0x31534750U; // "PGS1"

struct PicaGlWarmup {
    std::mutex Mutex;
    std::unordered_map<uint64_t, GLuint> Ready;
};

PicaGlWarmup& PicaGlWarmupState() {
    static auto* state = new PicaGlWarmup(); // shared with the warm-up thread; never destroyed
    return *state;
}

uint64_t HashPicaGlProgramKey(std::string_view vertex, std::string_view fragment) {
    uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&](std::string_view text) {
        for (const unsigned char byte : text) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        hash ^= 0xFFU;
        hash *= 1099511628211ULL;
    };
    mix(vertex);
    mix(fragment);
    return hash;
}

std::string PicaGlProgramSourcePath(uint64_t key) {
    char name[32];
    std::snprintf(name, sizeof(name), "/%016llx.src", static_cast<unsigned long long>(key));
    return std::string(kPicaGlProgramCacheDirectory) + name;
}

void RecordPicaGlProgramSource(uint64_t key, std::string_view vertex, std::string_view fragment) {
    const auto path = PicaGlProgramSourcePath(key);
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        return;
    }
    std::filesystem::create_directories(kPicaGlProgramCacheDirectory, error);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    const uint32_t header[3] = {kPicaGlProgramSourceMagic, static_cast<uint32_t>(vertex.size()),
                                static_cast<uint32_t>(fragment.size())};
    file.write(reinterpret_cast<const char*>(header), sizeof(header));
    file.write(vertex.data(), static_cast<std::streamsize>(vertex.size()));
    file.write(fragment.data(), static_cast<std::streamsize>(fragment.size()));
    if (!file) {
        file.close();
        std::filesystem::remove(path, error);
    }
}

bool ReadPicaGlProgramSource(const std::filesystem::path& path, std::string& vertex,
                             std::string& fragment) {
    std::ifstream file(path, std::ios::binary);
    uint32_t header[3]{};
    if (!file.read(reinterpret_cast<char*>(header), sizeof(header)) ||
        header[0] != kPicaGlProgramSourceMagic) {
        return false;
    }
    vertex.resize(header[1]);
    fragment.resize(header[2]);
    return static_cast<bool>(file.read(vertex.data(), header[1])) &&
           static_cast<bool>(file.read(fragment.data(), header[2]));
}

// Called once from the render thread, with its context current, before the
// first PICA program is needed. A shared context on another thread would be
// faster but is unsafe here: nvc0 contexts in one share group use the same
// push buffer without a lock, and the main thread's next buffer upload
// crashed in pushbuf_submit while the warm-up thread linked. So the recorded
// programs are linked synchronously, as one pause at start-up instead of
// stalls in play.
void StartPicaGlWarmup() {
    auto& state = PicaGlWarmupState();
    std::error_code error;
    size_t sources = 0U;
    size_t linked = 0U;
    size_t failed = 0U;
    size_t skipped = 0U;
    const auto started = std::chrono::steady_clock::now();
    for (const auto& entry : std::filesystem::directory_iterator(kPicaGlProgramCacheDirectory, error)) {
        if (entry.path().extension() != ".src") {
            continue;
        }
        ++sources;
        // Programs already in Mesa's disk cache link in ~12 ms; one that is not
        // takes 1-3 s. Bound the pause; the rest compile on first use as before.
        if (std::chrono::steady_clock::now() - started > std::chrono::seconds(4)) {
            ++skipped;
            continue;
        }
        uint64_t key = 0U;
        try {
            key = std::stoull(entry.path().stem().string(), nullptr, 16);
        } catch (...) {
            ++failed;
            continue;
        }
        std::string vertex;
        std::string fragment;
        if (!ReadPicaGlProgramSource(entry.path(), vertex, fragment) ||
            HashPicaGlProgramKey(vertex, fragment) != key) {
            ++failed;
            continue;
        }
        try {
            state.Ready.emplace(key, LinkPicaGlProgram(vertex, fragment));
            ++linked;
        } catch (...) {
            ++failed;
        }
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::filesystem::create_directories(kPicaGlProgramCacheDirectory, error);
    GLint binaryFormats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &binaryFormats);
    std::ofstream(std::string(kPicaGlProgramCacheDirectory) + "/status.txt", std::ios::trunc)
        << "binary_formats=" << binaryFormats << "\nwarmup_sources=" << sources << "\nwarmup_linked=" << linked
        << "\nwarmup_failed=" << failed << "\nwarmup_skipped=" << skipped << "\nwarmup_seconds=" << seconds << '\n';
}
#endif

PicaGlProgram BuildPicaGlProgram(const GfxNativePicaDrawView& draw) {
    std::string vertexSource;
    std::string fragmentSource;
    std::string translateError;
    if (!Renderer3ds::TranslatePicaShaderToOpenGl43(
            draw.VertexShaderSource, vertexSource, &translateError)) {
        throw std::runtime_error(
            "PICA vertex shader translation failed: " + translateError);
    }
    if (!Renderer3ds::TranslatePicaShaderToOpenGl43(
            draw.FragmentShaderSource, fragmentSource, &translateError)) {
        throw std::runtime_error(
            "PICA fragment shader translation failed: " + translateError);
    }

#if defined(__SWITCH__)
    // Linking every recorded program up front took minutes on the console
    // (1-3 s per program with the stock Mesa). The warm-up stays available
    // for a Mesa built with its disk cache, behind config/shader_warmup.
    static const bool warmupStarted = [] {
        // A Mesa with its disk cache reports a program binary format; then a
        // recorded program loads in ~12 ms and warming all of them is cheap.
        GLint binaryFormats = 0;
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &binaryFormats);
        std::error_code error;
        if (binaryFormats > 0 ||
            std::filesystem::exists("sdmc:/switch/oot3dre/config/shader_warmup", error)) {
            StartPicaGlWarmup();
        }
        return true;
    }();
    (void)warmupStarted;
    const uint64_t cacheKey = HashPicaGlProgramKey(vertexSource, fragmentSource);
    {
        auto& state = PicaGlWarmupState();
        std::unique_lock lock(state.Mutex);
        if (const auto found = state.Ready.find(cacheKey); found != state.Ready.end()) {
            const GLuint program = found->second;
            state.Ready.erase(found);
            lock.unlock();
            BindPicaGlProgramSamplers(program);
            const auto usage = PicaGlUniformTrim::ProgramUsedBytes(vertexSource, fragmentSource);
            return {program, PicaGlGpuLean::ClassifyFragmentSource(fragmentSource),
                    usage.Vertex, usage.Fragment};
        }
    }
#endif
    const GLuint program = LinkPicaGlProgram(vertexSource, fragmentSource);
    BindPicaGlProgramSamplers(program);
#if defined(__SWITCH__)
    RecordPicaGlProgramSource(cacheKey, vertexSource, fragmentSource);
#endif
    const auto usage = PicaGlUniformTrim::ProgramUsedBytes(vertexSource, fragmentSource);
    return {program, PicaGlGpuLean::ClassifyFragmentSource(fragmentSource),
            usage.Vertex, usage.Fragment};
}

GLenum ToPicaGlCompare(GfxNativePicaCompareFunction compare) {
    switch (compare) {
        case GfxNativePicaCompareFunction::Never: return GL_NEVER;
        case GfxNativePicaCompareFunction::Always: return GL_ALWAYS;
        case GfxNativePicaCompareFunction::Equal: return GL_EQUAL;
        case GfxNativePicaCompareFunction::NotEqual: return GL_NOTEQUAL;
        case GfxNativePicaCompareFunction::Less: return GL_LESS;
        case GfxNativePicaCompareFunction::LessOrEqual: return GL_LEQUAL;
        case GfxNativePicaCompareFunction::Greater: return GL_GREATER;
        case GfxNativePicaCompareFunction::GreaterOrEqual: return GL_GEQUAL;
    }
    throw std::runtime_error("invalid PICA compare function");
}

GLenum ToPicaGlStencil(GfxNativePicaStencilAction action) {
    switch (action) {
        case GfxNativePicaStencilAction::Keep: return GL_KEEP;
        case GfxNativePicaStencilAction::Zero: return GL_ZERO;
        case GfxNativePicaStencilAction::Replace: return GL_REPLACE;
        case GfxNativePicaStencilAction::Increment: return GL_INCR;
        case GfxNativePicaStencilAction::Decrement: return GL_DECR;
        case GfxNativePicaStencilAction::Invert: return GL_INVERT;
        case GfxNativePicaStencilAction::IncrementWrap: return GL_INCR_WRAP;
        case GfxNativePicaStencilAction::DecrementWrap: return GL_DECR_WRAP;
    }
    throw std::runtime_error("invalid PICA stencil action");
}

GLenum ToPicaGlLogic(GfxNativePicaLogicOperation operation) {
    switch (operation) {
        case GfxNativePicaLogicOperation::Clear: return GL_CLEAR;
        case GfxNativePicaLogicOperation::And: return GL_AND;
        case GfxNativePicaLogicOperation::AndReverse: return GL_AND_REVERSE;
        case GfxNativePicaLogicOperation::Copy: return GL_COPY;
        case GfxNativePicaLogicOperation::Set: return GL_SET;
        case GfxNativePicaLogicOperation::CopyInverted: return GL_COPY_INVERTED;
        case GfxNativePicaLogicOperation::NoOp: return GL_NOOP;
        case GfxNativePicaLogicOperation::Invert: return GL_INVERT;
        case GfxNativePicaLogicOperation::Nand: return GL_NAND;
        case GfxNativePicaLogicOperation::Or: return GL_OR;
        case GfxNativePicaLogicOperation::Nor: return GL_NOR;
        case GfxNativePicaLogicOperation::Xor: return GL_XOR;
        case GfxNativePicaLogicOperation::Equivalent: return GL_EQUIV;
        case GfxNativePicaLogicOperation::AndInverted: return GL_AND_INVERTED;
        case GfxNativePicaLogicOperation::OrReverse: return GL_OR_REVERSE;
        case GfxNativePicaLogicOperation::OrInverted: return GL_OR_INVERTED;
    }
    throw std::runtime_error("invalid PICA logic operation");
}

GLenum ToPicaGlBlendEquation(GfxNativeBlendEquation equation) {
    switch (equation) {
        case GfxNativeBlendEquation::Add: return GL_FUNC_ADD;
        case GfxNativeBlendEquation::Subtract: return GL_FUNC_SUBTRACT;
        case GfxNativeBlendEquation::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;
        case GfxNativeBlendEquation::Min: return GL_MIN;
        case GfxNativeBlendEquation::Max: return GL_MAX;
    }
    throw std::runtime_error("invalid PICA blend equation");
}

GLenum ToPicaGlBlendFactor(GfxNativeBlendFactor factor) {
    switch (factor) {
        case GfxNativeBlendFactor::Zero: return GL_ZERO;
        case GfxNativeBlendFactor::One: return GL_ONE;
        case GfxNativeBlendFactor::SourceColor: return GL_SRC_COLOR;
        case GfxNativeBlendFactor::OneMinusSourceColor: return GL_ONE_MINUS_SRC_COLOR;
        case GfxNativeBlendFactor::DestColor: return GL_DST_COLOR;
        case GfxNativeBlendFactor::OneMinusDestColor: return GL_ONE_MINUS_DST_COLOR;
        case GfxNativeBlendFactor::SourceAlpha: return GL_SRC_ALPHA;
        case GfxNativeBlendFactor::OneMinusSourceAlpha: return GL_ONE_MINUS_SRC_ALPHA;
        case GfxNativeBlendFactor::DestAlpha: return GL_DST_ALPHA;
        case GfxNativeBlendFactor::OneMinusDestAlpha: return GL_ONE_MINUS_DST_ALPHA;
        case GfxNativeBlendFactor::ConstantColor: return GL_CONSTANT_COLOR;
        case GfxNativeBlendFactor::OneMinusConstantColor: return GL_ONE_MINUS_CONSTANT_COLOR;
        case GfxNativeBlendFactor::ConstantAlpha: return GL_CONSTANT_ALPHA;
        case GfxNativeBlendFactor::OneMinusConstantAlpha: return GL_ONE_MINUS_CONSTANT_ALPHA;
        case GfxNativeBlendFactor::SourceAlphaSaturate: return GL_SRC_ALPHA_SATURATE;
    }
    throw std::runtime_error("invalid PICA blend factor");
}

GLenum ToPicaGlTopology(GfxNativePicaTopology topology) {
    switch (topology) {
        case GfxNativePicaTopology::TriangleList:
        case GfxNativePicaTopology::GeometryShader:
            return GL_TRIANGLES;
        case GfxNativePicaTopology::TriangleStrip:
            return GL_TRIANGLE_STRIP;
        case GfxNativePicaTopology::TriangleFan:
            return GL_TRIANGLE_FAN;
    }
    throw std::runtime_error("invalid PICA topology");
}

GLenum ToPicaGlVertexType(GfxNativePicaVertexFormat format) {
    switch (format) {
        case GfxNativePicaVertexFormat::SignedByte: return GL_BYTE;
        case GfxNativePicaVertexFormat::UnsignedByte: return GL_UNSIGNED_BYTE;
        case GfxNativePicaVertexFormat::SignedShort: return GL_SHORT;
        case GfxNativePicaVertexFormat::Float: return GL_FLOAT;
    }
    throw std::runtime_error("invalid PICA vertex format");
}

GLenum ToPicaGlWrap(uint8_t wrap) {
    switch (wrap) {
        case 0U: return GL_CLAMP_TO_EDGE;
        case 2U:
        case 6U:
        case 7U: return GL_REPEAT;
        case 3U: return GL_MIRRORED_REPEAT;
        case 1U:
            throw std::runtime_error(
                "PICA clamp-to-border requires border color state");
        case 4U:
        case 5U:
            throw std::runtime_error("PICA asymmetric wrap is unsupported");
        default:
            throw std::runtime_error("invalid PICA texture wrap mode");
    }
}

uint32_t PicaMipCount(const GfxNativePicaTextureView& texture) {
    return Oot3d::PicaTextureMipCount(texture.Width, texture.Height,
                                      texture.MaxMipLevel);
}

std::optional<size_t> PicaEncodedMipSize(
    uint8_t format, uint32_t width, uint32_t height) {
    return Oot3d::PicaTextureEncodedMipSize(format, width, height);
}

GLint PicaGlMinFilter(const GfxNativePicaTextureView& texture,
                      uint32_t mipLevels) {
    if (mipLevels <= 1U) {
        return texture.MinLinear ? GL_LINEAR : GL_NEAREST;
    }
    if (texture.MipLinear) {
        return texture.MinLinear ? GL_LINEAR_MIPMAP_LINEAR
                                 : GL_NEAREST_MIPMAP_LINEAR;
    }
    return texture.MinLinear ? GL_LINEAR_MIPMAP_NEAREST
                             : GL_NEAREST_MIPMAP_NEAREST;
}

uint32_t PicaColorBytesPerPixel(uint8_t format) {
    return format == 0U ? 4U : (format == 1U ? 3U : 2U);
}

uint32_t PicaDepthBytesPerPixel(uint8_t format) {
    return format == 0U ? 2U : (format == 2U ? 3U : 4U);
}

uint32_t PicaFillElementSize(uint16_t control) {
    return (control & (1U << 9U)) != 0U
        ? 4U
        : ((control & (1U << 8U)) != 0U ? 3U : 2U);
}

uint8_t PicaFillByte(const GfxNativePicaMemoryFillView& fill,
                     uint32_t index) {
    const uint32_t shift =
        (index % PicaFillElementSize(fill.Control)) * 8U;
    return static_cast<uint8_t>(fill.Value >> shift);
}

float PicaUnorm(uint32_t value, uint32_t maximum) {
    return static_cast<float>(value) / static_cast<float>(maximum);
}

std::array<float, 4> DecodePicaClearColor(
    const GfxNativePicaMemoryFillView& fill, uint8_t format) {
    const uint8_t b0 = PicaFillByte(fill, 0U);
    const uint8_t b1 = PicaFillByte(fill, 1U);
    const uint8_t b2 = PicaFillByte(fill, 2U);
    const uint8_t b3 = PicaFillByte(fill, 3U);
    if (format == 0U) {
        return {PicaUnorm(b3, 0xFFU), PicaUnorm(b2, 0xFFU),
                PicaUnorm(b1, 0xFFU), PicaUnorm(b0, 0xFFU)};
    }
    if (format == 1U) {
        return {PicaUnorm(b2, 0xFFU), PicaUnorm(b1, 0xFFU),
                PicaUnorm(b0, 0xFFU), 1.0F};
    }
    const uint16_t packed = static_cast<uint16_t>(b0) |
                            static_cast<uint16_t>(b1) << 8U;
    if (format == 2U) {
        return {PicaUnorm((packed >> 11U) & 0x1FU, 0x1FU),
                PicaUnorm((packed >> 5U) & 0x3FU, 0x3FU),
                PicaUnorm(packed & 0x1FU, 0x1FU), 1.0F};
    }
    if (format == 3U) {
        return {PicaUnorm((packed >> 11U) & 0x1FU, 0x1FU),
                PicaUnorm((packed >> 6U) & 0x1FU, 0x1FU),
                PicaUnorm((packed >> 1U) & 0x1FU, 0x1FU),
                static_cast<float>(packed & 1U)};
    }
    return {PicaUnorm((packed >> 12U) & 0xFU, 0xFU),
            PicaUnorm((packed >> 8U) & 0xFU, 0xFU),
            PicaUnorm((packed >> 4U) & 0xFU, 0xFU),
            PicaUnorm(packed & 0xFU, 0xFU)};
}

std::pair<float, uint8_t> DecodePicaClearDepthStencil(
    const GfxNativePicaMemoryFillView& fill, uint8_t format) {
    const uint32_t b0 = PicaFillByte(fill, 0U);
    const uint32_t b1 = PicaFillByte(fill, 1U);
    const uint32_t b2 = PicaFillByte(fill, 2U);
    const uint32_t depth = format == 0U
        ? b0 | (b1 << 8U)
        : b0 | (b1 << 8U) | (b2 << 16U);
    return {PicaUnorm(depth, format == 0U ? 0xFFFFU : 0xFFFFFFU),
            format == 3U ? PicaFillByte(fill, 3U) : 0U};
}

void ClearPicaShadowNow(const PicaGlTarget& target, uint32_t value) {
    glBindFramebuffer(GL_FRAMEBUFFER, target.ShadowFramebuffer);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glClearBufferuiv(GL_COLOR, 0, &value);
}

// Eager unless lean: then it only records the value (see
// PicaGlTarget::ShadowClearPending) and ResolvePicaShadow clears on use.
void ClearPicaShadow(PicaGlTarget& target, uint32_t value, bool lean) {
    if (lean) {
        target.ShadowClearPending = true;
        target.ShadowClearValue = value;
        return;
    }
    ClearPicaShadowNow(target, value);
}

// Performs a deferred Shadow2D clear (true when it did; the shadow
// framebuffer is then bound). The eager clear ran wherever the fill was,
// under whatever scissor and colour mask the previous draw left (a shadow
// producer leaves the mask off, which made it a no-op); the deferred one
// clears the whole image, as a PICA fill of the buffer does.
bool ResolvePicaShadow(PicaGlTarget& target) {
    if (!target.ShadowClearPending) {
        return false;
    }
    target.ShadowClearPending = false;
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    ClearPicaShadowNow(target, target.ShadowClearValue);
    return true;
}

void DeletePicaTarget(PicaGlTarget& target) {
    glDeleteFramebuffers(1, &target.Framebuffer);
    glDeleteFramebuffers(1, &target.ShadowFramebuffer);
    glDeleteTextures(1, &target.Color);
    glDeleteTextures(1, &target.Shadow);
    glDeleteRenderbuffers(1, &target.DepthStencil);
    target = {};
}

void DeletePicaDisplay(PicaGlDisplay& display) {
    glDeleteFramebuffers(1, &display.Framebuffer);
    glDeleteTextures(1, &display.Color);
    display = {};
}

#endif

} // namespace

struct GfxRenderingAPIOGL::NativePicaState {
    std::vector<uint64_t> Completions;
#if !defined(USE_OPENGLES) && !defined(__APPLE__)
    Renderer3ds::PicaCompositionSchedule Composition;
    std::map<PicaGlShaderKey, PicaGlProgram> Programs;
    std::map<PicaGlTargetKey, PicaGlTarget> Targets;
    std::map<PicaGlTextureKey, PicaGlTexture> Textures;
    std::map<uint64_t, PicaGlTexture> LightingLuts;
    std::map<std::pair<uint64_t, uint32_t>, PicaGlDisplay> Displays;
    std::vector<GfxNativePicaMemoryFillView> PendingFills;
    std::optional<GfxNativePicaDisplayTransferView> LastPresented;
    Oot3d::PicaGeometryRegistry GeometryRegistry;
    std::unordered_map<uint64_t, PicaGlGeometryBuffer> GeometryBuffers;
    std::vector<Oot3d::PicaNriSourceVertexStream> GeometrySourceStreams;
    std::vector<Oot3d::PicaNriSourceVertexAttribute> GeometrySourceAttributes;
    std::array<GLuint, 16> VertexBuffers{};
    GLuint VertexArray = 0U;
    GLuint IndexBuffer = 0U;
    GLuint VertexUniformBuffer = 0U;
    GLuint FragmentUniformBuffer = 0U;
    GLuint FallbackColor = 0U;
    GLuint FallbackInteger = 0U;
    GLuint ScanoutProgram = 0U;
    GLint ScanoutFlipLocation = -1;
    // Part of the scanout image the plain scanout samples (dynamic
    // resolution: its content sub-rectangle); last value uploaded.
    GLint ScanoutUvScaleLocation = -1;
    std::array<float, 2> ScanoutUvScale{1.0F, 1.0F};
    uint16_t EnabledAttributeMask = 0U;
    std::array<uint8_t, 16> AttributeDivisors{};
    std::array<PicaGlAttributeState, 16> AttributeStates{};
    bool GeometryCacheEnabled = true;
    uint64_t GeometryPersistentDraws = 0U;
    uint64_t GeometryStreamingDraws = 0U;
    uint64_t GeometryPersistentUploads = 0U;
    uint64_t GeometryPersistentUploadBytes = 0U;
    uint64_t GeometryStreamingUploads = 0U;
    uint64_t GeometryStreamingUploadBytes = 0U;
    uint64_t UniformUploads = 0U;
    uint64_t UniformUploadBytes = 0U;
    // Residency: the frame of the current draws, decoded texture and LUT
    // bytes, and the policy that bounds them.
    uint64_t Frame = 0U;
    uint64_t TextureBytes = 0U;
    uint64_t LutBytes = 0U;
    uint64_t GeometryBufferBytes = 0U;
    PicaGlResidency::Policy TexturePolicy;
    std::vector<PicaGlTextureKey> TextureEvictions;
    std::vector<uint64_t> LutEvictions;
    PicaGlResidencyCounters Counters;
    // Stream rings: per-draw uniforms, streamed vertices and indices.
    PicaGlStreamBuffer UniformRing;
    PicaGlStreamBuffer VertexRing;
    PicaGlStreamBuffer IndexRing;
    uint64_t UniformAlignment = 256U;
    // config/gl_uniform_trim (gfx_opengl_pica_uniform_trim.h): copy only
    // the prefix of each uniform block the program reads.
    bool UniformTrim = true;
    float ResolutionScale = 1.0F;
    // Namespaces drawn at their own scale (a reduced in-between frame).
    std::map<uint64_t, float> NamespaceScales;
    // GL_NEAREST samplers for the relocated menu pieces, one per PICA unit
    // (PicaGlRelocatedMenuNearestSampling); 0 until first use.
    std::array<GLuint, 3> MenuNearestSamplers{};
    // config/gl_lean (exact savings; see PicaGlScale::ParseLeanSetting).
    bool Lean = true;
    // config/gl_gpu_lean (exact GPU savings; gfx_opengl_pica_gpu_lean.h).
    bool GpuLean = true;
    uint64_t NextTargetId = 1U;
    // Lean: what the uniform ring holds of the last blocks, and image unit 5.
    PicaGlUniformTrim::LastUpload LastVertexUniforms;
    PicaGlUniformTrim::LastUpload LastFragmentUniforms;
    GLuint LastShadowImage = 0U;
    PicaGlPostState Post;
    // config/gl_state_cache: shadow of the draw path's GL state (see
    // gfx_opengl_pica_state_cache.h); counters run in both modes.
    PicaGlStateCache::State Gl;
    // GL_DEPTH_CLAMP is off for PICA draws (near/far clipping, as the PICA)
    // and back on for the legacy path (gfx_opengl_pica_state_cache.h).
    PicaGlStateCache::DepthClamp DepthClamp;
    // config/gl_stats: draw counters and render-thread time inside
    // SubmitPicaDraw, published at each presentation for perf.txt.
    bool Stats = false;
    uint64_t Draws = 0U;
    uint64_t DrawNanoseconds = 0U;
    uint64_t Presents = 0U;
    struct Published {
        std::atomic<uint64_t> Draws{0U};
        std::atomic<uint64_t> DrawNanoseconds{0U};
        std::atomic<uint64_t> Presents{0U};
        std::atomic<uint64_t> Issued{0U};
        std::atomic<uint64_t> Skipped{0U};
        std::atomic<uint64_t> ProgramBinds{0U};
        std::atomic<uint64_t> TextureBinds{0U};
        std::atomic<uint64_t> VertexArrayBinds{0U};
        std::atomic<uint64_t> FramebufferBinds{0U};
        std::atomic<uint64_t> UniformUploads{0U};
    } Publish;
#endif
};

#if !defined(USE_OPENGLES) && !defined(__APPLE__)
namespace {

// GL 4.6 / ARB_texture_filter_anisotropic / EXT_texture_filter_anisotropic
// share these enums; the Switch glad header (GL 4.3) does not define them.
#ifndef GL_TEXTURE_MAX_ANISOTROPY
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#endif

// Init-time only: the extension list is walked once.
bool PicaGlAnisotropySupported() {
    GLint major = 0;
    GLint minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if (major > 4 || (major == 4 && minor >= 6)) {
        return true;
    }
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint index = 0; index < count; ++index) {
        const auto* name = reinterpret_cast<const char*>(
            glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(index)));
        if (name != nullptr &&
            (std::strcmp(name, "GL_EXT_texture_filter_anisotropic") == 0 ||
             std::strcmp(name, "GL_ARB_texture_filter_anisotropic") == 0)) {
            return true;
        }
    }
    return false;
}

void DeletePicaGlPostImage(PicaGlPostImage& image) {
    glDeleteFramebuffers(1, &image.Framebuffer);
    glDeleteTextures(1, &image.Texture);
    image = {};
}

void DeletePicaGlPostProgram(PicaGlPostProgram& program) {
    glDeleteProgram(program.Handle);
    program = {};
}

void DeletePicaGlPost(PicaGlPostState& post) {
    DeletePicaGlPostProgram(post.Fxaa);
    DeletePicaGlPostProgram(post.SmaaEdges);
    DeletePicaGlPostProgram(post.SmaaWeights);
    DeletePicaGlPostProgram(post.SmaaBlend);
    DeletePicaGlPostProgram(post.Easu);
    DeletePicaGlPostProgram(post.Rcas);
    glDeleteTextures(1, &post.SmaaArea);
    glDeleteTextures(1, &post.SmaaSearch);
    post.SmaaArea = 0U;
    post.SmaaSearch = 0U;
    DeletePicaGlPostImage(post.Color);
    DeletePicaGlPostImage(post.Edges);
    DeletePicaGlPostImage(post.Weights);
    DeletePicaGlPostImage(post.Upscaled);
}

void SetPicaGlPostSampling(GLenum filter) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    static_cast<GLint>(filter));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    static_cast<GLint>(filter));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
}

// Steady state is one comparison; GL work only when the extent changes.
bool EnsurePicaGlPostImage(PicaGlPostImage& image, GLenum format,
                           uint32_t width, uint32_t height) {
    if (image.Texture != 0U && image.Width == width &&
        image.Height == height) {
        return true;
    }
    if (image.Texture == 0U) {
        glGenTextures(1, &image.Texture);
        glGenFramebuffers(1, &image.Framebuffer);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, image.Texture);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(format),
                 static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    SetPicaGlPostSampling(GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, image.Framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, image.Texture, 0);
    image.Width = width;
    image.Height = height;
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
           GL_FRAMEBUFFER_COMPLETE;
}

GLuint CreatePicaGlPostLookup(const std::vector<uint8_t>& rgba8,
                              uint32_t width, uint32_t height) {
    GLuint texture = 0U;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width),
                 static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba8.data());
    // Same sampler as the NRI path (SMAA addresses lookup texel centres).
    SetPicaGlPostSampling(GL_LINEAR);
    return texture;
}

PicaGlPostProgram CreatePicaGlPostProgram(
    std::string_view vertexSource, const std::string& fragmentSource,
    std::initializer_list<const char*> parameters) {
    PicaGlPostProgram program;
    program.Handle = LinkPicaGlProgram(vertexSource, fragmentSource);
    program.Orient = glGetUniformLocation(program.Handle, "pica_post_orient");
    program.FlipY = glGetUniformLocation(program.Handle, "pica_flip_y");
    program.OutputSize =
        glGetUniformLocation(program.Handle, "pica_post_output_size");
    size_t index = 0U;
    for (const char* name : parameters) {
        program.Params[index++] = glGetUniformLocation(program.Handle, name);
    }
    return program;
}

void LogPicaGlPost(const std::string& message) {
    std::fprintf(stderr, "[pica-gl] %s\n", message.c_str());
}

// Builds only the programs the configuration needs, at init. A failure
// turns that effect off instead of failing the renderer.
void InitPicaGlPost(PicaGlPostState& post, std::string_view vertexSource) {
    post.Config = ReadPicaGlPostConfig();
    auto& config = post.Config;
    if (config.Anisotropy > 1U) {
        GLfloat maximum = 0.0F;
        if (PicaGlAnisotropySupported()) {
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maximum);
        }
        config.Anisotropy = std::min<uint32_t>(
            config.Anisotropy,
            static_cast<uint32_t>(std::max(maximum, 0.0F)));
        if (config.Anisotropy <= 1U) {
            config.Anisotropy = 0U;
            LogPicaGlPost("anisotropic filter unavailable on this driver");
        }
    }
    if (config.Aa == PicaGlPost::Antialiasing::Fxaa) {
        try {
            post.Fxaa = CreatePicaGlPostProgram(
                vertexSource, PicaGlPost::BuildFxaaFragmentShader(),
                {"scanout.inverse_size"});
        } catch (const std::exception& exception) {
            LogPicaGlPost(std::string("FXAA off: ") + exception.what());
            config.Aa = PicaGlPost::Antialiasing::Off;
        }
    } else if (config.Aa == PicaGlPost::Antialiasing::Smaa) {
        try {
            const auto lookup = Oot3d::DecodeSmaaLookupData();
            if (!lookup.Valid()) {
                throw std::runtime_error("SMAA lookup data is invalid");
            }
            using Stage = PicaGlPost::SmaaStage;
            post.SmaaEdges = CreatePicaGlPostProgram(
                vertexSource,
                PicaGlPost::BuildSmaaFragmentShader(Stage::EdgeDetection),
                {"pica_smaa_metrics"});
            post.SmaaWeights = CreatePicaGlPostProgram(
                vertexSource,
                PicaGlPost::BuildSmaaFragmentShader(
                    Stage::BlendWeightCalculation),
                {"pica_smaa_metrics"});
            post.SmaaBlend = CreatePicaGlPostProgram(
                vertexSource,
                PicaGlPost::BuildSmaaFragmentShader(
                    Stage::NeighborhoodBlending),
                {"pica_smaa_metrics"});
            post.SmaaArea = CreatePicaGlPostLookup(
                lookup.AreaRgba8, Oot3d::SmaaLookupData::AreaWidth,
                Oot3d::SmaaLookupData::AreaHeight);
            post.SmaaSearch = CreatePicaGlPostLookup(
                lookup.SearchRgba8, Oot3d::SmaaLookupData::SearchWidth,
                Oot3d::SmaaLookupData::SearchHeight);
        } catch (const std::exception& exception) {
            LogPicaGlPost(std::string("SMAA off: ") + exception.what());
            DeletePicaGlPostProgram(post.SmaaEdges);
            DeletePicaGlPostProgram(post.SmaaWeights);
            DeletePicaGlPostProgram(post.SmaaBlend);
            glDeleteTextures(1, &post.SmaaArea);
            glDeleteTextures(1, &post.SmaaSearch);
            post.SmaaArea = 0U;
            post.SmaaSearch = 0U;
            config.Aa = PicaGlPost::Antialiasing::Off;
        }
    }
    if (config.Upscale == PicaGlPost::Upscaler::Fsr) {
        try {
            post.Easu = CreatePicaGlPostProgram(
                vertexSource, PicaGlPost::BuildEasuFragmentShader(),
                {"pica_easu_con0", "pica_easu_con1", "pica_easu_con2",
                 "pica_easu_con3"});
        } catch (const std::exception& exception) {
            LogPicaGlPost(std::string("FSR EASU off: ") +
                          exception.what());
            config.Upscale = PicaGlPost::Upscaler::Linear;
        }
    }
    if (config.Sharpen > 0.0F) {
        try {
            post.Rcas = CreatePicaGlPostProgram(
                vertexSource, PicaGlPost::BuildRcasFragmentShader(),
                {"pica_rcas_con", "pica_rcas_resample",
                 "pica_rcas_uv_scale"});
            const auto con = PicaGlPost::FsrRcasConstants(config.Sharpen);
            glUseProgram(post.Rcas.Handle);
            glUniform4ui(post.Rcas.Params[0], con[0], con[1], con[2], con[3]);
            if (post.Rcas.Params[2] >= 0) {
                glUniform2f(post.Rcas.Params[2], 1.0F, 1.0F);
            }
        } catch (const std::exception& exception) {
            LogPicaGlPost(std::string("RCAS off: ") + exception.what());
            config.Sharpen = 0.0F;
        }
    }
    if (config.ScanoutEffects() || config.Anisotropy != 0U) {
        static constexpr std::array<const char*, 3> kAa{"off", "fxaa", "smaa"};
        char summary[160];
        std::snprintf(summary, sizeof(summary),
                      "post-processing: aa=%s upscaler=%s sharpen=%.2f "
                      "anisotropy=%u",
                      kAa[static_cast<size_t>(config.Aa)],
                      config.Upscale == PicaGlPost::Upscaler::Fsr ? "fsr"
                                                                  : "linear",
                      static_cast<double>(config.Sharpen),
                      static_cast<unsigned>(config.Anisotropy));
        LogPicaGlPost(summary);
    }
}

uint64_t PicaGlPostExtentKey(uint32_t width, uint32_t height) {
    return (static_cast<uint64_t>(width) << 32U) | height;
}

// Uploads the destination description of the bound post program when it
// changed.
void SetPicaGlPostDestination(PicaGlPostProgram& program, bool window,
                              bool flip, uint32_t width, uint32_t height) {
    const int orient = window ? 1 : 0;
    if (program.LastOrient != orient) {
        glUniform1i(program.Orient, orient);
        program.LastOrient = orient;
    }
    const int flipY = window && flip ? 1 : 0;
    if (program.LastFlipY != flipY) {
        glUniform1i(program.FlipY, flipY);
        program.LastFlipY = flipY;
    }
    const uint64_t key = PicaGlPostExtentKey(width, height);
    if (program.LastOutputKey != key) {
        glUniform2f(program.OutputSize, static_cast<float>(width),
                    static_cast<float>(height));
        program.LastOutputKey = key;
    }
}

void DrawPicaGlPostTriangle(GLuint source) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void BeginPicaGlPostImage(const PicaGlPostImage& image) {
    glBindFramebuffer(GL_FRAMEBUFFER, image.Framebuffer);
    glViewport(0, 0, static_cast<GLsizei>(image.Width),
               static_cast<GLsizei>(image.Height));
}

void SetPicaGlSmaaMetrics(PicaGlPostProgram& program, uint32_t width,
                          uint32_t height) {
    const uint64_t key = PicaGlPostExtentKey(width, height);
    if (program.LastInputKey != key) {
        glUniform4f(program.Params[0], 1.0F / static_cast<float>(width),
                    1.0F / static_cast<float>(height),
                    static_cast<float>(width), static_cast<float>(height));
        program.LastInputKey = key;
    }
}

void DrawPicaGlFxaa(PicaGlPostProgram& program, GLuint source,
                    uint32_t sourceWidth, uint32_t sourceHeight, bool window,
                    bool flip, uint32_t outputWidth, uint32_t outputHeight) {
    glUseProgram(program.Handle);
    SetPicaGlPostDestination(program, window, flip, outputWidth, outputHeight);
    const uint64_t key = PicaGlPostExtentKey(sourceWidth, sourceHeight);
    if (program.LastInputKey != key) {
        glUniform2f(program.Params[0], 1.0F / static_cast<float>(sourceWidth),
                    1.0F / static_cast<float>(sourceHeight));
        program.LastInputKey = key;
    }
    DrawPicaGlPostTriangle(source);
}

// sourceWidth/Height: the content (FSR's input viewport, the bottom-left
// sub-rectangle at a dynamic scale); textureWidth/Height: the image.
void DrawPicaGlEasu(PicaGlPostProgram& program, GLuint source,
                    uint32_t sourceWidth, uint32_t sourceHeight,
                    uint32_t textureWidth, uint32_t textureHeight, bool window,
                    bool flip, uint32_t outputWidth, uint32_t outputHeight) {
    glUseProgram(program.Handle);
    SetPicaGlPostDestination(program, window, flip, outputWidth, outputHeight);
    // Extents are below 2^16, so this packs both pairs without collisions.
    const uint64_t key =
        (static_cast<uint64_t>(sourceWidth) << 48U) |
        (static_cast<uint64_t>(sourceHeight) << 32U) |
        (static_cast<uint64_t>(outputWidth) << 16U) | outputHeight;
    const uint64_t textureKey = PicaGlPostExtentKey(textureWidth, textureHeight);
    if (program.LastInputKey != key || program.LastTextureKey != textureKey) {
        const auto con = PicaGlPost::FsrEasuConstants(
            static_cast<float>(sourceWidth), static_cast<float>(sourceHeight),
            static_cast<float>(textureWidth), static_cast<float>(textureHeight),
            static_cast<float>(outputWidth), static_cast<float>(outputHeight));
        for (size_t index = 0U; index < 4U; ++index) {
            glUniform4ui(program.Params[index], con[index * 4U],
                         con[index * 4U + 1U], con[index * 4U + 2U],
                         con[index * 4U + 3U]);
        }
        program.LastInputKey = key;
        program.LastTextureKey = textureKey;
    }
    DrawPicaGlPostTriangle(source);
}

// The plain scanout program is bound: sample the content sub-rectangle of a
// width x height image.
void SetPicaGlScanoutContent(GfxRenderingAPIOGL::NativePicaState& state,
                             uint32_t contentWidth, uint32_t contentHeight,
                             uint32_t width, uint32_t height) {
    const std::array<float, 2> scale{
        width != 0U && contentWidth != 0U
            ? static_cast<float>(contentWidth) / static_cast<float>(width)
            : 1.0F,
        height != 0U && contentHeight != 0U
            ? static_cast<float>(contentHeight) / static_cast<float>(height)
            : 1.0F,
    };
    if (state.ScanoutUvScaleLocation >= 0 && scale != state.ScanoutUvScale) {
        glUniform2f(state.ScanoutUvScaleLocation, scale[0], scale[1]);
        state.ScanoutUvScale = scale;
    }
}

// Runs the configured chain for one Replace-mode scanout and draws its last
// pass into the window viewport. The caller has prepared the window (cleared,
// blend/depth/stencil/scissor off) and bound the VAO. Steady-state cost per
// pass: one FBO bind, viewport, program, texture bind and draw; no
// allocation, no queries.
void PresentPicaGlPost(GfxRenderingAPIOGL::NativePicaState& state,
                       const PicaGlDisplay& display, bool flip,
                       GLint viewportX, GLint viewportY,
                       GLsizei viewportWidth, GLsizei viewportHeight) {
    auto& post = state.Post;
    auto& config = post.Config;
    const uint32_t sourceWidth = display.AllocWidth;
    const uint32_t sourceHeight = display.AllocHeight;
    // Dynamic resolution: the copy filled only this bottom-left part (with a
    // replicated border); otherwise the whole image. The AA passes never see
    // a sub-rectangle (dynamic resolution is off with SMAA/FXAA).
    const uint32_t contentWidth =
        display.ContentWidth != 0U ? display.ContentWidth : sourceWidth;
    const uint32_t contentHeight =
        display.ContentHeight != 0U ? display.ContentHeight : sourceHeight;
    // The window viewport, in the portrait layout of the source image.
    const uint32_t outputWidth = static_cast<uint32_t>(viewportHeight);
    const uint32_t outputHeight = static_cast<uint32_t>(viewportWidth);

    // EASU only enlarges; at 1:1 (e.g. scale 3 on the 720p handheld screen)
    // FSR reduces to RCAS.
    const bool easu = config.Upscale == PicaGlPost::Upscaler::Fsr &&
                      (outputWidth > contentWidth || outputHeight > contentHeight);
    const bool rcas = config.Sharpen > 0.0F;
    // FXAA alone near 1:1 samples the source from the window pass, exactly
    // like the PC scanout. When the window has at least twice the source's
    // pixels (scale 1-2, or docked), FXAA at source size plus the plain
    // scanout is cheaper (measured), and it resolves into an image first.
    const uint64_t sourcePixels =
        static_cast<uint64_t>(sourceWidth) * sourceHeight;
    const uint64_t outputPixels =
        static_cast<uint64_t>(outputWidth) * outputHeight;
    const bool fxaaInWindow =
        config.Aa == PicaGlPost::Antialiasing::Fxaa && !easu && !rcas &&
        outputPixels < sourcePixels * 2U;

    GLuint current = display.Color;
    uint32_t currentWidth = contentWidth;
    uint32_t currentHeight = contentHeight;
    uint32_t currentTextureWidth = sourceWidth;
    uint32_t currentTextureHeight = sourceHeight;
    bool complete = true;

    if (config.Aa == PicaGlPost::Antialiasing::Smaa) {
        GlPerf::Mark(GlPerf::Stage::PostAa);
        complete = EnsurePicaGlPostImage(post.Edges, GL_RG8, sourceWidth,
                                         sourceHeight) &&
                   EnsurePicaGlPostImage(post.Weights, GL_RGBA8, sourceWidth,
                                         sourceHeight) &&
                   EnsurePicaGlPostImage(post.Color, GL_RGBA8, sourceWidth,
                                         sourceHeight);
        if (complete) {
            BeginPicaGlPostImage(post.Edges);
            // The official edge pass discards edge-free pixels.
            glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
            glClear(GL_COLOR_BUFFER_BIT);
            glUseProgram(post.SmaaEdges.Handle);
            SetPicaGlSmaaMetrics(post.SmaaEdges, sourceWidth, sourceHeight);
            DrawPicaGlPostTriangle(current);

            BeginPicaGlPostImage(post.Weights);
            glUseProgram(post.SmaaWeights.Handle);
            SetPicaGlSmaaMetrics(post.SmaaWeights, sourceWidth, sourceHeight);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, post.SmaaArea);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, post.SmaaSearch);
            DrawPicaGlPostTriangle(post.Edges.Texture);

            BeginPicaGlPostImage(post.Color);
            glUseProgram(post.SmaaBlend.Handle);
            SetPicaGlSmaaMetrics(post.SmaaBlend, sourceWidth, sourceHeight);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, post.Weights.Texture);
            DrawPicaGlPostTriangle(current);
            current = post.Color.Texture;
        }
    } else if (config.Aa == PicaGlPost::Antialiasing::Fxaa && !fxaaInWindow) {
        GlPerf::Mark(GlPerf::Stage::PostAa);
        complete = EnsurePicaGlPostImage(post.Color, GL_RGBA8, sourceWidth,
                                         sourceHeight);
        if (complete) {
            BeginPicaGlPostImage(post.Color);
            DrawPicaGlFxaa(post.Fxaa, current, sourceWidth, sourceHeight,
                           false, flip, sourceWidth, sourceHeight);
            current = post.Color.Texture;
        }
    }
    if (complete && easu && rcas) {
        GlPerf::Mark(GlPerf::Stage::PostEasu);
        const GLuint upscaledBefore = post.Upscaled.Texture;
        const uint64_t upscaledExtentBefore =
            PicaGlPostExtentKey(post.Upscaled.Width, post.Upscaled.Height);
        complete = EnsurePicaGlPostImage(post.Upscaled, GL_RGBA8, outputWidth,
                                         outputHeight);
        // Lean: the same display image, filled by the same copy, already
        // went through EASU into Upscaled (which was not reallocated).
        const bool upscaledCurrent =
            state.Lean && config.Aa == PicaGlPost::Antialiasing::Off &&
            upscaledBefore == post.Upscaled.Texture &&
            upscaledExtentBefore ==
                PicaGlPostExtentKey(post.Upscaled.Width, post.Upscaled.Height) &&
            post.UpscaledSource == display.Color &&
            display.LastCopy.Valid() && post.UpscaledCopy == display.LastCopy &&
            post.UpscaledFlip == flip;
        if (complete && !upscaledCurrent) {
            BeginPicaGlPostImage(post.Upscaled);
            DrawPicaGlEasu(post.Easu, current, currentWidth, currentHeight,
                           currentTextureWidth, currentTextureHeight, false,
                           flip, outputWidth, outputHeight);
            post.UpscaledSource =
                config.Aa == PicaGlPost::Antialiasing::Off ? display.Color : 0U;
            post.UpscaledCopy = display.LastCopy;
            post.UpscaledFlip = flip;
        }
        if (complete) {
            current = post.Upscaled.Texture;
            currentWidth = outputWidth;
            currentHeight = outputHeight;
            currentTextureWidth = outputWidth;
            currentTextureHeight = outputHeight;
        }
    }

    GlPerf::Mark(GlPerf::Stage::Scanout);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(viewportX, viewportY, viewportWidth, viewportHeight);
    if (!complete) {
        // An intermediate image could not be attached: keep the plain
        // scanout from now on rather than a broken chain.
        LogPicaGlPost("intermediate image incomplete; post-processing "
                      "off");
        config.Aa = PicaGlPost::Antialiasing::Off;
        config.Upscale = PicaGlPost::Upscaler::Linear;
        config.Sharpen = 0.0F;
        current = display.Color;
        currentWidth = contentWidth;
        currentHeight = contentHeight;
        currentTextureWidth = sourceWidth;
        currentTextureHeight = sourceHeight;
    } else if (rcas) {
        glUseProgram(post.Rcas.Handle);
        SetPicaGlPostDestination(post.Rcas, true, flip, outputWidth,
                                 outputHeight);
        const int resample =
            currentWidth != outputWidth || currentHeight != outputHeight ||
                    currentTextureWidth != outputWidth ||
                    currentTextureHeight != outputHeight
                ? 1
                : 0;
        if (post.Rcas.LastResample != resample) {
            glUniform1i(post.Rcas.Params[1], resample);
            post.Rcas.LastResample = resample;
        }
        const uint64_t contentKey =
            PicaGlPostExtentKey(currentWidth, currentHeight);
        const uint64_t textureKey =
            PicaGlPostExtentKey(currentTextureWidth, currentTextureHeight);
        if (post.Rcas.LastInputKey != contentKey ||
            post.Rcas.LastTextureKey != textureKey) {
            glUniform2f(post.Rcas.Params[2],
                        static_cast<float>(currentWidth) /
                            static_cast<float>(currentTextureWidth),
                        static_cast<float>(currentHeight) /
                            static_cast<float>(currentTextureHeight));
            post.Rcas.LastInputKey = contentKey;
            post.Rcas.LastTextureKey = textureKey;
        }
        DrawPicaGlPostTriangle(current);
        return;
    } else if (easu) {
        DrawPicaGlEasu(post.Easu, current, currentWidth, currentHeight,
                       currentTextureWidth, currentTextureHeight, true, flip,
                       outputWidth, outputHeight);
        return;
    } else if (fxaaInWindow) {
        DrawPicaGlFxaa(post.Fxaa, current, currentWidth, currentHeight, true,
                       flip, outputWidth, outputHeight);
        return;
    }
    // AA alone resolved into an image (or the fallback): the plain scanout
    // of the current image.
    glUseProgram(state.ScanoutProgram);
    if (state.ScanoutFlipLocation >= 0) {
        glUniform1i(state.ScanoutFlipLocation, flip ? 1 : 0);
    }
    SetPicaGlScanoutContent(state, currentWidth, currentHeight,
                            currentTextureWidth, currentTextureHeight);
    DrawPicaGlPostTriangle(current);
}

} // namespace
#endif

GfxRenderingAPIOGL::GfxRenderingAPIOGL() = default;

GfxRenderingAPIOGL::~GfxRenderingAPIOGL() {
    ShutdownNativePica();
}

void GfxRenderingAPIOGL::InitNativePica() {
    ShutdownNativePica();
    mNativePica = std::make_unique<NativePicaState>();
#if !defined(USE_OPENGLES) && !defined(__APPLE__)
    auto& state = *mNativePica;
    state.GeometryCacheEnabled = mNativePicaGeometryCacheEnabled;
    state.ResolutionScale = ReadPicaGlResolutionScale();
    state.Lean = ReadPicaGlLean();
    state.GpuLean = ReadPicaGlGpuLean();
    static const bool uniformTrim = ReadPicaGlSwitch(
        "gl_uniform_trim", "TRIAEVUM_GL_UNIFORM_TRIM", true, true);
    state.UniformTrim = uniformTrim;
    static const bool stateCache = ReadPicaGlSwitch(
        "gl_state_cache", "TRIAEVUM_GL_STATE_CACHE", true, true);
    static const bool stats =
        ReadPicaGlSwitch("gl_stats", "TRIAEVUM_GL_STATS", false, true);
    state.Gl.Enabled = stateCache;
    state.Stats = stats;
    if (state.ResolutionScale > 1.0F) {
        // Keep the largest PICA target (1024 per axis) within the driver's
        // limits; otherwise fall back to a smaller scale.
        GLint maxTexture = 0;
        GLint maxRenderbuffer = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexture);
        glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxRenderbuffer);
        const GLint limit = std::min(maxTexture, maxRenderbuffer);
        state.ResolutionScale =
            PicaGlScale::FitResolutionScale(state.ResolutionScale, limit);
    }
    // Dynamic resolution can lower the configured scale (never raise it)
    // unless SMAA/FXAA, whose images have the source's extent, are on.
    PicaGlDynamicScale::gState.MaxScale = state.ResolutionScale;
    PicaGlDynamicScale::gState.Supported =
        state.ResolutionScale > 1.0F &&
        ReadPicaGlPostConfig().Aa == PicaGlPost::Antialiasing::Off;
    glGenVertexArrays(1, &state.VertexArray);
    glGenBuffers(static_cast<GLsizei>(state.VertexBuffers.size()),
                 state.VertexBuffers.data());
    glGenBuffers(1, &state.IndexBuffer);
    glGenBuffers(1, &state.VertexUniformBuffer);
    glGenBuffers(1, &state.FragmentUniformBuffer);
    state.TexturePolicy.BudgetBytes = ReadPicaGlTextureBudget();
    {
        GLint alignment = 0;
        glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &alignment);
        if (alignment > 0) state.UniformAlignment = static_cast<uint64_t>(alignment);
        // A draw's fragment block is 6 KiB: the uniform ring wraps about
        // once a frame; the others less often. At most 2 MiB each: nouveau
        // suballocates buffers up to 2 MiB from slabs it keeps
        // (nouveau_mm), so orphaning one recycles a slab chunk. A larger
        // ring is a fresh memalign() from the newlib heap on every orphan
        // (with the old storage freed when the GPU is done), and those
        // multi-MiB holes fragmented the heap: on the console the arena
        // grew from 631 to 1659 MB in 8.5 min with ~700 MB in use.
        state.UniformRing = {GL_UNIFORM_BUFFER, 0U,
                             PicaGlResidency::StreamRing(2ULL << 20U)};
        state.VertexRing = {GL_ARRAY_BUFFER, 0U,
                            PicaGlResidency::StreamRing(2ULL << 20U)};
        state.IndexRing = {GL_ELEMENT_ARRAY_BUFFER, 0U,
                           PicaGlResidency::StreamRing(1ULL << 20U)};
        for (auto* stream : {&state.UniformRing, &state.VertexRing, &state.IndexRing}) {
            if (!CreatePicaGlPersistentStream(*stream)) glGenBuffers(1, &stream->Handle);
        }
#if defined(__SWITCH__)
        // Own file: logs/glthread.txt is rewritten after this, when the
        // context moves to the render thread.
        if (FILE* log = std::fopen("sdmc:/switch/oot3dre/logs/gl_streams.txt", "w")) {
            std::fprintf(log, "persistent_streams=%d\n",
                         state.UniformRing.Persistent && state.VertexRing.Persistent &&
                                 state.IndexRing.Persistent ? 1 : 0);
            std::fclose(log);
        }
#endif
    }

    const std::array<uint8_t, 4> white{255U, 255U, 255U, 255U};
    glGenTextures(1, &state.FallbackColor);
    glBindTexture(GL_TEXTURE_2D, state.FallbackColor);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, white.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    const uint32_t emptyShadow = 0xFFFFFFFFU;
    glGenTextures(1, &state.FallbackInteger);
    glBindTexture(GL_TEXTURE_2D, state.FallbackInteger);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, 1, 1, 0, GL_RED_INTEGER,
                 GL_UNSIGNED_INT, &emptyShadow);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    static constexpr std::string_view kScanoutVertex = R"glsl(#version 430
out vec2 pica_uv;
void main() {
    const vec2 positions[3] = vec2[3](
        vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 position = positions[gl_VertexID];
    gl_Position = vec4(position, 0.0, 1.0);
    pica_uv = position * 0.5 + 0.5;
}
)glsl";
    static constexpr std::string_view kScanoutFragment = R"glsl(#version 430
layout(binding=0) uniform sampler2D pica_scanout;
uniform int pica_flip_y;
// Content sub-rectangle of the image (dynamic resolution; 1,1 otherwise).
uniform vec2 pica_uv_scale;
in vec2 pica_uv;
layout(location=0) out vec4 pica_color;
void main() {
    // CTR display-transfer images use the physical portrait layout. OpenGL's
    // framebuffer origin is the opposite of Vulkan's, so compensate that
    // origin before applying the same portrait-to-landscape transform.
    vec2 uv = vec2(pica_uv.y, pica_uv.x);
    if (pica_flip_y != 0) uv.y = 1.0 - uv.y;
    pica_color = texture(pica_scanout, uv * pica_uv_scale);
}
)glsl";
    const GLuint vertex = CompilePicaGlShader(GL_VERTEX_SHADER, kScanoutVertex);
    GLuint fragment = 0U;
    try {
        fragment = CompilePicaGlShader(GL_FRAGMENT_SHADER, kScanoutFragment);
        state.ScanoutProgram = glCreateProgram();
        glAttachShader(state.ScanoutProgram, vertex);
        glAttachShader(state.ScanoutProgram, fragment);
        glLinkProgram(state.ScanoutProgram);
        GLint linked = GL_FALSE;
        glGetProgramiv(state.ScanoutProgram, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            throw std::runtime_error("PICA OpenGL scanout program failed to link");
        }
        glUseProgram(state.ScanoutProgram);
        const GLint sampler =
            glGetUniformLocation(state.ScanoutProgram, "pica_scanout");
        if (sampler >= 0) {
            glUniform1i(sampler, 0);
        }
        state.ScanoutFlipLocation =
            glGetUniformLocation(state.ScanoutProgram, "pica_flip_y");
        state.ScanoutUvScaleLocation =
            glGetUniformLocation(state.ScanoutProgram, "pica_uv_scale");
        if (state.ScanoutUvScaleLocation >= 0) {
            glUniform2f(state.ScanoutUvScaleLocation, 1.0F, 1.0F);
        }
        state.ScanoutUvScale = {1.0F, 1.0F};
    } catch (...) {
        glDeleteShader(fragment);
        glDeleteShader(vertex);
        ShutdownNativePica();
        throw;
    }
    glDetachShader(state.ScanoutProgram, fragment); // see LinkPicaGlProgram
    glDetachShader(state.ScanoutProgram, vertex);
    glDeleteShader(fragment);
    glDeleteShader(vertex);
    // Optional scanout effects and anisotropy; nothing is created when the
    // configuration files are absent.
    InitPicaGlPost(state.Post, kScanoutVertex);
#endif
}

void GfxRenderingAPIOGL::ShutdownNativePica() noexcept {
    if (!mNativePica) {
        return;
    }
#if !defined(USE_OPENGLES) && !defined(__APPLE__)
    auto& state = *mNativePica;
    if (state.DepthClamp.LeavePica()) {
        glEnable(GL_DEPTH_CLAMP);
    }
    for (auto& [key, program] : state.Programs) {
        glDeleteProgram(program.Handle);
    }
    for (auto& [key, target] : state.Targets) {
        DeletePicaTarget(target);
    }
    for (auto& [key, texture] : state.Textures) {
        glDeleteTextures(1, &texture.Handle);
    }
    for (auto& [key, texture] : state.LightingLuts) {
        glDeleteTextures(1, &texture.Handle);
    }
    for (auto& [key, display] : state.Displays) {
        DeletePicaDisplay(display);
    }
    for (auto& [identity, geometry] : state.GeometryBuffers) {
        glDeleteBuffers(1, &geometry.Handle);
    }
    DeletePicaGlPost(state.Post);
    glDeleteProgram(state.ScanoutProgram);
    if (state.MenuNearestSamplers[0] != 0U) {
        glDeleteSamplers(static_cast<GLsizei>(state.MenuNearestSamplers.size()),
                         state.MenuNearestSamplers.data());
        state.MenuNearestSamplers = {};
    }
    glDeleteTextures(1, &state.FallbackColor);
    glDeleteTextures(1, &state.FallbackInteger);
    glDeleteBuffers(static_cast<GLsizei>(state.VertexBuffers.size()),
                    state.VertexBuffers.data());
    glDeleteBuffers(1, &state.IndexBuffer);
    glDeleteBuffers(1, &state.VertexUniformBuffer);
    glDeleteBuffers(1, &state.FragmentUniformBuffer);
    DeletePicaGlStream(state.UniformRing);
    DeletePicaGlStream(state.VertexRing);
    DeletePicaGlStream(state.IndexRing);
    glDeleteVertexArrays(1, &state.VertexArray);
#endif
    mNativePica.reset();
}

void GfxRenderingAPIOGL::InvalidateLegacyStateAfterPica() noexcept {
    // The PICA path binds its own vertex array object and leaves it bound.
    // Every legacy (Fast3D) draw after it - the host UI overlay: TopScreen
    // hearts, rupee counter, item icons - would otherwise set its attribute
    // pointers on the PICA VAO, inheriting the per-instance divisors of PICA
    // fixed attributes (one value for the whole quad: transparent or flat
    // sprites) and silently invalidating the PICA attribute cache. Restore the
    // VAO the legacy path was initialised with.
    // With the state cache the restore is deferred to the first legacy call
    // that needs it (RestoreLegacyVertexArray): binding it after every PICA
    // draw and the PICA VAO again before the next one made Mesa rebuild the
    // vertex arrays of every draw.
    mPicaGlStateForeign = true;
    mPicaVertexArrayBound = true;
#if !defined(USE_OPENGLES) && !defined(__APPLE__)
    if (mNativePica == nullptr || !mNativePica->Gl.Enabled) {
        RestoreLegacyVertexArray();
    }
#else
    RestoreLegacyVertexArray();
#endif
    mLastLoadedShader = nullptr;
    mCurrentArrayBuffer = 0U;
    mLastActiveTexture = -1;
    mLastBlendEnabled = -1;
    mLastScissorEnabled = -1;
    mLastDepthTest = -1;
    mLastDepthMask = -1;
    mLastZmodeDecal = -1;
    std::fill(std::begin(mLastBoundTextures),
              std::end(mLastBoundTextures), 0U);
}

void GfxRenderingAPIOGL::RestoreLegacyVertexArray() noexcept {
#if !defined(USE_OPENGLES) && !defined(__APPLE__)
    // The legacy path draws with the GL_DEPTH_CLAMP its Init enabled.
    if (mNativePica != nullptr && mNativePica->DepthClamp.LeavePica()) {
        glEnable(GL_DEPTH_CLAMP);
    }
#endif
    if (!mPicaVertexArrayBound) {
        return;
    }
    mPicaVertexArrayBound = false;
    mPicaGlStateForeign = true;
#if !defined(USE_OPENGLES) && !defined(__APPLE__)
    if (mNativePica != nullptr) {
        // Counted with the draw path's calls: without the cache this
        // rebind follows every PICA draw.
        ++mNativePica->Gl.Count.Issued;
        ++mNativePica->Gl.Count.VertexArrayBinds;
    }
#endif
#if defined(__APPLE__) || defined(__SWITCH__) || defined(USE_OPENGLES)
    glBindVertexArray(mOpenglVao);
#else
    glBindVertexArray(0U);
#endif
}

// The PICA state cache is only trusted across consecutive PICA draws. The
// legacy path's own caches are reset after each PICA draw
// (InvalidateLegacyStateAfterPica); any legacy draw sets them again.
bool GfxRenderingAPIOGL::PicaGlStateMayBeStale() const noexcept {
    return mPicaGlStateForeign || !mPicaVertexArrayBound ||
           mLastLoadedShader != nullptr || mCurrentArrayBuffer != 0U ||
           mLastActiveTexture != -1 || mLastBlendEnabled != -1 ||
           mLastScissorEnabled != -1 || mLastDepthTest != -1 ||
           mLastDepthMask != -1 || mLastZmodeDecal != -1;
}

bool GfxRenderingAPIOGL::PublishPicaCompositionSequence(
    const Renderer3ds::PicaCompositionSequenceView& sequence,
    std::string* error) {
    // A new execution list: whatever ran since the last draw is unknown.
    mPicaGlStateForeign = true;
#if defined(USE_OPENGLES) || defined(__APPLE__)
    SetPicaGlError(error, "native PICA requires OpenGL 4.3");
    return false;
#else
    if (!mNativePica) {
        SetPicaGlError(error, "PICA OpenGL backend is not initialized");
        return false;
    }
    if (mNativePica->Composition.Valid()) {
        const auto previous = mNativePica->Composition.Stats();
        if (previous.ConsumedDrawCount != previous.DrawCount) {
            SetPicaGlError(
                error, "previous PICA composition sequence was not consumed");
            return false;
        }
        if (mNativePica->Composition.SequenceId() == sequence.SequenceId) {
            SetPicaGlError(error, "PICA composition identity was reused");
            return false;
        }
    }
    Renderer3ds::PicaCompositionSchedule compiled;
    if (!compiled.Compile(sequence, error)) {
        return false;
    }
    mNativePica->Composition = std::move(compiled);
    return true;
#endif
}

#if !defined(USE_OPENGLES) && !defined(__APPLE__)
namespace {

void DeletePicaGlGeometry(
    GfxRenderingAPIOGL::NativePicaState& state, uint64_t identity) {
    const auto found = state.GeometryBuffers.find(identity);
    if (found == state.GeometryBuffers.end()) {
        return;
    }
    const GLuint handle = found->second.Handle;
    for (auto& attribute : state.AttributeStates) {
        if (attribute.Buffer == handle) {
            attribute = {};
        }
    }
    glDeleteBuffers(1, &found->second.Handle);
    state.GeometryBufferBytes -=
        std::min(state.GeometryBufferBytes, found->second.Bytes);
    state.GeometryBuffers.erase(found);
}

// Widescreen factor of the top-screen target for the current window (1 when
// the option is off or the window is not wider than 5:3). Queried only when
// a target is created.
float ResolvePicaGlWidescreenFactor() {
    if (!Oot3d::GlWidescreenEnabled()) {
        return 1.0F;
    }
    SDL_Window* window = SDL_GL_GetCurrentWindow();
    if (window == nullptr) {
        return 1.0F;
    }
    int width = 0;
    int height = 0;
#ifdef __SWITCH__
    SDL_GetWindowSize(window, &width, &height);
#else
    SDL_GL_GetDrawableSize(window, &width, &height);
#endif
    if (width <= 0 || height <= 0) {
        return 1.0F;
    }
    return Oot3d::GlWidescreenFactor(static_cast<uint32_t>(width),
                                     static_cast<uint32_t>(height));
}

// Diagnostic of the widescreen placement (config/widescreen_menus_log): one
// line per distinct draw signature on the widened target, capped. GL draws
// are submitted from a single thread.
void LogPicaGlWidescreenPlacement(const std::string& path,
                                  const GfxNativePicaDrawView& draw,
                                  const Oot3d::GlWidescreenDrawEvidence& evidence,
                                  const Oot3d::GlWidescreenDrawPlacement& placement,
                                  bool uniformsAvailable) {
    constexpr size_t kMaximumLines = 4000U;
    static std::unordered_map<uint64_t, uint32_t> seen;
    static std::ofstream file;
    static bool opened = false;
    if (seen.size() >= kMaximumLines) {
        return;
    }
    Oot3d::GlWidescreenDrawLogFacts facts;
    facts.Viewport = {draw.ViewportX, draw.ViewportY, draw.ViewportWidth,
                      draw.ViewportHeight};
    facts.FramebufferWidth = draw.FramebufferWidth;
    facts.FramebufferHeight = draw.FramebufferHeight;
    facts.ScissorMode = draw.ScissorMode;
    facts.Scissor = {draw.ScissorX1, draw.ScissorY1, draw.ScissorX2,
                     draw.ScissorY2};
    facts.Domain = static_cast<uint8_t>(draw.CompositionDomain);
    facts.Layer = static_cast<uint8_t>(draw.Composition.Layer);
    facts.VertexCount = draw.VertexCount;
    facts.VertexShaderKey = draw.VertexShaderKey;
    facts.TextureCount = static_cast<uint8_t>(
        std::min<size_t>(draw.Textures.size(), 255U));
    if (!draw.Textures.empty()) {
        facts.Texture0Address = draw.Textures.front().PhysicalAddress;
        facts.Texture0Format = draw.Textures.front().NativeFormat;
    }
    facts.TypedTransform =
        draw.TemporalVertexProgram.Hooks.TransformProgram() != nullptr;
    facts.RowsAvailable = uniformsAvailable;
    if (uniformsAvailable) {
        std::memcpy(facts.Rows.data(),
                    draw.VertexUniformBytes.data() +
                        Oot3d::kPicaPackedVertexFloatOffset,
                    sizeof(facts.Rows));
    }
    const uint64_t signature =
        Oot3d::GlWidescreenDrawLogSignature(facts, evidence, placement);
    auto [entry, inserted] = seen.try_emplace(signature, 0U);
    ++entry->second;
    if (!inserted) {
        return;
    }
    if (!opened) {
        opened = true;
        file.open(path, std::ios::app);
        if (file) {
            file << "# widescreen_menus: draw_id signature decision ...\n";
        }
    }
    if (!file) {
        return;
    }
    char prefix[64];
    std::snprintf(prefix, sizeof(prefix), "%llu %016llx ",
                  static_cast<unsigned long long>(draw.SubmissionId),
                  static_cast<unsigned long long>(signature));
    file << prefix
         << Oot3d::FormatGlWidescreenDrawLogLine(facts, evidence, placement)
         << '\n';
    file.flush();
}

// Placement of one draw on the widened top-screen target: the scene's
// stretched mapping or the centred native canvas
// (gl_widescreen_menu_policy.h). Reads 64 bytes of uniforms; the vertex
// coverage proof runs only for the few draws that need it.
Oot3d::GlWidescreenDrawPlacement ResolvePicaGlWidescreenPlacement(
    const GfxNativePicaDrawView& draw, float widescreenFactor) {
    const bool menus = Oot3d::GlWidescreenMenusEnabled();
    Oot3d::GlWidescreenDrawEvidence evidence;
    evidence.UiDomain =
        draw.CompositionDomain == Renderer3ds::PicaCompositionDomain::Ui;
    evidence.Atmosphere = draw.Composition.Layer ==
                          Renderer3ds::PicaCompositionLayer::Atmosphere;
    evidence.ViewportSpansPresentedWidth =
        draw.ViewportY <= 0.5F &&
        draw.ViewportY + draw.ViewportHeight >=
            static_cast<float>(draw.FramebufferHeight) - 0.5F;
    evidence.ViewportCoversTarget =
        draw.ViewportX <= 0.0F && draw.ViewportY <= 0.0F &&
        draw.ViewportX + draw.ViewportWidth >=
            static_cast<float>(draw.FramebufferWidth) &&
        draw.ViewportY + draw.ViewportHeight >=
            static_cast<float>(draw.FramebufferHeight);
    const bool uniformsAvailable =
        draw.VertexUniformBytes.size() >=
        Oot3d::kPicaPackedVertexFloatOffset + 4U * Oot3d::kPicaPackedVec4Bytes;
    if (menus && uniformsAvailable) {
        std::array<float, 16> rows{};
        std::memcpy(rows.data(),
                    draw.VertexUniformBytes.data() +
                        Oot3d::kPicaPackedVertexFloatOffset,
                    sizeof(rows));
        const auto projection = Oot3d::DecodeGlProjection(rows);
        evidence.Projection = projection.Shape;
        evidence.PixelAnisotropy = Oot3d::GlNativePixelAnisotropy(
            projection, draw.ViewportWidth, draw.ViewportHeight,
            draw.FramebufferWidth);
    }
    if (menus && !evidence.UiDomain) {
        evidence.SamplesRenderedCopy = std::any_of(
            draw.Textures.begin(), draw.Textures.end(),
            [](const auto& texture) { return !texture.CopyReferences.empty(); });
    }
    if (menus) {
        evidence.Untextured = draw.Textures.empty();
        evidence.GuestScissor = draw.ScissorMode != 0U;
    }
    const bool relocatedFill =
        menus && Oot3d::GlWidescreenRelocatedFillCandidate(evidence);
    const bool needsCoverage =
        uniformsAvailable &&
        ((evidence.UiDomain && evidence.ViewportCoversTarget) ||
         (menus && Oot3d::GlWidescreenOrthographicPanelCandidate(evidence)) ||
         relocatedFill);
    // Diagnostic only: projected bounds of the relocated lower menu's draws.
    const bool relocatedBounds = menus && uniformsAvailable &&
                                 !Oot3d::GlWidescreenMenusLogPath().empty() &&
                                 widescreenFactor > 1.0F &&
                                 !evidence.ViewportSpansPresentedWidth &&
                                 !evidence.SamplesRenderedCopy;
    uint32_t booleanUniforms = 0U;
    if (needsCoverage || relocatedBounds) {
        std::memcpy(&booleanUniforms,
                    draw.VertexUniformBytes.data() +
                        Oot3d::kPicaPackedVertexBooleanMaskOffset,
                    sizeof(booleanUniforms));
    }
    if (relocatedBounds) {
        constexpr uint32_t kMaximumElementVertices = 4096U;
        const auto element = Renderer3ds::PicaDrawGeometryNdcBounds(
            draw, booleanUniforms,
            draw.VertexUniformBytes.subspan(Oot3d::kPicaPackedVertexFloatOffset),
            kMaximumElementVertices);
        if (element.has_value()) {
            evidence.BoundsKnown = true;
            evidence.NdcMin = element->Minimum;
            evidence.NdcMax = element->Maximum;
        }
    }
    if (needsCoverage) {
        const auto bounds = Renderer3ds::PicaDrawGeometryNdcBounds(
            draw, booleanUniforms,
            draw.VertexUniformBytes.subspan(Oot3d::kPicaPackedVertexFloatOffset));
        // A fill of the relocated menu covers its native viewport, whose edge
        // a widened frustum puts at 1/factor on the presented horizontal.
        const float horizontal = relocatedFill
            ? Oot3d::GlWidescreenNativeHorizontalExtent(evidence, widescreenFactor)
            : 1.0F;
        evidence.GeometryCoversViewport =
            bounds.has_value() && bounds->Reaches({1.0F, horizontal});
    }
    auto placement = Oot3d::ResolveGlWidescreenDrawPlacement(
        evidence, widescreenFactor, menus);
    placement.SplitCorners =
        placement.SplitCorners && Oot3d::GlWidescreenMenuCornersEnabled();
    if (const auto& logPath = Oot3d::GlWidescreenMenusLogPath(); !logPath.empty()) {
        LogPicaGlWidescreenPlacement(logPath, draw, evidence, placement,
                                     uniformsAvailable);
    }
    return placement;
}

// Relocated bottom-screen pieces: GL_NEAREST on magnification at an integer
// scale (GlRelocatedMenuNearestSampling). Applies with and without widescreen.
bool PicaGlRelocatedMenuNearestSampling(const GfxNativePicaDrawView& draw,
                                        float resolutionScale) {
    if (draw.Textures.empty() || !Oot3d::GlMenuNearestEnabled()) {
        return false;
    }
    Oot3d::GlWidescreenDrawEvidence evidence;
    evidence.UiDomain =
        draw.CompositionDomain == Renderer3ds::PicaCompositionDomain::Ui;
    evidence.ViewportSpansPresentedWidth =
        draw.ViewportY <= 0.5F &&
        draw.ViewportY + draw.ViewportHeight >=
            static_cast<float>(draw.FramebufferHeight) - 0.5F;
    if (evidence.ViewportSpansPresentedWidth ||
        draw.VertexUniformBytes.size() <
            Oot3d::kPicaPackedVertexFloatOffset + 4U * Oot3d::kPicaPackedVec4Bytes) {
        return false;
    }
    std::array<float, 16> rows{};
    std::memcpy(rows.data(),
                draw.VertexUniformBytes.data() + Oot3d::kPicaPackedVertexFloatOffset,
                sizeof(rows));
    const auto projection = Oot3d::DecodeGlProjection(rows);
    evidence.Projection = projection.Shape;
    evidence.PixelAnisotropy = Oot3d::GlNativePixelAnisotropy(
        projection, draw.ViewportWidth, draw.ViewportHeight, draw.FramebufferWidth);
    evidence.SamplesRenderedCopy = std::any_of(
        draw.Textures.begin(), draw.Textures.end(),
        [](const auto& texture) { return !texture.CopyReferences.empty(); });
    return Oot3d::GlRelocatedMenuNearestSampling(evidence, resolutionScale);
}

// Dynamic resolution (gl_pica_dynamic_scale.h): the scale a target should
// be drawn at now. Its allocation scale unless it is the eligible top-screen
// target and a lower scale is requested.
float PicaGlDynamicTargetScale(const PicaGlTarget& target) {
    const auto& dynamic = PicaGlDynamicScale::gState;
    if (!target.DynamicEligible || target.DynamicExempt ||
        !dynamic.Supported || !(dynamic.Requested > 0.0F)) {
        return target.AllocScale;
    }
    const float quarter = std::round(dynamic.Requested * 4.0F) / 4.0F;
    return std::clamp(quarter, 1.0F, target.AllocScale);
}

// Moves the drawn extent of a target to `scale` (at most its allocation):
// the bottom-left sub-rectangle of the same images, nothing reallocated.
// Called only where the target's content starts again (a colour clear), so
// no frame mixes two scales.
void SetPicaGlTargetScale(PicaGlTarget& target, const PicaGlTargetKey& key,
                          float scale) {
    scale = std::clamp(scale, 1.0F, target.AllocScale);
    if (scale == target.Scale) {
        return;
    }
    if (scale == target.AllocScale) {
        target.Width = target.AllocWidth;
        target.Height = target.AllocHeight;
        target.ScaleY = target.AllocScaleY;
    } else {
        const auto extent = PicaGlScale::DynamicSubRectangle(
            key.Width, key.Height, scale, target.AllocWidth,
            target.AllocHeight, target.Wide, target.WideFactor);
        target.Width = extent.Width;
        target.Height = extent.Height;
        target.ScaleY = static_cast<float>(target.Height) /
                        static_cast<float>(key.Height);
    }
    target.Scale = scale;
    PicaGlDynamicScale::gState.Applied.fetch_add(1U,
                                                 std::memory_order_relaxed);
}

void RefreshPicaGlTargetScale(PicaGlTarget& target,
                              const PicaGlTargetKey& key) {
    if (target.DynamicEligible) {
        PicaGlDynamicScale::TargetPhase phase{target.Scale, target.ScaleSerial};
        const float scale = PicaGlDynamicScale::ScaleAtColourClear(
            phase, PicaGlDynamicScale::gState.FrameSerial,
            PicaGlDynamicTargetScale(target));
        target.ScaleSerial = phase.Serial;
        SetPicaGlTargetScale(target, key, scale);
    }
}

// tour19: the frame's first draw into an eligible target applies the scale
// its colour clear never did (OoT3D's top scene is not cleared through a
// covering colour fill). The colour is cleared here (a logical clear): what
// the frame leaves undrawn is black instead of pixels at the other scale.
// True when the extent changed (the caller forgets the cached clear state).
bool ApplyPicaGlTargetScaleAtDraw(PicaGlTarget& target,
                                  const PicaGlTargetKey& key) {
    if (!target.DynamicEligible || target.DynamicExempt) {
        return false;
    }
    PicaGlDynamicScale::TargetPhase phase{target.Scale, target.ScaleSerial};
    const float desired = PicaGlDynamicTargetScale(target);
    const bool moved = PicaGlDynamicScale::ScaleAtDraw(
        phase, PicaGlDynamicScale::gState.FrameSerial, desired);
    target.ScaleSerial = phase.Serial;
    if (!moved) {
        return false;
    }
    SetPicaGlTargetScale(target, key, desired);
    glBindFramebuffer(GL_FRAMEBUFFER, target.Framebuffer);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    ++target.Version;
    PicaGlDynamicScale::gState.AppliedAtDraw.fetch_add(
        1U, std::memory_order_relaxed);
    return true;
}

void MarkPicaGlTargetDynamicExempt(PicaGlTarget& target) {
    if (target.DynamicEligible && !target.DynamicExempt) {
        PicaGlDynamicScale::gState.ExemptTargets.fetch_add(
            1U, std::memory_order_relaxed);
    }
    target.DynamicExempt = true;
}

PicaGlTarget& GetPicaGlTarget(
    GfxRenderingAPIOGL::NativePicaState& state,
    const GfxNativePicaDrawView& draw) {
    const PicaGlTargetKey key{
        draw.RenderTargetNamespace,
        draw.FramebufferColorPhysicalAddress,
        draw.FramebufferDepthPhysicalAddress,
        draw.FramebufferWidth,
        draw.FramebufferHeight,
        draw.FramebufferColorFormat,
        draw.FramebufferDepthFormat,
    };
    auto [found, inserted] = state.Targets.try_emplace(key);
    if (!inserted) {
        return found->second;
    }
    auto& target = found->second;
    target.Id = state.NextTargetId++;
    const auto namespaceScale = state.NamespaceScales.find(key.Namespace);
    target.Scale = namespaceScale != state.NamespaceScales.end()
        ? namespaceScale->second
        : state.ResolutionScale;
    target.Width = PicaGlScale::ScaledExtent(key.Width, target.Scale);
    target.Height = PicaGlScale::ScaledExtent(key.Height, target.Scale);
    target.ScaleY = target.Scale;
    // Widescreen: only the top LCD target (480x400 stored rotated; its
    // 400-pixel axis is the presented width) grows, by the window's aspect
    // over 5:3, matching the frustum widening of the projection hook.
    if (key.Width == Oot3d::kNativeTopScreenPhysicalWidth &&
        key.Height == Oot3d::kNativeTopScreenPhysicalHeight) {
        const float factor = ResolvePicaGlWidescreenFactor();
        if (factor > 1.0F) {
            target.Height = static_cast<uint32_t>(std::lround(
                static_cast<float>(target.Height) * factor));
            target.ScaleY = static_cast<float>(target.Height) /
                            static_cast<float>(key.Height);
            target.Wide = true;
            target.WideFactor = factor;
        }
    }
    target.AllocWidth = target.Width;
    target.AllocHeight = target.Height;
    target.AllocScale = target.Scale;
    target.AllocScaleY = target.ScaleY;
    target.DynamicEligible =
        namespaceScale == state.NamespaceScales.end() &&
        key.Width == Oot3d::kNativeTopScreenPhysicalWidth &&
        key.Height == Oot3d::kNativeTopScreenPhysicalHeight;
    try {
        glGenTextures(1, &target.Color);
        glBindTexture(GL_TEXTURE_2D, target.Color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                     static_cast<GLsizei>(target.AllocWidth),
                     static_cast<GLsizei>(target.AllocHeight), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        glGenRenderbuffers(1, &target.DepthStencil);
        glBindRenderbuffer(GL_RENDERBUFFER, target.DepthStencil);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                              static_cast<GLsizei>(target.AllocWidth),
                              static_cast<GLsizei>(target.AllocHeight));

        glGenFramebuffers(1, &target.Framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, target.Framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, target.Color, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, target.DepthStencil);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
            GL_FRAMEBUFFER_COMPLETE) {
            throw std::runtime_error("PICA OpenGL render target is incomplete");
        }

        glGenTextures(1, &target.Shadow);
        glBindTexture(GL_TEXTURE_2D, target.Shadow);
        // The shadow producer addresses this image with gl_FragCoord and the
        // consumer filters it through textureSize(), so both follow the scale.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI,
                     static_cast<GLsizei>(target.AllocWidth),
                     static_cast<GLsizei>(target.AllocHeight), 0,
                     GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &target.ShadowFramebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, target.ShadowFramebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, target.Shadow, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
            GL_FRAMEBUFFER_COMPLETE) {
            throw std::runtime_error("PICA OpenGL shadow target is incomplete");
        }
        ClearPicaShadow(target, 0xFFFFFFFFU, state.Lean);

        glBindFramebuffer(GL_FRAMEBUFFER, target.Framebuffer);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glStencilMask(0xFFU);
        glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        glClearDepth(1.0);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                GL_STENCIL_BUFFER_BIT);
        ++target.Version;
    } catch (...) {
        DeletePicaTarget(target);
        state.Targets.erase(found);
        throw;
    }
    return target;
}

PicaGlProgram& GetPicaGlProgram(
    GfxRenderingAPIOGL::NativePicaState& state,
    const GfxNativePicaDrawView& draw) {
    auto vertexIdentity = draw.VertexShaderSourceIdentity;
    auto fragmentIdentity = draw.FragmentShaderSourceIdentity;
    if (!vertexIdentity.Available()) {
        vertexIdentity =
            Renderer3ds::IdentifyPicaShaderSource(draw.VertexShaderSource);
    }
    if (!fragmentIdentity.Available()) {
        fragmentIdentity =
            Renderer3ds::IdentifyPicaShaderSource(draw.FragmentShaderSource);
    }
    const PicaGlShaderKey key{vertexIdentity, fragmentIdentity};
    auto found = state.Programs.find(key);
    if (found != state.Programs.end()) {
        return found->second;
    }

    // The frontend publishes stable source identities with every draw. Hash
    // the complete GLSL text only while creating a new program; repeating
    // that work on every cache hit made shader lookup scale with source size
    // instead of draw count. The cache-miss check retains the rejecting
    // identity contract before any program is compiled.
    if (draw.VertexShaderSourceIdentity.Available() &&
        Renderer3ds::IdentifyPicaShaderSource(draw.VertexShaderSource) !=
            vertexIdentity) {
        throw std::runtime_error("PICA vertex source identity mismatch");
    }
    if (draw.FragmentShaderSourceIdentity.Available() &&
        Renderer3ds::IdentifyPicaShaderSource(draw.FragmentShaderSource) !=
            fragmentIdentity) {
        throw std::runtime_error("PICA fragment source identity mismatch");
    }
    return state.Programs.emplace(key, BuildPicaGlProgram(draw)).first->second;
}

// *rebind is set when a deferred clear changed the framebuffer binding.
// The image of the consumer's own namespace wins: another namespace replays
// a different guest frame (the native replay runs ahead of the delayed
// visual sample) or is not replayed at all (SkipsPicaNativeNamespace).
GLuint FindLivePicaShadow(
    GfxRenderingAPIOGL::NativePicaState& state,
    const GfxNativePicaTextureView& texture, uint64_t renderTargetNamespace,
    bool* rebind) {
    PicaGlTarget* found = nullptr;
    for (auto& [key, target] : state.Targets) {
        if (key.ColorAddress == texture.PhysicalAddress &&
            key.Width == texture.Width && key.Height == texture.Height) {
            if (found == nullptr || key.Namespace == renderTargetNamespace) {
                found = &target;
            }
            if (key.Namespace == renderTargetNamespace) {
                break;
            }
        }
    }
    if (found == nullptr) {
        return 0U;
    }
    // Sampled as Shadow2D (textureSize() of the allocation): no dynamic
    // scale for it from its next clear on.
    MarkPicaGlTargetDynamicExempt(*found);
    // A consumer samples it: a deferred clear must land first.
    if (ResolvePicaShadow(*found)) {
        *rebind = true;
    }
    return found->Shadow;
}

PicaGlTexture& GetPicaGlTexture(
    GfxRenderingAPIOGL::NativePicaState& state,
    const GfxNativePicaTextureView& texture) {
    const bool perfOn = GlPerf::Enabled();
    if (perfOn) {
        GlPerf::PhaseLap(GlPerf::Phase::TexLookup);
    }
    uint64_t contentHash = texture.NativeContentHash;
    if (!texture.NativeContentHashAvailable) {
        contentHash = HashBytes(texture.NativeBytes);
        if (perfOn) {
            GlPerf::PhaseCount(GlPerf::DrawCount::TexHashes);
            GlPerf::PhaseCount(GlPerf::DrawCount::TexHashBytes,
                               texture.NativeBytes.size());
            GlPerf::PhaseLap(GlPerf::Phase::TexHash);
        }
    }
    const PicaGlTextureKey key{
        contentHash, texture.PhysicalAddress, texture.Width, texture.Height,
        texture.NativeFormat, texture.NativeType, texture.NativeWrapS,
        texture.NativeWrapT, texture.MinLinear, texture.MagLinear,
        texture.MipLinear, texture.LodBiasRaw, texture.MinMipLevel,
        texture.MaxMipLevel,
    };
    auto [found, inserted] = state.Textures.try_emplace(key);
    if (perfOn) {
        GlPerf::PhaseCount(GlPerf::DrawCount::TexLookups);
        GlPerf::PhaseLap(GlPerf::Phase::TexLookup);
    }
    if (!inserted) {
        found->second.LastUsedFrame = state.Frame;
        return found->second;
    }
    auto& result = found->second;
    result.LastUsedFrame = state.Frame;
    try {
        (void)TakePicaGlOutOfMemory();
        glGenTextures(1, &result.Handle);
        glBindTexture(GL_TEXTURE_2D, result.Handle);
        if (texture.NativeType == 2U) {
            std::vector<uint32_t> pixels;
            std::string decodeError;
            if (!Oot3d::DetilePicaShadow2d(
                    texture.NativeBytes, texture.Width, texture.Height,
                    pixels, &decodeError)) {
                throw std::runtime_error(
                    "PICA Shadow2D decode failed: " + decodeError);
            }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, texture.Width,
                         texture.Height, 0, GL_RED_INTEGER, GL_UNSIGNED_INT,
                         pixels.data());
            result.Format = GL_R32UI;
            result.Bytes = static_cast<uint64_t>(texture.Width) *
                           texture.Height * 4U;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        } else {
            const uint32_t mipLevels = PicaMipCount(texture);
            if (mipLevels == 0U) {
                throw std::runtime_error("PICA texture has no mip levels");
            }
            // Decoded ahead by the PICA plan worker when it got here first
            // (oot3d_native_pica_deferred_plan.cpp); same mips either way.
            const auto predecoded = Oot3d::SharedPicaPredecodeCache().Take(
                {contentHash, texture.Width, texture.Height,
                 texture.NativeFormat, static_cast<uint8_t>(mipLevels)});
            // ETC1 straight to the GPU (pica_etc1_transcode.h): every level
            // or none, so a refused level falls back to RGBA8 below.
            bool compressed = false;
            if ((texture.NativeFormat == 12U || texture.NativeFormat == 13U) &&
                Oot3d::PicaEtc2UploadEnabled()) {
                std::vector<std::vector<uint8_t>> blocks(mipLevels);
                size_t offset = 0U;
                compressed = true;
                for (uint32_t level = 0U; level < mipLevels && compressed;
                     ++level) {
                    const uint32_t width = std::max(
                        1U, static_cast<uint32_t>(texture.Width) >> level);
                    const uint32_t height = std::max(
                        1U, static_cast<uint32_t>(texture.Height) >> level);
                    const auto encoded = PicaEncodedMipSize(
                        texture.NativeFormat, width, height);
                    compressed =
                        encoded.has_value() &&
                        offset + *encoded <= texture.NativeBytes.size() &&
                        Oot3d::TranscodePicaEtc1ToEtc2(
                            texture.NativeBytes.subspan(offset, *encoded),
                            width, height, texture.NativeFormat == 13U,
                            blocks[level]);
                    offset += encoded.value_or(0U);
                }
                for (uint32_t level = 0U; level < mipLevels && compressed;
                     ++level) {
                    glCompressedTexImage2D(
                        GL_TEXTURE_2D, static_cast<GLint>(level),
                        kGlCompressedRgb8Etc2,
                        static_cast<GLsizei>(std::max(
                            1U, static_cast<uint32_t>(texture.Width) >> level)),
                        static_cast<GLsizei>(std::max(
                            1U, static_cast<uint32_t>(texture.Height) >> level)),
                        0, static_cast<GLsizei>(blocks[level].size()),
                        blocks[level].data());
                    result.Bytes += blocks[level].size();
                }
            }
            size_t nativeOffset = 0U;
            for (uint32_t level = 0U; level < mipLevels && !compressed;
                 ++level) {
                const uint32_t width =
                    std::max(1U, static_cast<uint32_t>(texture.Width) >> level);
                const uint32_t height =
                    std::max(1U, static_cast<uint32_t>(texture.Height) >> level);
                const auto encoded = PicaEncodedMipSize(
                    texture.NativeFormat, width, height);
                if (!encoded.has_value() ||
                    nativeOffset + *encoded > texture.NativeBytes.size()) {
                    throw std::runtime_error("PICA texture mip chain is truncated");
                }
                if (predecoded != nullptr) {
                    glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level),
                                 GL_RGBA8, static_cast<GLsizei>(width),
                                 static_cast<GLsizei>(height), 0, GL_RGBA,
                                 GL_UNSIGNED_BYTE,
                                 predecoded->Mips[level].data());
                    result.Bytes += static_cast<uint64_t>(width) * height * 4U;
                    nativeOffset += *encoded;
                    continue;
                }
                std::vector<uint8_t> rgba8;
                std::string decodeError;
                if (!::Oot3d::Renderer::DecodePicaTextureRgba8(
                        texture.NativeFormat, static_cast<uint16_t>(width),
                        static_cast<uint16_t>(height),
                        texture.NativeBytes.subspan(nativeOffset, *encoded),
                        rgba8, &decodeError)) {
                    throw std::runtime_error(
                        "PICA texture decode failed: " + decodeError);
                }
                glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level),
                             GL_RGBA8, static_cast<GLsizei>(width),
                             static_cast<GLsizei>(height), 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, rgba8.data());
                result.Bytes += static_cast<uint64_t>(width) * height * 4U;
                nativeOffset += *encoded;
            }
            result.Format = compressed ? kGlCompressedRgb8Etc2 : GL_RGBA8;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            PicaGlMinFilter(texture, mipLevels));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                            texture.MagLinear ? GL_LINEAR : GL_NEAREST);
            // Optional anisotropic filtering, set once per texture object:
            // only for a linear minification filter over a real mip chain,
            // so NEAREST and unmipmapped sampling keep the PICA result.
            if (state.Post.Config.Anisotropy > 1U && texture.MinLinear &&
                mipLevels > 1U &&
                std::min<uint32_t>(texture.MaxMipLevel, mipLevels - 1U) >
                    texture.MinMipLevel) {
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY,
                                static_cast<GLfloat>(
                                    state.Post.Config.Anisotropy));
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL,
                            texture.MinMipLevel);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                            std::min<uint32_t>(texture.MaxMipLevel,
                                               mipLevels - 1U));
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS,
                            static_cast<float>(texture.LodBiasRaw) / 256.0F);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                        ToPicaGlWrap(texture.NativeWrapS));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                        ToPicaGlWrap(texture.NativeWrapT));
        if (TakePicaGlOutOfMemory()) {
            throw PicaGlOutOfMemory("PICA texture allocation ran out of memory");
        }
    } catch (...) {
        glDeleteTextures(1, &result.Handle);
        state.Textures.erase(found);
        throw;
    }
    state.TextureBytes += result.Bytes;
    state.Counters.TexturesCreated.fetch_add(1U, std::memory_order_relaxed);
    if (perfOn) {
        GlPerf::PhaseCount(GlPerf::DrawCount::TexUploads);
        GlPerf::PhaseCount(GlPerf::DrawCount::TexUploadBytes, result.Bytes);
        GlPerf::PhaseLap(GlPerf::Phase::TexUpload);
    }
    return result;
}

GLuint GetPicaLightingLut(
    GfxRenderingAPIOGL::NativePicaState& state,
    const Renderer3ds::PicaLightingLutView& lut) {
    if (!lut.Valid()) {
        throw std::runtime_error("PICA lighting LUT is invalid");
    }
    auto [found, inserted] = state.LightingLuts.try_emplace(lut.ContentHash);
    if (!inserted) {
        found->second.LastUsedFrame = state.Frame;
        return found->second.Handle;
    }
    auto& texture = found->second;
    texture.LastUsedFrame = state.Frame;
    (void)TakePicaGlOutOfMemory();
    glGenTextures(1, &texture.Handle);
    glBindTexture(GL_TEXTURE_2D, texture.Handle);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, 256, 24, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_INT, lut.PackedEntries.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    texture.Format = GL_R32UI;
    if (TakePicaGlOutOfMemory()) {
        glDeleteTextures(1, &texture.Handle);
        state.LightingLuts.erase(found);
        throw PicaGlOutOfMemory("PICA lighting LUT allocation ran out of memory");
    }
    texture.Bytes = 256U * 24U * 4U;
    state.LutBytes += texture.Bytes;
    return texture.Handle;
}

void ApplyPicaFill(const GfxNativePicaMemoryFillView& fill,
                   const PicaGlTargetKey& key,
                   PicaGlTarget& target, bool lean, bool gpuLean) {
    const uint64_t pixels = static_cast<uint64_t>(key.Width) * key.Height;
    const uint64_t colorEnd = static_cast<uint64_t>(key.ColorAddress) +
                              pixels * PicaColorBytesPerPixel(key.ColorFormat);
    const uint64_t depthEnd = static_cast<uint64_t>(key.DepthAddress) +
                              pixels * PicaDepthBytesPerPixel(key.DepthFormat);
    const bool color = fill.StartPhysicalAddress <= key.ColorAddress &&
                       fill.EndPhysicalAddress >= colorEnd;
    const bool depth = key.DepthAddress != 0U &&
                       fill.StartPhysicalAddress <= key.DepthAddress &&
                       fill.EndPhysicalAddress >= depthEnd;
    if (!color && !depth) {
        return;
    }
    GlPerf::Scope perfStage(GlPerf::Stage::Clear);
    if (color) {
        const uint32_t shadow =
            static_cast<uint32_t>(PicaFillByte(fill, 0U)) |
            (static_cast<uint32_t>(PicaFillByte(fill, 1U)) << 8U) |
            (static_cast<uint32_t>(PicaFillByte(fill, 2U)) << 16U) |
            (static_cast<uint32_t>(PicaFillByte(fill, 3U)) << 24U);
        ClearPicaShadow(target, shadow, lean);
        // The content starts again: the dynamic scale may move here.
        RefreshPicaGlTargetScale(target, key);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target.Framebuffer);
    glDisable(GL_SCISSOR_TEST);
    GLbitfield mask = 0U;
    if (color) {
        const auto decoded = DecodePicaClearColor(fill, key.ColorFormat);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(decoded[0], decoded[1], decoded[2], decoded[3]);
        mask |= GL_COLOR_BUFFER_BIT;
        ++target.Version;
    }
    if (depth) {
        const auto [depthValue, stencil] =
            DecodePicaClearDepthStencil(fill, key.DepthFormat);
        glDepthMask(GL_TRUE);
        glStencilMask(0xFFU);
        glClearDepth(depthValue);
        glClearStencil(stencil);
        mask |= GL_DEPTH_BUFFER_BIT;
        // GPU lean: without a PICA stencil plane (D16/D24) nothing reads or
        // writes the renderbuffer's stencil, so clearing it too (with the
        // value the fill decodes: 0) makes this a full depth/stencil clear.
        if (PicaGlGpuLean::DepthFillClearsStencil(key.DepthFormat, gpuLean)) {
            mask |= GL_STENCIL_BUFFER_BIT;
            if (key.DepthFormat != 3U) {
                GlPerf::CountPica(PicaGlGpuLean::kCountFullDepthClear);
            }
        }
    }
    glClear(mask);
}

void ApplyPendingPicaFills(GfxRenderingAPIOGL::NativePicaState& state,
                           const PicaGlTargetKey& key,
                           PicaGlTarget& target) {
    auto fill = state.PendingFills.begin();
    while (fill != state.PendingFills.end()) {
        if (fill->RenderTargetNamespace != key.Namespace) {
            ++fill;
            continue;
        }
        const uint64_t pixels = static_cast<uint64_t>(key.Width) * key.Height;
        const uint64_t colorEnd = static_cast<uint64_t>(key.ColorAddress) +
                                  pixels * PicaColorBytesPerPixel(key.ColorFormat);
        const uint64_t depthEnd = static_cast<uint64_t>(key.DepthAddress) +
                                  pixels * PicaDepthBytesPerPixel(key.DepthFormat);
        const bool covers =
            (fill->StartPhysicalAddress <= key.ColorAddress &&
             fill->EndPhysicalAddress >= colorEnd) ||
            (key.DepthAddress != 0U &&
             fill->StartPhysicalAddress <= key.DepthAddress &&
             fill->EndPhysicalAddress >= depthEnd);
        if (!covers) {
            ++fill;
            continue;
        }
        ApplyPicaFill(*fill, key, target, state.Lean, state.GpuLean);
        fill = state.PendingFills.erase(fill);
    }
}

PicaGlDisplay& GetPicaGlDisplay(
    GfxRenderingAPIOGL::NativePicaState& state,
    uint64_t targetNamespace, uint32_t outputAddress,
    uint32_t width, uint32_t height,
    uint32_t allocWidth, uint32_t allocHeight) {
    const auto key = std::make_pair(targetNamespace, outputAddress);
    auto [found, inserted] = state.Displays.try_emplace(key);
    auto& display = found->second;
    if (!inserted && display.Width == width && display.Height == height &&
        display.AllocWidth == allocWidth &&
        display.AllocHeight == allocHeight) {
        return display;
    }
    if (!inserted) {
        DeletePicaDisplay(display);
    }
    glGenTextures(1, &display.Color);
    glBindTexture(GL_TEXTURE_2D, display.Color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 static_cast<GLsizei>(allocWidth),
                 static_cast<GLsizei>(allocHeight), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &display.Framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, display.Framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, display.Color, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
        GL_FRAMEBUFFER_COMPLETE) {
        DeletePicaDisplay(display);
        throw std::runtime_error("PICA OpenGL display image is incomplete");
    }
    display.Width = width;
    display.Height = height;
    display.AllocWidth = allocWidth;
    display.AllocHeight = allocHeight;
    return display;
}

// Releases textures and LUTs the policy no longer keeps. `all`: every one
// idle for a frame or more, after an allocation failed.
void TrimPicaGlResidency(GfxRenderingAPIOGL::NativePicaState& state,
                         bool all) {
    auto policy = state.TexturePolicy;
    if (all) {
        policy.BudgetBytes = 0U;
        policy.MinIdleFrames = 1U;
    }
    PicaGlResidency::CollectEvictions(state.Textures, state.Frame,
                                      state.TextureBytes, policy,
                                      state.TextureEvictions);
    for (const auto& key : state.TextureEvictions) {
        const auto found = state.Textures.find(key);
        if (found == state.Textures.end()) continue;
        glDeleteTextures(1, &found->second.Handle);
        state.TextureBytes -= std::min(state.TextureBytes, found->second.Bytes);
        state.Textures.erase(found);
    }
    state.Counters.TexturesEvicted.fetch_add(state.TextureEvictions.size(),
                                             std::memory_order_relaxed);
    // LUTs are small; only the idle limit (or a failure) releases them.
    auto lutPolicy = policy;
    lutPolicy.BudgetBytes = all ? 0U : std::numeric_limits<uint64_t>::max();
    PicaGlResidency::CollectEvictions(state.LightingLuts, state.Frame,
                                      state.LutBytes, lutPolicy,
                                      state.LutEvictions);
    for (const uint64_t key : state.LutEvictions) {
        const auto found = state.LightingLuts.find(key);
        if (found == state.LightingLuts.end()) continue;
        glDeleteTextures(1, &found->second.Handle);
        state.LutBytes -= std::min(state.LutBytes, found->second.Bytes);
        state.LightingLuts.erase(found);
    }
}

void PublishPicaGlResidency(GfxRenderingAPIOGL::NativePicaState& state) {
    auto& counters = state.Counters;
    constexpr auto relaxed = std::memory_order_relaxed;
    counters.TexturesLive.store(state.Textures.size(), relaxed);
    counters.TextureBytes.store(state.TextureBytes, relaxed);
    counters.LutsLive.store(state.LightingLuts.size(), relaxed);
    counters.LutBytes.store(state.LutBytes, relaxed);
    counters.ProgramsLive.store(state.Programs.size(), relaxed);
    counters.TargetsLive.store(state.Targets.size(), relaxed);
    counters.DisplaysLive.store(state.Displays.size(), relaxed);
    counters.GeometryBuffersLive.store(state.GeometryBuffers.size(), relaxed);
    counters.GeometryBufferBytes.store(state.GeometryBufferBytes, relaxed);
}

// The first draw of a frame: advance the residency clock, trim (every 30
// frames, or whenever the textures exceed the budget) and publish.
void BeginPicaGlFrame(GfxRenderingAPIOGL::NativePicaState& state,
                      uint64_t frame) {
    if (state.Frame == frame) return;
    state.Frame = frame;
#if defined(__SWITCH__)
    // An allocation of the last frame failed (GL thread: seen at the swap).
    if (TakeSwitchGlOutOfMemory()) {
        TrimPicaGlResidency(state, true);
    }
#endif
    if (frame % 30U == 0U ||
        state.TextureBytes > state.TexturePolicy.BudgetBytes) {
        TrimPicaGlResidency(state, false);
    }
    PublishPicaGlResidency(state);
}

// Reserves `total` bytes (sum of StreamRing::Span) of a stream ring for one
// draw, orphaning the storage when a new generation starts. False when the
// ring cannot hold them: the caller uses its own buffers for this draw.
bool ReservePicaGlStream(GfxRenderingAPIOGL::NativePicaState& state,
                         PicaGlStreamBuffer& stream, uint64_t total) {
    if (!stream.Ring.Fits(total)) return false;
    if (!stream.Ring.Reserve(total)) return true;
    if (stream.Persistent) {
        // Next slot, once the GPU is done with its previous generation.
        stream.Fences[stream.Slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        stream.Slot = (stream.Slot + 1U) % stream.Slots.size();
        if (GLsync& fence = stream.Fences[stream.Slot]; fence != nullptr) {
            while (glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ULL) ==
                   GL_TIMEOUT_EXPIRED) {
            }
            glDeleteSync(fence);
            fence = nullptr;
        }
        stream.Handle = stream.Slots[stream.Slot];
        state.Counters.RingOrphans.fetch_add(1U, std::memory_order_relaxed);
    } else {
        // Fresh storage; the GPU keeps reading the previous one until done.
        glBindBuffer(stream.Target, stream.Handle);
        glBufferData(stream.Target,
                     static_cast<GLsizeiptr>(stream.Ring.Capacity()), nullptr,
                     GL_STREAM_DRAW);
        state.Counters.RingOrphans.fetch_add(1U, std::memory_order_relaxed);
    }
    return true;
}

// Copies `bytes` into the next reserved range of a stream ring and returns
// its offset. Throws PicaGlOutOfMemory when the driver cannot map it.
uint64_t WritePicaGlStream(PicaGlStreamBuffer& stream,
                           std::span<const uint8_t> bytes, uint64_t alignment,
                           uint64_t reserved = 0U) {
    // `reserved` > bytes.size(): the range keeps that many bytes (bound as a
    // whole) while only `bytes` are written (uniform trim).
    const auto allocation = stream.Ring.Allocate(
        std::max<uint64_t>(reserved, bytes.size()), alignment);
    if (!allocation.Valid) {
        throw std::runtime_error("PICA stream range was not reserved");
    }
    if (bytes.empty()) return allocation.Offset; // nothing to copy (trim)
    // The bind also sets the VAO's element binding for the index ring.
    glBindBuffer(stream.Target, stream.Handle);
    if (stream.Persistent) {
        std::memcpy(stream.Mapped[stream.Slot] + allocation.Offset, bytes.data(), bytes.size());
        return allocation.Offset;
    }
    void* mapped = glMapBufferRange(
        stream.Target, static_cast<GLintptr>(allocation.Offset),
        static_cast<GLsizeiptr>(bytes.size()),
        GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT |
            GL_MAP_INVALIDATE_RANGE_BIT);
    if (mapped == nullptr) {
        stream.Ring.Reset();
        (void)TakePicaGlOutOfMemory();
        throw PicaGlOutOfMemory("PICA stream buffer could not be mapped");
    }
    std::memcpy(mapped, bytes.data(), bytes.size());
    if (glUnmapBuffer(stream.Target) == GL_FALSE) {
        stream.Ring.Reset();
        throw PicaGlOutOfMemory("PICA stream buffer contents were lost");
    }
    return allocation.Offset;
}

// A whole buffer object replaced by `bytes` without letting the driver copy
// from a failed allocation (glBufferData with data would memcpy into NULL).
void UploadPicaGlBuffer(GLenum target, GLuint buffer,
                        std::span<const uint8_t> bytes, GLenum usage) {
    glBindBuffer(target, buffer);
    glBufferData(target, static_cast<GLsizeiptr>(bytes.size()), nullptr, usage);
    if (bytes.empty()) return;
    void* mapped = glMapBufferRange(
        target, 0, static_cast<GLsizeiptr>(bytes.size()),
        GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    if (mapped == nullptr) {
        (void)TakePicaGlOutOfMemory();
        throw PicaGlOutOfMemory("PICA buffer could not be mapped");
    }
    std::memcpy(mapped, bytes.data(), bytes.size());
    if (glUnmapBuffer(target) == GL_FALSE) {
        throw PicaGlOutOfMemory("PICA buffer contents were lost");
    }
}

} // namespace
#endif

#if !defined(USE_OPENGLES) && !defined(__APPLE__)
namespace {
// Render-thread totals made readable by the thread that writes perf.txt
// (relaxed stores, once per presented frame).
void PublishPicaGlStats(GfxRenderingAPIOGL::NativePicaState& state) {
    ++state.Presents;
    auto& out = state.Publish;
    const auto& count = state.Gl.Count;
    out.Draws.store(state.Draws, std::memory_order_relaxed);
    out.DrawNanoseconds.store(state.DrawNanoseconds, std::memory_order_relaxed);
    out.Presents.store(state.Presents, std::memory_order_relaxed);
    out.Issued.store(count.Issued, std::memory_order_relaxed);
    out.Skipped.store(count.Skipped, std::memory_order_relaxed);
    out.ProgramBinds.store(count.ProgramBinds, std::memory_order_relaxed);
    out.TextureBinds.store(count.TextureBinds, std::memory_order_relaxed);
    out.VertexArrayBinds.store(count.VertexArrayBinds,
                               std::memory_order_relaxed);
    out.FramebufferBinds.store(count.FramebufferBinds,
                               std::memory_order_relaxed);
    out.UniformUploads.store(state.UniformUploads, std::memory_order_relaxed);
}
} // namespace
#endif

GfxNativePicaBackendStats
GfxRenderingAPIOGL::GetNativePicaBackendStats() const noexcept {
    GfxNativePicaBackendStats result;
#if !defined(USE_OPENGLES) && !defined(__APPLE__)
    if (mNativePica != nullptr) {
        result.Available = true;
        const auto& published = mNativePica->Publish;
        result.GlStateCacheEnabled = mNativePica->Gl.Enabled;
        result.GlStatsEnabled = mNativePica->Stats;
        result.PicaDraws = published.Draws.load(std::memory_order_relaxed);
        result.PicaDrawNanoseconds =
            published.DrawNanoseconds.load(std::memory_order_relaxed);
        result.PicaPresents =
            published.Presents.load(std::memory_order_relaxed);
        result.GlStateCallsIssued =
            published.Issued.load(std::memory_order_relaxed);
        result.GlStateCallsSkipped =
            published.Skipped.load(std::memory_order_relaxed);
        result.GlProgramBinds =
            published.ProgramBinds.load(std::memory_order_relaxed);
        result.GlTextureBinds =
            published.TextureBinds.load(std::memory_order_relaxed);
        result.GlVertexArrayBinds =
            published.VertexArrayBinds.load(std::memory_order_relaxed);
        result.GlFramebufferBinds =
            published.FramebufferBinds.load(std::memory_order_relaxed);
        result.PublishedUniformUploads =
            published.UniformUploads.load(std::memory_order_relaxed);
        const auto registry = mNativePica->GeometryRegistry.Stats();
        result.GeometryCacheEnabled = mNativePica->GeometryCacheEnabled;
        result.GeometryRegistryHits = registry.Hits;
        result.GeometryRegistryMisses = registry.Misses;
        result.GeometryRegistryUpdates = registry.Updates;
        result.GeometryRegistryEvictions = registry.Evictions;
        result.GeometryRegistryEntries = registry.Entries;
        result.GeometryPersistentDraws =
            mNativePica->GeometryPersistentDraws;
        result.GeometryStreamingDraws = mNativePica->GeometryStreamingDraws;
        result.GeometryPersistentUploads =
            mNativePica->GeometryPersistentUploads;
        result.GeometryPersistentUploadBytes =
            mNativePica->GeometryPersistentUploadBytes;
        result.GeometryStreamingUploads =
            mNativePica->GeometryStreamingUploads;
        result.GeometryStreamingUploadBytes =
            mNativePica->GeometryStreamingUploadBytes;
        result.UniformUploads = mNativePica->UniformUploads;
        result.UniformUploadBytes = mNativePica->UniformUploadBytes;
        const auto& counters = mNativePica->Counters;
        constexpr auto relaxed = std::memory_order_relaxed;
        result.TexturesLive = counters.TexturesLive.load(relaxed);
        result.TextureBytes = counters.TextureBytes.load(relaxed);
        result.LutsLive = counters.LutsLive.load(relaxed);
        result.LutBytes = counters.LutBytes.load(relaxed);
        result.ProgramsLive = counters.ProgramsLive.load(relaxed);
        result.TargetsLive = counters.TargetsLive.load(relaxed);
        result.DisplaysLive = counters.DisplaysLive.load(relaxed);
        result.GeometryBuffersLive = counters.GeometryBuffersLive.load(relaxed);
        result.GeometryBufferBytes = counters.GeometryBufferBytes.load(relaxed);
        result.TexturesCreated = counters.TexturesCreated.load(relaxed);
        result.TexturesEvicted = counters.TexturesEvicted.load(relaxed);
        result.StreamRingOrphans = counters.RingOrphans.load(relaxed);
        result.OutOfMemoryEvents = counters.OutOfMemoryEvents.load(relaxed);
        result.SkippedDraws = counters.SkippedDraws.load(relaxed);
        result.TextureBudgetBytes = mNativePica->TexturePolicy.BudgetBytes;
    }
#endif
    return result;
}

void GfxRenderingAPIOGL::SetNativePicaGeometryCacheEnabled(
    bool enabled) noexcept {
    mNativePicaGeometryCacheEnabled = enabled;
#if !defined(USE_OPENGLES) && !defined(__APPLE__)
    if (mNativePica != nullptr) {
        mNativePica->GeometryCacheEnabled = enabled;
    }
#else
    static_cast<void>(enabled);
#endif
}

bool GfxRenderingAPIOGL::SubmitPicaDraw(
    const GfxNativePicaDrawView& draw, std::string* error) {
#if defined(USE_OPENGLES) || defined(__APPLE__)
    SetPicaGlError(error, "native PICA requires OpenGL 4.3");
    return false;
#else
    try {
        GlPerf::PhaseBegin(); // glperf step_draw_*: CPU per draw phase
        if (!mNativePica || draw.VertexShaderSource.empty() ||
            draw.FragmentShaderSource.empty() || draw.VertexCount == 0U ||
            draw.VertexBindings.empty() || draw.VertexAttributes.empty()) {
            throw std::runtime_error("PICA OpenGL draw is incomplete");
        }
        if (draw.FragmentOperationMode != 0U &&
            draw.FragmentOperationMode != 3U) {
            throw std::runtime_error("PICA fragment operation is unsupported");
        }
        if (draw.ScissorMode == 1U) {
            throw std::runtime_error("PICA exclude scissor requires shader emulation");
        }
        if (draw.ScissorMode != 0U && draw.ScissorMode != 3U) {
            throw std::runtime_error("invalid PICA scissor mode");
        }
        if (draw.FramebufferWidth == 0U || draw.FramebufferHeight == 0U) {
            // A draw with no render target configured (MM3D's GPU initialisation list triggers one before it
            // programs the framebuffer registers) draws to nothing; the composition schedule leaves it out too
            // (oot3d_native_pica_presentation_scheduler.cpp). docs/decisiones.md D-022.
            GlPerf::PhaseEnd();
            return true;
        }
        if (mNativePica->Composition.Valid() &&
            !mNativePica->Composition.ConsumeDraw(
                Renderer3ds::BuildPicaCompositionDrawReference(draw), error)) {
            return false;
        }
        BeginPicaGlFrame(*mNativePica, mFrameCount);
        GlPerf::MarkPicaDraw(draw.FramebufferWidth, draw.FramebufferHeight);

        // State cache (gfx_opengl_pica_state_cache.h): trusted only across
        // consecutive PICA draws; helpers below that create programs,
        // targets or textures, or run fills and Shadow2D clears, forget the
        // state they touch.
        auto& gl = mNativePica->Gl;
        if (PicaGlStateMayBeStale()) {
            gl.Invalidate();
        }
        GlPerf::PhaseLap(GlPerf::Phase::Validate);
        const uint64_t drawStart =
            mNativePica->Stats ? PicaGlNowNanoseconds() : 0U;
        const size_t programsBefore = mNativePica->Programs.size();
        const size_t targetsBefore = mNativePica->Targets.size();
        const size_t fillsBefore = mNativePica->PendingFills.size();

        auto& program = GetPicaGlProgram(*mNativePica, draw);
        if (mNativePica->Programs.size() != programsBefore) {
            gl.Program.Reset(); // the sampler setup used the new program
        }
        GlPerf::PhaseLap(GlPerf::Phase::Program);
        auto& target = GetPicaGlTarget(*mNativePica, draw);
        const PicaGlTargetKey targetKey{
            draw.RenderTargetNamespace,
            draw.FramebufferColorPhysicalAddress,
            draw.FramebufferDepthPhysicalAddress,
            draw.FramebufferWidth,
            draw.FramebufferHeight,
            draw.FramebufferColorFormat,
            draw.FramebufferDepthFormat,
        };
        ApplyPendingPicaFills(*mNativePica, targetKey, target);
        if (mNativePica->Targets.size() != targetsBefore ||
            mNativePica->PendingFills.size() != fillsBefore) {
            gl.ForgetClearState();
        }
        if (draw.FragmentOperationMode == 3U) {
            // A shadow producer reads and writes its Shadow2D image.
            if (ResolvePicaShadow(target)) {
                gl.ForgetClearState();
            }
            // Its consumer filters the image through textureSize(): the
            // whole allocation from now on (dynamic resolution).
            if (target.DynamicEligible && !target.DynamicExempt) {
                MarkPicaGlTargetDynamicExempt(target);
                SetPicaGlTargetScale(target, targetKey, target.AllocScale);
            }
        }
        // Dynamic resolution: the frame's first draw applies the scale when
        // no colour clear did (tour19).
        if (ApplyPicaGlTargetScaleAtDraw(target, targetKey)) {
            gl.ForgetClearState();
        }
        // Every draw may write colour (a producer masks it, but counting it
        // keeps the version a strict upper bound on content changes).
        ++target.Version;
        GlPerf::PhaseLap(GlPerf::Phase::Target);

        if (gl.BindFramebuffer(target.Framebuffer)) {
            glBindFramebuffer(GL_FRAMEBUFFER, target.Framebuffer);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
        }
        if (gl.BindVertexArray(mNativePica->VertexArray)) {
            glBindVertexArray(mNativePica->VertexArray);
        }
        if (gl.UseProgram(program.Handle)) {
            glUseProgram(program.Handle);
        }
        GlPerf::PhaseLap(GlPerf::Phase::State);

        // Lean: a block equal to the one the buffer already holds is not
        // sent again. nvc0 pushes a constant-buffer upload through the
        // command FIFO word by word (and orphans the storage), so each
        // repeat (draws of one object share matrices, draws of one material
        // its constants) costs GPU front-end time as well as CPU.
        // Each block goes to the next range of the uniform ring (no buffer
        // reallocation per draw). Both blocks of a draw are reserved first,
        // so they share one generation of the ring's storage.
        auto& uniformRing = mNativePica->UniformRing;
        const uint64_t uniformAlignment = mNativePica->UniformAlignment;
        const bool uniformsInRing = ReservePicaGlStream(
            *mNativePica, uniformRing,
            PicaGlResidency::StreamRing::Span(draw.VertexUniformBytes.size(),
                                              uniformAlignment) +
                PicaGlResidency::StreamRing::Span(
                    draw.FragmentUniformBytes.size(), uniformAlignment));
        // Uniform trim: only the prefix the program reads is copied (and
        // compared); the range reserved and bound is still the whole block.
        const auto uploadUniforms = [&](GLuint buffer, GLuint binding,
                                        std::span<const uint8_t> bytes,
                                        size_t used,
                                        PicaGlUniformTrim::LastUpload& last) {
            if (bytes.empty()) {
                glBindBufferBase(GL_UNIFORM_BUFFER, binding, buffer);
                return;
            }
            const auto plan = PicaGlUniformTrim::PlanUpload(
                mNativePica->UniformTrim && uniformsInRing,
                mNativePica->Lean && uniformsInRing, last,
                uniformRing.Ring.Generation(), bytes, used);
            const size_t send = plan.Send;
            if (plan.Reuse) {
                glBindBufferRange(GL_UNIFORM_BUFFER, binding, uniformRing.Handle,
                                  static_cast<GLintptr>(last.Offset),
                                  static_cast<GLsizeiptr>(bytes.size()));
                GlPerf::PhaseCount(GlPerf::DrawCount::UniformSkipped);
                GlPerf::PhaseCount(GlPerf::DrawCount::UniformSkippedBytes,
                                   send);
                return;
            }
            ++mNativePica->UniformUploads;
            mNativePica->UniformUploadBytes += send;
            GlPerf::PhaseCount(GlPerf::DrawCount::UniformSent);
            GlPerf::PhaseCount(GlPerf::DrawCount::UniformSentBytes, send);
            last.Forget();
            if (uniformsInRing) {
                const uint64_t offset = WritePicaGlStream(
                    uniformRing, bytes.first(send), uniformAlignment,
                    bytes.size());
                glBindBufferRange(GL_UNIFORM_BUFFER, binding, uniformRing.Handle,
                                  static_cast<GLintptr>(offset),
                                  static_cast<GLsizeiptr>(bytes.size()));
                if (mNativePica->Lean) {
                    PicaGlUniformTrim::Remember(last, bytes, send,
                                                uniformRing.Ring.Generation(),
                                                offset);
                }
                return;
            }
            UploadPicaGlBuffer(GL_UNIFORM_BUFFER, buffer, bytes,
                               GL_STREAM_DRAW);
            glBindBufferBase(GL_UNIFORM_BUFFER, binding, buffer);
        };
        uploadUniforms(mNativePica->VertexUniformBuffer, 0U,
                       draw.VertexUniformBytes, program.VertexUniformUsed,
                       mNativePica->LastVertexUniforms);
        uploadUniforms(mNativePica->FragmentUniformBuffer, 4U,
                       draw.FragmentUniformBytes, program.FragmentUniformUsed,
                       mNativePica->LastFragmentUniforms);
        GlPerf::PhaseLap(GlPerf::Phase::Uniforms);

        uint16_t nextAttributeMask = 0U;
        for (const auto& binding : draw.VertexBindings) {
            if (binding.Binding >= mNativePica->VertexBuffers.size() ||
                binding.ByteStride == 0U || binding.Bytes.empty() ||
                binding.ByteStride > static_cast<uint32_t>(
                                         std::numeric_limits<GLsizei>::max()) ||
                binding.Bytes.size() > static_cast<size_t>(
                                           std::numeric_limits<GLsizeiptr>::max())) {
                throw std::runtime_error("PICA vertex binding is invalid");
            }
        }
        for (const auto& attribute : draw.VertexAttributes) {
            if (attribute.Location >= 16U || attribute.Binding >= 16U ||
                attribute.ComponentCount == 0U ||
                attribute.ComponentCount > 4U) {
                throw std::runtime_error("PICA vertex attribute is invalid");
            }
            nextAttributeMask |= static_cast<uint16_t>(
                1U << attribute.Location);
        }
        const uint16_t disabledAttributes =
            mNativePica->EnabledAttributeMask & ~nextAttributeMask;
        for (GLuint location = 0U; location < 16U; ++location) {
            if ((disabledAttributes & (1U << location)) != 0U) {
                glDisableVertexAttribArray(location);
            }
        }

        const bool persistentGeometry =
            mNativePica->GeometryCacheEnabled &&
            draw.GeometryIdentityAvailable && draw.GeometryIdentity != 0U &&
            draw.GeometryContentVersion != 0U;
        const Oot3d::PicaPreparedGeometry* preparedGeometry = nullptr;
        GLuint persistentGeometryBuffer = 0U;
        // Streamed bindings: the buffer and offset their bytes went to.
        std::array<GLuint, 16> streamBuffers{};
        std::array<uint64_t, 16> streamOffsets{};
        if (persistentGeometry) {
            auto& streams = mNativePica->GeometrySourceStreams;
            streams.clear();
            streams.reserve(draw.VertexBindings.size());
            for (const auto& binding : draw.VertexBindings) {
                streams.push_back({
                    {binding.Binding, binding.ByteStride,
                     binding.PerInstance},
                    binding.Bytes,
                });
            }
            auto& attributes = mNativePica->GeometrySourceAttributes;
            attributes.clear();
            attributes.reserve(draw.VertexAttributes.size());
            for (const auto& attribute : draw.VertexAttributes) {
                attributes.push_back({
                    attribute.Location,
                    attribute.Binding,
                    static_cast<Oot3d::PicaNriVertexScalar>(
                        attribute.Format),
                    attribute.ComponentCount,
                    attribute.ByteOffset,
                });
            }
            const auto resolution = mNativePica->GeometryRegistry.Resolve({
                draw.GeometryIdentity,
                draw.GeometryContentVersion,
                true,
                mFrameCount,
                streams,
                attributes,
                draw.IndexBytes,
                draw.Indexed,
                draw.IndicesAre16Bit,
                draw.VertexCount,
                false,
            });
            if (resolution.Geometry == nullptr) {
                throw std::runtime_error(
                    "PICA OpenGL geometry preparation failed");
            }
            for (const uint64_t evicted :
                 mNativePica->GeometryRegistry.TakeEvictedIdentities()) {
                DeletePicaGlGeometry(*mNativePica, evicted);
            }
            preparedGeometry = resolution.Geometry;
            auto& buffer = mNativePica
                               ->GeometryBuffers[preparedGeometry->Identity];
            if (buffer.Handle == 0U) {
                glGenBuffers(1, &buffer.Handle);
                if (buffer.Handle == 0U) {
                    throw std::runtime_error(
                        "PICA OpenGL geometry buffer allocation failed");
                }
            }
            if (buffer.ContentVersion !=
                    preparedGeometry->ContentVersion ||
                buffer.StructuralSignature !=
                    preparedGeometry->StructuralSignature) {
                if (preparedGeometry->Payload.size() >
                    static_cast<size_t>(
                        std::numeric_limits<GLsizeiptr>::max())) {
                    throw std::runtime_error(
                        "PICA OpenGL geometry payload is too large");
                }
                // Invalid until the upload succeeded (a failed map leaves
                // the buffer without these contents).
                buffer.ContentVersion = 0U;
                mNativePica->GeometryBufferBytes -=
                    std::min(mNativePica->GeometryBufferBytes, buffer.Bytes);
                buffer.Bytes = 0U;
                UploadPicaGlBuffer(GL_ARRAY_BUFFER, buffer.Handle,
                                   preparedGeometry->Payload, GL_STATIC_DRAW);
                buffer.Bytes = preparedGeometry->Payload.size();
                mNativePica->GeometryBufferBytes += buffer.Bytes;
                buffer.ContentVersion = preparedGeometry->ContentVersion;
                buffer.StructuralSignature =
                    preparedGeometry->StructuralSignature;
                ++mNativePica->GeometryPersistentUploads;
                mNativePica->GeometryPersistentUploadBytes +=
                    preparedGeometry->Payload.size();
                GlPerf::PhaseCount(GlPerf::DrawCount::VertexBytes,
                                   preparedGeometry->Payload.size());
            }
            persistentGeometryBuffer = buffer.Handle;
            ++mNativePica->GeometryPersistentDraws;
        } else {
            // All bindings of the draw in one generation of the vertex ring;
            // bindings larger than the ring keep their own buffers.
            uint64_t vertexTotal = 0U;
            for (const auto& binding : draw.VertexBindings) {
                vertexTotal += PicaGlResidency::StreamRing::Span(
                    binding.Bytes.size(), 16U);
            }
            const bool verticesInRing = ReservePicaGlStream(
                *mNativePica, mNativePica->VertexRing, vertexTotal);
            for (const auto& binding : draw.VertexBindings) {
                if (verticesInRing) {
                    streamBuffers[binding.Binding] =
                        mNativePica->VertexRing.Handle;
                    streamOffsets[binding.Binding] = WritePicaGlStream(
                        mNativePica->VertexRing, binding.Bytes, 16U);
                } else {
                    UploadPicaGlBuffer(
                        GL_ARRAY_BUFFER,
                        mNativePica->VertexBuffers[binding.Binding],
                        binding.Bytes, GL_STREAM_DRAW);
                    streamBuffers[binding.Binding] =
                        mNativePica->VertexBuffers[binding.Binding];
                    streamOffsets[binding.Binding] = 0U;
                }
                ++mNativePica->GeometryStreamingUploads;
                mNativePica->GeometryStreamingUploadBytes +=
                    binding.Bytes.size();
                GlPerf::PhaseCount(GlPerf::DrawCount::VertexBytes,
                                   binding.Bytes.size());
            }
            ++mNativePica->GeometryStreamingDraws;
        }
        for (const auto& attribute : draw.VertexAttributes) {
            const auto binding = std::find_if(
                draw.VertexBindings.begin(), draw.VertexBindings.end(),
                [&](const auto& candidate) {
                    return candidate.Binding == attribute.Binding;
                });
            if (binding == draw.VertexBindings.end()) {
                throw std::runtime_error("PICA vertex attribute has no binding");
            }
            const GLuint buffer = persistentGeometry
                                      ? persistentGeometryBuffer
                                      : streamBuffers[attribute.Binding];
            const GLenum type = ToPicaGlVertexType(attribute.Format);
            uint64_t byteOffset = attribute.ByteOffset;
            if (!persistentGeometry) {
                byteOffset += streamOffsets[attribute.Binding];
            }
            if (persistentGeometry) {
                const auto preparedBinding = std::find_if(
                    preparedGeometry->SourceBindings.begin(),
                    preparedGeometry->SourceBindings.end(),
                    [&](const auto& candidate) {
                        return candidate.Binding == attribute.Binding;
                    });
                if (preparedBinding ==
                    preparedGeometry->SourceBindings.end()) {
                    throw std::runtime_error(
                        "PICA prepared geometry has no vertex binding");
                }
                if (preparedBinding->Offset >
                    std::numeric_limits<uint64_t>::max() - byteOffset) {
                    throw std::runtime_error(
                        "PICA OpenGL vertex offset overflow");
                }
                byteOffset += preparedBinding->Offset;
            }
            if (byteOffset > std::numeric_limits<uintptr_t>::max()) {
                throw std::runtime_error(
                    "PICA OpenGL vertex offset is too large");
            }
            auto& state =
                mNativePica->AttributeStates[attribute.Location];
            if (!state.Configured || state.Buffer != buffer ||
                state.ByteStride != binding->ByteStride ||
                state.ByteOffset != static_cast<uintptr_t>(byteOffset) ||
                state.Type != type ||
                state.ComponentCount != attribute.ComponentCount) {
                glBindBuffer(GL_ARRAY_BUFFER, buffer);
                glVertexAttribPointer(
                    attribute.Location, attribute.ComponentCount, type,
                    GL_FALSE, binding->ByteStride,
                    reinterpret_cast<const void*>(static_cast<uintptr_t>(
                        byteOffset)));
                state.Buffer = buffer;
                state.ByteStride = binding->ByteStride;
                state.ByteOffset = static_cast<uintptr_t>(byteOffset);
                state.Type = type;
                state.ComponentCount = attribute.ComponentCount;
                state.Configured = true;
            }
            const uint16_t attributeBit = static_cast<uint16_t>(
                1U << attribute.Location);
            if ((mNativePica->EnabledAttributeMask & attributeBit) == 0U) {
                glEnableVertexAttribArray(attribute.Location);
            }
            const uint8_t divisor = binding->PerInstance ? 1U : 0U;
            if (mNativePica->AttributeDivisors[attribute.Location] !=
                divisor) {
                glVertexAttribDivisor(attribute.Location, divisor);
                mNativePica->AttributeDivisors[attribute.Location] = divisor;
            }
        }
        mNativePica->EnabledAttributeMask = nextAttributeMask;
        GlPerf::PhaseLap(GlPerf::Phase::Vertices);

        std::array<GLuint, 3> textures{
            mNativePica->FallbackColor,
            mNativePica->FallbackColor,
            mNativePica->FallbackColor,
        };
        bool rebindTarget = false;
        for (const auto& texture : draw.Textures) {
            if (texture.Slot >= textures.size()) {
                throw std::runtime_error("PICA texture slot is invalid");
            }
            if (texture.NativeType != 0U &&
                !(texture.Slot == 0U &&
                  (texture.NativeType == 2U || texture.NativeType == 3U))) {
                throw std::runtime_error("PICA texture type is unsupported");
            }
            GLuint handle = texture.NativeType == 2U
                ? FindLivePicaShadow(*mNativePica, texture,
                                     draw.RenderTargetNamespace, &rebindTarget)
                : 0U;
            if (handle == 0U) {
                handle = GetPicaGlTexture(*mNativePica, texture).Handle;
            }
            textures[texture.Slot] = handle;
        }
        if (rebindTarget) {
            // The deferred Shadow2D clear bound its own framebuffer.
            gl.ForgetClearState();
            if (gl.BindFramebuffer(target.Framebuffer)) {
                glBindFramebuffer(GL_FRAMEBUFFER, target.Framebuffer);
                glDrawBuffer(GL_COLOR_ATTACHMENT0);
            }
        }
        // Looked up before binding: a new LUT is created (bound) on the
        // active unit like any new texture.
        GLuint lighting = mNativePica->FallbackInteger;
        if (!draw.LightingLut.PackedEntries.empty()) {
            lighting = GetPicaLightingLut(*mNativePica, draw.LightingLut);
        }
        GlPerf::PhaseLap(GlPerf::Phase::TexLookup);
        // Texture creation/upload above binds on the active unit.
        gl.ForgetActiveTextureBinding();
        const auto bindTexture = [&](uint32_t unit, GLuint handle) {
            bool activate = false;
            if (gl.BindTexture(unit, handle, &activate)) {
                if (activate) {
                    glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
                }
                glBindTexture(GL_TEXTURE_2D, handle);
            }
        };
        for (size_t slot = 0U; slot < textures.size(); ++slot) {
            bindTexture(static_cast<uint32_t>(slot + 1U), textures[slot]);
        }
        bindTexture(13U, lighting);

        const bool shadowProducer = draw.FragmentOperationMode == 3U;
        // Only this backend binds image unit 5; Mesa revalidates every
        // image binding on each call, even an unchanged one.
        if (!mNativePica->Lean || mNativePica->LastShadowImage != target.Shadow) {
            glBindImageTexture(5, target.Shadow, 0, GL_FALSE, 0,
                               GL_READ_WRITE, GL_R32UI);
            mNativePica->LastShadowImage = target.Shadow;
        }
        // Each call below is issued only when it changes the value the
        // context holds (always, with config/gl_state_cache 0).
        const auto toggle = [&](PicaGlStateCache::Slot<bool>& slot,
                                GLenum capability, bool enabled) {
            if (gl.Change(slot, enabled)) {
                if (enabled) {
                    glEnable(capability);
                } else {
                    glDisable(capability);
                }
            }
        };
        const auto setColorMask = [&](uint8_t mask) {
            if (gl.Change(gl.ColorMask, mask)) {
                glColorMask((mask & 1U) != 0U, (mask & 2U) != 0U,
                            (mask & 4U) != 0U, (mask & 8U) != 0U);
            }
        };
        // GPU lean (gfx_opengl_pica_gpu_lean.h): an identity blend or the
        // logic op COPY becomes "both off"; same pixels, no raster op.
        PicaGlGpuLean::BlendState blendState;
        blendState.Enabled = draw.Blend.Enabled;
        if (draw.Blend.Enabled) {
            blendState.EquationRgb = ToPicaGlBlendEquation(draw.Blend.EquationRgb);
            blendState.EquationAlpha =
                ToPicaGlBlendEquation(draw.Blend.EquationAlpha);
            blendState.SourceRgb = ToPicaGlBlendFactor(draw.Blend.SourceRgb);
            blendState.DestRgb = ToPicaGlBlendFactor(draw.Blend.DestRgb);
            blendState.SourceAlpha = ToPicaGlBlendFactor(draw.Blend.SourceAlpha);
            blendState.DestAlpha = ToPicaGlBlendFactor(draw.Blend.DestAlpha);
        } else {
            blendState.LogicOp = ToPicaGlLogic(draw.LogicOperation);
        }
        const auto outputMerger = PicaGlGpuLean::ResolveOutputMerger(
            blendState, mNativePica->GpuLean);
        uint32_t perfBits = 0U;
        if (shadowProducer) {
            toggle(gl.Blend, GL_BLEND, false);
            toggle(gl.ColorLogicOp, GL_COLOR_LOGIC_OP, false);
            setColorMask(0U);
        } else if (outputMerger == PicaGlGpuLean::OutputMerger::Plain) {
            setColorMask(static_cast<uint8_t>(draw.ColorWriteMask & 0xFU));
            toggle(gl.Blend, GL_BLEND, false);
            toggle(gl.ColorLogicOp, GL_COLOR_LOGIC_OP, false);
            perfBits |= draw.Blend.Enabled
                ? PicaGlGpuLean::kCountBlendIdentityOff
                : PicaGlGpuLean::kCountLogicCopyOff;
        } else {
            setColorMask(static_cast<uint8_t>(draw.ColorWriteMask & 0xFU));
            if (draw.Blend.Enabled) {
                perfBits |= PicaGlGpuLean::kCountBlended;
                toggle(gl.Blend, GL_BLEND, true);
                toggle(gl.ColorLogicOp, GL_COLOR_LOGIC_OP, false);
                const std::array<uint32_t, 2> equation{
                    ToPicaGlBlendEquation(draw.Blend.EquationRgb),
                    ToPicaGlBlendEquation(draw.Blend.EquationAlpha)};
                if (gl.Change(gl.BlendEquation, equation)) {
                    glBlendEquationSeparate(equation[0], equation[1]);
                }
                const std::array<uint32_t, 4> factors{
                    ToPicaGlBlendFactor(draw.Blend.SourceRgb),
                    ToPicaGlBlendFactor(draw.Blend.DestRgb),
                    ToPicaGlBlendFactor(draw.Blend.SourceAlpha),
                    ToPicaGlBlendFactor(draw.Blend.DestAlpha)};
                if (gl.Change(gl.BlendFunc, factors)) {
                    glBlendFuncSeparate(factors[0], factors[1], factors[2],
                                        factors[3]);
                }
                const std::array<uint32_t, 4> constant{
                    PicaGlStateCache::FloatBits(draw.Blend.ConstantColor[0]),
                    PicaGlStateCache::FloatBits(draw.Blend.ConstantColor[1]),
                    PicaGlStateCache::FloatBits(draw.Blend.ConstantColor[2]),
                    PicaGlStateCache::FloatBits(draw.Blend.ConstantColor[3])};
                if (gl.Change(gl.BlendColor, constant)) {
                    glBlendColor(draw.Blend.ConstantColor[0],
                                 draw.Blend.ConstantColor[1],
                                 draw.Blend.ConstantColor[2],
                                 draw.Blend.ConstantColor[3]);
                }
            } else {
                toggle(gl.Blend, GL_BLEND, false);
                toggle(gl.ColorLogicOp, GL_COLOR_LOGIC_OP, true);
                const uint32_t logic = ToPicaGlLogic(draw.LogicOperation);
                if (gl.Change(gl.LogicOp, logic)) {
                    glLogicOp(logic);
                }
            }
        }

        // Clip at the near/far planes like the PICA (and the Vulkan
        // backend): with the legacy path's GL_DEPTH_CLAMP, geometry between
        // the eye and the near plane was drawn as a screen-sized polygon.
        if (mNativePica->DepthClamp.EnterPica()) {
            glDisable(GL_DEPTH_CLAMP);
        }
        if (draw.DepthTestEnabled || draw.DepthWriteEnabled) {
            toggle(gl.DepthTest, GL_DEPTH_TEST, true);
            const uint32_t depthFunc = draw.DepthTestEnabled
                ? ToPicaGlCompare(draw.DepthCompare)
                : GL_ALWAYS;
            if (gl.Change(gl.DepthFunc, depthFunc)) {
                glDepthFunc(depthFunc);
            }
        } else {
            toggle(gl.DepthTest, GL_DEPTH_TEST, false);
        }
        if (gl.Change(gl.DepthMask, draw.DepthWriteEnabled)) {
            glDepthMask(draw.DepthWriteEnabled ? GL_TRUE : GL_FALSE);
        }
        if (draw.Stencil.Enabled) {
            toggle(gl.StencilTest, GL_STENCIL_TEST, true);
            const std::array<uint32_t, 3> stencilFunc{
                ToPicaGlCompare(draw.Stencil.Compare),
                static_cast<uint32_t>(draw.Stencil.Reference),
                static_cast<uint32_t>(draw.Stencil.CompareMask)};
            if (gl.Change(gl.StencilFunc, stencilFunc)) {
                glStencilFunc(stencilFunc[0],
                              static_cast<GLint>(stencilFunc[1]),
                              stencilFunc[2]);
            }
            const uint32_t stencilMask = draw.Stencil.WriteMask;
            if (gl.Change(gl.StencilMask, stencilMask)) {
                glStencilMask(stencilMask);
            }
            const std::array<uint32_t, 3> stencilOp{
                ToPicaGlStencil(draw.Stencil.Fail),
                ToPicaGlStencil(draw.Stencil.DepthFail),
                ToPicaGlStencil(draw.Stencil.Pass)};
            if (gl.Change(gl.StencilOp, stencilOp)) {
                glStencilOp(stencilOp[0], stencilOp[1], stencilOp[2]);
            }
        } else {
            toggle(gl.StencilTest, GL_STENCIL_TEST, false);
        }
        if (draw.CullMode == GfxNativeCullMode::KeepAll) {
            toggle(gl.CullFace, GL_CULL_FACE, false);
        } else {
            toggle(gl.CullFace, GL_CULL_FACE, true);
            // Vulkan's positive-height viewport reverses window-space winding
            // relative to OpenGL. Keep the PICA semantic winding here instead
            // of copying the Vulkan pipeline's compensating front-face value.
            const uint32_t frontFace =
                draw.CullMode == GfxNativeCullMode::KeepCounterClockwise
                    ? GL_CCW
                    : GL_CW;
            if (gl.Change(gl.FrontFace, frontFace)) {
                glFrontFace(frontFace);
            }
            const uint32_t cullFace =
                draw.FramebufferFlipped ? GL_FRONT : GL_BACK;
            if (gl.Change(gl.CullFaceMode, cullFace)) {
                glCullFace(cullFace);
            }
        }
        const auto setViewport = [&](GLint x, GLint y, GLsizei width,
                                     GLsizei height) {
            if (gl.Change(gl.Viewport,
                          std::array<int32_t, 4>{x, y, width, height})) {
                glViewport(x, y, width, height);
            }
        };
        const auto setScissor = [&](GLint x, GLint y, GLsizei width,
                                    GLsizei height) {
            toggle(gl.ScissorTest, GL_SCISSOR_TEST, true);
            if (gl.Change(gl.Scissor,
                          std::array<int32_t, 4>{x, y, width, height})) {
                glScissor(x, y, width, height);
            }
        };

        // Guest viewport and scissor are in native pixels; the target holds
        // ResolutionScale host pixels per native pixel on each axis. The
        // guest values are truncated first, so N=1 is the original call and
        // N>1 covers exactly the same native pixels.
        // A fractional scale rounds both edges of every rectangle
        // (PicaGlScale::ScaledEdge); at an integer scale these are the
        // former products native * N.
        const float resolutionScale = target.Scale;
        // Widescreen: a relocated lower-menu draw is issued once per pass
        // (its middle in place, its bottom corners at the widened target's
        // edges); none = the single draw with the state set below.
        std::array<Oot3d::GlWidescreenPass, 4> cornerPasses{};
        size_t cornerPassCount = 0U;
        GLint cornerViewportX = 0;
        GLint cornerViewportY = 0;
        GLsizei cornerViewportWidth = 0;
        GLsizei cornerViewportHeight = 0;
        if (!target.Wide) {
            const auto edge = [&](int64_t native) {
                return static_cast<GLint>(
                    PicaGlScale::ScaledEdge(native, resolutionScale));
            };
            const int64_t viewportX = static_cast<GLint>(draw.ViewportX);
            const int64_t viewportY = static_cast<GLint>(draw.ViewportY);
            const int64_t viewportWidth =
                static_cast<GLsizei>(draw.ViewportWidth);
            const int64_t viewportHeight =
                static_cast<GLsizei>(draw.ViewportHeight);
            setViewport(edge(viewportX), edge(viewportY),
                        edge(viewportX + viewportWidth) - edge(viewportX),
                        edge(viewportY + viewportHeight) - edge(viewportY));
            if (draw.ScissorMode == 3U) {
                const int64_t scissorWidth = draw.ScissorX2 >= draw.ScissorX1
                    ? static_cast<int64_t>(draw.ScissorX2) - draw.ScissorX1 + 1
                    : 0;
                const int64_t scissorHeight = draw.ScissorY2 >= draw.ScissorY1
                    ? static_cast<int64_t>(draw.ScissorY2) - draw.ScissorY1 + 1
                    : 0;
                const int64_t scissorX = draw.ScissorX1;
                const int64_t scissorY = draw.ScissorY1;
                setScissor(edge(scissorX), edge(scissorY),
                           edge(scissorX + scissorWidth) - edge(scissorX),
                           edge(scissorY + scissorHeight) - edge(scissorY));
            } else {
                setScissor(0, 0, static_cast<GLsizei>(target.Width),
                           static_cast<GLsizei>(target.Height));
            }
        } else {
            // Widescreen top-screen target (see PicaGlTarget::Wide), as the
            // Vulkan raster canvas (ResolvePicaRasterCanvas): the scene and
            // screen-wide frame content (a wash, a full-frame redraw) stretch
            // over the whole widened axis, where the projection hook already
            // widened the frustum by the same factor; placed UI (HUD, menus,
            // dialogue, file select) keeps the native proportions on a
            // centred canvas, which the touch/pointer mapping also assumes.
            // Menus outside the Ui domain (title, file select, the relocated
            // lower menu) are recognised by their PICA state
            // (gl_widescreen_menu_policy.h, config/widescreen_menus).
            const auto placement = ResolvePicaGlWidescreenPlacement(
                draw, target.ScaleY / resolutionScale);
            // Presented horizontal axis (target Y): canvas, stretch, fill of
            // the relocated menu's fade, widening compensation and their
            // clipping (ResolveGlWidescreenPresentedAxis, host-tested).
            Oot3d::GlWidescreenPresentedAxisInput axisInput;
            axisInput.ViewportY = draw.ViewportY;
            axisInput.ViewportHeight = draw.ViewportHeight;
            axisInput.GuestScissor = draw.ScissorMode == 3U;
            axisInput.ScissorY1 = draw.ScissorY1;
            axisInput.ScissorY2 = draw.ScissorY2;
            axisInput.FramebufferHeight = draw.FramebufferHeight;
            axisInput.TargetHeight = target.Height;
            axisInput.ResolutionScale = resolutionScale;
            axisInput.TargetScaleY = target.ScaleY;
            const auto axis =
                Oot3d::ResolveGlWidescreenPresentedAxis(placement, axisInput);
            const float scaleX = resolutionScale;
            const auto edgeX = [&](float native) {
                return static_cast<GLint>(std::lround(native * scaleX));
            };
            const GLint viewportX0 = edgeX(draw.ViewportX);
            setViewport(viewportX0, axis.ViewportBegin,
                        std::max<GLint>(0, edgeX(draw.ViewportX + draw.ViewportWidth) - viewportX0),
                        axis.ViewportEnd - axis.ViewportBegin);
            GLint scissorX0 = 0;
            GLint scissorX1 = static_cast<GLint>(target.Width);
            if (draw.ScissorMode == 3U) {
                scissorX0 = edgeX(static_cast<float>(draw.ScissorX1));
                scissorX1 = draw.ScissorX2 >= draw.ScissorX1
                    ? edgeX(static_cast<float>(draw.ScissorX2) + 1.0F)
                    : scissorX0;
            }
            setScissor(scissorX0, axis.ScissorBegin,
                       std::max<GLint>(0, scissorX1 - scissorX0),
                       axis.ScissorEnd - axis.ScissorBegin);
            if (placement.SplitCorners) {
                Oot3d::GlWidescreenCornerInput corners;
                corners.ScissorX0 = scissorX0;
                corners.ScissorX1 = scissorX1;
                corners.BottomX = viewportX0;
                // Presented lines hold FramebufferWidth / 240 native columns.
                corners.CornerHeight = edgeX(
                    static_cast<float>(Oot3d::kGlWidescreenCornerHeight) *
                    static_cast<float>(draw.FramebufferWidth) / 240.0F);
                corners.CornerWidth = static_cast<int32_t>(std::lround(
                    static_cast<float>(Oot3d::kGlWidescreenCornerWidth) *
                    resolutionScale));
                corners.TargetHeight = static_cast<int32_t>(target.Height);
                cornerPassCount = Oot3d::ResolveGlWidescreenCornerPasses(
                    axis, corners, cornerPasses);
                if (cornerPassCount == 0U) {
                    cornerPassCount = 1U; // fully clipped: an empty scissor
                    cornerPasses[0] = {0, 0, 0, 0, 0};
                }
            }
            if (cornerPassCount != 0U) {
                // Re-issued below per pass with these viewport/scissor bases.
                cornerViewportX = viewportX0;
                cornerViewportWidth = std::max<GLint>(
                    0, edgeX(draw.ViewportX + draw.ViewportWidth) - viewportX0);
                cornerViewportY = axis.ViewportBegin;
                cornerViewportHeight = axis.ViewportEnd - axis.ViewportBegin;
            }
        }
        // Issues the draw call once, or once per corner pass.
        const auto issuePasses = [&](const auto& issue) {
            if (cornerPassCount == 0U) {
                issue();
                return;
            }
            for (size_t index = 0; index < cornerPassCount; ++index) {
                const auto& pass = cornerPasses[index];
                setViewport(cornerViewportX, cornerViewportY + pass.ViewportShift,
                            cornerViewportWidth, cornerViewportHeight);
                setScissor(pass.ScissorX0, pass.ScissorY0,
                           std::max<GLint>(0, pass.ScissorX1 - pass.ScissorX0),
                           std::max<GLint>(0, pass.ScissorY1 - pass.ScissorY0));
                issue();
            }
        };

        // Relocated menu pieces: GL_NEAREST on magnification (no atlas seam
        // at the internal resolution; GlRelocatedMenuNearestSampling). The
        // sampler overrides the texture's filter and wrap only for this draw.
        uint32_t menuNearestUnits = 0U;
        if (PicaGlRelocatedMenuNearestSampling(draw, resolutionScale)) {
            auto& samplers = mNativePica->MenuNearestSamplers;
            if (samplers[0] == 0U) {
                glGenSamplers(static_cast<GLsizei>(samplers.size()), samplers.data());
            }
            for (const auto& texture : draw.Textures) {
                if (texture.Slot >= samplers.size() || texture.NativeType != 0U ||
                    !texture.MagLinear) {
                    continue;
                }
                const GLuint sampler = samplers[texture.Slot];
                glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER,
                                    PicaGlMinFilter(texture, texture.MaxMipLevel + 1U));
                glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S,
                                    ToPicaGlWrap(texture.NativeWrapS));
                glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T,
                                    ToPicaGlWrap(texture.NativeWrapT));
                glBindSampler(texture.Slot + 1U, sampler);
                menuNearestUnits |= 1U << texture.Slot;
            }
        }

        // GPU lean: a draw that cannot change any buffer is not issued (its
        // uploads and state above are kept, so later draws see the same).
        PicaGlGpuLean::DrawEffects effects;
        effects.ShadowProducer = shadowProducer;
        effects.ColorWriteMask = static_cast<uint8_t>(draw.ColorWriteMask & 0xFU);
        effects.Blend = blendState;
        effects.DepthWrite = draw.DepthWriteEnabled;
        effects.StencilTest = draw.Stencil.Enabled;
        if (draw.Stencil.Enabled) {
            effects.StencilWriteMask = draw.Stencil.WriteMask;
            effects.StencilFail = ToPicaGlStencil(draw.Stencil.Fail);
            effects.StencilDepthFail = ToPicaGlStencil(draw.Stencil.DepthFail);
            effects.StencilPass = ToPicaGlStencil(draw.Stencil.Pass);
        }
        effects.FragmentFeatures = program.FragmentFeatures;
        const bool skipDraw =
            mNativePica->GpuLean && PicaGlGpuLean::DrawHasNoEffect(effects);
        if (GlPerf::Enabled()) {
            if ((program.FragmentFeatures & PicaGlGpuLean::kFragmentDiscard) != 0U) {
                perfBits |= PicaGlGpuLean::kCountFragmentDiscard;
            }
            if ((program.FragmentFeatures & PicaGlGpuLean::kFragmentWritesDepth) != 0U) {
                perfBits |= PicaGlGpuLean::kCountFragmentDepth;
            }
            if ((program.FragmentFeatures & PicaGlGpuLean::kFragmentStoresImage) != 0U) {
                perfBits |= PicaGlGpuLean::kCountFragmentImage;
            }
            if (skipDraw) {
                perfBits |= PicaGlGpuLean::kCountNoEffectSkipped;
            }
            GlPerf::CountPica(perfBits);
        }

        const GLenum topology = ToPicaGlTopology(draw.Topology);
        if (GlPerf::Enabled() && !skipDraw) {
            GlPerf::PhaseLap(GlPerf::Phase::State);
        }
        if (skipDraw) {
            // Nothing to draw: the uploads below are skipped as well (each
            // draw writes its own ring range; nothing later reads this one).
        } else if (draw.Indexed) {
            if (draw.IndexBytes.empty()) {
                throw std::runtime_error("PICA indexed draw has no indices");
            }
            const size_t indexSize = draw.IndicesAre16Bit
                                         ? sizeof(uint16_t)
                                         : sizeof(uint8_t);
            if (draw.IndexBytes.size() % indexSize != 0U) {
                throw std::runtime_error(
                    "PICA indexed draw has a partial index");
            }
            if (persistentGeometry) {
                if (!preparedGeometry->Indexed ||
                    preparedGeometry->VertexOrIndexCount >
                        static_cast<uint32_t>(
                            std::numeric_limits<GLsizei>::max()) ||
                    preparedGeometry->IndexOffset >
                        std::numeric_limits<uintptr_t>::max()) {
                    throw std::runtime_error(
                        "PICA OpenGL prepared index data is invalid");
                }
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,
                             persistentGeometryBuffer);
                issuePasses([&] {
                    glDrawElementsBaseVertex(
                        topology,
                        static_cast<GLsizei>(
                            preparedGeometry->VertexOrIndexCount),
                        GL_UNSIGNED_SHORT,
                        reinterpret_cast<const void*>(static_cast<uintptr_t>(
                            preparedGeometry->IndexOffset)),
                        draw.BaseVertex);
                });
            } else {
                const size_t indexCount = draw.IndexBytes.size() / indexSize;
                if (draw.IndexBytes.size() >
                        static_cast<size_t>(
                            std::numeric_limits<GLsizeiptr>::max()) ||
                    indexCount > static_cast<size_t>(
                                     std::numeric_limits<GLsizei>::max())) {
                    throw std::runtime_error(
                        "PICA OpenGL streaming index data is too large");
                }
                std::optional<uint64_t> indexOffset;
                if (ReservePicaGlStream(
                        *mNativePica, mNativePica->IndexRing,
                        PicaGlResidency::StreamRing::Span(
                            draw.IndexBytes.size(), 4U))) {
                    // Bound to the VAO's element binding by the write.
                    indexOffset = WritePicaGlStream(mNativePica->IndexRing,
                                                    draw.IndexBytes, 4U);
                } else {
                    UploadPicaGlBuffer(GL_ELEMENT_ARRAY_BUFFER,
                                       mNativePica->IndexBuffer,
                                       draw.IndexBytes, GL_STREAM_DRAW);
                }
                ++mNativePica->GeometryStreamingUploads;
                mNativePica->GeometryStreamingUploadBytes +=
                    draw.IndexBytes.size();
                GlPerf::PhaseCount(GlPerf::DrawCount::IndexBytes,
                                   draw.IndexBytes.size());
                GlPerf::PhaseLap(GlPerf::Phase::Indices);

                const GLenum type = draw.IndicesAre16Bit
                                        ? GL_UNSIGNED_SHORT
                                        : GL_UNSIGNED_BYTE;
                issuePasses([&] {
                    glDrawElementsBaseVertex(
                        topology, static_cast<GLsizei>(indexCount), type,
                        reinterpret_cast<const void*>(static_cast<uintptr_t>(
                            indexOffset.value_or(0U))),
                        draw.BaseVertex);
                });
            }
        } else {
            if (draw.VertexCount > static_cast<uint32_t>(
                                       std::numeric_limits<GLsizei>::max())) {
                throw std::runtime_error(
                    "PICA non-indexed draw has too many vertices");
            }
            issuePasses([&] {
                glDrawArrays(topology, 0,
                             static_cast<GLsizei>(draw.VertexCount));
            });
        }
        GlPerf::PhaseLap(GlPerf::Phase::Draw);
        for (uint32_t slot = 0U; slot < 3U; ++slot) {
            if ((menuNearestUnits & (1U << slot)) != 0U) {
                glBindSampler(slot + 1U, 0U);
            }
        }
        // Ordinary raster and framebuffer commands are ordered by GL. Only
        // the PICA shadow producer writes through an image binding and needs
        // an explicit visibility barrier before a later shadow consumer.
        if (shadowProducer) {
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                            GL_TEXTURE_FETCH_BARRIER_BIT);
        }
        ++mNativePica->Draws;
        if (mNativePica->Stats) {
            mNativePica->DrawNanoseconds += PicaGlNowNanoseconds() - drawStart;
        }
        InvalidateLegacyStateAfterPica();
        // Nothing but this draw touched GL since the cache was checked.
        mPicaGlStateForeign = false;
        GlPerf::PhaseEnd();
        return true;
    } catch (const PicaGlOutOfMemory& exception) {
        GlPerf::PhaseEnd();
        // Out of memory: release every idle texture and LUT, forget the
        // stream storage and skip this draw instead of failing the frame.
        auto& state = *mNativePica;
        TrimPicaGlResidency(state, true);
        state.UniformRing.Ring.Reset();
        state.VertexRing.Ring.Reset();
        state.IndexRing.Ring.Reset();
        state.LastVertexUniforms.Forget();
        state.LastFragmentUniforms.Forget();
        for (auto& attribute : state.AttributeStates) attribute = {};
        PublishPicaGlResidency(state);
        const uint64_t events = state.Counters.OutOfMemoryEvents.fetch_add(
                                    1U, std::memory_order_relaxed) + 1U;
        state.Counters.SkippedDraws.fetch_add(1U, std::memory_order_relaxed);
        if ((events & (events - 1U)) == 0U) {
            std::fprintf(stderr,
                         "TRIAEVUM_GL_OUT_OF_MEMORY events=%llu textures=%zu "
                         "texture_bytes=%llu: %s\n",
                         static_cast<unsigned long long>(events),
                         state.Textures.size(),
                         static_cast<unsigned long long>(state.TextureBytes),
                         exception.what());
        }
        InvalidateLegacyStateAfterPica();
        return true;
    } catch (const std::exception& exception) {
        GlPerf::PhaseEnd();
        InvalidateLegacyStateAfterPica();
        SetPicaGlError(error, exception.what());
        return false;
    }
#endif
}

bool GfxRenderingAPIOGL::SubmitPicaDisplayTransfer(
    const GfxNativePicaDisplayTransferView& transfer,
    std::string* error) {
#if defined(USE_OPENGLES) || defined(__APPLE__)
    SetPicaGlError(error, "native PICA requires OpenGL 4.3");
    return false;
#else
    try {
        if (!mNativePica || transfer.InputPhysicalAddress == 0U ||
            transfer.OutputPhysicalAddress == 0U ||
            transfer.InputWidth == 0U || transfer.InputHeight == 0U ||
            transfer.OutputWidth == 0U || transfer.OutputHeight == 0U) {
            throw std::runtime_error("PICA display transfer is incomplete");
        }
        const uint32_t scaling = (transfer.Flags >> 24U) & 3U;
        if ((transfer.Flags & 8U) != 0U ||
            (transfer.Flags & 0x10000U) != 0U || scaling > 2U) {
            throw std::runtime_error("PICA display transfer mode is unsupported");
        }
        GlPerf::Mark(GlPerf::Stage::Transfer);
        const auto source = std::find_if(
            mNativePica->Targets.begin(), mNativePica->Targets.end(),
            [&](const auto& entry) {
                return entry.first.Namespace == transfer.RenderTargetNamespace &&
                       entry.first.ColorAddress ==
                           transfer.InputPhysicalAddress;
            });
        if (source == mNativePica->Targets.end()) {
            throw std::runtime_error("PICA display transfer source is missing");
        }
        if (transfer.InputWidth > source->first.Width ||
            transfer.InputHeight > source->first.Height) {
            throw std::runtime_error("PICA display transfer exceeds its source");
        }

        const uint32_t outputWidth = std::max<uint32_t>(1U,
            (static_cast<uint64_t>(transfer.OutputWidth) *
                 source->first.Width + transfer.InputWidth / 2U) /
                transfer.InputWidth);
        const uint32_t outputHeight = std::max<uint32_t>(1U,
            (static_cast<uint64_t>(transfer.OutputHeight) *
                 source->first.Height + transfer.InputHeight / 2U) /
                transfer.InputHeight);
        const uint32_t horizontalSamples = scaling != 0U ? 2U : 1U;
        const uint32_t verticalSamples = scaling == 2U ? 2U : 1U;
        if (static_cast<uint64_t>(outputWidth) * horizontalSamples >
                source->first.Width ||
            static_cast<uint64_t>(outputHeight) * verticalSamples >
                source->first.Height) {
            throw std::runtime_error("PICA display transfer samples out of bounds");
        }
        // Both images hold ResolutionScale host pixels per native pixel (a
        // widescreen top-screen target holds ScaleY along its height). A 2:1
        // linear blit samples each destination centre on the boundary of two
        // source texels, so the PICA box filter stays exact at any scale.
        // A fractional scale rounds each extent (PicaGlScale::ScaledEdge); a
        // 2:1 transfer then may read one host pixel more or less than twice
        // the output, a sub-pixel offset of the box filter.
        // Dynamic resolution (gl_pica_dynamic_scale.h): the source may be
        // drawn at a lower scale than it was allocated at. The display image
        // keeps the allocation's extent and the copy fills its bottom-left
        // part (the content), which the scanout then reads.
        const auto& sourceTarget = source->second;
        const float resolutionScale = sourceTarget.Scale;
        const float sourceScaleY = sourceTarget.ScaleY;
        const auto scaledY = [&](uint32_t native) {
            return static_cast<uint32_t>(std::lround(
                static_cast<float>(native) * sourceScaleY));
        };
        const uint32_t displayAllocWidth =
            PicaGlScale::ScaledExtent(outputWidth, sourceTarget.AllocScale);
        const uint32_t displayAllocHeight = std::max<uint32_t>(
            1U, static_cast<uint32_t>(std::lround(
                    static_cast<float>(outputHeight) *
                    sourceTarget.AllocScaleY)));
        const uint32_t contentWidth = std::min(
            displayAllocWidth,
            PicaGlScale::ScaledExtent(outputWidth, resolutionScale));
        const uint32_t contentHeight = std::min(
            displayAllocHeight, std::max<uint32_t>(1U, scaledY(outputHeight)));
        auto& display = GetPicaGlDisplay(
            *mNativePica, transfer.RenderTargetNamespace,
            transfer.OutputPhysicalAddress, outputWidth, outputHeight,
            displayAllocWidth, displayAllocHeight);
        const PicaGlScale::DisplayCopyIdentity copy{
            source->second.Id,
            source->second.Version,
            static_cast<int32_t>(std::min<uint32_t>(
                PicaGlScale::ScaledExtent(outputWidth * horizontalSamples,
                                          resolutionScale),
                source->second.Width)),
            static_cast<int32_t>(std::min<uint32_t>(
                scaledY(outputHeight * verticalSamples),
                source->second.Height)),
            scaling == 0U ? static_cast<uint32_t>(GL_NEAREST)
                          : static_cast<uint32_t>(GL_LINEAR),
        };
        // Lean: a presentation re-submits the transfer its sample already
        // executed (and a reused snapshot one whose source may be unchanged);
        // an identical copy from an unchanged source writes the same pixels.
        if (!(mNativePica->Lean && display.Initialized &&
              display.LastCopy.Valid() && display.LastCopy == copy)) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, source->second.Framebuffer);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, display.Framebuffer);
            glDisable(GL_SCISSOR_TEST);
            glBlitFramebuffer(
                0, 0, copy.SourceWidth, copy.SourceHeight,
                0, 0, static_cast<GLint>(contentWidth),
                static_cast<GLint>(contentHeight),
                GL_COLOR_BUFFER_BIT, static_cast<GLenum>(copy.Filter));
            if (contentWidth < displayAllocWidth ||
                contentHeight < displayAllocHeight) {
                // The filters reading the content (bilinear scanout, EASU's
                // 12 taps, RCAS) reach up to two texels past its edge, where
                // an older, larger copy may still lie: replicate the last
                // column, then the last row (with that border), into a
                // two-texel band. Same image, disjoint rectangles.
                glBindFramebuffer(GL_READ_FRAMEBUFFER, display.Framebuffer);
                const auto cw = static_cast<GLint>(contentWidth);
                const auto ch = static_cast<GLint>(contentHeight);
                const GLint bandX = std::min<GLint>(
                    2, static_cast<GLint>(displayAllocWidth) - cw);
                const GLint bandY = std::min<GLint>(
                    2, static_cast<GLint>(displayAllocHeight) - ch);
                if (bandX > 0) {
                    glBlitFramebuffer(cw - 1, 0, cw, ch, cw, 0, cw + bandX, ch,
                                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
                }
                if (bandY > 0) {
                    glBlitFramebuffer(0, ch - 1, cw + bandX, ch, 0, ch,
                                      cw + bandX, ch + bandY,
                                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
                }
            }
            display.LastCopy = copy;
        }
        display.ContentWidth = contentWidth;
        display.ContentHeight = contentHeight;
        display.ContentScale = resolutionScale;
        display.Initialized = true;

        if (transfer.Present) {
            SDL_Window* window = SDL_GL_GetCurrentWindow();
            if (window == nullptr) {
                throw std::runtime_error("PICA OpenGL scanout has no SDL window");
            }
            int windowWidth = 0;
            int windowHeight = 0;
#ifdef __SWITCH__
            SDL_GetWindowSize(window, &windowWidth, &windowHeight);
#else
            SDL_GL_GetDrawableSize(window, &windowWidth, &windowHeight);
#endif
            if (windowWidth <= 0 || windowHeight <= 0) {
                throw std::runtime_error("PICA OpenGL scanout extent is invalid");
            }
            // The transfer image is stored in the CTR's physical portrait
            // layout; presentation uses the transposed logical dimensions.
            // The placement uses the native extent; the sampled image holds
            // ResolutionScale times as many pixels (e.g. 3x: 1200x720, an
            // exact 1:1 fit of the 1280x720 handheld screen).
            // A widescreen top-screen image is presented wider by the same
            // factor its target was widened (16:9 on the Switch: no bars).
            // (The allocation's scale: a dynamic scale changes the content,
            // never the presented shape.)
            const float presentedWidth = static_cast<float>(outputHeight) *
                static_cast<float>(display.AllocHeight) /
                (static_cast<float>(outputHeight) *
                 static_cast<float>(sourceTarget.AllocScale));
            const float presentedHeight = static_cast<float>(outputWidth);
            const float scale = std::min(
                static_cast<float>(windowWidth) / presentedWidth,
                static_cast<float>(windowHeight) / presentedHeight);
            int viewportWidth = std::max(
                1, static_cast<int>(std::lround(presentedWidth * scale)));
            int viewportHeight = std::max(
                1, static_cast<int>(std::lround(presentedHeight * scale)));
            // Rounding of the widened extent must not leave a 1-pixel bar.
            if (std::abs(viewportWidth - windowWidth) <= 2) {
                viewportWidth = windowWidth;
            }
            if (std::abs(viewportHeight - windowHeight) <= 2) {
                viewportHeight = windowHeight;
            }
            const int viewportX = (windowWidth - viewportWidth) / 2;
            const int viewportY = (windowHeight - viewportHeight) / 2;

            GlPerf::Mark(GlPerf::Stage::Scanout);
            FlushDeferredWindowClear();
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(viewportX, viewportY, viewportWidth, viewportHeight);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_COLOR_LOGIC_OP);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            if (transfer.PresentationMode ==
                GfxNativePicaPresentationMode::AlphaOverlay) {
                glEnable(GL_BLEND);
                glBlendEquation(GL_FUNC_ADD);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            } else {
                glDisable(GL_BLEND);
                // Lean: the scanout below overwrites every pixel of a
                // viewport that is the whole window (widescreen 16:9), so
                // clearing it first is dead work.
                if (!(mNativePica->Lean &&
                      PicaGlScale::ScanoutCoversWindow(
                          viewportX, viewportY, viewportWidth,
                          viewportHeight, windowWidth, windowHeight))) {
                    glViewport(0, 0, windowWidth, windowHeight);
                    glClearColor(mClearColor[0], mClearColor[1],
                                 mClearColor[2], mClearColor[3]);
                    glClear(GL_COLOR_BUFFER_BIT);
                    glViewport(viewportX, viewportY,
                               viewportWidth, viewportHeight);
                }
            }
            glBindVertexArray(mNativePica->VertexArray);
            const bool transferInputLinear =
                (transfer.Flags & (1U << 1U)) != 0U;
            const bool dontSwizzle =
                (transfer.Flags & (1U << 5U)) != 0U;
            const bool flip =
                ((!transferInputLinear !=
                  (transferInputLinear != dontSwizzle)) ^
                 ((transfer.Flags & 1U) != 0U));
            // Opaque (top-screen) presentations may run the optional
            // AA/FSR/sharpen chain; alpha overlays never do, as on PC.
            if (transfer.PresentationMode !=
                    GfxNativePicaPresentationMode::AlphaOverlay &&
                mNativePica->Post.Config.ScanoutEffects()) {
                PresentPicaGlPost(*mNativePica, display, flip, viewportX,
                                  viewportY, viewportWidth, viewportHeight);
            } else {
                glUseProgram(mNativePica->ScanoutProgram);
                if (mNativePica->ScanoutFlipLocation >= 0) {
                    glUniform1i(mNativePica->ScanoutFlipLocation,
                                flip ? 1 : 0);
                }
                SetPicaGlScanoutContent(*mNativePica, display.ContentWidth,
                                        display.ContentHeight,
                                        display.AllocWidth,
                                        display.AllocHeight);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, display.Color);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
            mNativePica->LastPresented = transfer;
            if (sourceTarget.DynamicEligible) {
                auto& dynamic = PicaGlDynamicScale::gState;
                dynamic.PresentedFrames.fetch_add(1U,
                                                  std::memory_order_relaxed);
                dynamic.PresentedScaleMilli.fetch_add(
                    static_cast<uint64_t>(
                        std::lround(display.ContentScale * 1000.0F)),
                    std::memory_order_relaxed);
            }
            PublishPicaGlStats(*mNativePica);
        }
        InvalidateLegacyStateAfterPica();
        return true;
    } catch (const std::exception& exception) {
        InvalidateLegacyStateAfterPica();
        SetPicaGlError(error, exception.what());
        return false;
    }
#endif
}

bool GfxRenderingAPIOGL::ClearPicaRenderTarget(
    uint64_t renderTargetNamespace, uint32_t colorPhysicalAddress,
    std::string* error) {
#if defined(USE_OPENGLES) || defined(__APPLE__)
    SetPicaGlError(error, "native PICA requires OpenGL 4.3");
    return false;
#else
    if (!mNativePica || colorPhysicalAddress == 0U) {
        SetPicaGlError(error, "PICA render-target clear is incomplete");
        return false;
    }
    for (auto& [key, target] : mNativePica->Targets) {
        if (key.Namespace != renderTargetNamespace ||
            key.ColorAddress != colorPhysicalAddress) {
            continue;
        }
        GlPerf::Mark(GlPerf::Stage::Clear);
        ClearPicaShadow(target, 0xFFFFFFFFU, mNativePica->Lean);
        RefreshPicaGlTargetScale(target, key);
        ++target.Version;
        glBindFramebuffer(GL_FRAMEBUFFER, target.Framebuffer);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glStencilMask(0xFFU);
        glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        glClearDepth(1.0);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                GL_STENCIL_BUFFER_BIT);
    }
    InvalidateLegacyStateAfterPica();
    return true;
#endif
}

bool GfxRenderingAPIOGL::SubmitPicaMemoryFill(
    const GfxNativePicaMemoryFillView& fill, std::string* error) {
#if defined(USE_OPENGLES) || defined(__APPLE__)
    SetPicaGlError(error, "native PICA requires OpenGL 4.3");
    return false;
#else
    if (!mNativePica || fill.StartPhysicalAddress == 0U ||
        fill.EndPhysicalAddress <= fill.StartPhysicalAddress ||
        (fill.Control & 1U) == 0U) {
        SetPicaGlError(error, "PICA memory fill is incomplete");
        return false;
    }
    const auto previous = std::find_if(
        mNativePica->PendingFills.begin(),
        mNativePica->PendingFills.end(),
        [&](const auto& pending) {
            return pending.RenderTargetNamespace == fill.RenderTargetNamespace &&
                   pending.StartPhysicalAddress == fill.StartPhysicalAddress &&
                   pending.EndPhysicalAddress == fill.EndPhysicalAddress;
        });
    if (previous != mNativePica->PendingFills.end()) {
        *previous = fill;
    } else {
        mNativePica->PendingFills.push_back(fill);
    }
    for (auto& [key, target] : mNativePica->Targets) {
        ApplyPendingPicaFills(*mNativePica, key, target);
    }
    InvalidateLegacyStateAfterPica();
    return true;
#endif
}

bool GfxRenderingAPIOGL::QueuePicaCompletion(
    uint64_t completionId, std::string* error) {
    if (!mNativePica || completionId == 0U) {
        SetPicaGlError(error, "PICA completion identity is invalid");
        return false;
    }
#if !defined(USE_OPENGLES) && !defined(__APPLE__)
    // SubmitPicaDraw has already copied every guest source span into
    // backend-owned GL buffers/textures. A completion acknowledges that copy;
    // it does not need to idle the GPU. GL preserves command ordering through
    // the later display blit and SDL swap.
#endif
    mNativePica->Completions.push_back(completionId);
    return true;
}

std::vector<uint64_t> GfxRenderingAPIOGL::TakePicaCompletions() {
    if (!mNativePica) {
        return {};
    }
    auto result = std::move(mNativePica->Completions);
    mNativePica->Completions.clear();
    return result;
}

float GfxRenderingAPIOGL::PicaNamespaceResolutionScale(
    uint64_t renderTargetNamespace) const noexcept {
#if !defined(USE_OPENGLES) && !defined(__APPLE__)
    if (!mNativePica) {
        return 1.0F;
    }
    const auto found = mNativePica->NamespaceScales.find(renderTargetNamespace);
    return found != mNativePica->NamespaceScales.end()
        ? found->second
        : mNativePica->ResolutionScale;
#else
    (void)renderTargetNamespace;
    return 1.0F;
#endif
}

bool GfxRenderingAPIOGL::SetPicaNamespaceResolutionScale(
    uint64_t renderTargetNamespace, float scale) {
#if !defined(USE_OPENGLES) && !defined(__APPLE__)
    // Anti-aliasing images have the source's extent: alternating extents
    // would reallocate them every frame.
    if (!mNativePica || !(scale >= 1.0F) ||
        scale > mNativePica->ResolutionScale ||
        mNativePica->Post.Config.Aa != PicaGlPost::Antialiasing::Off) {
        return false;
    }
    if (PicaNamespaceResolutionScale(renderTargetNamespace) == scale) {
        mNativePica->NamespaceScales[renderTargetNamespace] = scale;
        return true;
    }
    // Only before the namespace has images: changing the scale of live
    // ones would mean deleting GL objects other caches may still name (the
    // draw state cache, image unit 5) in the middle of a session.
    for (const auto& entry : mNativePica->Targets) {
        if (entry.first.Namespace == renderTargetNamespace) {
            return false;
        }
    }
    for (const auto& entry : mNativePica->Displays) {
        if (entry.first.first == renderTargetNamespace) {
            return false;
        }
    }
    mNativePica->NamespaceScales[renderTargetNamespace] = scale;
    return true;
#else
    (void)renderTargetNamespace;
    (void)scale;
    return false;
#endif
}

bool GfxRenderingAPIOGL::ResetPicaState(std::string* error) {
    try {
        if (!mNativePica) {
            throw std::runtime_error("PICA OpenGL backend is not initialized");
        }
#if !defined(USE_OPENGLES) && !defined(__APPLE__)
        glFinish();
#endif
        InitNativePica();
        InvalidateLegacyStateAfterPica();
        return true;
    } catch (const std::exception& exception) {
        SetPicaGlError(error, exception.what());
        return false;
    }
}

} // namespace Fast

#endif
