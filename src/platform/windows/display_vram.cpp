/**
 * @file src/platform/windows/display_vram.cpp
 * @brief Definitions for handling video ram.
 */
// standard includes
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

// platform includes
#include <d3dcompiler.h>
#include <DirectXMath.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_d3d11va.h>
}

// lib includes
#include <AMF/core/Factory.h>
#include <boost/algorithm/string/predicate.hpp>

// local includes
#include "display.h"
#include "dual_display_compositor.h"
#include "misc.h"
#include "src/config.h"
#include "src/display_device.h"
#include "src/logging.h"
#include "src/nvenc/nvenc_config.h"
#include "src/nvenc/nvenc_d3d11_native.h"
#include "src/nvenc/nvenc_d3d11_on_cuda.h"
#include "src/nvenc/nvenc_utils.h"
#include "src/video.h"
#include "utf_utils.h"

#if !defined(SUNSHINE_SHADERS_DIR)  // for testing this needs to be defined in cmake as we don't do an install
  #define SUNSHINE_SHADERS_DIR SUNSHINE_ASSETS_DIR "/shaders/directx"
#endif
namespace platf {
  using namespace std::literals;
}

static void free_frame(AVFrame *frame) {
  av_frame_free(&frame);
}

using frame_t = util::safe_ptr<AVFrame, free_frame>;

namespace platf::dxgi {
  namespace {
    constexpr auto slow_wgc_copy_threshold = std::chrono::milliseconds(25);
    constexpr auto slow_wgc_snapshot_threshold = std::chrono::milliseconds(100);

    // Windows can renumber GDI output names when the physical panels are
    // powered off for a stream.  The prepared dual-display pair therefore
    // keeps the provider names as provenance, but capture must resolve the
    // current GDI names from the stable display-device identity before it
    // opens DXGI duplication.
    std::optional<std::string> current_display_name_for_identity(const std::string_view device_id) {
      if (device_id.empty()) {
        return std::nullopt;
      }

      for (const auto &device : display_device::enumerate_devices()) {
        if (device.m_device_id == device_id && !device.m_display_name.empty()) {
          return device.m_display_name;
        }
      }

      return std::nullopt;
    }

    double elapsed_ms(const std::chrono::steady_clock::duration duration) {
      return std::chrono::duration<double, std::milli>(duration).count();
    }
  }  // namespace

