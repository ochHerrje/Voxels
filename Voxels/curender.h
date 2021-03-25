#pragma once

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <D3Dcompiler.h>
#include <DirectXMath.h>
#include "d3dx12.h"

#include <wrl.h>

#include "cuda_runtime.h"
#include "cuda_runtime_api.h"
#include "cuda_device_runtime_api.h"
#include "device_launch_parameters.h"

#include "och_lib.h"

#include "och_setints_gpu.cuh"

//DEBUG SWITCH
#define GRAPHICS_DEBUG

template<typename T>
using comptr = Microsoft::WRL::ComPtr<T>;

void dump_and_flee(uint64_t error_number, const char* error_name, const char* error_desc, const char* src_file, int line_number)
{
	//Remove path from file-name
	int last_backslash = -1;

	for (int i = 0; src_file[i]; ++i)
		if (src_file[i] == '\\')
			last_backslash = i;

	const char* filename = src_file + last_backslash + 1;
	och::print("\nERROR ({0} | 0x{0:X}): {1}\n\n{2}\n\nFile: {3}\nLine: {4}\n\n", error_number, error_name, error_desc, filename, line_number);

	exit(1);
}

void dump_and_flee(const char* message, const char* src_file, int line_number)
{
	och::print("\nRUNTIME-ERROR:\n\n{}\n\nFile: {}\nLine: {}\n\n", message, src_file, line_number);
}

void check_(HRESULT err, const char* src_file, int line_number)
{
	if (!FAILED(err))
		return;

	char error_buf[1024];

	uint64_t error_code = static_cast<uint64_t>(err);

	const char* error_desc;

	if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0, error_buf, sizeof(error_buf), nullptr))
		error_desc = "[[No error information available. Error is HRESULT]]";
	else
		error_desc = error_buf;

	dump_and_flee(error_code, "", error_desc, src_file, line_number);
}

void check_(cudaError_t err, const char* src_file, int line_number)
{
	if (err == cudaSuccess)
		return;

	dump_and_flee(static_cast<uint64_t>(err), cudaGetErrorName(err), cudaGetErrorString(err), src_file, line_number);
}

#define check(x) check_(x, __FILE__, __LINE__);

#define panic(msg) dump_and_flee(msg, __FILE__, __LINE__);

LRESULT CALLBACK window_function(HWND window, UINT msg, WPARAM wp, LPARAM lp);

struct render_data
{
	static constexpr uint8_t m_frame_cnt = 2;

	static constexpr const wchar_t* m_window_class_name = L"OCHVXWN";
	uint16_t m_window_width = 1280;
	uint16_t m_window_height = 720;
	const wchar_t* m_window_title;
	HWND m_window;
	RECT m_window_rect;

	ID3D12Device2* m_device;
	ID3D12CommandQueue* m_cmd_queue;
	IDXGISwapChain4* m_swapchain;
	ID3D12Resource* m_backbuffers[m_frame_cnt];
	ID3D12GraphicsCommandList* m_cmd_list;
	ID3D12CommandAllocator* m_cmd_allocators[m_frame_cnt];
	ID3D12DescriptorHeap* m_rtv_desc_heap;

	uint16_t m_rtv_desc_size;
	uint8_t m_curr_frame = 0;

	ID3D12Fence* m_fence;
	uint64_t m_fence_value = 0;
	uint64_t m_frame_fence_values[m_frame_cnt]{};
	HANDLE m_fence_event;

	bool m_vsync = true;
	bool m_supports_tearing;
	bool m_is_fullscreen = false;
	bool m_is_initialized = false;

	uint64_t m_keystates[4]{};
	int16_t m_mouse_x;
	int16_t m_mouse_y;
	int16_t m_mouse_scroll;
	int16_t m_mouse_h_scroll;

	//TODO
	cudaExternalMemory_t m_cu_backbuffer_ext_mem[m_frame_cnt];
	uint32_t* m_cu_backbuffers[m_frame_cnt];
	cudaExternalSemaphore_t m_cu_fence;
	HANDLE m_cu_backbuffer_shared_handles[m_frame_cnt];//Only used for deleting
	HANDLE m_cu_fence_shared_handle;//Only used for deleting

#ifdef GRAPHICS_DEBUG
	comptr<ID3D12Debug> m_debug_interface;
#endif

