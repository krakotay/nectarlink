// SPDX-License-Identifier: GPL-3.0-or-later
#include "linux_video_builtin.h"
#include "linux_video_codec.h"

#undef nectarlink_video_decoder_create
#undef nectarlink_video_decoder_destroy
#undef nectarlink_video_decoder_next_device
#undef nectarlink_video_decoder_backend
#undef nectarlink_video_decoder_send
#undef nectarlink_video_decoder_receive
#undef nectarlink_video_decoder_flush
#undef nectarlink_video_decoder_receive_owned
#undef nectarlink_video_frame_destroy
#undef nectarlink_video_frame_info
#undef nectarlink_video_frame_copy_nv12
#undef nectarlink_video_vulkan_init
#undef nectarlink_video_decoder_create_vulkan
#undef nectarlink_video_frame_present
#undef nectarlink_video_surface_release
#undef nectarlink_video_encoder_create
#undef nectarlink_video_encoder_destroy
#undef nectarlink_video_encoder_next_device
#undef nectarlink_video_encoder_backend
#undef nectarlink_video_encoder_send
#undef nectarlink_video_encoder_receive
#undef nectarlink_video_encoder_request_keyframe
#undef nectarlink_video_encoder_set_bitrate
#undef nectarlink_video_aac_encoder_create
#undef nectarlink_video_aac_encoder_destroy
#undef nectarlink_video_aac_encoder_send
#undef nectarlink_video_aac_encoder_receive
#undef nectarlink_video_aac_decoder_create
#undef nectarlink_video_aac_decoder_destroy
#undef nectarlink_video_aac_decoder_send
#undef nectarlink_video_aac_decoder_receive

#include <array>
#include <atomic>
#include <cstdio>
#include <dlfcn.h>
#include <exception>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <system_error>

