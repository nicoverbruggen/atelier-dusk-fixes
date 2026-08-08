// SPDX-License-Identifier: MIT
//
// `DUSK_FRAME_CAPTURE`: one back buffer, written as a PNG, with a checksum.
// See frame_capture.h for why the checksum matters as much as the picture.
//
// THE PNG IS WRITTEN BY HAND, and that is a smaller decision than it looks. A
// PNG's image data is a zlib stream, and zlib permits STORED blocks -- deflate
// with no compression at all. So the encoder here is a header, a CRC32, an
// Adler-32, and the rows copied out with a filter byte in front of each. No
// compression, no dependency, about forty lines. The file is large and opens in
// anything.
//
// The alternative was writing raw pixels and converting on the host, which adds
// a step that can be forgotten between capturing and looking.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "frame_capture.h"
#include "d3d11_hooks.h"
#include "log.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

std::atomic<uint64_t> frame{0};
std::atomic<bool> done{false};

// Which frame to write, from the switch. Zero means the feature is off.
uint64_t captureFrame() {
  static const uint64_t n = [] () -> uint64_t {
    const char* env = std::getenv("DUSK_FRAME_CAPTURE");
    if (!env || !env[0])
      return 0;
    const long long value = std::atoll(env);
    return value > 0 ? uint64_t(value) : 0;
  }();
  return n;
}

uint32_t crcTable(uint32_t index) {
  uint32_t c = index;
  for (int k = 0; k < 8; ++k)
    c = (c & 1) ? 0xedb88320u ^ (c >> 1) : (c >> 1);
  return c;
}

uint32_t crc32(const uint8_t* data, size_t length, uint32_t seed = 0) {
  uint32_t c = seed ^ 0xffffffffu;
  for (size_t i = 0; i < length; ++i)
    c = crcTable((c ^ data[i]) & 0xff) ^ (c >> 8);
  return c ^ 0xffffffffu;
}

void putBigEndian(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(uint8_t(value >> 24));
  out.push_back(uint8_t(value >> 16));
  out.push_back(uint8_t(value >> 8));
  out.push_back(uint8_t(value));
}

void putChunk(std::vector<uint8_t>& out, const char tag[4],
              const std::vector<uint8_t>& body) {
  putBigEndian(out, uint32_t(body.size()));
  const size_t start = out.size();
  out.insert(out.end(), tag, tag + 4);
  out.insert(out.end(), body.begin(), body.end());
  out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(0);
  const uint32_t sum = crc32(out.data() + start, 4 + body.size());
  out[out.size() - 4] = uint8_t(sum >> 24);
  out[out.size() - 3] = uint8_t(sum >> 16);
  out[out.size() - 2] = uint8_t(sum >> 8);
  out[out.size() - 1] = uint8_t(sum);
}

// `raw` is the filtered scanline stream: one 0x00 filter byte then RGB for each
// row. Wrapped in a zlib stream of stored deflate blocks.
std::vector<uint8_t> zlibStored(const std::vector<uint8_t>& raw) {
  std::vector<uint8_t> out;
  out.push_back(0x78);   // CMF: deflate, 32K window
  out.push_back(0x01);   // FLG: no dictionary, fastest -- (0x78<<8|0x01) % 31 == 0
  size_t offset = 0;
  while (offset < raw.size()) {
    const uint16_t block = uint16_t((raw.size() - offset > 65535)
                                      ? 65535 : (raw.size() - offset));
    const bool last = (offset + block) >= raw.size();
    out.push_back(last ? 1 : 0);
    out.push_back(uint8_t(block & 0xff));
    out.push_back(uint8_t(block >> 8));
    out.push_back(uint8_t(~block & 0xff));
    out.push_back(uint8_t((~block >> 8) & 0xff));
    out.insert(out.end(), raw.begin() + offset, raw.begin() + offset + block);
    offset += block;
  }
  uint32_t a = 1, b = 0;
  for (uint8_t byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  putBigEndian(out, (b << 16) | a);
  return out;
}

bool writePng(const std::string& path, unsigned int width, unsigned int height,
              const std::vector<uint8_t>& rgb) {
  std::vector<uint8_t> raw;
  raw.reserve(height * (1 + width * 3));
  for (unsigned int y = 0; y < height; ++y) {
    raw.push_back(0);   // filter: none
    const uint8_t* row = rgb.data() + size_t(y) * width * 3;
    raw.insert(raw.end(), row, row + size_t(width) * 3);
  }

  std::vector<uint8_t> png = {
    0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
  };
  std::vector<uint8_t> ihdr;
  putBigEndian(ihdr, width);
  putBigEndian(ihdr, height);
  ihdr.push_back(8);   // bit depth
  ihdr.push_back(2);   // colour type: truecolour
  ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
  putChunk(png, "IHDR", ihdr);
  putChunk(png, "IDAT", zlibStored(raw));
  putChunk(png, "IEND", {});

  FILE* file = std::fopen(path.c_str(), "wb");
  if (!file)
    return false;
  const size_t written = std::fwrite(png.data(), 1, png.size(), file);
  std::fclose(file);
  return written == png.size();
}

}  // namespace