	render_data(uint32_t width, uint32_t height, const wchar_t* title)
	{
		och::print("Initializing...\n");

		//Timer for initialization
		och::timer initialization_timer;

		//Output-Codepage to unicode (UTF-8)
		SetConsoleOutputCP(65001);

		//Set DPI to be separate for each screen, to allow for flexible tearing-behaviour
		SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

		//init_debug();

		//Initialize D3D12 Debugger
		{
#ifdef GRAPHICS_DEBUG
			check(D3D12GetDebugInterface(IID_PPV_ARGS(&m_debug_interface)));

			m_debug_interface->EnableDebugLayer();
#endif // GRAPHICS_DEBUG
		}

		//LUID dev_luid = set_best_cuda_device_idx();

		union
		{
			char cuda[8];
			LUID d3d12{ (DWORD)~0, (LONG)~0 };
		} luid;

		//Select the best cuda device by major ver, minor ver, sm-count and global mem.
		//Device-LUID is stored in the above luid union
		{
			int32_t cuda_dev_cnt;

			check(cudaGetDeviceCount(&cuda_dev_cnt));

			int32_t best_major_ver = -1;
			int32_t best_minor_ver = -1;
			int32_t best_sm_cnt = -1;
			uint64_t best_gmem_bytes = 0;

			uint32_t best_idx = -1;

			for (int32_t i = 0; i != cuda_dev_cnt; ++i)
			{
				cudaDeviceProp prop;

				cudaError_t e2 = cudaGetDeviceProperties(&prop, i);

				if (e2 != cudaSuccess)
					check(e2);

				if (prop.major >= best_major_ver && prop.minor >= best_minor_ver && prop.multiProcessorCount >= best_sm_cnt && prop.totalGlobalMem >= best_gmem_bytes)
				{
					best_major_ver = prop.major;
					best_minor_ver = prop.minor;
					best_sm_cnt = prop.multiProcessorCount;
					best_gmem_bytes = prop.totalGlobalMem;

					best_idx = i;

					for (int j = 0; j != 8; ++j)
						luid.cuda[j] = prop.luid[j];
				}
			}

			cudaSetDevice(best_idx);
		}

		//init_window(width, height, title);

		//Initialize window
		{
			m_window_width = width;
			m_window_height = height;
			m_window_title = title;

			WNDCLASSEXW window_class{};

			window_class.cbSize = sizeof(window_class);
			window_class.style = CS_VREDRAW | CS_HREDRAW;
			window_class.lpfnWndProc = window_function;
			window_class.hInstance = GetModuleHandleW(nullptr);
			window_class.lpszClassName = m_window_class_name;
			RegisterClassExW(&window_class);

			int32_t screen_width = GetSystemMetrics(SM_CXSCREEN);

			int32_t screen_height = GetSystemMetrics(SM_CYSCREEN);

			RECT window_rect = { 0, 0, static_cast<LONG>(m_window_width), static_cast<LONG>(m_window_height) };

			AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);

			int32_t actual_width = window_rect.right - window_rect.left;

			int32_t actual_height = window_rect.bottom - window_rect.top;

			// Center the window within the screen. Clamp to 0, 0 for the top-left corner.
			int32_t window_x = (screen_width - actual_width) >> 1;
			if (window_x < 0) window_x = 0;

			int32_t window_y = (screen_height - actual_height) >> 1;
			if (window_y < 0) window_y = 0;

			m_window = CreateWindowExW(0, m_window_class_name, m_window_title, WS_OVERLAPPEDWINDOW, window_x, window_y, actual_width, actual_height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

			SetWindowLongPtrW(m_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

			GetWindowRect(m_window, &m_window_rect);
		}

		//comptr<IDXGIFactory4> dxgi_factory = create_dxgi_factory();

		IDXGIFactory4* dxgi_factory;

		//Create DXGI-factory COM-Interface, which is stored in the above pointer
		{
			uint32_t factory_flags = 0;

#ifdef GRAPHICS_DEBUG
			factory_flags = DXGI_CREATE_FACTORY_DEBUG;
#endif // GRAPHICS_DEBUG

			check(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&dxgi_factory)));
		}

		//m_supports_tearing = has_tearing_support(dxgi_factory);

		//Query whether tearing is supported
		{
			IDXGIFactory5* dxgi_factory_5;

			check(dxgi_factory->QueryInterface(&dxgi_factory_5));

			BOOL is_allowed;

			if (FAILED(dxgi_factory_5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &is_allowed, sizeof(is_allowed))))
				m_supports_tearing = false;

			m_supports_tearing = is_allowed;

			dxgi_factory_5->Release();
		}

		//comptr<IDXGIAdapter4> dx12_adapter = get_adapter_by_luid(dev_luid, dxgi_factory);

		IDXGIAdapter4* dxgi_adapter;

		//Get the DXGI adapter corresponding to the luid of the previously selected cuda-device, and save it in the above pointer
		{
			IDXGIAdapter1* adapter_1;

			check(dxgi_factory->EnumAdapterByLuid(luid.d3d12, IID_PPV_ARGS(&adapter_1)));

			//Check if the chosen device actually supports D3D12
			check(D3D12CreateDevice(adapter_1, D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr));

			check(adapter_1->QueryInterface(&dxgi_adapter));

			adapter_1->Release();
		}

		//m_device = create_d3d12_device(dxgi_adapter);

		//Create D3D12-Device from DXGI adapter
		{
			check(D3D12CreateDevice(dxgi_adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)));