namespace {
struct Backend {
    decltype(&builtin_video_decoder_create) decoder_create;
    decltype(&builtin_video_decoder_destroy) decoder_destroy;
    decltype(&builtin_video_decoder_next_device) decoder_next_device;
    decltype(&builtin_video_decoder_backend) decoder_backend;
    decltype(&builtin_video_decoder_send) decoder_send;
    decltype(&builtin_video_decoder_receive) decoder_receive;
    decltype(&builtin_video_decoder_flush) decoder_flush;
    decltype(&builtin_video_encoder_create) encoder_create;
    decltype(&builtin_video_encoder_destroy) encoder_destroy;
    decltype(&builtin_video_encoder_next_device) encoder_next_device;
    decltype(&builtin_video_encoder_backend) encoder_backend;
    decltype(&builtin_video_encoder_send) encoder_send;
    decltype(&builtin_video_encoder_receive) encoder_receive;
    decltype(&builtin_video_encoder_request_keyframe) encoder_request_keyframe;
    decltype(&builtin_video_encoder_set_bitrate) encoder_set_bitrate;
    decltype(&builtin_video_aac_encoder_create) aac_encoder_create;
    decltype(&builtin_video_aac_encoder_destroy) aac_encoder_destroy;
    decltype(&builtin_video_aac_encoder_send) aac_encoder_send;
    decltype(&builtin_video_aac_encoder_receive) aac_encoder_receive;
    decltype(&builtin_video_aac_decoder_create) aac_decoder_create;
    decltype(&builtin_video_aac_decoder_destroy) aac_decoder_destroy;
    decltype(&builtin_video_aac_decoder_send) aac_decoder_send;
    decltype(&builtin_video_aac_decoder_receive) aac_decoder_receive;
    decltype(&builtin_video_decoder_receive_owned) decoder_receive_owned = nullptr;
    decltype(&builtin_video_frame_destroy) frame_destroy = nullptr;
    decltype(&builtin_video_frame_info) frame_info = nullptr;
    decltype(&builtin_video_frame_copy_nv12) frame_copy_nv12 = nullptr;
};

const Backend builtin = {
    builtin_video_decoder_create, builtin_video_decoder_destroy, builtin_video_decoder_next_device,
    builtin_video_decoder_backend, builtin_video_decoder_send, builtin_video_decoder_receive,
    builtin_video_decoder_flush, builtin_video_encoder_create, builtin_video_encoder_destroy,
    builtin_video_encoder_next_device, builtin_video_encoder_backend, builtin_video_encoder_send,
    builtin_video_encoder_receive, builtin_video_encoder_request_keyframe, builtin_video_encoder_set_bitrate,
    builtin_video_aac_encoder_create, builtin_video_aac_encoder_destroy, builtin_video_aac_encoder_send,
    builtin_video_aac_encoder_receive, builtin_video_aac_decoder_create, builtin_video_aac_decoder_destroy,
    builtin_video_aac_decoder_send, builtin_video_aac_decoder_receive,
    builtin_video_decoder_receive_owned, builtin_video_frame_destroy, builtin_video_frame_info,
    builtin_video_frame_copy_nv12,
};

template<typename T> bool load_symbol(void* library, T& target, const char* name)
{
    target = reinterpret_cast<T>(dlsym(library, name));
    return target != nullptr;
}

struct CloseLibrary {
    void operator()(void* library) const { dlclose(library); }
};

std::unique_ptr<Backend> load_backend(const std::filesystem::path& path)
{
    std::unique_ptr<void, CloseLibrary> library(dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL));
    if (!library) return {};
    auto result = std::make_unique<Backend>();
#define LOAD(name) load_symbol(library.get(), result->name, "nectarlink_video_" #name)
    if (!(LOAD(decoder_create) && LOAD(decoder_destroy) && LOAD(decoder_next_device)
        && LOAD(decoder_backend) && LOAD(decoder_send) && LOAD(decoder_receive) && LOAD(decoder_flush)
        && LOAD(encoder_create) && LOAD(encoder_destroy) && LOAD(encoder_next_device)
        && LOAD(encoder_backend) && LOAD(encoder_send) && LOAD(encoder_receive)
        && LOAD(encoder_request_keyframe) && LOAD(encoder_set_bitrate)
        && LOAD(aac_encoder_create) && LOAD(aac_encoder_destroy)
        && LOAD(aac_encoder_send) && LOAD(aac_encoder_receive)
        && LOAD(aac_decoder_create) && LOAD(aac_decoder_destroy)
        && LOAD(aac_decoder_send) && LOAD(aac_decoder_receive))) {
        return {};
    }
    // Old packaged backends retain their CPU interface; shared frames are optional.
    if (!(LOAD(decoder_receive_owned) && LOAD(frame_destroy) && LOAD(frame_info)
          && LOAD(frame_copy_nv12))) result->decoder_receive_owned = nullptr;
#undef LOAD
    // Keep the library loaded for the lifetime of its function pointers.
    library.release();
    return result;
}

const auto& alternates()
{
    static const auto backends = [] {
        std::array<std::unique_ptr<Backend>, 3> result;
        std::error_code error;
        const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
        if (error) return result;
        for (size_t i = 0; i < result.size(); ++i) {
            const auto name = "libnectarlink-video-ffmpeg" + std::to_string(i + 7) + ".so";
            result[i] = load_backend(executable.parent_path() / name);
        }
        return result;
    }();
    return backends;
}

struct Handle {
    const Backend* backend;
    void* object;
};

template<typename Create, typename Destroy>
void* create(Create create, Destroy destroy, char* error, size_t capacity)
{
    auto attempt = [&](const Backend* backend) -> void* {
        if (!backend) return nullptr;
        void* object = create(*backend);
        if (!object) return nullptr;
        auto* handle = new (std::nothrow) Handle{backend, object};
        if (handle) return handle;
        destroy(*backend, object);
        if (capacity) std::snprintf(error, capacity, "Out of memory");
        return nullptr;
    };
    try {
        const auto& candidates = alternates();
        for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
            if (auto* handle = attempt(it->get())) return handle;
        }
        return attempt(&builtin);
    } catch (const std::exception& failure) {
        if (capacity) std::snprintf(error, capacity, "%s", failure.what());
    }
    return nullptr;
}
}