void frameCaptureTick(IDXGISwapChain* swapChain) {
  const uint64_t want = captureFrame();
  if (!want || !swapChain || done.load(std::memory_order_relaxed))
    return;
  if (frame.fetch_add(1, std::memory_order_relaxed) + 1 != want)
    return;
  done.store(true, std::memory_order_relaxed);

  ID3D11Texture2D* back = nullptr;
  if (FAILED(swapChain->GetBuffer(0, IID_ID3D11Texture2D,
                                  reinterpret_cast<void**>(&back))) || !back) {
    log("CAPTURE: the back buffer could not be read");
    return;
  }
  D3D11_TEXTURE2D_DESC desc = {};
  back->GetDesc(&desc);

  ID3D11Device* device = nullptr;
  back->GetDevice(&device);
  ID3D11DeviceContext* context = nullptr;
  if (device)
    device->GetImmediateContext(&context);

  D3D11_TEXTURE2D_DESC staged = desc;
  staged.Usage = D3D11_USAGE_STAGING;
  staged.BindFlags = 0;
  staged.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staged.MiscFlags = 0;
  staged.SampleDesc.Count = 1;
  staged.SampleDesc.Quality = 0;
  ID3D11Texture2D* copy = nullptr;
  // Through the original device, for the same reason supersampling's own
  // targets are: the mod's D3D11 work must not travel through the mod's hooks.
  const bool made = device && SUCCEEDED(
    createTexture2DUnhooked(device, &staged, nullptr, &copy)) && copy;

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  bool ok = false;
  if (made && context) {
    context->CopyResource(copy, back);
    ok = SUCCEEDED(context->Map(copy, 0, D3D11_MAP_READ, 0, &mapped));
  }

  if (ok) {
    // The back buffer is B8G8R8A8 on all three games (format 87). Alpha is
    // dropped: it is meaningless in a presented frame and its absence keeps the
    // checksum a statement about what is visible.
    const bool bgr = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                     desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    std::vector<uint8_t> rgb(size_t(desc.Width) * desc.Height * 3);
    for (unsigned int y = 0; y < desc.Height; ++y) {
      const uint8_t* src =
        static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
      uint8_t* dst = rgb.data() + size_t(y) * desc.Width * 3;
      for (unsigned int x = 0; x < desc.Width; ++x) {
        dst[x * 3 + 0] = src[x * 4 + (bgr ? 2 : 0)];
        dst[x * 3 + 1] = src[x * 4 + 1];
        dst[x * 3 + 2] = src[x * 4 + (bgr ? 0 : 2)];
      }
    }
    context->Unmap(copy, 0);

    const uint32_t sum = crc32(rgb.data(), rgb.size());
    char name[64] = {};
    std::snprintf(name, sizeof(name), "dusk-frame-%llu.png",
                  static_cast<unsigned long long>(want));
    if (writePng(name, desc.Width, desc.Height, rgb))
      log("CAPTURE: frame ", std::dec, want, " ", desc.Width, "x", desc.Height,
          " format=", unsigned(desc.Format), " written to ", name,
          " crc=0x", std::hex, sum, std::dec,
          " (an identical crc across runs means nothing changed)");
    else
      log("CAPTURE: frame ", std::dec, want, " could not be written to ", name);
  } else {
    log("CAPTURE: the back buffer could not be staged (made=", made ? 1 : 0,
        " context=", context ? 1 : 0, ")");
  }

  if (copy) copy->Release();
  if (context) context->Release();
  if (device) device->Release();
  back->Release();
}

}  // namespace atfix