#ifdef GRAPHICS_DEBUG

			//Set up warnings
			ID3D12InfoQueue* info_queue;
			
			check(m_device->QueryInterface(&info_queue));

			info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
			info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
			info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);

			D3D12_MESSAGE_SEVERITY info_severity = D3D12_MESSAGE_SEVERITY_INFO;

			D3D12_INFO_QUEUE_FILTER message_filter{};
			message_filter.DenyList.pSeverityList = &info_severity;
			message_filter.DenyList.NumSeverities = 1;

			check(info_queue->PushStorageFilter(&message_filter));

			info_queue->Release();

#endif // GRAPHICS_DEBUG
		}

		//m_cmd_queue = create_d3d12_command_queue(m_device, D3D12_COMMAND_LIST_TYPE_DIRECT);

		//Create D3D12 command-queue
		{
			D3D12_COMMAND_QUEUE_DESC queue_desc{};

			queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
			queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
			queue_desc.NodeMask = 0;

			check(m_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&m_cmd_queue)));
		}

		//m_swapchain = create_swapchain(m_window, m_cmd_queue, m_window_width, m_window_height, m_frame_cnt, dxgi_factory);

		//Create DXGI swapchain
		{
			DXGI_SWAP_CHAIN_DESC1 swapchain_desc{};

			swapchain_desc.Width = width;
			swapchain_desc.Height = height;
			swapchain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			swapchain_desc.Stereo = false;
			swapchain_desc.SampleDesc = { 1, 0 };
			swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			swapchain_desc.BufferCount = m_frame_cnt;
			swapchain_desc.Scaling = DXGI_SCALING_STRETCH;//CUSTOMIZE
			swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			swapchain_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;//CUSTOM
			if (m_supports_tearing)
				swapchain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

			IDXGISwapChain1* swapchain_1;

			check(dxgi_factory->CreateSwapChainForHwnd(m_cmd_queue, m_window, &swapchain_desc, nullptr, nullptr, &swapchain_1));

			check(dxgi_factory->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER));

			check(swapchain_1->QueryInterface(&m_swapchain));

			swapchain_1->Release();
		}

		//Get index of current backbuffer from swapchain
		m_curr_frame = m_swapchain->GetCurrentBackBufferIndex();

		//m_rtv_desc_heap = create_descriptor_heap(m_device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, m_frame_cnt);

		//Create Render Target View descriptor heap for use by the swapchain
		{
			D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};

			heap_desc.NumDescriptors = m_frame_cnt;
			heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
			heap_desc.NodeMask = 0;

			check(m_device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_rtv_desc_heap)));
		}
		
		//Query size of RTV descriptors created above
		m_rtv_desc_size = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

		//Update the RTVs
		update_rtvs(m_device, m_swapchain, m_rtv_desc_heap);

		//for (int32_t i = 0; i != m_frame_cnt; ++i)
		//	m_cmd_allocators[i] = create_command_allocator(m_device, D3D12_COMMAND_LIST_TYPE_DIRECT);

		//Create command allocators for each backbuffer, to allow cycling through them
			for (int32_t i = 0; i != m_frame_cnt; ++i)
				check(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS((m_cmd_allocators + i))));

		//m_cmd_list = create_command_list(m_device, m_cmd_allocators[m_curr_frame], D3D12_COMMAND_LIST_TYPE_DIRECT);

		//Create command list, which initially uses the allocator corresponding to the swapchain's current backbuffer
		{
			check(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_cmd_allocators[m_curr_frame], nullptr, IID_PPV_ARGS(&m_cmd_list)));

			check(m_cmd_list->Close());
		}

		//m_fence = create_fence(m_device, D3D12_FENCE_FLAG_SHARED);

		//Create a shared D3D12 fence which can also be accessed by cuda
		m_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fence));

		//map_fence_to_cuda();

		//Map the previously created fence for access by cuda
		{
			check(m_device->CreateSharedHandle(m_fence, nullptr, GENERIC_ALL, nullptr, &m_cu_fence_shared_handle));

			cudaExternalSemaphoreHandleDesc fence_desc{};
			fence_desc.type = cudaExternalSemaphoreHandleTypeD3D12Fence;
			fence_desc.handle.win32.handle = m_cu_fence_shared_handle;
			fence_desc.flags = 0;

			check(cudaImportExternalSemaphore(&m_cu_fence, &fence_desc));
		}

		m_fence_event = create_event_handle();

		//Create an awaitable event handle for the fence
		{
			m_fence_event = CreateEventW(nullptr, false, false, nullptr);

			if (!m_fence_event)
				panic("Could not create fence-event");
		}

		//Indicate initialization has finished
		m_is_initialized = true;

		//Release temporary COM-Interfaces
		dxgi_factory->Release();
		dxgi_adapter->Release();

		och::print("Finished in {}\n", initialization_timer.read());
	}

	~render_data()
	{
		//Cuda stuff
		for (int32_t i = 0; i != m_frame_cnt; ++i)
		{
			cudaDestroyExternalMemory(m_cu_backbuffer_ext_mem[i]);

			CloseHandle(m_cu_backbuffer_shared_handles[i]);
		}

		cudaDestroyExternalSemaphore(m_cu_fence);

		CloseHandle(m_cu_fence_shared_handle);


		//D3D12/DXGI stuff
		m_fence->Release();

		m_cmd_list->Release();

		for (int i = 0; i != m_frame_cnt; ++i)
		{
			m_backbuffers[i]->Release();

			m_cmd_allocators[i]->Release();
		}

		m_rtv_desc_heap->Release();

		m_swapchain->Release();

		m_cmd_queue->Release();

		m_device->Release();

		CloseHandle(m_fence_event);
	}

	void run()
	{
		och::print("Running...\n");

		ShowWindow(m_window, SW_SHOW);

		MSG msg{};

		while (GetMessageW(&msg, m_window, 0, 0) > 0)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}

		flush(m_cmd_queue, m_fence, m_fence_value, m_fence_event);

		CloseHandle(m_fence_event);

		och::print("Finished\n");
	}

	void init_debug()
	{
#ifdef GRAPHICS_DEBUG

		check(D3D12GetDebugInterface(IID_PPV_ARGS(&m_debug_interface)));

		m_debug_interface->EnableDebugLayer();

#endif
	}

	void init_window(uint32_t width, uint32_t height, const wchar_t* title)
	{
		m_window_width = width;
		m_window_height = height;
		m_window_title = title;

		WNDCLASSEXW window_class{};

		window_class.cbSize = sizeof(window_class);
		window_class.style = CS_VREDRAW | CS_HREDRAW;
		window_class.lpfnWndProc = window_function;
		window_class.hInstance = GetModuleHandleW(nullptr);
		window_class.lpszClassName = m_window_class_name;
		RegisterClassExW(&window_class);

		int32_t screen_width = GetSystemMetrics(SM_CXSCREEN);

		int32_t screen_height = GetSystemMetrics(SM_CYSCREEN);

		RECT window_rect = { 0, 0, static_cast<LONG>(m_window_width), static_cast<LONG>(m_window_height) };

		AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);

		int32_t actual_width = window_rect.right - window_rect.left;

		int32_t actual_height = window_rect.bottom - window_rect.top;

		// Center the window within the screen. Clamp to 0, 0 for the top-left corner.
		int32_t window_x = (screen_width - actual_width) >> 1;
		if (window_x < 0) window_x = 0;

		int32_t window_y = (screen_height - actual_height) >> 1;
		if (window_y < 0) window_y = 0;

		m_window = CreateWindowExW(0, m_window_class_name, m_window_title, WS_OVERLAPPEDWINDOW, window_x, window_y, actual_width, actual_height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

		SetWindowLongPtrW(m_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

		GetWindowRect(m_window, &m_window_rect);
	}

	void map_fence_to_cuda()
	{
		check(m_device->CreateSharedHandle(m_fence.Get(), nullptr, GENERIC_ALL, nullptr, &m_cu_fence_shared_handle));

		cudaExternalSemaphoreHandleDesc fence_desc{};
		fence_desc.type = cudaExternalSemaphoreHandleTypeD3D12Fence;
		fence_desc.handle.win32.handle = m_cu_fence_shared_handle;
		fence_desc.flags = 0;

		check(cudaImportExternalSemaphore(&m_cu_fence, &fence_desc));
	}

	void map_backbuffers_to_cuda()
	{
		for (int32_t i = 0; i != m_frame_cnt; ++i)
		{
			check(m_device->CreateSharedHandle(m_backbuffers[i].Get(), nullptr, GENERIC_ALL, nullptr, (m_cu_backbuffer_shared_handles + i)));

			D3D12_RESOURCE_DESC buffer_desc = m_backbuffers[i]->GetDesc();

			D3D12_RESOURCE_ALLOCATION_INFO buffer_info = m_device->GetResourceAllocationInfo(0, 1, &buffer_desc);

			cudaExternalMemoryHandleDesc cu_handle_desc{};
			cu_handle_desc.flags = cudaExternalMemoryDedicated;
			cu_handle_desc.handle.win32.handle = m_cu_backbuffer_shared_handles[i];
			cu_handle_desc.size = buffer_info.SizeInBytes;
			cu_handle_desc.type = cudaExternalMemoryHandleTypeD3D12Resource;

			check(cudaImportExternalMemory((m_cu_backbuffer_ext_mem + i), &cu_handle_desc));

			cudaExternalMemoryBufferDesc cu_buf_desc{};
			cu_buf_desc.flags = 0;
			cu_buf_desc.offset = 0;
			cu_buf_desc.size = buffer_info.SizeInBytes;

			check(cudaExternalMemoryGetMappedBuffer(reinterpret_cast<void**>(m_cu_backbuffers + i), m_cu_backbuffer_ext_mem[i], &cu_buf_desc));
		}
	}

	LUID set_best_cuda_device_idx()
	{
		int32_t cuda_dev_cnt;

		check(cudaGetDeviceCount(&cuda_dev_cnt));

		int32_t best_major_ver = -1;
		int32_t best_minor_ver = -1;
		int32_t best_sm_cnt = -1;
		uint64_t best_gmem_bytes = 0;

		uint32_t best_idx = -1;

		union
		{
			char cuda[8];
			LUID d3d12{ (DWORD)~0, (LONG)~0 };
		} luid;

		for (int32_t i = 0; i != cuda_dev_cnt; ++i)
		{
			cudaDeviceProp prop;

			cudaError_t e2 = cudaGetDeviceProperties(&prop, i);

			if (e2 != cudaSuccess)
				check(e2);

			if (prop.major >= best_major_ver && prop.minor >= best_minor_ver && prop.multiProcessorCount >= best_sm_cnt && prop.totalGlobalMem >= best_gmem_bytes)
			{
				best_major_ver = prop.major;
				best_minor_ver = prop.minor;
				best_sm_cnt = prop.multiProcessorCount;
				best_gmem_bytes = prop.totalGlobalMem;

				best_idx = i;

				for (int j = 0; j != 8; ++j)
					luid.cuda[j] = prop.luid[j];
			}
		}

		cudaSetDevice(best_idx);

		return luid.d3d12;
	}

	comptr<IDXGIFactory4> create_dxgi_factory()
	{
		comptr<IDXGIFactory4> dxgi_factory;

		UINT factory_flags = 0;

#ifdef GRAPHICS_DEBUG
		factory_flags = DXGI_CREATE_FACTORY_DEBUG;
#endif // GRAPHICS_DEBUG

		check(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&dxgi_factory)));

		return dxgi_factory;
	}

	comptr<IDXGIAdapter4> get_adapter_by_luid(LUID luid, const comptr<IDXGIFactory4>& factory)
	{
		comptr<IDXGIAdapter1> adapter_1;

		comptr<IDXGIAdapter4> adapter_4;

		check(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter_1)));

		//Check if the chosen device actually supports D3D12
		check(D3D12CreateDevice(adapter_1.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr));

		check(adapter_1.As(&adapter_4));

		return adapter_4;
	}

	comptr<ID3D12Device2> create_d3d12_device(const comptr<IDXGIAdapter4>& adapter)
	{
		comptr<ID3D12Device2> device;

		check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

#ifdef GRAPHICS_DEBUG

		//Set up warnings
		comptr<ID3D12InfoQueue> info_queue;

		check(device.As(&info_queue));

		info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
		info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
		info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);

		D3D12_MESSAGE_SEVERITY info_severity = D3D12_MESSAGE_SEVERITY_INFO;

		D3D12_INFO_QUEUE_FILTER message_filter{};

		message_filter.DenyList.pSeverityList = &info_severity;
		message_filter.DenyList.NumSeverities = 1;

		check(info_queue->PushStorageFilter(&message_filter));