extern "C" void* nectarlink_video_decoder_create(bool hevc, char* error, size_t capacity)
{
    return create([&](const Backend& b) { return b.decoder_create(hevc, error, capacity); },
                  [](const Backend& b, void* p) { b.decoder_destroy(p); }, error, capacity);
}
extern "C" void nectarlink_video_decoder_destroy(void* p)
{
    auto* h = static_cast<Handle*>(p);
    h->backend->decoder_destroy(h->object);
    delete h;
}
extern "C" bool nectarlink_video_decoder_next_device(void* p, char* error, size_t n)
{
    auto* h = static_cast<Handle*>(p);
    return h->backend->decoder_next_device(h->object, error, n);
}
extern "C" const char* nectarlink_video_decoder_backend(const void* p)
{
    auto* h = static_cast<const Handle*>(p);
    return h->backend->decoder_backend(h->object);
}
extern "C" int nectarlink_video_decoder_send(void* p, const uint8_t* data, size_t len, char* error, size_t n)
{
    auto* h = static_cast<Handle*>(p);
    return h->backend->decoder_send(h->object, data, len, error, n);
}
extern "C" int nectarlink_video_decoder_receive(void* p, NectarlinkDecodedNv12* frame, char* error, size_t n)
{
    auto* h = static_cast<Handle*>(p);
    return h->backend->decoder_receive(h->object, frame, error, n);
}
extern "C" void nectarlink_video_decoder_flush(void* p)
{
    auto* h = static_cast<Handle*>(p);
    h->backend->decoder_flush(h->object);
}

extern "C" void* nectarlink_video_encoder_create(bool hevc, int width, int height, int fps, int bitrate,
                                                   char* error, size_t capacity)
{
    return create([&](const Backend& b) { return b.encoder_create(hevc, width, height, fps, bitrate, error, capacity); },
                  [](const Backend& b, void* p) { b.encoder_destroy(p); }, error, capacity);
}
extern "C" void nectarlink_video_encoder_destroy(void* p)
{
    auto* h = static_cast<Handle*>(p);
    h->backend->encoder_destroy(h->object);
    delete h;
}
extern "C" bool nectarlink_video_encoder_next_device(void* p, char* error, size_t n)
{
    auto* h = static_cast<Handle*>(p);
    return h->backend->encoder_next_device(h->object, error, n);
}
extern "C" const char* nectarlink_video_encoder_backend(const void* p)
{
    auto* h = static_cast<const Handle*>(p);
    return h->backend->encoder_backend(h->object);
}
extern "C" int nectarlink_video_encoder_send(void* p, const uint8_t* data, size_t len, int64_t time,
                                             char* error, size_t n)
{
    auto* h = static_cast<Handle*>(p);
    return h->backend->encoder_send(h->object, data, len, time, error, n);
}
extern "C" int nectarlink_video_encoder_receive(void* p, const uint8_t** data, size_t* len, bool* key,
                                                char* error, size_t n)
{
    auto* h = static_cast<Handle*>(p);
    return h->backend->encoder_receive(h->object, data, len, key, error, n);
}
extern "C" void nectarlink_video_encoder_request_keyframe(void* p)
{
    auto* h = static_cast<Handle*>(p);
    h->backend->encoder_request_keyframe(h->object);
}
extern "C" bool nectarlink_video_encoder_set_bitrate(void* p, int bitrate)
{
    auto* h = static_cast<Handle*>(p);
    return h->backend->encoder_set_bitrate(h->object, bitrate);
}

extern "C" void* nectarlink_video_aac_encoder_create(int sample_rate, int channels, char* error, size_t capacity)
{
    return create([&](const Backend& b) { return b.aac_encoder_create(sample_rate, channels, error, capacity); },
                  [](const Backend& b, void* p) { b.aac_encoder_destroy(p); }, error, capacity);
}
extern "C" void nectarlink_video_aac_encoder_destroy(void* p)
{
    auto* h = static_cast<Handle*>(p);
    h->backend->aac_encoder_destroy(h->object);
    delete h;
}
extern "C" int nectarlink_video_aac_encoder_send(void* p, const int16_t* pcm, size_t frames, char* error, size_t n)
{
    auto* h = static_cast<Handle*>(p);
    return h->backend->aac_encoder_send(h->object, pcm, frames, error, n);
}
extern "C" int nectarlink_video_aac_encoder_receive(void* p, const uint8_t** bytes, size_t* len, char* error, size_t n)
{
    auto* h = static_cast<Handle*>(p);
    return h->backend->aac_encoder_receive(h->object, bytes, len, error, n);
}

