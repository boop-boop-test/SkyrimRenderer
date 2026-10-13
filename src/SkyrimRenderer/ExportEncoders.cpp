#include "SkyrimRenderer/ExportEncoders.h"

#include <SKSE/SKSE.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "windowscodecs.lib")

namespace SkyrimRenderer::ExportEncoders {

using Microsoft::WRL::ComPtr;

namespace {

/// COM for the calling thread for the scope. A thread that already has COM
/// (in either apartment) keeps it; we only undo what we did.
class ComScope {
public:
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned_ = SUCCEEDED(hr);   // S_OK or S_FALSE; RPC_E_CHANGED_MODE = already STA
    }
    ~ComScope() {
        if (owned_) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    bool owned_ = false;
};

ComPtr<IWICImagingFactory> Factory() {
    ComPtr<IWICImagingFactory> factory;
    const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        SKSE::log::error("[SkyrimRenderer] Export: WIC factory unavailable ({:#x})", static_cast<uint32_t>(hr));
        return nullptr;
    }
    return factory;
}

void CreateParentDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
}

/// One PNG frame into `stream`. Written in strips so a huge image needs no
/// second full-size copy; BGRA is swizzled per strip when WIC asks for it.
bool EncodePNGToStream(IWICImagingFactory* factory, IStream* stream,
                       const uint8_t* rgba, uint32_t width, uint32_t height) {
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) return false;
    if (FAILED(frame->Initialize(props.Get()))) return false;
    if (FAILED(frame->SetSize(width, height))) return false;

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame->SetPixelFormat(&format))) return false;
    const bool bgra = format == GUID_WICPixelFormat32bppBGRA;
    if (!bgra && format != GUID_WICPixelFormat32bppRGBA) {
        SKSE::log::error("[SkyrimRenderer] Export: PNG encoder refused 32-bit RGBA");
        return false;
    }

    const uint32_t stride = width * 4;
    constexpr uint32_t kStripRows = 256;
    std::vector<uint8_t> strip;
    for (uint32_t row = 0; row < height; row += kStripRows) {
        const uint32_t rows = std::min(kStripRows, height - row);
        const uint8_t* src = rgba + static_cast<size_t>(row) * stride;
        if (bgra) {
            strip.assign(src, src + static_cast<size_t>(rows) * stride);
            for (size_t i = 0; i < strip.size(); i += 4) std::swap(strip[i], strip[i + 2]);
            src = strip.data();
        }
        if (FAILED(frame->WritePixels(rows, stride, rows * stride, const_cast<BYTE*>(src)))) return false;
    }
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(encoder->Commit());
}

// â”€â”€ PNG chunk helpers (APNG assembly) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

uint32_t Crc32(const uint8_t* data, size_t len, uint32_t crc = 0xFFFFFFFFu) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

void PutU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void PutU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

uint32_t GetU32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void WriteChunk(FILE* fp, const char type[4], const std::vector<uint8_t>& data) {
    std::vector<uint8_t> head;
    PutU32(head, static_cast<uint32_t>(data.size()));
    head.insert(head.end(), type, type + 4);
    uint32_t crc = Crc32(reinterpret_cast<const uint8_t*>(type), 4);
    if (!data.empty()) crc = Crc32(data.data(), data.size(), crc);
    std::vector<uint8_t> tail;
    PutU32(tail, ~crc);
    fwrite(head.data(), 1, head.size(), fp);
    if (!data.empty()) fwrite(data.data(), 1, data.size(), fp);
    fwrite(tail.data(), 1, tail.size(), fp);
}

