#include "jpeg_encoder.hpp"

#include <Windows.h>
#include <Objidl.h>
#include <OleAuto.h>
#include <wincodec.h>

#include <algorithm>

namespace {
class ComInit {
public:
	ComInit()
	{
		hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	}

	~ComInit()
	{
		if (SUCCEEDED(hr_))
			CoUninitialize();
	}

	bool ok() const
	{
		return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE;
	}

private:
	HRESULT hr_ = E_FAIL;
};

template <typename T> void release(T *&ptr)
{
	if (ptr) {
		ptr->Release();
		ptr = nullptr;
	}
}
}

std::vector<uint8_t> JpegEncoder::encodeRgba(const uint8_t *rgba, int width, int height, int quality)
{
	if (!rgba || width <= 0 || height <= 0)
		return {};

	thread_local ComInit com;
	if (!com.ok())
		return {};

	IWICImagingFactory *factory = nullptr;
	IWICBitmap *bitmap = nullptr;
	IWICFormatConverter *converter = nullptr;
	IStream *stream = nullptr;
	IWICBitmapEncoder *encoder = nullptr;
	IWICBitmapFrameEncode *frame = nullptr;
	IPropertyBag2 *propertyBag = nullptr;

	auto cleanup = [&]() {
		release(propertyBag);
		release(frame);
		release(encoder);
		release(stream);
		release(converter);
		release(bitmap);
		release(factory);
	};

	HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
	if (FAILED(hr)) {
		cleanup();
		return {};
	}

	hr = factory->CreateBitmapFromMemory(static_cast<UINT>(width), static_cast<UINT>(height), GUID_WICPixelFormat32bppRGBA,
					     static_cast<UINT>(width * 4), static_cast<UINT>(width * height * 4),
					     const_cast<BYTE *>(rgba), &bitmap);
	if (FAILED(hr)) {
		cleanup();
		return {};
	}

	hr = factory->CreateFormatConverter(&converter);
	if (FAILED(hr)) {
		cleanup();
		return {};
	}

	hr = converter->Initialize(bitmap, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
				   WICBitmapPaletteTypeCustom);
	if (FAILED(hr)) {
		cleanup();
		return {};
	}

	hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
	if (FAILED(hr)) {
		cleanup();
		return {};
	}

	hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
	if (FAILED(hr)) {
		cleanup();
		return {};
	}

	hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
	if (FAILED(hr)) {
		cleanup();
		return {};
	}

	hr = encoder->CreateNewFrame(&frame, &propertyBag);
	if (FAILED(hr)) {
		cleanup();
		return {};
	}

	if (propertyBag) {
		PROPBAG2 option = {};
		option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");

		VARIANT value;
		VariantInit(&value);
		value.vt = VT_R4;
		value.fltVal = static_cast<float>(std::clamp(quality, 1, 100)) / 100.0f;
		propertyBag->Write(1, &option, &value);
		VariantClear(&value);
	}

	hr = frame->Initialize(propertyBag);
	if (FAILED(hr)) {
		cleanup();
		return {};
	}

	hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
	if (FAILED(hr)) {
		cleanup();
		return {};
	}

	WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat24bppBGR;
	hr = frame->SetPixelFormat(&pixelFormat);
	if (FAILED(hr)) {
		cleanup();
		return {};
	}

	hr = frame->WriteSource(converter, nullptr);
	if (FAILED(hr)) {
		cleanup();
		return {};
	}

	hr = frame->Commit();
	if (FAILED(hr)) {
		cleanup();
		return {};
	}

	hr = encoder->Commit();
	if (FAILED(hr)) {
		cleanup();
		return {};
	}

	HGLOBAL memory = nullptr;
	hr = GetHGlobalFromStream(stream, &memory);
	if (FAILED(hr) || !memory) {
		cleanup();
		return {};
	}

	const auto size = static_cast<size_t>(GlobalSize(memory));
	const auto *data = static_cast<const uint8_t *>(GlobalLock(memory));
	std::vector<uint8_t> jpeg;
	if (data && size > 0)
		jpeg.assign(data, data + size);
	if (data)
		GlobalUnlock(memory);

	cleanup();
	return jpeg;
}