extern "C" void* nectarlink_video_aac_decoder_create(char* error, size_t capacity)
{
    return create([&](const Backend& b) { return b.aac_decoder_create(error, capacity); },
                  [](const Backend& b, void* p) { b.aac_decoder_destroy(p); }, error, capacity);
}
extern "C" void nectarlink_video_aac_decoder_destroy(void* p)
{
    auto* h = static_cast<Handle*>(p);
    h->backend->aac_decoder_destroy(h->object);
    delete h;
}
extern "C" int nectarlink_video_aac_decoder_send(void* p, const uint8_t* data, size_t len, char* error, size_t n)
{
    auto* h = static_cast<Handle*>(p);
    return h->backend->aac_decoder_send(h->object, data, len, error, n);
}
extern "C" int nectarlink_video_aac_decoder_receive(void* p, const int16_t** pcm, size_t* frames,
                                                    int* sample_rate, int* channels, char* error, size_t n)
{
    auto* h = static_cast<Handle*>(p);
    return h->backend->aac_decoder_receive(h->object, pcm, frames, sample_rate, channels, error, n);
}

namespace {
struct OwnedHandle {
    const Backend* backend;
    void* object;
    std::atomic_uint refs{1};
};
}
extern "C" int nectarlink_video_decoder_receive_owned(void* pointer, void** output, char* error, size_t capacity)
{
    auto* decoder = static_cast<Handle*>(pointer);
    if (!decoder->backend->decoder_receive_owned) return -2;
    void* frame = nullptr;
    const int result = decoder->backend->decoder_receive_owned(decoder->object, &frame, error, capacity);
    if (result != 1) return result;
    auto* owned = new (std::nothrow) OwnedHandle{decoder->backend, frame};
    if (!owned) { decoder->backend->frame_destroy(frame); return -1; }
    *output = owned;
    return 1;
}
extern "C" void* nectarlink_video_frame_retain(void* pointer)
{
    static_cast<OwnedHandle*>(pointer)->refs.fetch_add(1, std::memory_order_relaxed);
    return pointer;
}
extern "C" void nectarlink_video_frame_destroy(void* pointer)
{
    auto* owned = static_cast<OwnedHandle*>(pointer);
    if (owned->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        owned->backend->frame_destroy(owned->object);
        delete owned;
    }
}
extern "C" bool nectarlink_video_frame_info(void* pointer, NectarlinkVideoFrameInfo* info)
{
    auto* f = static_cast<OwnedHandle*>(pointer);
    return f->backend->frame_info(f->object, info);
}
extern "C" bool nectarlink_video_frame_copy_nv12(void* pointer, NectarlinkDecodedNv12* picture, char* error, size_t capacity)
{
    auto* f = static_cast<OwnedHandle*>(pointer);
    return f->backend->frame_copy_nv12(f->object, picture, error, capacity);
}
extern "C" bool nectarlink_video_vulkan_init(uintptr_t instance, NectarlinkVulkanDevice* device, char* error, size_t capacity)
{ return builtin_video_vulkan_init(instance, device, error, capacity); }
extern "C" void* nectarlink_video_decoder_create_vulkan(char* error, size_t capacity)
{
    void* decoder = builtin_video_decoder_create_vulkan(error, capacity);
    if (!decoder) return nullptr;
    auto* handle = new (std::nothrow) Handle{&builtin, decoder};
    if (!handle) { builtin.decoder_destroy(decoder); if (capacity) std::snprintf(error, capacity, "Vulkan decoder handle allocation failed"); }
    return handle;
}
extern "C" void* nectarlink_video_frame_present(void* pointer, NectarlinkVulkanSurface* surface, char* error, size_t capacity)
{
    auto* f = static_cast<OwnedHandle*>(pointer);
    if (f->backend != &builtin) { if (capacity) std::snprintf(error, capacity, "Vulkan mirror requires the built-in FFmpeg ABI"); return nullptr; }
    return builtin_video_frame_present(f->object, surface, error, capacity);
}
extern "C" bool nectarlink_video_surface_release(void* surface, char* error, size_t capacity)
{ return builtin_video_surface_release(surface, error, capacity); }