/// IHDR and IDAT payloads of a PNG file in memory.
bool SplitPNG(const std::vector<uint8_t>& png, std::vector<uint8_t>& ihdr,
              std::vector<std::vector<uint8_t>>& idat) {
    static constexpr uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (png.size() < 8 || std::memcmp(png.data(), kSig, 8) != 0) return false;
    size_t pos = 8;
    while (pos + 12 <= png.size()) {
        const uint32_t len = GetU32(&png[pos]);
        if (pos + 12 + static_cast<size_t>(len) > png.size()) return false;
        const char* type = reinterpret_cast<const char*>(&png[pos + 4]);
        const uint8_t* data = &png[pos + 8];
        if (std::memcmp(type, "IHDR", 4) == 0) ihdr.assign(data, data + len);
        else if (std::memcmp(type, "IDAT", 4) == 0) idat.emplace_back(data, data + len);
        else if (std::memcmp(type, "IEND", 4) == 0) break;
        pos += 12 + len;
    }
    return ihdr.size() == 13 && !idat.empty();
}

FILE* OpenForWrite(const std::filesystem::path& path) {
    CreateParentDirectory(path);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"wb") != 0) return nullptr;
    return fp;
}

} // namespace

std::filesystem::path PathFromUtf8(const char* utf8) {
    if (!utf8 || !*utf8) return {};
    const std::u8string s(reinterpret_cast<const char8_t*>(utf8));
    return std::filesystem::path(s);
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const auto s = path.u8string();
    return std::string(s.begin(), s.end());
}

// ============================================================================
// PNG
// ============================================================================