#endif // GRAPHICS_DEBUG

		return device;
	}

	comptr<ID3D12CommandQueue> create_d3d12_command_queue(const comptr<ID3D12Device2>& device, D3D12_COMMAND_LIST_TYPE type)
	{
		comptr<ID3D12CommandQueue> command_queue;

		D3D12_COMMAND_QUEUE_DESC queue_desc{};

		queue_desc.Type = type;
		queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
		queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
		queue_desc.NodeMask = 0;

		check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue)));

		return command_queue;
	}

	bool has_tearing_support(const comptr<IDXGIFactory4>& factory)
	{
		comptr<IDXGIFactory5> factory_5;

		check(factory.As(&factory_5));

		BOOL is_allowed;

		if (FAILED(factory_5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &is_allowed, sizeof(is_allowed))))
			return false;

		return is_allowed;
	}

	comptr<IDXGISwapChain4> create_swapchain(HWND window, const comptr<ID3D12CommandQueue>& command_queue, int32_t w, int32_t h, int32_t buffer_cnt, const comptr<IDXGIFactory4>& factory)
	{
		comptr<IDXGISwapChain4> swapchain;

		uint32_t factory_flags = 0;

		DXGI_SWAP_CHAIN_DESC1 swapchain_desc{};

		swapchain_desc.Width = w;
		swapchain_desc.Height = h;
		swapchain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		swapchain_desc.Stereo = false;
		swapchain_desc.SampleDesc = { 1, 0 };
		swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swapchain_desc.BufferCount = buffer_cnt;
		swapchain_desc.Scaling = DXGI_SCALING_STRETCH;//CUSTOMIZE
		swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		swapchain_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;//CUSTOM
		if (m_supports_tearing)
			swapchain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

		comptr<IDXGISwapChain1> swapchain_1;

		check(factory->CreateSwapChainForHwnd(command_queue.Get(), window, &swapchain_desc, nullptr, nullptr, &swapchain_1));

		check(factory->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER));

		check(swapchain_1.As(&swapchain));

		return swapchain;
	}

	comptr<ID3D12DescriptorHeap> create_descriptor_heap(const comptr<ID3D12Device2>& device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t descriptor_cnt)
	{
		comptr<ID3D12DescriptorHeap> heap;

		D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};

		heap_desc.NumDescriptors = descriptor_cnt;
		heap_desc.Type = type;
		heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		heap_desc.NodeMask = 0;

		check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));

		return heap;
	}

	void update_rtvs(const comptr<ID3D12Device2>& device, const comptr<IDXGISwapChain4>& swapchain, const comptr<ID3D12DescriptorHeap>& heap)
	{
		CD3DX12_CPU_DESCRIPTOR_HANDLE rtv_handle(heap->GetCPUDescriptorHandleForHeapStart());

		for (int32_t i = 0; i != m_frame_cnt; ++i)
		{
			comptr<ID3D12Resource> backbuffer;

			check(swapchain->GetBuffer(i, IID_PPV_ARGS(&backbuffer)));

			device->CreateRenderTargetView(backbuffer.Get(), nullptr, rtv_handle);

			m_backbuffers[i] = backbuffer;

			rtv_handle.Offset(m_rtv_desc_size);
		}

		map_backbuffers_to_cuda();
	}

	comptr<ID3D12CommandAllocator> create_command_allocator(const comptr<ID3D12Device2>& device, D3D12_COMMAND_LIST_TYPE type)
	{
		comptr<ID3D12CommandAllocator> command_allocator;

		check(device->CreateCommandAllocator(type, IID_PPV_ARGS(&command_allocator)));

		return command_allocator;
	}

	comptr<ID3D12GraphicsCommandList> create_command_list(const comptr<ID3D12Device2>& device, const comptr<ID3D12CommandAllocator>& allocator, D3D12_COMMAND_LIST_TYPE type)
	{
		comptr<ID3D12GraphicsCommandList> command_list;

		check(device->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(&command_list)));

		check(command_list->Close());

		return command_list;
	}
	
	comptr<ID3D12Fence> create_fence(const comptr<ID3D12Device2>& device, const D3D12_FENCE_FLAGS flags, uint64_t initial_value = 0)
	{
		comptr<ID3D12Fence> fence;

		device->CreateFence(initial_value, flags, IID_PPV_ARGS(&fence));

		return fence;
	}

	HANDLE create_event_handle()
	{
		HANDLE fence_event = CreateEventW(nullptr, false, false, nullptr);

		if (!fence_event)
			panic("Could not create fence-event");

		return fence_event;
	}

	uint64_t signal(const comptr<ID3D12CommandQueue>& queue, const comptr<ID3D12Fence>& fence, uint64_t& fence_value)
	{
		uint64_t value_for_signal = ++fence_value;

		check(queue->Signal(fence.Get(), value_for_signal));

		return fence_value;
	}

	void wait_for_fence(const comptr<ID3D12Fence>& fence, uint64_t value_to_await, HANDLE fence_event)
	{
		if (fence->GetCompletedValue() < value_to_await)
		{
			check(fence->SetEventOnCompletion(value_to_await, fence_event));

			WaitForSingleObject(fence_event, INFINITE);
		}
	}

	void flush(const comptr<ID3D12CommandQueue>& queue, const comptr<ID3D12Fence>& fence, uint64_t& fence_value, HANDLE fence_event)
	{
		uint64_t value_for_signal = signal(queue, fence, fence_value);

		wait_for_fence(fence, value_for_signal, fence_event);
	}

	void update()
	{
		static uint64_t elapsed_frames = 0;
		static och::time last_report_time = och::time::now();

		++elapsed_frames;
		
		och::time now = och::time::now();

		if ((now - last_report_time).seconds())
		{
			och::print("{}\n", elapsed_frames);

			elapsed_frames = 0;

			last_report_time = now;
		}
	}

	void render()
	{
		const comptr<ID3D12CommandAllocator>& cmd_allocator = m_cmd_allocators[m_curr_frame];
		const comptr<ID3D12Resource>& backbuffer = m_backbuffers[m_curr_frame];
		
		cmd_allocator->Reset();

		m_cmd_list->Reset(cmd_allocator.Get(), nullptr);

		//Render
		CD3DX12_RESOURCE_BARRIER clear_barrier = CD3DX12_RESOURCE_BARRIER::Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

		m_cmd_list->ResourceBarrier(1, &clear_barrier);

		//float clear_colour[4]{ 0.352F, 0.588F, 0.600F, 1.0F };
		//
		//CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(m_rtv_desc_heap->GetCPUDescriptorHandleForHeapStart(), m_curr_frame, m_rtv_desc_size);
		//
		//m_cmd_list->ClearRenderTargetView(rtv, clear_colour, 0, nullptr);

		////////////////////////////////////////////////////////////////////////
		//////////////////////////////////CUDA//////////////////////////////////
		////////////////////////////////////////////////////////////////////////

		//HANDLE backbuffer_handle;
		//
		//check(m_device->CreateSharedHandle(backbuffer.Get(), nullptr, GENERIC_ALL, nullptr, &backbuffer_handle));
		//
		//HANDLE fence_handle;
		//
		//check(m_device->CreateSharedHandle(m_fence.Get(), nullptr, GENERIC_ALL, nullptr, &fence_handle));
		//
		//cudaExternalSemaphoreHandleDesc fence_desc{};
		//
		//fence_desc.type = cudaExternalSemaphoreHandleTypeD3D12Fence;
		//fence_desc.handle.win32.handle = fence_handle;
		//fence_desc.flags = 0;
		//
		//cudaExternalSemaphore_t cu_fence;
		//
		//check(cudaImportExternalSemaphore(&cu_fence, &fence_desc));
		//
		//cudaExternalSemaphoreWaitParams fence_wait_params{};
		//fence_wait_params.params.fence.value = m_fence_value;
		//fence_wait_params.flags = 0;
		//
		//D3D12_RESOURCE_DESC buffer_desc = backbuffer->GetDesc();
		//
		//D3D12_RESOURCE_ALLOCATION_INFO buffer_info = m_device->GetResourceAllocationInfo(0, 1, &buffer_desc);
		//
		//cudaExternalMemoryHandleDesc cu_handle_desc{};
		//cu_handle_desc.flags = cudaExternalMemoryDedicated;
		//cu_handle_desc.handle.win32.handle = backbuffer_handle;
		//cu_handle_desc.size = buffer_info.SizeInBytes;
		//cu_handle_desc.type = cudaExternalMemoryHandleTypeD3D12Resource;
		//
		//cudaExternalMemory_t cu_buffer_mem;
		//
		//check(cudaImportExternalMemory(&cu_buffer_mem, &cu_handle_desc));
		//
		//void* dev_backbuffer;
		//
		//cudaExternalMemoryBufferDesc cu_buf_desc{};
		//cu_buf_desc.flags = 0;
		//cu_buf_desc.offset = 0;
		//cu_buf_desc.size = buffer_info.SizeInBytes;
		//
		//check(cudaExternalMemoryGetMappedBuffer(&dev_backbuffer, cu_buffer_mem, &cu_buf_desc));
		//
		//dim3 threads_per_block(64, 64);
		//dim3 blocks_per_grid((m_window_width + 63) / 64, (m_window_height + 63) / 64);
		//
		////check(launch_set_to(threads_per_block, blocks_per_grid, 0xFF007FFF, dev_backbuffer, m_window_width, m_window_width, m_window_height));
		//
		////Cleanup
		//check(cudaFree(dev_backbuffer));
		//
		//check(cudaDestroyExternalMemory(cu_buffer_mem));
		//
		//CloseHandle(fence_handle);
		//
		//CloseHandle(backbuffer_handle);

		////////////////////////////////////////////////////////////////////////
		////////////////////////////////END CUDA////////////////////////////////
		////////////////////////////////////////////////////////////////////////

		//Present
		CD3DX12_RESOURCE_BARRIER present_barrier = CD3DX12_RESOURCE_BARRIER::Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

		m_cmd_list->ResourceBarrier(1, &present_barrier);

		check(m_cmd_list->Close());

		ID3D12CommandList* const cmd_lists[]{ m_cmd_list.Get() };

		m_cmd_queue->ExecuteCommandLists(1, cmd_lists);

		int32_t sync_interval = static_cast<int32_t>(m_vsync & !m_supports_tearing);

		int32_t present_flags = m_supports_tearing && m_vsync ? DXGI_PRESENT_ALLOW_TEARING : 0;

		check(m_swapchain->Present(sync_interval, present_flags));

		m_frame_fence_values[m_curr_frame] = signal(m_cmd_queue, m_fence, m_fence_value);

		m_curr_frame = m_swapchain->GetCurrentBackBufferIndex();

		wait_for_fence(m_fence, m_frame_fence_values[m_curr_frame], m_fence_event);
	}

	void resize(uint16_t new_width, uint16_t new_height)
	{
		m_window_width = new_width;
		m_window_height = new_height;

		flush(m_cmd_queue, m_fence, m_fence_value, m_fence_event);

		for (int32_t i = 0; i != m_frame_cnt; ++i)
		{
			m_backbuffers[i].Reset();

			m_frame_fence_values[i] = m_frame_fence_values[m_curr_frame];
		}

		DXGI_SWAP_CHAIN_DESC swapchain_desc{};

		check(m_swapchain->GetDesc(&swapchain_desc));

		check(m_swapchain->ResizeBuffers(m_frame_cnt, m_window_width, m_window_height, swapchain_desc.BufferDesc.Format, swapchain_desc.Flags));

		m_curr_frame = m_swapchain->GetCurrentBackBufferIndex();

		update_rtvs(m_device, m_swapchain, m_rtv_desc_heap);
	}

	void set_fullscreen(bool fullscreen)
	{
		if (m_is_fullscreen == fullscreen)
			return;

		m_is_fullscreen = fullscreen;

		if(fullscreen)
		{
			GetWindowRect(m_window, &m_window_rect);

			uint32_t window_style = 0;

			SetWindowLongPtrW(m_window, GWL_STYLE, window_style);

			HMONITOR monitor = MonitorFromWindow(m_window, MONITOR_DEFAULTTONEAREST);

			MONITORINFOEXW mon_info{};

			mon_info.cbSize = sizeof(MONITORINFOEXW);

			GetMonitorInfoW(monitor, &mon_info);

			RECT& mr = mon_info.rcMonitor;

			SetWindowPos(m_window, HWND_TOP, mr.left, mr.top, mr.right - mr.left, mr.bottom - mr.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);

			ShowWindow(m_window, SW_MAXIMIZE);
		}
		else
		{
			SetWindowLongPtrW(m_window, GWL_STYLE, WS_OVERLAPPEDWINDOW);

			SetWindowPos(m_window, HWND_NOTOPMOST, m_window_rect.left, m_window_rect.top, m_window_rect.right - m_window_rect.left, m_window_rect.bottom - m_window_rect.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);

			ShowWindow(m_window, SW_NORMAL);
		}
	}

	void set_key(uint8_t vk) noexcept
	{
		m_keystates[vk >> 6] |= 1ull << (vk & 63);
	}

	void unset_key(uint8_t vk) noexcept
	{
		m_keystates[vk >> 6] &= ~(1ull << (vk & 63));
	}

	void update_mouse_pos(int64_t lparam) noexcept
	{
		m_mouse_x = static_cast<int16_t>(lparam & 0xFFFF);
		m_mouse_y = static_cast<int16_t>((lparam >> 16) & 0xFFFF);
	}

	bool key_is_down(uint8_t vk) const noexcept
	{
		return m_keystates[vk >> 6] & (1ull << (vk & 63));
	}
};

