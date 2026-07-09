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

class WicFactory {
public:
	WicFactory()
	{
		CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory_));
	}

	~WicFactory()
	{
		if (factory_)
			factory_->Release();
	}

	IWICImagingFactory *get() const
	{
		return factory_;
	}

private:
	IWICImagingFactory *factory_ = nullptr;
};

template <typename T> void release(T *&ptr)
{
	if (ptr) {
		ptr->Release();
		ptr = nullptr;
	}
}
}

std::vector<uint8_t> JpegEncoder::encodeBgr(const uint8_t *bgr, int width, int height, int quality)
{
	if (!bgr || width <= 0 || height <= 0)
		return {};

	thread_local ComInit com;
	if (!com.ok())
		return {};

	thread_local WicFactory factoryStore;
	auto *factory = factoryStore.get();
	if (!factory)
		return {};

	IWICBitmap *bitmap = nullptr;
	IStream *stream = nullptr;
	IWICBitmapEncoder *encoder = nullptr;
	IWICBitmapFrameEncode *frame = nullptr;
	IPropertyBag2 *propertyBag = nullptr;

	auto cleanup = [&]() {
		release(propertyBag);
		release(frame);
		release(encoder);
		release(stream);
		release(bitmap);
	};

	HRESULT hr = factory->CreateBitmapFromMemory(static_cast<UINT>(width), static_cast<UINT>(height),
						     GUID_WICPixelFormat24bppBGR, static_cast<UINT>(width * 3),
						     static_cast<UINT>(width * height * 3), const_cast<BYTE *>(bgr),
						     &bitmap);
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

	hr = frame->WriteSource(bitmap, nullptr);
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