  template<class T>
  buf_t make_buffer(device_t::pointer device, const T &t) {
    static_assert(sizeof(T) % 16 == 0, "Buffer needs to be aligned on a 16-byte alignment");

    D3D11_BUFFER_DESC buffer_desc {
      sizeof(T),
      D3D11_USAGE_IMMUTABLE,
      D3D11_BIND_CONSTANT_BUFFER
    };

    D3D11_SUBRESOURCE_DATA init_data {
      &t
    };

    buf_t::pointer buf_p;
    auto status = device->CreateBuffer(&buffer_desc, &init_data, &buf_p);
    if (status) {
      BOOST_LOG(error) << "Failed to create buffer: [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    return buf_t {buf_p};
  }

  blend_t make_blend(device_t::pointer device, bool enable, bool invert) {
    D3D11_BLEND_DESC bdesc {};
    auto &rt = bdesc.RenderTarget[0];
    rt.BlendEnable = enable;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    if (enable) {
      rt.BlendOp = D3D11_BLEND_OP_ADD;
      rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;

      if (invert) {
        // Invert colors
        rt.SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
        rt.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
      } else {
        // Regular alpha blending
        rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
      }

      rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
      rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    }

    blend_t blend;
    auto status = device->CreateBlendState(&bdesc, &blend);
    if (status) {
      BOOST_LOG(error) << "Failed to create blend state: [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    return blend;
  }

  blob_t convert_yuv420_packed_uv_type0_ps_hlsl;
  blob_t convert_yuv420_packed_uv_type0_ps_linear_hlsl;
  blob_t convert_yuv420_packed_uv_type0_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv420_packed_uv_type0_vs_hlsl;
  blob_t convert_yuv420_packed_uv_type0s_ps_hlsl;
  blob_t convert_yuv420_packed_uv_type0s_ps_linear_hlsl;
  blob_t convert_yuv420_packed_uv_type0s_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv420_packed_uv_type0s_vs_hlsl;
  blob_t convert_yuv420_planar_y_ps_hlsl;
  blob_t convert_yuv420_planar_y_ps_linear_hlsl;
  blob_t convert_yuv420_planar_y_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv420_planar_y_vs_hlsl;
  blob_t convert_yuv444_packed_ayuv_ps_hlsl;
  blob_t convert_yuv444_packed_ayuv_ps_linear_hlsl;
  blob_t convert_yuv444_packed_vs_hlsl;
  blob_t convert_yuv444_planar_ps_hlsl;
  blob_t convert_yuv444_planar_ps_linear_hlsl;
  blob_t convert_yuv444_planar_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv444_packed_y410_ps_hlsl;
  blob_t convert_yuv444_packed_y410_ps_linear_hlsl;
  blob_t convert_yuv444_packed_y410_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv444_planar_vs_hlsl;
  blob_t cursor_ps_hlsl;
  blob_t cursor_ps_normalize_white_hlsl;
  blob_t cursor_vs_hlsl;

  struct img_d3d_t: public platf::img_t {
    // These objects are owned by the display_t's ID3D11Device
    texture2d_t capture_texture;
    render_target_t capture_rt;
    keyed_mutex_t capture_mutex;

    // This is the shared handle used by hwdevice_t to open capture_texture
    HANDLE encoder_texture_handle = {};

    // Set to true if the image corresponds to a dummy texture used prior to
    // the first successful capture of a desktop frame
    bool dummy = false;

    // Set to true if the image is blank (contains no content at all, including a cursor)
    bool blank = true;

    // Unique identifier for this image
    uint32_t id = 0;

    // DXGI format of this image texture
    DXGI_FORMAT format;

    virtual ~img_d3d_t() override {
      if (encoder_texture_handle) {
        CloseHandle(encoder_texture_handle);
      }
    };
  };

  struct texture_lock_helper {
    keyed_mutex_t _mutex;
    bool _locked = false;

    texture_lock_helper(const texture_lock_helper &) = delete;
    texture_lock_helper &operator=(const texture_lock_helper &) = delete;

    texture_lock_helper(texture_lock_helper &&other) {
      _mutex.reset(other._mutex.release());
      _locked = other._locked;
      other._locked = false;
    }

    texture_lock_helper &operator=(texture_lock_helper &&other) {
      if (_locked) {
        _mutex->ReleaseSync(0);
      }
      _mutex.reset(other._mutex.release());
      _locked = other._locked;
      other._locked = false;
      return *this;
    }

    texture_lock_helper(IDXGIKeyedMutex *mutex):
        _mutex(mutex) {
      if (_mutex) {
        _mutex->AddRef();
      }
    }

    ~texture_lock_helper() {
      if (_locked) {
        _mutex->ReleaseSync(0);
      }
    }

    bool lock() {
      if (_locked) {
        return true;
      }
      HRESULT status = _mutex->AcquireSync(0, INFINITE);
      if (status == S_OK) {
        _locked = true;
      } else {
        BOOST_LOG(error) << "Failed to acquire texture mutex [0x"sv << util::hex(status).to_string_view() << ']';
      }
      return _locked;
    }
  };

  util::buffer_t<std::uint8_t> make_cursor_xor_image(const util::buffer_t<std::uint8_t> &img_data, DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info) {
    constexpr std::uint32_t inverted = 0xFFFFFFFF;
    constexpr std::uint32_t transparent = 0;

    switch (shape_info.Type) {
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
        // This type doesn't require any XOR-blending
        return {};
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
        {
          util::buffer_t<std::uint8_t> cursor_img = img_data;
          std::for_each((std::uint32_t *) std::begin(cursor_img), (std::uint32_t *) std::end(cursor_img), [](auto &pixel) {
            auto alpha = (std::uint8_t) ((pixel >> 24) & 0xFF);
            if (alpha == 0xFF) {
              // Pixels with 0xFF alpha will be XOR-blended as is.
            } else if (alpha == 0x00) {
              // Pixels with 0x00 alpha will be blended by make_cursor_alpha_image().
              // We make them transparent for the XOR-blended cursor image.
              pixel = transparent;
            } else {
              // Other alpha values are illegal in masked color cursors
              BOOST_LOG(warning) << "Illegal alpha value in masked color cursor: " << alpha;
            }
          });
          return cursor_img;
        }
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
        // Monochrome is handled below
        break;
      default:
        BOOST_LOG(error) << "Invalid cursor shape type: " << shape_info.Type;
        return {};
    }

    shape_info.Height /= 2;

    util::buffer_t<std::uint8_t> cursor_img {shape_info.Width * shape_info.Height * 4};

    auto bytes = shape_info.Pitch * shape_info.Height;
    auto pixel_begin = (std::uint32_t *) std::begin(cursor_img);
    auto pixel_data = pixel_begin;
    auto and_mask = std::begin(img_data);
    auto xor_mask = std::begin(img_data) + bytes;

    for (auto x = 0; x < bytes; ++x) {
      for (auto c = 7; c >= 0 && ((std::uint8_t *) pixel_data) != std::end(cursor_img); --c) {
        auto bit = 1 << c;
        auto color_type = ((*and_mask & bit) ? 1 : 0) + ((*xor_mask & bit) ? 2 : 0);

        switch (color_type) {
          case 0:  // Opaque black (handled by alpha-blending)
          case 2:  // Opaque white (handled by alpha-blending)
          case 1:  // Color of screen (transparent)
            *pixel_data = transparent;
            break;
          case 3:  // Inverse of screen
            *pixel_data = inverted;
            break;
        }

        ++pixel_data;
      }
      ++and_mask;
      ++xor_mask;
    }

    return cursor_img;
  }

  util::buffer_t<std::uint8_t> make_cursor_alpha_image(const util::buffer_t<std::uint8_t> &img_data, DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info) {
    constexpr std::uint32_t black = 0xFF000000;
    constexpr std::uint32_t white = 0xFFFFFFFF;
    constexpr std::uint32_t transparent = 0;

    switch (shape_info.Type) {
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
        {
          util::buffer_t<std::uint8_t> cursor_img = img_data;
          std::for_each((std::uint32_t *) std::begin(cursor_img), (std::uint32_t *) std::end(cursor_img), [](auto &pixel) {
            auto alpha = (std::uint8_t) ((pixel >> 24) & 0xFF);
            if (alpha == 0xFF) {
              // Pixels with 0xFF alpha will be XOR-blended by make_cursor_xor_image().
              // We make them transparent for the alpha-blended cursor image.
              pixel = transparent;
            } else if (alpha == 0x00) {
              // Pixels with 0x00 alpha will be blended as opaque with the alpha-blended image.
              pixel |= 0xFF000000;
            } else {
              // Other alpha values are illegal in masked color cursors
              BOOST_LOG(warning) << "Illegal alpha value in masked color cursor: " << alpha;
            }
          });
          return cursor_img;
        }
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
        // Color cursors are just an ARGB bitmap which requires no processing.
        return img_data;
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
        // Monochrome cursors are handled below.
        break;
      default:
        BOOST_LOG(error) << "Invalid cursor shape type: " << shape_info.Type;
        return {};
    }

    shape_info.Height /= 2;

    util::buffer_t<std::uint8_t> cursor_img {shape_info.Width * shape_info.Height * 4};

    auto bytes = shape_info.Pitch * shape_info.Height;
    auto pixel_begin = (std::uint32_t *) std::begin(cursor_img);
    auto pixel_data = pixel_begin;
    auto and_mask = std::begin(img_data);
    auto xor_mask = std::begin(img_data) + bytes;

    for (auto x = 0; x < bytes; ++x) {
      for (auto c = 7; c >= 0 && ((std::uint8_t *) pixel_data) != std::end(cursor_img); --c) {
        auto bit = 1 << c;
        auto color_type = ((*and_mask & bit) ? 1 : 0) + ((*xor_mask & bit) ? 2 : 0);

        switch (color_type) {
          case 0:  // Opaque black
            *pixel_data = black;
            break;
          case 2:  // Opaque white
            *pixel_data = white;
            break;
          case 3:  // Inverse of screen (handled by XOR blending)
          case 1:  // Color of screen (transparent)
            *pixel_data = transparent;
            break;
        }

        ++pixel_data;
      }
      ++and_mask;
      ++xor_mask;
    }

    return cursor_img;
  }

  blob_t compile_shader(LPCSTR file, LPCSTR entrypoint, LPCSTR shader_model) {
    blob_t::pointer msg_p = nullptr;
    blob_t::pointer compiled_p;

    DWORD flags = D3DCOMPILE_ENABLE_STRICTNESS;

#ifndef NDEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    auto wFile = utf_utils::from_utf8(file);
    auto status = D3DCompileFromFile(wFile.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entrypoint, shader_model, flags, 0, &compiled_p, &msg_p);

    if (msg_p) {
      BOOST_LOG(warning) << std::string_view {(const char *) msg_p->GetBufferPointer(), msg_p->GetBufferSize() - 1};
      msg_p->Release();
    }

    if (status) {
      BOOST_LOG(error) << "Couldn't compile ["sv << file << "] [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    return blob_t {compiled_p};
  }

  blob_t compile_pixel_shader(LPCSTR file) {
    return compile_shader(file, "main_ps", "ps_5_0");
  }

  blob_t compile_vertex_shader(LPCSTR file) {
    return compile_shader(file, "main_vs", "vs_5_0");
  }

  class d3d_base_encode_device final {
  public:
    int convert(platf::img_t &img_base) {
      // Garbage collect mapped capture images whose weak references have expired
      for (auto it = img_ctx_map.begin(); it != img_ctx_map.end();) {
        if (it->second.img_weak.expired()) {
          it = img_ctx_map.erase(it);
        } else {
          it++;
        }
      }

      auto &img = (img_d3d_t &) img_base;
      if (!img.blank) {
        auto &img_ctx = img_ctx_map[img.id];

        // Open the shared capture texture with our ID3D11Device
        if (initialize_image_context(img, img_ctx)) {
          return -1;
        }

        // Acquire encoder mutex to synchronize with capture code
        auto status = img_ctx.encoder_mutex->AcquireSync(0, INFINITE);
        if (status != S_OK) {
          BOOST_LOG(error) << "Failed to acquire encoder mutex [0x"sv << util::hex(status).to_string_view() << ']';
          return -1;
        }

        auto draw = [&](auto &input, auto &y_or_yuv_viewports, auto &uv_viewport) {
          device_ctx->PSSetShaderResources(0, 1, &input);

          // Draw Y/YUV
          device_ctx->OMSetRenderTargets(1, &out_Y_or_YUV_rtv, nullptr);
          device_ctx->VSSetShader(convert_Y_or_YUV_vs.get(), nullptr, 0);
          device_ctx->PSSetShader(img.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? convert_Y_or_YUV_fp16_ps.get() : convert_Y_or_YUV_ps.get(), nullptr, 0);
          auto viewport_count = (format == DXGI_FORMAT_R16_UINT) ? 3 : 1;
          assert(viewport_count <= y_or_yuv_viewports.size());
          device_ctx->RSSetViewports(viewport_count, y_or_yuv_viewports.data());
          device_ctx->Draw(3 * viewport_count, 0);  // vertex shader will spread vertices across viewports

          // Draw UV if needed
          if (out_UV_rtv) {
            assert(format == DXGI_FORMAT_NV12 || format == DXGI_FORMAT_P010);
            device_ctx->OMSetRenderTargets(1, &out_UV_rtv, nullptr);
            device_ctx->VSSetShader(convert_UV_vs.get(), nullptr, 0);
            device_ctx->PSSetShader(img.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? convert_UV_fp16_ps.get() : convert_UV_ps.get(), nullptr, 0);
            device_ctx->RSSetViewports(1, &uv_viewport);
            device_ctx->Draw(3, 0);
          }
        };

        // Clear render target view(s) once so that the aspect ratio mismatch "bars" appear black
        if (!rtvs_cleared) {
          auto black = create_black_texture_for_rtv_clear();
          if (black) {
            draw(black, out_Y_or_YUV_viewports_for_clear, out_UV_viewport_for_clear);
          }
          rtvs_cleared = true;
        }

        // Draw captured frame
        draw(img_ctx.encoder_input_res, out_Y_or_YUV_viewports, out_UV_viewport);

        // Release encoder mutex to allow capture code to reuse this image
        img_ctx.encoder_mutex->ReleaseSync(0);

        ID3D11ShaderResourceView *emptyShaderResourceView = nullptr;
        device_ctx->PSSetShaderResources(0, 1, &emptyShaderResourceView);
      }

      return 0;
    }

    void apply_colorspace(const ::video::sunshine_colorspace_t &colorspace) {
      auto color_vectors = ::video::color_vectors_from_colorspace(colorspace, true);

      if (format == DXGI_FORMAT_AYUV ||
          format == DXGI_FORMAT_R16_UINT ||
          format == DXGI_FORMAT_Y410) {
        color_vectors = ::video::color_vectors_from_colorspace(colorspace, false);
      }

      if (!color_vectors) {
        BOOST_LOG(error) << "No vector data for colorspace"sv;
        return;
      }

      auto color_matrix = make_buffer(device.get(), *color_vectors);
      if (!color_matrix) {
        BOOST_LOG(warning) << "Failed to create color matrix"sv;
        return;
      }

      device_ctx->VSSetConstantBuffers(3, 1, &color_matrix);
      device_ctx->PSSetConstantBuffers(0, 1, &color_matrix);
      this->color_matrix = std::move(color_matrix);
    }

    int init_output(ID3D11Texture2D *frame_texture, int width, int height) {
      // The underlying frame pool owns the texture, so we must reference it for ourselves
      frame_texture->AddRef();
      output_texture.reset(frame_texture);

      HRESULT status = S_OK;

#define create_vertex_shader_helper(x, y) \
  if (FAILED(status = device->CreateVertexShader(x->GetBufferPointer(), x->GetBufferSize(), nullptr, &y))) { \
    BOOST_LOG(error) << "Failed to create vertex shader " << #x << ": " << util::log_hex(status); \
    return -1; \
  }
#define create_pixel_shader_helper(x, y) \
  if (FAILED(status = device->CreatePixelShader(x->GetBufferPointer(), x->GetBufferSize(), nullptr, &y))) { \
    BOOST_LOG(error) << "Failed to create pixel shader " << #x << ": " << util::log_hex(status); \
    return -1; \
  }

      const bool downscaling = display->width > width || display->height > height;

      switch (format) {
        case DXGI_FORMAT_NV12:
          // Semi-planar 8-bit YUV 4:2:0
          create_vertex_shader_helper(convert_yuv420_planar_y_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv420_planar_y_ps_hlsl, convert_Y_or_YUV_ps);
          create_pixel_shader_helper(convert_yuv420_planar_y_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
          if (downscaling) {
            create_vertex_shader_helper(convert_yuv420_packed_uv_type0s_vs_hlsl, convert_UV_vs);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_hlsl, convert_UV_ps);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_linear_hlsl, convert_UV_fp16_ps);
          } else {
            create_vertex_shader_helper(convert_yuv420_packed_uv_type0_vs_hlsl, convert_UV_vs);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_hlsl, convert_UV_ps);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_linear_hlsl, convert_UV_fp16_ps);
          }
          break;

        case DXGI_FORMAT_P010:
          // Semi-planar 16-bit YUV 4:2:0, 10 most significant bits store the value
          create_vertex_shader_helper(convert_yuv420_planar_y_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv420_planar_y_ps_hlsl, convert_Y_or_YUV_ps);
          if (display->is_hdr()) {
            create_pixel_shader_helper(convert_yuv420_planar_y_ps_perceptual_quantizer_hlsl, convert_Y_or_YUV_fp16_ps);
          } else {
            create_pixel_shader_helper(convert_yuv420_planar_y_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
          }
          if (downscaling) {
            create_vertex_shader_helper(convert_yuv420_packed_uv_type0s_vs_hlsl, convert_UV_vs);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_hlsl, convert_UV_ps);
            if (display->is_hdr()) {
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_perceptual_quantizer_hlsl, convert_UV_fp16_ps);
            } else {
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_linear_hlsl, convert_UV_fp16_ps);
            }
          } else {
            create_vertex_shader_helper(convert_yuv420_packed_uv_type0_vs_hlsl, convert_UV_vs);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_hlsl, convert_UV_ps);
            if (display->is_hdr()) {
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_perceptual_quantizer_hlsl, convert_UV_fp16_ps);
            } else {
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_linear_hlsl, convert_UV_fp16_ps);
            }
          }
          break;

        case DXGI_FORMAT_R16_UINT:
          // Planar 16-bit YUV 4:4:4, 10 most significant bits store the value
          create_vertex_shader_helper(convert_yuv444_planar_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv444_planar_ps_hlsl, convert_Y_or_YUV_ps);
          if (display->is_hdr()) {
            create_pixel_shader_helper(convert_yuv444_planar_ps_perceptual_quantizer_hlsl, convert_Y_or_YUV_fp16_ps);
          } else {
            create_pixel_shader_helper(convert_yuv444_planar_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
          }
          break;

        case DXGI_FORMAT_AYUV:
          // Packed 8-bit YUV 4:4:4
          create_vertex_shader_helper(convert_yuv444_packed_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv444_packed_ayuv_ps_hlsl, convert_Y_or_YUV_ps);
          create_pixel_shader_helper(convert_yuv444_packed_ayuv_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
          break;

        case DXGI_FORMAT_Y410:
          // Packed 10-bit YUV 4:4:4
          create_vertex_shader_helper(convert_yuv444_packed_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv444_packed_y410_ps_hlsl, convert_Y_or_YUV_ps);
          if (display->is_hdr()) {
            create_pixel_shader_helper(convert_yuv444_packed_y410_ps_perceptual_quantizer_hlsl, convert_Y_or_YUV_fp16_ps);
          } else {
            create_pixel_shader_helper(convert_yuv444_packed_y410_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
          }
          break;

        default:
          BOOST_LOG(error) << "Unable to create shaders because of the unrecognized surface format";
          return -1;
      }

#undef create_vertex_shader_helper
#undef create_pixel_shader_helper

      auto out_width = width;
      auto out_height = height;

      float in_width = display->width;
      float in_height = display->height;

      // Ensure aspect ratio is maintained
      auto scalar = std::fminf(out_width / in_width, out_height / in_height);
      auto out_width_f = in_width * scalar;
      auto out_height_f = in_height * scalar;

      // result is always positive
      auto offsetX = (out_width - out_width_f) / 2;
      auto offsetY = (out_height - out_height_f) / 2;

      out_Y_or_YUV_viewports[0] = {offsetX, offsetY, out_width_f, out_height_f, 0.0f, 1.0f};  // Y plane
      out_Y_or_YUV_viewports[1] = out_Y_or_YUV_viewports[0];  // U plane
      out_Y_or_YUV_viewports[1].TopLeftY += out_height;
      out_Y_or_YUV_viewports[2] = out_Y_or_YUV_viewports[1];  // V plane
      out_Y_or_YUV_viewports[2].TopLeftY += out_height;

      out_Y_or_YUV_viewports_for_clear[0] = {0, 0, (float) out_width, (float) out_height, 0.0f, 1.0f};  // Y plane
      out_Y_or_YUV_viewports_for_clear[1] = out_Y_or_YUV_viewports_for_clear[0];  // U plane
      out_Y_or_YUV_viewports_for_clear[1].TopLeftY += out_height;
      out_Y_or_YUV_viewports_for_clear[2] = out_Y_or_YUV_viewports_for_clear[1];  // V plane
      out_Y_or_YUV_viewports_for_clear[2].TopLeftY += out_height;

      out_UV_viewport = {offsetX / 2, offsetY / 2, out_width_f / 2, out_height_f / 2, 0.0f, 1.0f};
      out_UV_viewport_for_clear = {0, 0, (float) out_width / 2, (float) out_height / 2, 0.0f, 1.0f};

      float subsample_offset_in[16 / sizeof(float)] {1.0f / (float) out_width_f, 1.0f / (float) out_height_f};  // aligned to 16-byte
      subsample_offset = make_buffer(device.get(), subsample_offset_in);

      if (!subsample_offset) {
        BOOST_LOG(error) << "Failed to create subsample offset vertex constant buffer";
        return -1;
      }
      device_ctx->VSSetConstantBuffers(0, 1, &subsample_offset);

      {
        int32_t rotation_modifier = display->display_rotation == DXGI_MODE_ROTATION_UNSPECIFIED ? 0 : display->display_rotation - 1;
        int32_t rotation_data[16 / sizeof(int32_t)] {-rotation_modifier};  // aligned to 16-byte
        auto rotation = make_buffer(device.get(), rotation_data);
        if (!rotation) {
          BOOST_LOG(error) << "Failed to create display rotation vertex constant buffer";
          return -1;
        }
        device_ctx->VSSetConstantBuffers(1, 1, &rotation);
      }

      DXGI_FORMAT rtv_Y_or_YUV_format = DXGI_FORMAT_UNKNOWN;
      DXGI_FORMAT rtv_UV_format = DXGI_FORMAT_UNKNOWN;
      bool rtv_simple_clear = false;

      switch (format) {
        case DXGI_FORMAT_NV12:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R8_UNORM;
          rtv_UV_format = DXGI_FORMAT_R8G8_UNORM;
          rtv_simple_clear = true;
          break;

        case DXGI_FORMAT_P010:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R16_UNORM;
          rtv_UV_format = DXGI_FORMAT_R16G16_UNORM;
          rtv_simple_clear = true;
          break;

        case DXGI_FORMAT_AYUV:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R8G8B8A8_UINT;
          break;

        case DXGI_FORMAT_R16_UINT:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R16_UINT;
          break;

        case DXGI_FORMAT_Y410:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R10G10B10A2_UINT;
          break;

        default:
          BOOST_LOG(error) << "Unable to create render target views because of the unrecognized surface format";
          return -1;
      }

      auto create_rtv = [&](auto &rt, DXGI_FORMAT rt_format) -> bool {
        D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
        rtv_desc.Format = rt_format;
        rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

        auto status = device->CreateRenderTargetView(output_texture.get(), &rtv_desc, &rt);
        if (FAILED(status)) {
          BOOST_LOG(error) << "Failed to create render target view: " << util::log_hex(status);
          return false;
        }

        return true;
      };

      // Create Y/YUV render target view
      if (!create_rtv(out_Y_or_YUV_rtv, rtv_Y_or_YUV_format)) {
        return -1;
      }

      // Create UV render target view if needed
      if (rtv_UV_format != DXGI_FORMAT_UNKNOWN && !create_rtv(out_UV_rtv, rtv_UV_format)) {
        return -1;
      }

      if (rtv_simple_clear) {
        // Clear the RTVs to ensure the aspect ratio padding is black
        const float y_black[] = {0.0f, 0.0f, 0.0f, 0.0f};
        device_ctx->ClearRenderTargetView(out_Y_or_YUV_rtv.get(), y_black);
        if (out_UV_rtv) {
          const float uv_black[] = {0.5f, 0.5f, 0.5f, 0.5f};
          device_ctx->ClearRenderTargetView(out_UV_rtv.get(), uv_black);
        }
        rtvs_cleared = true;
      } else {
        // Can't use ClearRenderTargetView(), will clear on first convert()
        rtvs_cleared = false;
      }

      return 0;
    }

    int init(std::shared_ptr<platf::display_t> display, adapter_t::pointer adapter_p, pix_fmt_e pix_fmt) {
      switch (pix_fmt) {
        case pix_fmt_e::nv12:
          format = DXGI_FORMAT_NV12;
          break;

        case pix_fmt_e::p010:
          format = DXGI_FORMAT_P010;
          break;

        case pix_fmt_e::ayuv:
          format = DXGI_FORMAT_AYUV;
          break;

        case pix_fmt_e::yuv444p16:
          format = DXGI_FORMAT_R16_UINT;
          break;

        case pix_fmt_e::y410:
          format = DXGI_FORMAT_Y410;
          break;

        default:
          BOOST_LOG(error) << "D3D11 backend doesn't support pixel format: " << from_pix_fmt(pix_fmt);
          return -1;
      }

      D3D_FEATURE_LEVEL featureLevels[] {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1
      };

      HRESULT status = D3D11CreateDevice(
        adapter_p,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_FLAGS | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        featureLevels,
        sizeof(featureLevels) / sizeof(D3D_FEATURE_LEVEL),
        D3D11_SDK_VERSION,
        &device,
        nullptr,
        &device_ctx
      );

      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create encoder D3D11 device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      dxgi::dxgi_t dxgi;
      status = device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      status = dxgi->SetGPUThreadPriority(7);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to increase encoding GPU thread priority. Please run application as administrator for optimal performance.";
      }

      auto default_color_vectors = ::video::color_vectors_from_colorspace({::video::colorspace_e::rec601, false, 8}, true);
      if (!default_color_vectors) {
        BOOST_LOG(error) << "Missing color vectors for Rec. 601"sv;
        return -1;
      }

      color_matrix = make_buffer(device.get(), *default_color_vectors);
      if (!color_matrix) {
        BOOST_LOG(error) << "Failed to create color matrix buffer"sv;
        return -1;
      }
      device_ctx->VSSetConstantBuffers(3, 1, &color_matrix);
      device_ctx->PSSetConstantBuffers(0, 1, &color_matrix);

      this->display = std::dynamic_pointer_cast<display_base_t>(display);
      if (!this->display) {
        return -1;
      }
      display = nullptr;

      blend_disable = make_blend(device.get(), false, false);
      if (!blend_disable) {
        return -1;
      }

      D3D11_SAMPLER_DESC sampler_desc {};
      sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
      sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
      sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
      sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
      sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
      sampler_desc.MinLOD = 0;
      sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

      status = device->CreateSamplerState(&sampler_desc, &sampler_linear);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create point sampler state [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0xFFFFFFFFu);
      device_ctx->PSSetSamplers(0, 1, &sampler_linear);
      device_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

      return 0;
    }

    struct encoder_img_ctx_t {
      // Used to determine if the underlying texture changes.
      // Not safe for actual use by the encoder!
      texture2d_t::const_pointer capture_texture_p;

      texture2d_t encoder_texture;
      shader_res_t encoder_input_res;
      keyed_mutex_t encoder_mutex;

      std::weak_ptr<const platf::img_t> img_weak;

      void reset() {
        capture_texture_p = nullptr;
        encoder_texture.reset();
        encoder_input_res.reset();
        encoder_mutex.reset();
        img_weak.reset();
      }
    };

    int initialize_image_context(const img_d3d_t &img, encoder_img_ctx_t &img_ctx) {
      // If we've already opened the shared texture, we're done
      if (img_ctx.encoder_texture && img.capture_texture.get() == img_ctx.capture_texture_p) {
        return 0;
      }

      // Reset this image context in case it was used before with a different texture.
      // Textures can change when transitioning from a dummy image to a real image.
      img_ctx.reset();

      device1_t device1;
      auto status = device->QueryInterface(__uuidof(ID3D11Device1), (void **) &device1);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to query ID3D11Device1 [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // Open a handle to the shared texture
      status = device1->OpenSharedResource1(img.encoder_texture_handle, __uuidof(ID3D11Texture2D), (void **) &img_ctx.encoder_texture);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to open shared image texture [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // Get the keyed mutex to synchronize with the capture code
      status = img_ctx.encoder_texture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **) &img_ctx.encoder_mutex);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to query IDXGIKeyedMutex [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // Create the SRV for the encoder texture
      status = device->CreateShaderResourceView(img_ctx.encoder_texture.get(), nullptr, &img_ctx.encoder_input_res);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create shader resource view for encoding [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      img_ctx.capture_texture_p = img.capture_texture.get();

      img_ctx.img_weak = img.weak_from_this();

      return 0;
    }

    shader_res_t create_black_texture_for_rtv_clear() {
      constexpr auto width = 32;
      constexpr auto height = 32;

      D3D11_TEXTURE2D_DESC texture_desc = {};
      texture_desc.Width = width;
      texture_desc.Height = height;
      texture_desc.MipLevels = 1;
      texture_desc.ArraySize = 1;
      texture_desc.SampleDesc.Count = 1;
      texture_desc.Usage = D3D11_USAGE_IMMUTABLE;
      texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

      std::vector<uint8_t> mem(4 * width * height, 0);
      D3D11_SUBRESOURCE_DATA texture_data = {mem.data(), 4 * width, 0};

      texture2d_t texture;
      auto status = device->CreateTexture2D(&texture_desc, &texture_data, &texture);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create black texture: " << util::log_hex(status);
        return {};
      }

      shader_res_t resource_view;
      status = device->CreateShaderResourceView(texture.get(), nullptr, &resource_view);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create black texture resource view: " << util::log_hex(status);
        return {};
      }

      return resource_view;
    }

    ::video::color_t *color_p;

    buf_t subsample_offset;
    buf_t color_matrix;

    blend_t blend_disable;
    sampler_state_t sampler_linear;

    render_target_t out_Y_or_YUV_rtv;
    render_target_t out_UV_rtv;
    bool rtvs_cleared = false;

    // d3d_img_t::id -> encoder_img_ctx_t
    // These store the encoder textures for each img_t that passes through
    // convert(). We can't store them in the img_t itself because it is shared
    // amongst multiple hwdevice_t objects (and therefore multiple ID3D11Devices).
    std::map<uint32_t, encoder_img_ctx_t> img_ctx_map;

    std::shared_ptr<display_base_t> display;

    vs_t convert_Y_or_YUV_vs;
    ps_t convert_Y_or_YUV_ps;
    ps_t convert_Y_or_YUV_fp16_ps;

    vs_t convert_UV_vs;
    ps_t convert_UV_ps;
    ps_t convert_UV_fp16_ps;

    std::array<D3D11_VIEWPORT, 3> out_Y_or_YUV_viewports;
    std::array<D3D11_VIEWPORT, 3> out_Y_or_YUV_viewports_for_clear;
    D3D11_VIEWPORT out_UV_viewport;
    D3D11_VIEWPORT out_UV_viewport_for_clear;

    DXGI_FORMAT format;

    device_t device;
    device_ctx_t device_ctx;

    texture2d_t output_texture;
  };

  class d3d_avcodec_encode_device_t: public avcodec_encode_device_t {
  public:
    int init(std::shared_ptr<platf::display_t> display, adapter_t::pointer adapter_p, pix_fmt_e pix_fmt) {
      int result = base.init(display, adapter_p, pix_fmt);
      data = base.device.get();
      return result;
    }

    int convert(platf::img_t &img_base) override {
      return base.convert(img_base);
    }

    void apply_colorspace() override {
      base.apply_colorspace(colorspace);
    }

    void init_hwframes(AVHWFramesContext *frames) override {
      // We may be called with a QSV or D3D11VA context
      if (frames->device_ctx->type == AV_HWDEVICE_TYPE_D3D11VA) {
        auto d3d11_frames = (AVD3D11VAFramesContext *) frames->hwctx;

        // The encoder requires textures with D3D11_BIND_RENDER_TARGET set
        d3d11_frames->BindFlags = D3D11_BIND_RENDER_TARGET;
        d3d11_frames->MiscFlags = 0;
      }

      // We require a single texture
      frames->initial_pool_size = 1;
    }

    int prepare_to_derive_context(int hw_device_type) override {
      // QuickSync requires our device to be multithread-protected
      if (hw_device_type == AV_HWDEVICE_TYPE_QSV) {
        multithread_t mt;

        auto status = base.device->QueryInterface(IID_ID3D11Multithread, (void **) &mt);
        if (FAILED(status)) {
          BOOST_LOG(warning) << "Failed to query ID3D11Multithread interface from device [0x"sv << util::hex(status).to_string_view() << ']';
          return -1;
        }

        mt->SetMultithreadProtected(TRUE);
      }

      return 0;
    }

    int set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx) override {
      this->hwframe.reset(frame);
      this->frame = frame;

      // Populate this frame with a hardware buffer if one isn't there already
      if (!frame->buf[0]) {
        auto err = av_hwframe_get_buffer(hw_frames_ctx, frame, 0);
        if (err) {
          char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
          BOOST_LOG(error) << "Failed to get hwframe buffer: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);
          return -1;
        }
      }

      // If this is a frame from a derived context, we'll need to map it to D3D11
      ID3D11Texture2D *frame_texture;
      if (frame->format != AV_PIX_FMT_D3D11) {
        frame_t d3d11_frame {av_frame_alloc()};

        d3d11_frame->format = AV_PIX_FMT_D3D11;

        auto err = av_hwframe_map(d3d11_frame.get(), frame, AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_OVERWRITE);
        if (err) {
          char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
          BOOST_LOG(error) << "Failed to map D3D11 frame: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);
          return -1;
        }

        // Get the texture from the mapped frame
        frame_texture = (ID3D11Texture2D *) d3d11_frame->data[0];
      } else {
        // Otherwise, we can just use the texture inside the original frame
        frame_texture = (ID3D11Texture2D *) frame->data[0];
      }

      return base.init_output(frame_texture, frame->width, frame->height);
    }

  private:
    d3d_base_encode_device base;
    frame_t hwframe;
  };

  class d3d_nvenc_encode_device_t: public nvenc_encode_device_t {
  public:
    bool init_device(std::shared_ptr<platf::display_t> display, adapter_t::pointer adapter_p, pix_fmt_e pix_fmt) {
      buffer_format = nvenc::nvenc_format_from_sunshine_format(pix_fmt);
      if (buffer_format == NV_ENC_BUFFER_FORMAT_UNDEFINED) {
        BOOST_LOG(error) << "Unexpected pixel format for NvENC ["sv << from_pix_fmt(pix_fmt) << ']';
        return false;
      }

      if (base.init(display, adapter_p, pix_fmt)) {
        return false;
      }

      if (pix_fmt == pix_fmt_e::yuv444p16) {
        nvenc_d3d = std::make_unique<nvenc::nvenc_d3d11_on_cuda>(base.device.get());
      } else {
        nvenc_d3d = std::make_unique<nvenc::nvenc_d3d11_native>(base.device.get());
      }
      nvenc = nvenc_d3d.get();

      return true;
    }

    bool init_encoder(const ::video::config_t &client_config, const ::video::sunshine_colorspace_t &colorspace) override {
      if (!nvenc_d3d) {
        return false;
      }

      auto nvenc_colorspace = nvenc::nvenc_colorspace_from_sunshine_colorspace(colorspace);
      if (!nvenc_d3d->create_encoder(config::video.nv, client_config, nvenc_colorspace, buffer_format)) {
        return false;
      }

      base.apply_colorspace(colorspace);
      return base.init_output(nvenc_d3d->get_input_texture(), client_config.width, client_config.height) == 0;
    }

    int convert(platf::img_t &img_base) override {
      return base.convert(img_base);
    }

  private:
    d3d_base_encode_device base;
    std::unique_ptr<nvenc::nvenc_d3d11> nvenc_d3d;
    NV_ENC_BUFFER_FORMAT buffer_format = NV_ENC_BUFFER_FORMAT_UNDEFINED;
  };

  bool set_cursor_texture(device_t::pointer device, gpu_cursor_t &cursor, util::buffer_t<std::uint8_t> &&cursor_img, DXGI_OUTDUPL_POINTER_SHAPE_INFO &shape_info) {
    // This cursor image may not be used
    if (cursor_img.size() == 0) {
      cursor.input_res.reset();
      cursor.set_texture(0, 0, nullptr);
      return true;
    }

    D3D11_SUBRESOURCE_DATA data {
      std::begin(cursor_img),
      4 * shape_info.Width,
      0
    };

    // Create texture for cursor
    D3D11_TEXTURE2D_DESC t {};
    t.Width = shape_info.Width;
    t.Height = cursor_img.size() / data.SysMemPitch;
    t.MipLevels = 1;
    t.ArraySize = 1;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_IMMUTABLE;
    t.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    texture2d_t texture;
    auto status = device->CreateTexture2D(&t, &data, &texture);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create mouse texture [0x"sv << util::hex(status).to_string_view() << ']';
      return false;
    }

    // Free resources before allocating on the next line.
    cursor.input_res.reset();
    status = device->CreateShaderResourceView(texture.get(), nullptr, &cursor.input_res);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create cursor shader resource view [0x"sv << util::hex(status).to_string_view() << ']';
      return false;
    }

    cursor.set_texture(t.Width, t.Height, std::move(texture));
    return true;
  }

  capture_e display_ddup_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    HRESULT status;
    DXGI_OUTDUPL_FRAME_INFO frame_info;

    resource_t::pointer res_p {};
    auto capture_status = dup.next_frame(frame_info, timeout, &res_p);
    resource_t res {res_p};

    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    const bool mouse_update_flag = frame_info.LastMouseUpdateTime.QuadPart != 0 || frame_info.PointerShapeBufferSize > 0;
    const bool frame_update_flag = frame_info.LastPresentTime.QuadPart != 0;
    const bool update_flag = mouse_update_flag || frame_update_flag;

    if (!update_flag) {
      return capture_e::timeout;
    }

    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
    if (auto qpc_displayed = std::max(frame_info.LastPresentTime.QuadPart, frame_info.LastMouseUpdateTime.QuadPart)) {
      // Translate QueryPerformanceCounter() value to steady_clock time point
      frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), qpc_displayed);
    }

    if (frame_info.PointerShapeBufferSize > 0) {
      DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info {};

      util::buffer_t<std::uint8_t> img_data {frame_info.PointerShapeBufferSize};

      UINT dummy;
      status = dup.dup->GetFramePointerShape(img_data.size(), std::begin(img_data), &dummy, &shape_info);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to get new pointer shape [0x"sv << util::hex(status).to_string_view() << ']';

        return capture_e::error;
      }

      auto alpha_cursor_img = make_cursor_alpha_image(img_data, shape_info);
      auto xor_cursor_img = make_cursor_xor_image(img_data, shape_info);

      if (!set_cursor_texture(device.get(), cursor_alpha, std::move(alpha_cursor_img), shape_info) ||
          !set_cursor_texture(device.get(), cursor_xor, std::move(xor_cursor_img), shape_info)) {
        return capture_e::error;
      }
    }

    if (frame_info.LastMouseUpdateTime.QuadPart) {
      cursor_alpha.set_pos(frame_info.PointerPosition.Position.x, frame_info.PointerPosition.Position.y, width, height, display_rotation, frame_info.PointerPosition.Visible);

      cursor_xor.set_pos(frame_info.PointerPosition.Position.x, frame_info.PointerPosition.Position.y, width, height, display_rotation, frame_info.PointerPosition.Visible);
    } else {
      // DXGI can start a duplication session with a desktop frame but no
      // pointer-update record. In that case the host cursor is already
      // visible, but the cached GPU cursor position/visibility would remain
      // stale and the cursor could disappear from the stream. Refresh the
      // cached state from the Windows cursor snapshot until DXGI supplies a
      // pointer update.
      CURSORINFO cursor_info {sizeof(CURSORINFO)};
      if (GetCursorInfo(&cursor_info)) {
        const auto cursor_x = cursor_info.ptScreenPos.x - offset_x;
        const auto cursor_y = cursor_info.ptScreenPos.y - offset_y;
        const auto cursor_is_visible = (cursor_info.flags & CURSOR_SHOWING) != 0;
        cursor_alpha.set_pos(cursor_x, cursor_y, width, height, display_rotation, cursor_is_visible);
        cursor_xor.set_pos(cursor_x, cursor_y, width, height, display_rotation, cursor_is_visible);
      }
    }

    const bool blend_mouse_cursor_flag = (cursor_alpha.visible || cursor_xor.visible) && cursor_visible;

    if (!cursor_diagnostic_emitted) {
      CURSORINFO system_cursor {sizeof(CURSORINFO)};
      const bool system_cursor_available = GetCursorInfo(&system_cursor);
      BOOST_LOG(info) << "DDX cursor diagnostic [capture_requested="sv << (cursor_visible ? "yes"sv : "no"sv)
                      << ", dxgi_mouse_update="sv << (frame_info.LastMouseUpdateTime.QuadPart ? "yes"sv : "no"sv)
                      << ", dxgi_shape_bytes="sv << frame_info.PointerShapeBufferSize
                      << ", dxgi_pointer_visible="sv << (frame_info.PointerPosition.Visible ? "yes"sv : "no"sv)
                      << ", system_cursor_available="sv << (system_cursor_available ? "yes"sv : "no"sv)
                      << ", system_cursor_visible="sv << (system_cursor_available && (system_cursor.flags & CURSOR_SHOWING) ? "yes"sv : "no"sv)
                      << ", alpha_texture="sv << (cursor_alpha.texture.get() ? "yes"sv : "no"sv)
                      << ", xor_texture="sv << (cursor_xor.texture.get() ? "yes"sv : "no"sv)
                      << ", cached_visible="sv << ((cursor_alpha.visible || cursor_xor.visible) ? "yes"sv : "no"sv)
                      << ", blend_cursor="sv << (blend_mouse_cursor_flag ? "yes"sv : "no"sv)
                      << ", cursor_x="sv << cursor_alpha.cursor_view.TopLeftX
                      << ", cursor_y="sv << cursor_alpha.cursor_view.TopLeftY
                      << ", cursor_width="sv << cursor_alpha.cursor_view.Width
                      << ", cursor_height="sv << cursor_alpha.cursor_view.Height << ']';
      cursor_diagnostic_emitted = true;
    }

    texture2d_t src {};
    if (frame_update_flag) {
      // Get the texture object from this frame
      status = res->QueryInterface(IID_ID3D11Texture2D, (void **) &src);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't query interface [0x"sv << util::hex(status).to_string_view() << ']';
        return capture_e::error;
      }

      D3D11_TEXTURE2D_DESC desc;
      src->GetDesc(&desc);

      // It's possible for our display enumeration to race with mode changes and result in
      // mismatched image pool and desktop texture sizes. If this happens, just reinit again.
      if (desc.Width != width_before_rotation || desc.Height != height_before_rotation) {
        BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
        return capture_e::reinit;
      }

      // If we don't know the capture format yet, grab it from this texture
      if (capture_format == DXGI_FORMAT_UNKNOWN) {
        capture_format = desc.Format;
        BOOST_LOG(info) << "Capture format ["sv << dxgi_format_to_string(capture_format) << ']';
      }

      // It's also possible for the capture format to change on the fly. If that happens,
      // reinitialize capture to try format detection again and create new images.
      if (capture_format != desc.Format) {
        BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
        return capture_e::reinit;
      }
    }

    enum class lfa {
      nothing,
      replace_surface_with_img,
      replace_img_with_surface,
      copy_src_to_img,
      copy_src_to_surface,
    };

    enum class ofa {
      forward_last_img,
      copy_last_surface_and_blend_cursor,
      dummy_fallback,
    };

    auto last_frame_action = lfa::nothing;
    auto out_frame_action = ofa::dummy_fallback;

    if (capture_format == DXGI_FORMAT_UNKNOWN) {
      // We don't know the final capture format yet, so we will encode a black dummy image
      last_frame_action = lfa::nothing;
      out_frame_action = ofa::dummy_fallback;
    } else {
      if (src) {
        // We got a new frame from DesktopDuplication...
        if (blend_mouse_cursor_flag) {
          // ...and we need to blend the mouse cursor onto it.
          // Copy the frame to intermediate surface so we can blend this and future mouse cursor updates
          // without new frames from DesktopDuplication. We use direct3d surface directly here and not
          // an image from pull_free_image_cb mainly because it's lighter (surface sharing between
          // direct3d devices produce significant memory overhead).
          last_frame_action = lfa::copy_src_to_surface;
          // Copy the intermediate surface to a new image from pull_free_image_cb and blend the mouse cursor onto it.
          out_frame_action = ofa::copy_last_surface_and_blend_cursor;
        } else {
          // ...and we don't need to blend the mouse cursor.
          // Copy the frame to a new image from pull_free_image_cb and save the shared pointer to the image
          // in case the mouse cursor appears without a new frame from DesktopDuplication.
          last_frame_action = lfa::copy_src_to_img;
          // Use saved last image shared pointer as output image evading copy.
          out_frame_action = ofa::forward_last_img;
        }
      } else if (!std::holds_alternative<std::monostate>(last_frame_variant)) {
        // We didn't get a new frame from DesktopDuplication...
        if (blend_mouse_cursor_flag) {
          // ...but we need to blend the mouse cursor.
          if (std::holds_alternative<std::shared_ptr<platf::img_t>>(last_frame_variant)) {
            // We have the shared pointer of the last image, replace it with intermediate surface
            // while copying contents so we can blend this and future mouse cursor updates.
            last_frame_action = lfa::replace_img_with_surface;
          }
          // Copy the intermediate surface which contains last DesktopDuplication frame
          // to a new image from pull_free_image_cb and blend the mouse cursor onto it.
          out_frame_action = ofa::copy_last_surface_and_blend_cursor;
        } else {
          // ...and we don't need to blend the mouse cursor.
          // This happens when the mouse cursor disappears from screen,
          // or there's mouse cursor on screen, but its drawing is disabled in sunshine.
          if (std::holds_alternative<texture2d_t>(last_frame_variant)) {
            // We have the intermediate surface that was used as the mouse cursor blending base.
            // Replace it with an image from pull_free_image_cb copying contents and freeing up the surface memory.
            // Save the shared pointer to the image in case the mouse cursor reappears.
            last_frame_action = lfa::replace_surface_with_img;
          }
          // Use saved last image shared pointer as output image evading copy.
          out_frame_action = ofa::forward_last_img;
        }
      }
    }

    auto create_surface = [&](texture2d_t &surface) -> bool {
      // Try to reuse the old surface if it hasn't been destroyed yet.
      if (old_surface_delayed_destruction) {
        surface.reset(old_surface_delayed_destruction.release());
        return true;
      }

      // Otherwise create a new surface.
      D3D11_TEXTURE2D_DESC t {};
      t.Width = width_before_rotation;
      t.Height = height_before_rotation;
      t.MipLevels = 1;
      t.ArraySize = 1;
      t.SampleDesc.Count = 1;
      t.Usage = D3D11_USAGE_DEFAULT;
      t.Format = capture_format;
      t.BindFlags = 0;
      status = device->CreateTexture2D(&t, nullptr, &surface);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create frame copy texture [0x"sv << util::hex(status).to_string_view() << ']';
        return false;
      }

      return true;
    };

    auto get_locked_d3d_img = [&](std::shared_ptr<platf::img_t> &img, bool dummy = false) -> std::tuple<std::shared_ptr<img_d3d_t>, texture_lock_helper> {
      auto d3d_img = std::static_pointer_cast<img_d3d_t>(img);

      // Finish creating the image (if it hasn't happened already),
      // also creates synchronization primitives for shared access from multiple direct3d devices.
      if (complete_img(d3d_img.get(), dummy)) {
        return {nullptr, nullptr};
      }

      // This image is shared between capture direct3d device and encoders direct3d devices,
      // we must acquire lock before doing anything to it.
      texture_lock_helper lock_helper(d3d_img->capture_mutex.get());
      if (!lock_helper.lock()) {
        BOOST_LOG(error) << "Failed to lock capture texture";
        return {nullptr, nullptr};
      }

      // Clear the blank flag now that we're ready to capture into the image
      d3d_img->blank = false;

      return {std::move(d3d_img), std::move(lock_helper)};
    };

    switch (last_frame_action) {
      case lfa::nothing:
        {
          break;
        }

      case lfa::replace_surface_with_img:
        {
          auto p_surface = std::get_if<texture2d_t>(&last_frame_variant);
          if (!p_surface) {
            BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
            return capture_e::error;
          }

          std::shared_ptr<platf::img_t> img;
          if (!pull_free_image_cb(img)) {
            return capture_e::interrupted;
          }

          auto [d3d_img, lock] = get_locked_d3d_img(img);
          if (!d3d_img) {
            return capture_e::error;
          }

          device_ctx->CopyResource(d3d_img->capture_texture.get(), p_surface->get());

          // We delay the destruction of intermediate surface in case the mouse cursor reappears shortly.
          old_surface_delayed_destruction.reset(p_surface->release());
          old_surface_timestamp = std::chrono::steady_clock::now();

          last_frame_variant = img;
          break;
        }

      case lfa::replace_img_with_surface:
        {
          auto p_img = std::get_if<std::shared_ptr<platf::img_t>>(&last_frame_variant);
          if (!p_img) {
            BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
            return capture_e::error;
          }
          auto [d3d_img, lock] = get_locked_d3d_img(*p_img);
          if (!d3d_img) {
            return capture_e::error;
          }

          p_img = nullptr;
          last_frame_variant = texture2d_t {};
          auto &surface = std::get<texture2d_t>(last_frame_variant);
          if (!create_surface(surface)) {
            return capture_e::error;
          }

          device_ctx->CopyResource(surface.get(), d3d_img->capture_texture.get());
          break;
        }

      case lfa::copy_src_to_img:
        {
          last_frame_variant = {};

          std::shared_ptr<platf::img_t> img;
          if (!pull_free_image_cb(img)) {
            return capture_e::interrupted;
          }

          auto [d3d_img, lock] = get_locked_d3d_img(img);
          if (!d3d_img) {
            return capture_e::error;
          }

          device_ctx->CopyResource(d3d_img->capture_texture.get(), src.get());
          last_frame_variant = img;
          break;
        }

      case lfa::copy_src_to_surface:
        {
          auto p_surface = std::get_if<texture2d_t>(&last_frame_variant);
          if (!p_surface) {
            last_frame_variant = texture2d_t {};
            p_surface = std::get_if<texture2d_t>(&last_frame_variant);
            if (!create_surface(*p_surface)) {
              return capture_e::error;
            }
          }
          device_ctx->CopyResource(p_surface->get(), src.get());
          break;
        }
    }

    auto cursor_region_signature = [&](ID3D11Texture2D *texture) -> std::optional<std::uint64_t> {
      D3D11_TEXTURE2D_DESC source_desc {};
      texture->GetDesc(&source_desc);

      const auto left = std::clamp(static_cast<UINT>(std::max(0.0f, cursor_alpha.cursor_view.TopLeftX)), 0u, source_desc.Width);
      const auto top = std::clamp(static_cast<UINT>(std::max(0.0f, cursor_alpha.cursor_view.TopLeftY)), 0u, source_desc.Height);
      const auto right = std::clamp(left + static_cast<UINT>(cursor_alpha.cursor_view.Width), left, source_desc.Width);
      const auto bottom = std::clamp(top + static_cast<UINT>(cursor_alpha.cursor_view.Height), top, source_desc.Height);
      if (right == left || bottom == top) {
        return std::nullopt;
      }

      D3D11_TEXTURE2D_DESC staging_desc {};
      staging_desc.Width = right - left;
      staging_desc.Height = bottom - top;
      staging_desc.MipLevels = 1;
      staging_desc.ArraySize = 1;
      staging_desc.Format = source_desc.Format;
      staging_desc.SampleDesc.Count = 1;
      staging_desc.Usage = D3D11_USAGE_STAGING;
      staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

      texture2d_t staging;
      auto status = device->CreateTexture2D(&staging_desc, nullptr, &staging);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to create DDX cursor diagnostic staging texture [0x"sv
                           << util::hex(status).to_string_view() << ']';
        return std::nullopt;
      }

      D3D11_BOX source_box {left, top, 0, right, bottom, 1};
      device_ctx->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, texture, 0, &source_box);

      D3D11_MAPPED_SUBRESOURCE mapped {};
      status = device_ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to map DDX cursor diagnostic staging texture [0x"sv
                           << util::hex(status).to_string_view() << ']';
        return std::nullopt;
      }

      constexpr std::uint64_t fnv_offset = 1469598103934665603ULL;
      constexpr std::uint64_t fnv_prime = 1099511628211ULL;
      const auto bytes_per_pixel = source_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
      const auto row_bytes = staging_desc.Width * bytes_per_pixel;
      auto signature = fnv_offset;
      for (UINT row = 0; row < staging_desc.Height; ++row) {
        const auto *row_data = static_cast<const std::uint8_t *>(mapped.pData) + row * mapped.RowPitch;
        for (UINT index = 0; index < row_bytes; ++index) {
          signature ^= row_data[index];
          signature *= fnv_prime;
        }
      }
      device_ctx->Unmap(staging.get(), 0);
      return signature;
    };

    auto blend_cursor = [&](img_d3d_t &d3d_img) {
      device_ctx->VSSetShader(cursor_vs.get(), nullptr, 0);
      device_ctx->PSSetShader(cursor_ps.get(), nullptr, 0);
      device_ctx->OMSetRenderTargets(1, &d3d_img.capture_rt, nullptr);

      if (cursor_alpha.texture.get()) {
        // Perform an alpha blending operation
        device_ctx->OMSetBlendState(blend_alpha.get(), nullptr, 0xFFFFFFFFu);

        device_ctx->PSSetShaderResources(0, 1, &cursor_alpha.input_res);
        device_ctx->RSSetViewports(1, &cursor_alpha.cursor_view);
        device_ctx->Draw(3, 0);
      }

      if (cursor_xor.texture.get()) {
        // Perform an invert blending without touching alpha values
        device_ctx->OMSetBlendState(blend_invert.get(), nullptr, 0x00FFFFFFu);

        device_ctx->PSSetShaderResources(0, 1, &cursor_xor.input_res);
        device_ctx->RSSetViewports(1, &cursor_xor.cursor_view);
        device_ctx->Draw(3, 0);
      }

      device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0xFFFFFFFFu);

      ID3D11RenderTargetView *emptyRenderTarget = nullptr;
      device_ctx->OMSetRenderTargets(1, &emptyRenderTarget, nullptr);
      device_ctx->RSSetViewports(0, nullptr);
      ID3D11ShaderResourceView *emptyShaderResourceView = nullptr;
      device_ctx->PSSetShaderResources(0, 1, &emptyShaderResourceView);
    };

    switch (out_frame_action) {
      case ofa::forward_last_img:
        {
          auto p_img = std::get_if<std::shared_ptr<platf::img_t>>(&last_frame_variant);
          if (!p_img) {
            BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
            return capture_e::error;
          }
          img_out = *p_img;
          break;
        }

      case ofa::copy_last_surface_and_blend_cursor:
        {
          auto p_surface = std::get_if<texture2d_t>(&last_frame_variant);
          if (!p_surface) {
            BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
            return capture_e::error;
          }
          if (!blend_mouse_cursor_flag) {
            BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
            return capture_e::error;
          }

          if (!pull_free_image_cb(img_out)) {
            return capture_e::interrupted;
          }

          auto [d3d_img, lock] = get_locked_d3d_img(img_out);
          if (!d3d_img) {
            return capture_e::error;
          }

          device_ctx->CopyResource(d3d_img->capture_texture.get(), p_surface->get());
          const auto before_cursor_signature = cursor_pixels_diagnostic_emitted ? std::nullopt : cursor_region_signature(d3d_img->capture_texture.get());
          blend_cursor(*d3d_img);
          if (!cursor_pixels_diagnostic_emitted) {
            const auto after_cursor_signature = cursor_region_signature(d3d_img->capture_texture.get());
            BOOST_LOG(info) << "DDX cursor pixel diagnostic [before="sv
                            << (before_cursor_signature ? std::to_string(*before_cursor_signature) : "unavailable")
                            << ", after="sv
                            << (after_cursor_signature ? std::to_string(*after_cursor_signature) : "unavailable")
                            << ", changed="sv
                            << (before_cursor_signature && after_cursor_signature && *before_cursor_signature != *after_cursor_signature ? "yes"sv : "no"sv)
                            << ']';
            cursor_pixels_diagnostic_emitted = true;
          }
          break;
        }

      case ofa::dummy_fallback:
        {
          if (!pull_free_image_cb(img_out)) {
            return capture_e::interrupted;
          }

          // Clear the image if it has been used as a dummy.
          // It can have the mouse cursor blended onto it.
          auto old_d3d_img = (img_d3d_t *) img_out.get();
          bool reclear_dummy = !old_d3d_img->blank && old_d3d_img->capture_texture;

          auto [d3d_img, lock] = get_locked_d3d_img(img_out, true);
          if (!d3d_img) {
            return capture_e::error;
          }

          if (reclear_dummy) {
            const float rgb_black[] = {0.0f, 0.0f, 0.0f, 0.0f};
            device_ctx->ClearRenderTargetView(d3d_img->capture_rt.get(), rgb_black);
          }

          if (blend_mouse_cursor_flag) {
            blend_cursor(*d3d_img);
          }

          break;
        }
    }

    // Perform delayed destruction of the unused surface if the time is due.
    if (old_surface_delayed_destruction && old_surface_timestamp + 10s < std::chrono::steady_clock::now()) {
      old_surface_delayed_destruction.reset();
    }

    if (img_out) {
      img_out->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e display_ddup_vram_t::release_snapshot() {
    return dup.release_frame();
  }

  int display_ddup_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config)) {
      return -1;
    }

    D3D11_SAMPLER_DESC sampler_desc {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    auto status = device->CreateSamplerState(&sampler_desc, &sampler_linear);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create point sampler state [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    status = device->CreateVertexShader(cursor_vs_hlsl->GetBufferPointer(), cursor_vs_hlsl->GetBufferSize(), nullptr, &cursor_vs);
    if (status) {
      BOOST_LOG(error) << "Failed to create scene vertex shader [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    {
      int32_t rotation_modifier = display_rotation == DXGI_MODE_ROTATION_UNSPECIFIED ? 0 : display_rotation - 1;
      int32_t rotation_data[16 / sizeof(int32_t)] {rotation_modifier};  // aligned to 16-byte
      auto rotation = make_buffer(device.get(), rotation_data);
      if (!rotation) {
        BOOST_LOG(error) << "Failed to create display rotation vertex constant buffer";
        return -1;
      }
      device_ctx->VSSetConstantBuffers(2, 1, &rotation);
    }

    if (config.dynamicRange && is_hdr()) {
      // This shader will normalize scRGB white levels to a user-defined white level
      status = device->CreatePixelShader(cursor_ps_normalize_white_hlsl->GetBufferPointer(), cursor_ps_normalize_white_hlsl->GetBufferSize(), nullptr, &cursor_ps);
      if (status) {
        BOOST_LOG(error) << "Failed to create cursor blending (normalized white) pixel shader [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // Use a 300 nit target for the mouse cursor. We should really get
      // the user's SDR white level in nits, but there is no API that
      // provides that information to Win32 apps.
      float white_multiplier_data[16 / sizeof(float)] {300.0f / 80.f};  // aligned to 16-byte
      auto white_multiplier = make_buffer(device.get(), white_multiplier_data);
      if (!white_multiplier) {
        BOOST_LOG(warning) << "Failed to create cursor blending (normalized white) white multiplier constant buffer";
        return -1;
      }

      device_ctx->PSSetConstantBuffers(1, 1, &white_multiplier);
    } else {
      status = device->CreatePixelShader(cursor_ps_hlsl->GetBufferPointer(), cursor_ps_hlsl->GetBufferSize(), nullptr, &cursor_ps);
      if (status) {
        BOOST_LOG(error) << "Failed to create cursor blending pixel shader [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    }

    blend_alpha = make_blend(device.get(), true, false);
    blend_invert = make_blend(device.get(), true, true);
    blend_disable = make_blend(device.get(), false, false);

    if (!blend_disable || !blend_alpha || !blend_invert) {
      return -1;
    }

    device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0xFFFFFFFFu);
    device_ctx->PSSetSamplers(0, 1, &sampler_linear);
    device_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    return 0;
  }

  namespace {

    constexpr std::size_t pair_pane_a = 0;
    constexpr std::size_t pair_pane_b = 1;

    std::uint64_t adapter_identity(const LUID &luid) {
      return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(luid.HighPart)) << 32) |
             static_cast<std::uint64_t>(luid.LowPart);
    }

    bool same_adapter(const LUID &first, const LUID &second) {
      return first.HighPart == second.HighPart && first.LowPart == second.LowPart;
    }

    bool same_rect(const RECT &first, const RECT &second) {
      return first.left == second.left && first.top == second.top &&
             first.right == second.right && first.bottom == second.bottom;
    }

    bool output_is_sdr(output_t &output) {
      output6_t output6;
      const auto status = output->QueryInterface(IID_IDXGIOutput6, (void **) &output6);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Dual D3D11 capture requires IDXGIOutput6 to verify SDR output [0x"sv
                         << util::hex(status).to_string_view() << ']';
        return false;
      }

      DXGI_OUTPUT_DESC1 desc {};
      output6->GetDesc1(&desc);
      return desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    }

    std::optional<std::chrono::steady_clock::time_point> frame_timestamp_from(const DXGI_OUTDUPL_FRAME_INFO &frame_info) {
      if (frame_info.LastPresentTime.QuadPart == 0) {
        return std::nullopt;
      }
      return std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), frame_info.LastPresentTime.QuadPart);
    }

  }  // namespace

  display_ddup_pair_vram_t::~display_ddup_pair_vram_t() {
    (void) release_pair_frames();
  }

  std::vector<DXGI_FORMAT> display_ddup_pair_vram_t::get_supported_capture_formats() {
    return {DXGI_FORMAT_B8G8R8A8_UNORM};
  }

  int display_ddup_pair_vram_t::init(
    const ::video::config_t &config,
    const dual_display_launch::prepared_pair_t &prepared_pair
  ) {
    if (!config.dual_display_pair || config.dual_display_pair.get() != &prepared_pair) {
      BOOST_LOG(error) << "dual_display_pair_owner_mismatch: refusing capture without the exact prepared pair"sv;
      return -1;
    }
    if (const auto failure = dual_display_launch::validate_prepared_pair(prepared_pair)) {
      BOOST_LOG(error) << "dual_display_pair_invalid: "sv << *failure;
      return -1;
    }

    const auto &manifest = prepared_pair.manifest;
    dual_track_capture = config.dual_display_video_tracks &&
                          manifest.video_transport == dual_display_launch::two_video_tracks_transport;
    const auto expected_width = dual_track_capture ? dual_display::pane_width : dual_display::composite_width;
    const auto expected_height = dual_track_capture ? dual_display::pane_height : dual_display::composite_height;
    if (manifest.composite_width != dual_display::composite_width ||
        manifest.composite_height != dual_display::composite_height ||
        manifest.composite_pixel_format != "Bgra8Unorm" ||
        manifest.composite_color_mode != "Sdr" ||
        manifest.layout != "Horizontal" ||
        config.width != expected_width ||
        config.height != expected_height ||
        config.framerate != dual_display::pane_refresh_rate ||
        (config.framerateX100 != 0 && config.framerateX100 != 6000) ||
        config.dynamicRange != 0) {
      BOOST_LOG(error) << "dual_display_v1_profile_unsupported: expected two SDR BGRA8 1920x1080@60 panes"sv;
      return -1;
    }

    prepared_pair_owner = config.dual_display_pair;
    provider_display_names = prepared_pair.provider_display_names;
    capture_display_names = provider_display_names;
    prepared_pair_id = manifest.display_pair_id;
    prepared_pair_generation = manifest.session_generation;
    prepared_topology_fingerprint = manifest.topology_fingerprint;

    for (std::size_t index = 0; index < dual_display::display_pair_size; ++index) {
      const auto &host_identity = manifest.panes[index].host_display_identity;
      if (const auto current_name = current_display_name_for_identity(host_identity)) {
        capture_display_names[index] = *current_name;
        if (capture_display_names[index] != provider_display_names[index]) {
          BOOST_LOG(info) << "dual_display_capture_name_remapped: pane="sv << index
                          << " provider="sv << provider_display_names[index]
                          << " current="sv << capture_display_names[index];
        }
      } else {
        BOOST_LOG(warning) << "dual_display_capture_name_resolution_failed: pane="sv << index
                           << " identity="sv << host_identity
                           << " provider="sv << provider_display_names[index];
      }
    }

    if (display_base_t::init(config, capture_display_names[pair_pane_a])) {
      return -1;
    }

    DXGI_ADAPTER_DESC1 selected_adapter_desc {};
    adapter->GetDesc1(&selected_adapter_desc);
    int first_output_matches = 0;
    int second_output_matches = 0;
    bool first_output_on_other_adapter = false;
    bool second_output_on_other_adapter = false;

    for (UINT adapter_index = 0;; ++adapter_index) {
      adapter_t::pointer candidate_adapter_p {};
      const auto enum_adapter_status = factory->EnumAdapters1(adapter_index, &candidate_adapter_p);
      if (enum_adapter_status == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      if (FAILED(enum_adapter_status)) {
        BOOST_LOG(error) << "Failed to enumerate DXGI adapter for dual capture [0x"sv
                         << util::hex(enum_adapter_status).to_string_view() << ']';
        return -1;
      }

      adapter_t candidate_adapter {candidate_adapter_p};
      DXGI_ADAPTER_DESC1 candidate_adapter_desc {};
      candidate_adapter->GetDesc1(&candidate_adapter_desc);
      const bool selected_adapter = same_adapter(candidate_adapter_desc.AdapterLuid, selected_adapter_desc.AdapterLuid);

      for (UINT output_index = 0;; ++output_index) {
        output_t::pointer candidate_output_p {};
        const auto enum_output_status = candidate_adapter->EnumOutputs(output_index, &candidate_output_p);
        if (enum_output_status == DXGI_ERROR_NOT_FOUND) {
          break;
        }
        if (FAILED(enum_output_status)) {
          BOOST_LOG(error) << "Failed to enumerate DXGI output for dual capture [0x"sv
                           << util::hex(enum_output_status).to_string_view() << ']';
          return -1;
        }

        output_t candidate_output {candidate_output_p};
        DXGI_OUTPUT_DESC candidate_desc {};
        candidate_output->GetDesc(&candidate_desc);
        const auto candidate_name = utf_utils::to_utf8(candidate_desc.DeviceName);
        if (candidate_name == capture_display_names[pair_pane_a]) {
          if (selected_adapter) {
            ++first_output_matches;
          } else {
            first_output_on_other_adapter = true;
          }
        }
        if (candidate_name == capture_display_names[pair_pane_b]) {
          if (selected_adapter) {
            ++second_output_matches;
            if (second_output_matches == 1) {
              second_output = std::move(candidate_output);
              pane_output_desc[pair_pane_b] = candidate_desc;
            }
          } else {
            second_output_on_other_adapter = true;
          }
        }
      }
    }

    if (first_output_matches != 1 || second_output_matches != 1 ||
        first_output_on_other_adapter || second_output_on_other_adapter || !second_output) {
      BOOST_LOG(error) << "dual_display_output_resolution_failed: missing, duplicate, or cross-adapter prepared output"sv;
      return -1;
    }
    if (FAILED(output->GetDesc(&pane_output_desc[pair_pane_a]))) {
      BOOST_LOG(error) << "dual_display_output_a_description_unavailable"sv;
      return -1;
    }

    if (duplications[pair_pane_a].init(this, config)) {
      return -1;
    }

    auto first_output = std::move(output);
    output = std::move(second_output);
    auto restore_first_output = util::fail_guard([&]() {
      second_output = std::move(output);
      output = std::move(first_output);
    });
    if (duplications[pair_pane_b].init(this, config)) {
      return -1;
    }
    second_output = std::move(output);
    output = std::move(first_output);
    restore_first_output.disable();

    std::array<DXGI_OUTDUPL_DESC, dual_display::display_pair_size> duplication_desc {};
    for (std::size_t index = 0; index < dual_display::display_pair_size; ++index) {
      duplications[index].dup->GetDesc(&duplication_desc[index]);
    }

    const auto make_contract = [&](const std::size_t index) {
      const auto &pane = manifest.panes[index];
      return dual_display::pane_contract_t {
        capture_display_names[index],
        static_cast<int>(pane.width),
        static_cast<int>(pane.height),
        static_cast<int>(pane.refresh_rate),
        pane.color_mode == "Sdr",
        pane.pixel_format == "Bgra8Unorm",
        pane.orientation == "Landscape",
      };
    };
    const auto make_observation = [&](const std::size_t index, output_t &pane_output) {
      const auto &mode = duplication_desc[index].ModeDesc;
      return dual_display::pane_observation_t {
        utf_utils::to_utf8(pane_output_desc[index].DeviceName),
        adapter_identity(selected_adapter_desc.AdapterLuid),
        pane_output_desc[index].AttachedToDesktop != FALSE,
        static_cast<int>(mode.Width),
        static_cast<int>(mode.Height),
        static_cast<int>(std::lround(static_cast<double>(mode.RefreshRate.Numerator) / mode.RefreshRate.Denominator)),
        output_is_sdr(pane_output),
        mode.Format == DXGI_FORMAT_B8G8R8A8_UNORM,
        pane_output_desc[index].Rotation == DXGI_MODE_ROTATION_IDENTITY &&
          duplication_desc[index].Rotation == DXGI_MODE_ROTATION_IDENTITY,
      };
    };

    const std::array<dual_display::pane_contract_t, dual_display::display_pair_size> contract {
      make_contract(pair_pane_a),
      make_contract(pair_pane_b),
    };
    const std::array<dual_display::pane_observation_t, dual_display::display_pair_size> observation {
      make_observation(pair_pane_a, output),
      make_observation(pair_pane_b, second_output),
    };
    const auto validation = dual_display::validate_pair_capture(contract, observation);
    if (validation.state != dual_display::pair_validation_e::accepted) {
      BOOST_LOG(error) << "dual_display_capture_validation_failed: "sv
                       << dual_display::pair_validation_name(validation.state)
                       << " pane_a_contract="sv << contract[pair_pane_a].provider_display_name
                       << " pane_a_observed="sv << observation[pair_pane_a].output_display_name
                       << " pane_b_contract="sv << contract[pair_pane_b].provider_display_name
                       << " pane_b_observed="sv << observation[pair_pane_b].output_display_name
                       << " pane_a_identity="sv << manifest.panes[pair_pane_a].host_display_identity
                       << " pane_b_identity="sv << manifest.panes[pair_pane_b].host_display_identity;
      return -1;
    }

    capture_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    display_rotation = DXGI_MODE_ROTATION_IDENTITY;
    width = expected_width;
    height = expected_height;
    width_before_rotation = expected_width;
    height_before_rotation = expected_height;
    display_refresh_rate = {dual_display::pane_refresh_rate, 1};
    display_refresh_rate_rounded = dual_display::pane_refresh_rate;

    D3D11_TEXTURE2D_DESC cache_desc {};
    cache_desc.Width = dual_display::pane_width;
    cache_desc.Height = dual_display::pane_height;
    cache_desc.MipLevels = 1;
    cache_desc.ArraySize = 1;
    cache_desc.SampleDesc.Count = 1;
    cache_desc.Usage = D3D11_USAGE_DEFAULT;
    cache_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    for (auto &cache : pane_cache) {
      const auto status = device->CreateTexture2D(&cache_desc, nullptr, &cache);
      if (FAILED(status)) {
        BOOST_LOG(error) << "dual_display_cache_texture_creation_failed [0x"sv
                         << util::hex(status).to_string_view() << ']';
        return -1;
      }
    }

    D3D11_SAMPLER_DESC sampler_desc {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    auto status = device->CreateSamplerState(&sampler_desc, &sampler_linear);
    if (FAILED(status)) {
      BOOST_LOG(error) << "dual_display_cursor_sampler_creation_failed [0x"sv
                       << util::hex(status).to_string_view() << ']';
      return -1;
    }
    status = device->CreateVertexShader(cursor_vs_hlsl->GetBufferPointer(), cursor_vs_hlsl->GetBufferSize(), nullptr, &cursor_vs);
    if (FAILED(status)) {
      BOOST_LOG(error) << "dual_display_cursor_vertex_shader_creation_failed [0x"sv
                       << util::hex(status).to_string_view() << ']';
      return -1;
    }
    status = device->CreatePixelShader(cursor_ps_hlsl->GetBufferPointer(), cursor_ps_hlsl->GetBufferSize(), nullptr, &cursor_ps);
    if (FAILED(status)) {
      BOOST_LOG(error) << "dual_display_cursor_pixel_shader_creation_failed [0x"sv
                       << util::hex(status).to_string_view() << ']';
      return -1;
    }

    int32_t rotation_data[16 / sizeof(int32_t)] {};
    auto rotation = make_buffer(device.get(), rotation_data);
    if (!rotation) {
      BOOST_LOG(error) << "dual_display_cursor_rotation_buffer_creation_failed";
      return -1;
    }
    device_ctx->VSSetConstantBuffers(2, 1, &rotation);

    blend_alpha = make_blend(device.get(), true, false);
    blend_invert = make_blend(device.get(), true, true);
    blend_disable = make_blend(device.get(), false, false);
    if (!blend_alpha || !blend_invert || !blend_disable) {
      return -1;
    }
    device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0xFFFFFFFFu);
    device_ctx->PSSetSamplers(0, 1, &sampler_linear);
    device_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    return 0;
  }

bool display_ddup_pair_vram_t::topology_matches() {
    if (!prepared_pair_owner ||
        dual_display_launch::validate_prepared_pair(*prepared_pair_owner) ||
        prepared_pair_owner->provider_display_names != provider_display_names ||
        prepared_pair_owner->manifest.display_pair_id != prepared_pair_id ||
        prepared_pair_owner->manifest.session_generation != prepared_pair_generation ||
        prepared_pair_owner->manifest.topology_fingerprint != prepared_topology_fingerprint ||
        !output || !second_output) {
      return false;
    }

    std::array<DXGI_OUTPUT_DESC, dual_display::display_pair_size> current_desc {};
    if (FAILED(output->GetDesc(&current_desc[pair_pane_a])) ||
        FAILED(second_output->GetDesc(&current_desc[pair_pane_b])) ||
        !output_is_sdr(output) ||
        !output_is_sdr(second_output)) {
      return false;
    }
    for (std::size_t index = 0; index < dual_display::display_pair_size; ++index) {
      if (utf_utils::to_utf8(current_desc[index].DeviceName) != capture_display_names[index] ||
          !current_desc[index].AttachedToDesktop ||
          current_desc[index].Rotation != DXGI_MODE_ROTATION_IDENTITY ||
          !same_rect(current_desc[index].DesktopCoordinates, pane_output_desc[index].DesktopCoordinates)) {
        return false;
      }

      DXGI_OUTDUPL_DESC duplication_desc {};
      duplications[index].dup->GetDesc(&duplication_desc);
      if (duplication_desc.ModeDesc.Width != dual_display::pane_width ||
          duplication_desc.ModeDesc.Height != dual_display::pane_height ||
          duplication_desc.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
          duplication_desc.Rotation != DXGI_MODE_ROTATION_IDENTITY ||
          !dual_display::matches_pane_refresh_rate(
            duplication_desc.ModeDesc.RefreshRate.Numerator,
            duplication_desc.ModeDesc.RefreshRate.Denominator
          )) {
        return false;
      }
    }
    return true;
  }

  capture_e display_ddup_pair_vram_t::release_pair_frames() {
    capture_e result = capture_e::ok;
    dual_display::for_each_pane_release([&](const std::size_t index) {
      const auto release_result = duplications[index].release_frame();
      if (result == capture_e::ok && release_result != capture_e::ok) {
        result = release_result;
      }
    });
    return result;
  }

  capture_e display_ddup_pair_vram_t::snapshot(
    const pull_free_image_cb_t &pull_free_image_cb,
    std::shared_ptr<platf::img_t> &img_out,
    const std::chrono::milliseconds timeout,
    const bool cursor_visible
  ) {
    if (!topology_matches()) {
      BOOST_LOG(warning) << "dual_display_capture_topology_changed: reinitializing paired duplication"sv;
      return capture_e::reinit;
    }

    std::array<DXGI_OUTDUPL_FRAME_INFO, dual_display::display_pair_size> frame_info {};
    std::array<resource_t::pointer, dual_display::display_pair_size> resource_p {};
    std::array<resource_t, dual_display::display_pair_size> resources;
    std::array<capture_e, dual_display::display_pair_size> capture_status {};
    const auto capture_deadline = std::chrono::steady_clock::now() +
      std::min(timeout, dual_display::maximum_pane_capture_wait);
    auto release_frames = util::fail_guard([this]() {
      (void) release_pair_frames();
    });

    dual_display::for_each_pane_acquire([&](const std::size_t index) {
      const auto pane_timeout = dual_display::pane_capture_wait_remaining(
        capture_deadline,
        std::chrono::steady_clock::now()
      );
      capture_status[index] = duplications[index].next_frame(frame_info[index], pane_timeout, &resource_p[index]);
      resources[index].reset(resource_p[index]);
    });

    for (const auto status : capture_status) {
      if (status == capture_e::reinit) {
        return capture_e::reinit;
      }
      if (status != capture_e::ok && status != capture_e::timeout) {
        return status;
      }
    }

    for (std::size_t index = 0; index < dual_display::display_pair_size; ++index) {
      if (capture_status[index] != capture_e::ok || frame_info[index].LastPresentTime.QuadPart == 0) {
        continue;
      }
      if (!resources[index]) {
        BOOST_LOG(error) << "dual_display_capture_missing_frame_resource"sv;
        return capture_e::error;
      }

      texture2d_t source;
      const auto query_status = resources[index]->QueryInterface(IID_ID3D11Texture2D, (void **) &source);
      if (FAILED(query_status)) {
        BOOST_LOG(error) << "dual_display_capture_resource_query_failed [0x"sv
                         << util::hex(query_status).to_string_view() << ']';
        return capture_e::error;
      }

      D3D11_TEXTURE2D_DESC source_desc {};
      source->GetDesc(&source_desc);
      if (source_desc.Width != dual_display::pane_width ||
          source_desc.Height != dual_display::pane_height ||
          source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        BOOST_LOG(warning) << "dual_display_capture_surface_changed: reinitializing paired duplication"sv;
        return capture_e::reinit;
      }

      device_ctx->CopyResource(pane_cache[index].get(), source.get());
      pane_cache_valid[index] = true;
      pane_cache_captured_at[index] = std::chrono::steady_clock::now();
      pane_timestamp[index] = frame_timestamp_from(frame_info[index]);
      if (!pane_timestamp[index]) {
        BOOST_LOG(error) << "dual_display_capture_missing_present_timestamp"sv;
        return capture_e::reinit;
      }
    }

    if (cursor_visible) {
      CURSORINFO system_cursor {sizeof(CURSORINFO)};
      if (!GetCursorInfo(&system_cursor)) {
        BOOST_LOG(error) << "dual_display_cursor_state_unavailable: refusing host cursor blend"sv;
        return capture_e::error;
      }

      std::optional<std::size_t> cursor_pane;
      if ((system_cursor.flags & CURSOR_SHOWING) != 0) {
        for (std::size_t index = 0; index < dual_display::display_pair_size; ++index) {
          const auto &rect = pane_output_desc[index].DesktopCoordinates;
          if (system_cursor.ptScreenPos.x >= rect.left && system_cursor.ptScreenPos.x < rect.right &&
              system_cursor.ptScreenPos.y >= rect.top && system_cursor.ptScreenPos.y < rect.bottom) {
            cursor_pane = index;
            break;
          }
        }
      }

      if (cursor_pane) {
        auto update_shape = [&](const std::size_t index) -> bool {
          if (capture_status[index] != capture_e::ok || frame_info[index].PointerShapeBufferSize == 0) {
            return true;
          }

          DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info {};
          util::buffer_t<std::uint8_t> pixels {frame_info[index].PointerShapeBufferSize};
          UINT bytes_written {};
          const auto shape_status = duplications[index].dup->GetFramePointerShape(
            pixels.size(),
            std::begin(pixels),
            &bytes_written,
            &shape_info
          );
          if (FAILED(shape_status) || bytes_written != pixels.size()) {
            BOOST_LOG(error) << "dual_display_cursor_shape_unavailable [0x"sv
                             << util::hex(shape_status).to_string_view() << ']';
            return false;
          }

          return set_cursor_texture(device.get(), cursor_alpha, make_cursor_alpha_image(pixels, shape_info), shape_info) &&
                 set_cursor_texture(device.get(), cursor_xor, make_cursor_xor_image(pixels, shape_info), shape_info);
        };

        if (!update_shape(*cursor_pane)) {
          return capture_e::error;
        }
        if (!cursor_alpha.texture && !cursor_xor.texture) {
          dual_display::for_each_pane_acquire([&](const std::size_t index) {
            if (index != *cursor_pane && (cursor_alpha.texture || cursor_xor.texture) == false && !update_shape(index)) {
              cursor_alpha.visible = false;
              cursor_xor.visible = false;
            }
          });
        }
        if (!cursor_alpha.texture && !cursor_xor.texture) {
          // Desktop Duplication only publishes cursor shape bytes when the
          // shape changes. A new paired duplication can therefore see a
          // visible cursor before either pane has supplied its shape cache.
          // Keep the video tracks alive and begin blending on the first frame
          // that carries the shape instead of failing the whole session.
          if (!cursor_shape_pending_warning_emitted) {
            BOOST_LOG(warning) << "dual_display_cursor_shape_pending: action=defer_cursor_blend"sv;
            cursor_shape_pending_warning_emitted = true;
          }
          cursor_alpha.visible = false;
          cursor_xor.visible = false;
        } else {
          cursor_shape_pending_warning_emitted = false;
          const auto &rect = pane_output_desc[*cursor_pane].DesktopCoordinates;
          const auto composite_x = system_cursor.ptScreenPos.x - rect.left +
                                   static_cast<LONG>(*cursor_pane * dual_display::pane_width);
          const auto composite_y = system_cursor.ptScreenPos.y - rect.top;
          cursor_alpha.set_pos(
            composite_x,
            composite_y,
            dual_display::composite_width,
            dual_display::composite_height,
            DXGI_MODE_ROTATION_IDENTITY,
            true
          );
          cursor_xor.set_pos(
            composite_x,
            composite_y,
            dual_display::composite_width,
            dual_display::composite_height,
            DXGI_MODE_ROTATION_IDENTITY,
            true
          );
        }
      } else {
        cursor_alpha.visible = false;
        cursor_xor.visible = false;
      }
    }

    const auto release_status = release_pair_frames();
    release_frames.disable();
    if (release_status != capture_e::ok) {
      return release_status;
    }
    if (!topology_matches()) {
      BOOST_LOG(warning) << "dual_display_capture_topology_changed_after_snapshot: reinitializing paired duplication"sv;
      return capture_e::reinit;
    }

    const std::array<dual_display::pane_cache_t, dual_display::display_pair_size> cache {{
      {pane_cache_valid[pair_pane_a], pane_cache_captured_at[pair_pane_a], pane_timestamp[pair_pane_a]},
      {pane_cache_valid[pair_pane_b], pane_cache_captured_at[pair_pane_b], pane_timestamp[pair_pane_b]},
    }};
    const auto frame_policy = dual_display::evaluate_pair_frame(cache, std::chrono::steady_clock::now());
    switch (frame_policy.state) {
      case dual_display::frame_policy_e::first_frame_pending:
      case dual_display::frame_policy_e::missing_timestamp:
        return capture_e::timeout;
      case dual_display::frame_policy_e::stale_pane:
        // A normal DXGI timeout on a static pane leaves its last valid frame
        // cached.  Recreating the paired capture here also recreates the HEVC
        // encoder, which turns a static desktop into a reconnect/lag loop.
        // Topology changes and access loss are handled independently above;
        // reuse this cache and keep emitting the composite frame.
        if (!stale_cache_warning_emitted) {
          BOOST_LOG(warning) << "dual_display_capture_cache_unhealthy [state="sv
                             << dual_display::frame_policy_name(frame_policy.state)
                             << ", action=reuse_cached_pane, pane_a_age_ms="sv
                             << std::chrono::duration_cast<std::chrono::milliseconds>(frame_policy.ages[pair_pane_a]).count()
                             << ", pane_b_age_ms="sv
                             << std::chrono::duration_cast<std::chrono::milliseconds>(frame_policy.ages[pair_pane_b]).count()
                             << ", skew_ms="sv
                             << std::chrono::duration_cast<std::chrono::milliseconds>(frame_policy.skew).count() << ']';
          stale_cache_warning_emitted = true;
        }
        break;
      case dual_display::frame_policy_e::skew_exceeded:
        stale_cache_warning_emitted = false;
        // Skew is advisory while both cached panes remain fresh.  Reusing the
        // recent static pane prevents a needless capture/encoder restart.
        BOOST_LOG(debug) << "dual_display_capture_cache_skew_advisory [pane_a_age_ms="sv
                          << std::chrono::duration_cast<std::chrono::milliseconds>(frame_policy.ages[pair_pane_a]).count()
                          << ", pane_b_age_ms="sv
                          << std::chrono::duration_cast<std::chrono::milliseconds>(frame_policy.ages[pair_pane_b]).count()
                          << ", skew_ms="sv
                          << std::chrono::duration_cast<std::chrono::milliseconds>(frame_policy.skew).count() << ']';
        break;
      case dual_display::frame_policy_e::invalid_policy:
        return capture_e::error;
      case dual_display::frame_policy_e::ready:
        stale_cache_warning_emitted = false;
        break;
    }

    if (!pull_free_image_cb(img_out)) {
      return capture_e::interrupted;
    }
    auto d3d_img = std::static_pointer_cast<img_d3d_t>(img_out);
    if (complete_img(d3d_img.get(), false)) {
      return capture_e::error;
    }
    texture_lock_helper lock {d3d_img->capture_mutex.get()};
    if (!lock.lock()) {
      BOOST_LOG(error) << "dual_display_capture_texture_lock_failed"sv;
      return capture_e::error;
    }

    const float opaque_black[] = {0.0f, 0.0f, 0.0f, 1.0f};
    device_ctx->ClearRenderTargetView(d3d_img->capture_rt.get(), opaque_black);
    for (std::size_t index = 0; index < dual_display::display_pair_size; ++index) {
      const auto pane = dual_display::horizontal_geometry_t {
        dual_display::composite_width,
        dual_display::composite_height,
        {
          dual_display::rect_t {0, 0, dual_display::pane_width, dual_display::pane_height},
          dual_display::rect_t {dual_display::pane_width, 0, dual_display::pane_width, dual_display::pane_height},
        }
      }.panes[index];
      const D3D11_BOX pane_source {
        0,
        0,
        0,
        static_cast<UINT>(pane.width),
        static_cast<UINT>(pane.height),
        1,
      };
      device_ctx->CopySubresourceRegion(
        d3d_img->capture_texture.get(),
        0,
        static_cast<UINT>(pane.x),
        static_cast<UINT>(pane.y),
        0,
        pane_cache[index].get(),
        0,
        &pane_source
      );
    }

    if (cursor_visible && (cursor_alpha.visible || cursor_xor.visible)) {
      device_ctx->VSSetShader(cursor_vs.get(), nullptr, 0);
      device_ctx->PSSetShader(cursor_ps.get(), nullptr, 0);
      device_ctx->OMSetRenderTargets(1, &d3d_img->capture_rt, nullptr);

      if (cursor_alpha.texture) {
        device_ctx->OMSetBlendState(blend_alpha.get(), nullptr, 0xFFFFFFFFu);
        device_ctx->PSSetShaderResources(0, 1, &cursor_alpha.input_res);
        device_ctx->RSSetViewports(1, &cursor_alpha.cursor_view);
        device_ctx->Draw(3, 0);
      }
      if (cursor_xor.texture) {
        device_ctx->OMSetBlendState(blend_invert.get(), nullptr, 0x00FFFFFFu);
        device_ctx->PSSetShaderResources(0, 1, &cursor_xor.input_res);
        device_ctx->RSSetViewports(1, &cursor_xor.cursor_view);
        device_ctx->Draw(3, 0);
      }

      device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0xFFFFFFFFu);
      ID3D11RenderTargetView *empty_render_target = nullptr;
      device_ctx->OMSetRenderTargets(1, &empty_render_target, nullptr);
      device_ctx->RSSetViewports(0, nullptr);
      ID3D11ShaderResourceView *empty_shader_resource = nullptr;
      device_ctx->PSSetShaderResources(0, 1, &empty_shader_resource);
    }

    d3d_img->blank = false;
    img_out->frame_timestamp = frame_policy.timestamp;
    return capture_e::ok;
  }

  capture_e display_ddup_pair_vram_t::release_snapshot() {
    return release_pair_frames();
  }

  bool display_ddup_pair_vram_t::supports_multi_capture() const {
    return dual_track_capture;
  }

  capture_e display_ddup_pair_vram_t::capture_multi(
    const push_captured_multi_image_cb_t &push_captured_multi_image_cb,
    const pull_free_image_cb_t &pull_free_image_cb,
    bool *cursor
  ) {
    if (!dual_track_capture) {
      return capture_e::error;
    }

    const auto surface_plan = dual_display::multi_capture_surface_plan();

    while (true) {
      // Reuse the already validated paired snapshot/cursor path to produce one
      // synchronized composite GPU image, then crop it into two pane-sized
      // images before the encoders see it. This keeps both pane timestamps tied
      // to the same pair-frame policy.
      // The display dimensions are also read concurrently when each encoder
      // session starts. Never rewrite those shared pane dimensions just to
      // allocate the temporary composite, or AMF can initialize from a
      // 3840x1080 source and letterbox that union into each 1920x1080 track.
      auto composite_pool_image = alloc_img_with_dimensions(
        surface_plan.composite.width,
        surface_plan.composite.height
      );
      bool composite_image_claimed = false;
      const auto pull_composite_image = [&](std::shared_ptr<platf::img_t> &image) {
        if (composite_image_claimed) {
          return false;
        }
        composite_image_claimed = true;
        image = composite_pool_image;
        return true;
      };

      std::shared_ptr<platf::img_t> composite_image;
      const auto status = snapshot(
        pull_composite_image,
        composite_image,
        dual_display::maximum_pane_capture_wait,
        cursor ? *cursor : true
      );

      if (status == capture_e::timeout) {
        continue;
      }
      if (status != capture_e::ok) {
        return status;
      }
      if (!composite_image) {
        BOOST_LOG(error) << "dual_display_capture_missing_composite_image"sv;
        return capture_e::error;
      }

      const auto publication_time = std::chrono::steady_clock::now();
      if (!dual_display::pair_frame_publish_ready(last_multi_capture_publication, publication_time)) {
        continue;
      }
      last_multi_capture_publication = publication_time;

      auto composite = std::static_pointer_cast<img_d3d_t>(composite_image);
      for (std::size_t index = 0; index < dual_display::display_pair_size; ++index) {
        std::shared_ptr<platf::img_t> pane_image;
        if (!pull_free_image_cb(pane_image)) {
          return capture_e::interrupted;
        }
        auto pane = std::static_pointer_cast<img_d3d_t>(pane_image);
        if (complete_img(pane.get(), false)) {
          return capture_e::error;
        }

        texture_lock_helper composite_lock {composite->capture_mutex.get()};
        texture_lock_helper pane_lock {pane->capture_mutex.get()};
        if (!composite_lock.lock() || !pane_lock.lock()) {
          return capture_e::error;
        }
        const D3D11_BOX source {
          static_cast<UINT>(index * dual_display::pane_width),
          0,
          0,
          static_cast<UINT>((index + 1) * dual_display::pane_width),
          static_cast<UINT>(dual_display::pane_height),
          1,
        };
        device_ctx->CopySubresourceRegion(
          pane->capture_texture.get(),
          0,
          0,
          0,
          0,
          composite->capture_texture.get(),
          0,
          &source
        );
        pane->blank = false;
        // This pane image was assembled now, even when DXGI reused cached
        // pixels for a static desktop.  Publishing an old present timestamp
        // makes the generic encoder discard an otherwise valid paired frame.
        pane->frame_timestamp = publication_time;
        if (!push_captured_multi_image_cb(index, std::move(pane_image), true)) {
          return capture_e::interrupted;
        }
      }
    }
  }

  /**
   * Get the next frame from the Windows.Graphics.Capture API and copy it into a new snapshot texture.
   * @param pull_free_image_cb call this to get a new free image from the video subsystem.
   * @param img_out the captured frame is returned here
   * @param timeout how long to wait for the next frame
   * @param cursor_visible
   */
  capture_e display_wgc_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    const auto snapshot_started = std::chrono::steady_clock::now();
    texture2d_t src;
    uint64_t frame_qpc;
    dup.set_cursor_visible(cursor_visible);
    auto capture_status = dup.next_frame(timeout, &src, frame_qpc);
    if (capture_status != capture_e::ok) {
      const auto snapshot_elapsed = std::chrono::steady_clock::now() - snapshot_started;
      if (snapshot_elapsed >= slow_wgc_snapshot_threshold) {
        BOOST_LOG(warning) << "WGC VRAM snapshot was slow before returning ["sv
                           << "status="sv << static_cast<int>(capture_status)
                           << ", total_ms="sv << elapsed_ms(snapshot_elapsed)
                           << ", timeout_ms="sv << timeout.count() << ']';
      }
      return capture_status;
    }

    auto frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), frame_qpc);
    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);

    // It's possible for our display enumeration to race with mode changes and result in
    // mismatched image pool and desktop texture sizes. If this happens, just reinit again.
    if (desc.Width != width_before_rotation || desc.Height != height_before_rotation) {
      BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
      return capture_e::reinit;
    }

    // It's also possible for the capture format to change on the fly. If that happens,
    // reinitialize capture to try format detection again and create new images.
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    std::shared_ptr<platf::img_t> img;
    if (!pull_free_image_cb(img)) {
      return capture_e::interrupted;
    }

    auto d3d_img = std::static_pointer_cast<img_d3d_t>(img);
    d3d_img->blank = false;  // image is always ready for capture
    if (complete_img(d3d_img.get(), false) == 0) {
      texture_lock_helper lock_helper(d3d_img->capture_mutex.get());
      if (lock_helper.lock()) {
        const auto copy_started = std::chrono::steady_clock::now();
        device_ctx->CopyResource(d3d_img->capture_texture.get(), src.get());
        const auto copy_elapsed = std::chrono::steady_clock::now() - copy_started;
        const auto snapshot_elapsed = std::chrono::steady_clock::now() - snapshot_started;
        if (copy_elapsed >= slow_wgc_copy_threshold || snapshot_elapsed >= slow_wgc_snapshot_threshold) {
          BOOST_LOG(warning) << "WGC VRAM snapshot path was slow [copy_ms="sv << elapsed_ms(copy_elapsed)
                             << ", total_ms="sv << elapsed_ms(snapshot_elapsed)
                             << ", size="sv << desc.Width << 'x' << desc.Height
                             << ", timeout_ms="sv << timeout.count() << ']';
        }
      } else {
        BOOST_LOG(error) << "Failed to lock capture texture";
        return capture_e::error;
      }
    } else {
      return capture_e::error;
    }
    img_out = img;
    if (img_out) {
      img_out->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e display_wgc_vram_t::release_snapshot() {
    return dup.release_frame();
  }

  int display_wgc_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config)) {
      return -1;
    }

    return 0;
  }

  std::shared_ptr<platf::img_t> display_vram_t::alloc_img_with_dimensions(
    const int image_width,
    const int image_height
  ) {
    auto img = std::make_shared<img_d3d_t>();

    // Initialize format-independent fields
    img->width = image_width;
    img->height = image_height;
    img->id = next_image_id++;
    img->blank = true;

    return img;
  }

  std::shared_ptr<platf::img_t> display_vram_t::alloc_img() {
    return alloc_img_with_dimensions(width_before_rotation, height_before_rotation);
  }

  // This cannot use ID3D11DeviceContext because it can be called concurrently by the encoding thread
  int display_vram_t::complete_img(platf::img_t *img_base, bool dummy) {
    std::lock_guard completion_lock {image_completion_mutex};
    auto img = (img_d3d_t *) img_base;

    // If this already has a capture texture and it's not switching dummy state, nothing to do
    if (img->capture_texture && img->dummy == dummy) {
      return 0;
    }

    // If this is not a dummy image, we must know the format by now
    if (!dummy && capture_format == DXGI_FORMAT_UNKNOWN) {
      BOOST_LOG(error) << "display_vram_t::complete_img() called with unknown capture format!";
      return -1;
    }

    // Reset the image (in case this was previously a dummy)
    img->capture_texture.reset();
    img->capture_rt.reset();
    img->capture_mutex.reset();
    img->data = nullptr;
    if (img->encoder_texture_handle) {
      CloseHandle(img->encoder_texture_handle);
      img->encoder_texture_handle = nullptr;
    }

    // Initialize format-dependent fields
    img->pixel_pitch = get_pixel_pitch();
    img->row_pitch = img->pixel_pitch * img->width;
    img->dummy = dummy;
    img->format = (capture_format == DXGI_FORMAT_UNKNOWN) ? DXGI_FORMAT_B8G8R8A8_UNORM : capture_format;

    D3D11_TEXTURE2D_DESC t {};
    t.Width = img->width;
    t.Height = img->height;
    t.MipLevels = 1;
    t.ArraySize = 1;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_DEFAULT;
    t.Format = img->format;
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    t.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    auto status = device->CreateTexture2D(&t, nullptr, &img->capture_texture);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create img buf texture [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    status = device->CreateRenderTargetView(img->capture_texture.get(), nullptr, &img->capture_rt);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create render target view [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    // Get the keyed mutex to synchronize with the encoding code
    status = img->capture_texture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **) &img->capture_mutex);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to query IDXGIKeyedMutex [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    resource1_t resource;
    status = img->capture_texture->QueryInterface(__uuidof(IDXGIResource1), (void **) &resource);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to query IDXGIResource1 [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    // Create a handle for the encoder device to use to open this texture
    status = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &img->encoder_texture_handle);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create shared texture handle [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    img->data = (std::uint8_t *) img->capture_texture.get();

    return 0;
  }

  // This cannot use ID3D11DeviceContext because it can be called concurrently by the encoding thread
  /**
   * @memberof platf::dxgi::display_vram_t
   */
  int display_vram_t::dummy_img(platf::img_t *img_base) {
    return complete_img(img_base, true);
  }

  std::vector<DXGI_FORMAT> display_vram_t::get_supported_capture_formats() {
    return {
      // scRGB FP16 is the ideal format for Wide Color Gamut and Advanced Color
      // displays (both SDR and HDR). This format uses linear gamma, so we will
      // use a linear->PQ shader for HDR and a linear->sRGB shader for SDR.
      DXGI_FORMAT_R16G16B16A16_FLOAT,

      // DXGI_FORMAT_R10G10B10A2_UNORM seems like it might give us frames already
      // converted to SMPTE 2084 PQ, however it seems to actually just clamp the
      // scRGB FP16 values that DWM is using when the desktop format is scRGB FP16.
      //
      // If there is a case where the desktop format is really SMPTE 2084 PQ, it
      // might make sense to support capturing it without conversion to scRGB,
      // but we avoid it for now.

      // We include the 8-bit modes too for when the display is in SDR mode,
      // while the client stream is HDR-capable. These UNORM formats can
      // use our normal pixel shaders that expect sRGB input.
      DXGI_FORMAT_B8G8R8A8_UNORM,
      DXGI_FORMAT_B8G8R8X8_UNORM,
      DXGI_FORMAT_R8G8B8A8_UNORM,
    };
  }

  /**
   * @brief Check that a given codec is supported by the display device.
   * @param name The FFmpeg codec name (or similar for non-FFmpeg codecs).
   * @param config The codec configuration.
   * @return `true` if supported, `false` otherwise.
   */
  bool display_vram_t::is_codec_supported(std::string_view name, const ::video::config_t &config) {
    DXGI_ADAPTER_DESC adapter_desc;
    adapter->GetDesc(&adapter_desc);

    if (adapter_desc.VendorId == 0x1002) {  // AMD
      // If it's not an AMF encoder, it's not compatible with an AMD GPU
      if (!boost::algorithm::ends_with(name, "_amf")) {
        return false;
      }

      // Perform AMF version checks if we're using an AMD GPU. This check is placed in display_vram_t
      // to avoid hitting the display_ram_t path which uses software encoding and doesn't touch AMF.
      HMODULE amfrt = LoadLibraryW(AMF_DLL_NAME);
      if (amfrt) {
        auto unload_amfrt = util::fail_guard([amfrt]() {
          FreeLibrary(amfrt);
        });

        auto fnAMFQueryVersion = (AMFQueryVersion_Fn) GetProcAddress(amfrt, AMF_QUERY_VERSION_FUNCTION_NAME);
        if (fnAMFQueryVersion) {
          amf_uint64 version;
          auto result = fnAMFQueryVersion(&version);
          if (result == AMF_OK) {
            if (config.videoFormat == 2 && version < AMF_MAKE_FULL_VERSION(1, 4, 30, 0)) {
              // AMF 1.4.30 adds ultra low latency mode for AV1. Don't use AV1 on earlier versions.
              // This corresponds to driver version 23.5.2 (23.10.01.45) or newer.
              BOOST_LOG(warning) << "AV1 encoding is disabled on AMF version "sv
                                 << AMF_GET_MAJOR_VERSION(version) << '.'
                                 << AMF_GET_MINOR_VERSION(version) << '.'
                                 << AMF_GET_SUBMINOR_VERSION(version) << '.'
                                 << AMF_GET_BUILD_VERSION(version);
              BOOST_LOG(warning) << "If your AMD GPU supports AV1 encoding, update your graphics drivers!"sv;
              return false;
            } else if (config.dynamicRange && version < AMF_MAKE_FULL_VERSION(1, 4, 23, 0)) {
              // Older versions of the AMD AMF runtime can crash when fed P010 surfaces.
              // Fail if AMF version is below 1.4.23 where HEVC Main10 encoding was introduced.
              // AMF 1.4.23 corresponds to driver version 21.12.1 (21.40.11.03) or newer.
              BOOST_LOG(warning) << "HDR encoding is disabled on AMF version "sv
                                 << AMF_GET_MAJOR_VERSION(version) << '.'
                                 << AMF_GET_MINOR_VERSION(version) << '.'
                                 << AMF_GET_SUBMINOR_VERSION(version) << '.'
                                 << AMF_GET_BUILD_VERSION(version);
              BOOST_LOG(warning) << "If your AMD GPU supports HEVC Main10 encoding, update your graphics drivers!"sv;
              return false;
            }
          } else {
            BOOST_LOG(warning) << "AMFQueryVersion() failed: "sv << result;
          }
        } else {
          BOOST_LOG(warning) << "AMF DLL missing export: "sv << AMF_QUERY_VERSION_FUNCTION_NAME;
        }
      } else {
        BOOST_LOG(warning) << "Detected AMD GPU but AMF failed to load"sv;
      }
    } else if (adapter_desc.VendorId == 0x8086) {  // Intel
      // If it's not a QSV encoder, it's not compatible with an Intel GPU
      if (!boost::algorithm::ends_with(name, "_qsv")) {
        return false;
      }
      if (config.chromaSamplingType == 1) {
        if (config.videoFormat == 0 || config.videoFormat == 2) {
          // QSV doesn't support 4:4:4 in H.264 or AV1
          return false;
        }
        // TODO: Blacklist HEVC 4:4:4 based on adapter model
      }
    } else if (adapter_desc.VendorId == 0x10de) {  // Nvidia
      // If it's not an NVENC encoder, it's not compatible with an Nvidia GPU
      if (!boost::algorithm::ends_with(name, "_nvenc")) {
        return false;
      }
    } else if (adapter_desc.VendorId == 0x4D4F4351 ||  // Qualcomm (QCOM as MOQC reversed)
               adapter_desc.VendorId == 0x5143) {  // Qualcomm alternate ID
      // If it's not a MediaFoundation encoder, it's not compatible with a Qualcomm GPU
      if (!boost::algorithm::ends_with(name, "_mf")) {
        return false;
      }
    } else {
      BOOST_LOG(warning) << "Unknown GPU vendor ID: " << util::hex(adapter_desc.VendorId).to_string_view();
    }

    return true;
  }

  std::unique_ptr<avcodec_encode_device_t> display_vram_t::make_avcodec_encode_device(pix_fmt_e pix_fmt) {
    auto device = std::make_unique<d3d_avcodec_encode_device_t>();
    if (device->init(shared_from_this(), adapter.get(), pix_fmt) != 0) {
      return nullptr;
    }
    return device;
  }

  std::unique_ptr<nvenc_encode_device_t> display_vram_t::make_nvenc_encode_device(pix_fmt_e pix_fmt) {
    auto device = std::make_unique<d3d_nvenc_encode_device_t>();
    if (!device->init_device(shared_from_this(), adapter.get(), pix_fmt)) {
      return nullptr;
    }
    return device;
  }

  int init() {
    BOOST_LOG(info) << "Compiling shaders..."sv;

#define compile_vertex_shader_helper(x) \
  if (!(x##_hlsl = compile_vertex_shader(SUNSHINE_SHADERS_DIR "/" #x ".hlsl"))) \
    return -1;
#define compile_pixel_shader_helper(x) \
  if (!(x##_hlsl = compile_pixel_shader(SUNSHINE_SHADERS_DIR "/" #x ".hlsl"))) \
    return -1;

    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_linear);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_perceptual_quantizer);
    compile_vertex_shader_helper(convert_yuv420_packed_uv_type0_vs);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_linear);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_perceptual_quantizer);
    compile_vertex_shader_helper(convert_yuv420_packed_uv_type0s_vs);
    compile_pixel_shader_helper(convert_yuv420_planar_y_ps);
    compile_pixel_shader_helper(convert_yuv420_planar_y_ps_linear);
    compile_pixel_shader_helper(convert_yuv420_planar_y_ps_perceptual_quantizer);
    compile_vertex_shader_helper(convert_yuv420_planar_y_vs);
    compile_pixel_shader_helper(convert_yuv444_packed_ayuv_ps);
    compile_pixel_shader_helper(convert_yuv444_packed_ayuv_ps_linear);
    compile_vertex_shader_helper(convert_yuv444_packed_vs);
    compile_pixel_shader_helper(convert_yuv444_planar_ps);
    compile_pixel_shader_helper(convert_yuv444_planar_ps_linear);
    compile_pixel_shader_helper(convert_yuv444_planar_ps_perceptual_quantizer);
    compile_pixel_shader_helper(convert_yuv444_packed_y410_ps);
    compile_pixel_shader_helper(convert_yuv444_packed_y410_ps_linear);
    compile_pixel_shader_helper(convert_yuv444_packed_y410_ps_perceptual_quantizer);
    compile_vertex_shader_helper(convert_yuv444_planar_vs);
    compile_pixel_shader_helper(cursor_ps);
    compile_pixel_shader_helper(cursor_ps_normalize_white);
    compile_vertex_shader_helper(cursor_vs);

    BOOST_LOG(info) << "Compiled shaders"sv;

#undef compile_vertex_shader_helper
#undef compile_pixel_shader_helper

    return 0;
  }
}  // namespace platf::dxgi