bool EncodePNG(const uint8_t* rgba, uint32_t width, uint32_t height, std::vector<uint8_t>& out) {
    out.clear();
    if (!rgba || width == 0 || height == 0) return false;
    ComScope com;
    auto factory = Factory();
    if (!factory) return false;

    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;
    if (!EncodePNGToStream(factory.Get(), stream.Get(), rgba, width, height)) return false;

    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return false;
    out.resize(static_cast<size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    if (FAILED(stream->Read(out.data(), static_cast<ULONG>(out.size()), &read)) || read != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

Result WritePNG(const std::filesystem::path& path, const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (!rgba || width == 0 || height == 0 || path.empty()) return Result::InvalidParameter;
    ComScope com;
    auto factory = Factory();
    if (!factory) return Result::ExportFailed;

    CreateParentDirectory(path);
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
        SKSE::log::error("[SkyrimRenderer] Export: cannot open {} for writing", PathToUtf8(path));
        return Result::ExportFailed;
    }
    if (!EncodePNGToStream(factory.Get(), stream.Get(), rgba, width, height)) {
        SKSE::log::error("[SkyrimRenderer] Export: PNG encode failed for {}", PathToUtf8(path));
        return Result::ExportFailed;
    }
    return Result::Success;
}

// ============================================================================
// APNG
// ============================================================================

APNGWriter::APNGWriter(uint32_t width, uint32_t height, uint32_t loopCount)
    : width_(width), height_(height), loops_(loopCount) {}

bool APNGWriter::AddFrame(const uint8_t* rgba, uint32_t delayMs) {
    std::vector<uint8_t> png;
    if (!EncodePNG(rgba, width_, height_, png)) return false;
    std::vector<uint8_t> ihdr;
    Frame frame;
    if (!SplitPNG(png, ihdr, frame.idat)) return false;
    // Every frame must share the first one's format (bit depth, colour type).
    if (ihdr_.empty()) ihdr_ = std::move(ihdr);
    else if (ihdr != ihdr_) return false;
    frame.delayMs = delayMs;
    frames_.push_back(std::move(frame));
    return true;
}

Result APNGWriter::Finish(const std::filesystem::path& path) {
    if (frames_.empty() || ihdr_.empty()) return Result::ExportFailed;
    FILE* fp = OpenForWrite(path);
    if (!fp) {
        SKSE::log::error("[SkyrimRenderer] Export: cannot open {} for writing", PathToUtf8(path));
        return Result::ExportFailed;
    }

    static constexpr uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    fwrite(kSig, 1, 8, fp);
    WriteChunk(fp, "IHDR", ihdr_);

    std::vector<uint8_t> actl;
    PutU32(actl, static_cast<uint32_t>(frames_.size()));
    PutU32(actl, loops_);
    WriteChunk(fp, "acTL", actl);

    // Sequence numbers run across fcTL and fdAT chunks. Every frame covers the
    // whole canvas and replaces it (blend SOURCE), so no disposal is needed.
    uint32_t seq = 0;
    for (size_t i = 0; i < frames_.size(); ++i) {
        const auto& f = frames_[i];
        std::vector<uint8_t> fctl;
        PutU32(fctl, seq++);
        PutU32(fctl, width_);
        PutU32(fctl, height_);
        PutU32(fctl, 0);
        PutU32(fctl, 0);
        PutU16(fctl, static_cast<uint16_t>(std::min<uint32_t>(f.delayMs, 65535)));
        PutU16(fctl, 1000);
        fctl.push_back(0);   // dispose_op NONE
        fctl.push_back(0);   // blend_op SOURCE
        WriteChunk(fp, "fcTL", fctl);

        for (const auto& data : f.idat) {
            if (i == 0) {
                WriteChunk(fp, "IDAT", data);
            } else {
                std::vector<uint8_t> fdat;
                fdat.reserve(data.size() + 4);
                PutU32(fdat, seq++);
                fdat.insert(fdat.end(), data.begin(), data.end());
                WriteChunk(fp, "fdAT", fdat);
            }
        }
    }
    WriteChunk(fp, "IEND", {});
    const bool ok = ferror(fp) == 0;
    fclose(fp);
    if (!ok) return Result::ExportFailed;
    SKSE::log::info("[SkyrimRenderer] Exported APNG: {} ({}x{}, {} frames)", PathToUtf8(path), width_, height_,
                    frames_.size());
    return Result::Success;
}

// ============================================================================
// GIF
// ============================================================================

struct GIFWriter::Impl {
    ComScope com;   // first member: COM outlives the interfaces below
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    uint32_t width = 0, height = 0, frames = 0;
    std::filesystem::path path;
};

GIFWriter::GIFWriter() = default;
GIFWriter::~GIFWriter() = default;

bool GIFWriter::Begin(const std::filesystem::path& path, uint32_t width, uint32_t height, uint32_t loopCount) {
    impl_ = std::make_unique<Impl>();
    auto& d = *impl_;
    d.width = width;
    d.height = height;
    d.path = path;
    d.factory = Factory();
    if (!d.factory) return false;

    CreateParentDirectory(path);
    if (FAILED(d.factory->CreateStream(&d.stream)) ||
        FAILED(d.stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
        SKSE::log::error("[SkyrimRenderer] Export: cannot open {} for writing", PathToUtf8(path));
        return false;
    }
    if (FAILED(d.factory->CreateEncoder(GUID_ContainerFormatGif, nullptr, &d.encoder)) ||
        FAILED(d.encoder->Initialize(d.stream.Get(), WICBitmapEncoderNoCache))) {
        return false;
    }

    // Looping: the NETSCAPE2.0 application extension (sub-block 1, loop count LE).
    ComPtr<IWICMetadataQueryWriter> meta;
    if (SUCCEEDED(d.encoder->GetMetadataQueryWriter(&meta))) {
        static UCHAR kApp[] = "NETSCAPE2.0";
        PROPVARIANT app;
        PropVariantInit(&app);
        app.vt = VT_UI1 | VT_VECTOR;
        app.caub.cElems = 11;
        app.caub.pElems = kApp;
        meta->SetMetadataByName(L"/appext/Application", &app);

        UCHAR data[5] = {3, 1, static_cast<UCHAR>(loopCount & 0xFF), static_cast<UCHAR>((loopCount >> 8) & 0xFF), 0};
        PROPVARIANT ext;
        PropVariantInit(&ext);
        ext.vt = VT_UI1 | VT_VECTOR;
        ext.caub.cElems = 5;
        ext.caub.pElems = data;
        meta->SetMetadataByName(L"/appext/Data", &ext);
    }
    return true;
}

bool GIFWriter::AddFrame(const uint8_t* rgba, uint32_t delayMs) {
    if (!impl_ || !impl_->encoder || !rgba) return false;
    auto& d = *impl_;

    ComPtr<IWICBitmap> bitmap;
    const UINT stride = d.width * 4;
    if (FAILED(d.factory->CreateBitmapFromMemory(d.width, d.height, GUID_WICPixelFormat32bppRGBA, stride,
                                                 stride * d.height, const_cast<BYTE*>(rgba), &bitmap))) {
        return false;
    }

    // Per-frame palette: 255 colours from the frame plus a transparent entry.
    ComPtr<IWICPalette> palette;
    if (FAILED(d.factory->CreatePalette(&palette)) ||
        FAILED(palette->InitializeFromBitmap(bitmap.Get(), 256, TRUE))) {
        return false;
    }
    ComPtr<IWICFormatConverter> indexed;
    if (FAILED(d.factory->CreateFormatConverter(&indexed)) ||
        FAILED(indexed->Initialize(bitmap.Get(), GUID_WICPixelFormat8bppIndexed, WICBitmapDitherTypeErrorDiffusion,
                                   palette.Get(), 50.0, WICBitmapPaletteTypeCustom))) {
        return false;
    }

    int transparentIndex = -1;
    UINT colorCount = 0;
    palette->GetColorCount(&colorCount);
    std::vector<WICColor> colors(colorCount);
    UINT got = 0;
    if (colorCount && SUCCEEDED(palette->GetColors(colorCount, colors.data(), &got))) {
        for (UINT i = 0; i < got; ++i) {
            if ((colors[i] >> 24) == 0) {
                transparentIndex = static_cast<int>(i);
                break;
            }
        }
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(d.encoder->CreateNewFrame(&frame, &props)) || FAILED(frame->Initialize(props.Get())) ||
        FAILED(frame->SetSize(d.width, d.height))) {
        return false;
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat8bppIndexed;
    if (FAILED(frame->SetPixelFormat(&format)) || FAILED(frame->SetPalette(palette.Get()))) return false;

    ComPtr<IWICMetadataQueryWriter> meta;
    if (SUCCEEDED(frame->GetMetadataQueryWriter(&meta))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        v.vt = VT_UI2;
        v.uiVal = static_cast<USHORT>(std::clamp<uint32_t>((delayMs + 5) / 10, 2, 65535));   // centiseconds
        meta->SetMetadataByName(L"/grctlext/Delay", &v);

        PropVariantInit(&v);
        v.vt = VT_UI1;
        v.bVal = 2;   // restore to background: transparent frames do not pile up
        meta->SetMetadataByName(L"/grctlext/Disposal", &v);

        if (transparentIndex >= 0) {
            PropVariantInit(&v);
            v.vt = VT_BOOL;
            v.boolVal = VARIANT_TRUE;
            meta->SetMetadataByName(L"/grctlext/TransparencyFlag", &v);

            PropVariantInit(&v);
            v.vt = VT_UI1;
            v.bVal = static_cast<UCHAR>(transparentIndex);
            meta->SetMetadataByName(L"/grctlext/TransparentColorIndex", &v);
        }
    }

    if (FAILED(frame->WriteSource(indexed.Get(), nullptr)) || FAILED(frame->Commit())) return false;
    ++d.frames;
    return true;
}

Result GIFWriter::Finish() {
    if (!impl_ || !impl_->encoder) return Result::ExportFailed;
    auto& d = *impl_;
    const HRESULT hr = d.encoder->Commit();
    const auto path = d.path;
    const auto frames = d.frames;
    impl_.reset();
    if (FAILED(hr)) return Result::ExportFailed;
    SKSE::log::info("[SkyrimRenderer] Exported GIF: {} ({} frames)", PathToUtf8(path), frames);
    return Result::Success;
}

} // namespace SkyrimRenderer::ExportEncoders