LRESULT CALLBACK window_function(HWND window, UINT msg, WPARAM wp, LPARAM lp)
{
	render_data* rd_ptr = reinterpret_cast<render_data*>(GetWindowLongPtrW(window, GWLP_USERDATA));

	if (!rd_ptr || !rd_ptr->m_is_initialized)
		return DefWindowProcW(window, msg, wp, lp);

	render_data& rd = *rd_ptr;

		switch (msg)
		{
		case WM_PAINT:
			rd.update();
			rd.render();
			break;

		case WM_SIZE:
			RECT wr;
			GetClientRect(window, &wr);
			rd.resize(static_cast<uint16_t>(wr.right - wr.left), static_cast<uint16_t>(wr.bottom - wr.top));
			break;

		case WM_DESTROY:
			PostQuitMessage(0);
			break;

		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
			switch (wp)
			{
			case och::vk::enter:
				if (!rd.key_is_down(och::vk::alt))
					break;
			case och::vk::f11:
				rd.set_fullscreen(!rd.m_is_fullscreen);
				break;
			case och::vk::escape:
				PostQuitMessage(0);
				
				break;
			case och::vk::key_v:
				rd.m_vsync = !rd.m_vsync;
				break;
			}
			rd.set_key(static_cast<uint8_t>(wp));
			break;

		case WM_KEYUP:
		case WM_SYSKEYUP:
			rd.unset_key(static_cast<uint8_t>(wp));
			break;

		case WM_LBUTTONDOWN:
		case WM_RBUTTONDOWN:
		case WM_MBUTTONDOWN:
		case WM_XBUTTONDOWN:
			rd.update_mouse_pos(lp);
			rd.set_key(static_cast<uint8_t>(msg - (msg >> 1) - (msg == WM_XBUTTONDOWN && (wp & (1 << 16)))));//Figure out key from low four bits of message
			break;

		case WM_LBUTTONUP:
		case WM_RBUTTONUP:
		case WM_MBUTTONUP:
		case WM_XBUTTONUP:
			rd.update_mouse_pos(lp);
			rd.unset_key(static_cast<uint8_t>((msg >> 1) - (msg == WM_XBUTTONUP && (wp & (1 << 16)))));//Figure out key from low four bits of message
			break;

		case WM_MOUSEHWHEEL:
			rd.update_mouse_pos(lp);
			rd.m_mouse_h_scroll += static_cast<int16_t>(wp >> 16);
			break;

		case WM_MOUSEWHEEL:
			rd.update_mouse_pos(lp);
			rd.m_mouse_scroll += static_cast<int16_t>(wp >> 16);
			break;

		case WM_MOUSEMOVE:
			rd.update_mouse_pos(lp);
			break;

		default:
			return DefWindowProcW(window, msg, wp, lp);
		}

		return 0;
}
