#ifdef _WIN32

#define UNICODE
#define _UNICODE

#include "lumin.h"

#include <Windows.h>
#include <cstring>
#include <iostream>
#include <iterator>
#include <memory>


// For occlusion detection
#include <dwmapi.h>
#pragma comment(lib, "Dwmapi.lib")

// For DPI awareness functions
#include <shellscalingapi.h>
#pragma comment(lib, "Shlwapi.lib")

// For PathFindFileNameW
#include <shlwapi.h>
#pragma comment(lib, "Shcore.lib")

namespace lumin
{
	// Global variables to hold handles within the desktop hierarchy
	// g_progmanWindowHandle : top level Program Manager window
	// g_workerWindowHandle  : child WorkerW window rendering the static wallpaper
	// g_shellViewWindowHandle: child ListView window displaying the desktop icons
	// g_engineWindowHandle  : handle to the engine window we inject
	static HWND g_progmanWindowHandle = NULL;
	static HWND g_workerWindowHandle = NULL;
	static HWND g_shellViewWindowHandle = NULL;
	static HWND g_engineWindowHandle = NULL;

	// Current monitor in desktop coordinates
	static MonitorInfo g_selectedMonitor = {0, 0, 0, 0};

	// The offset to the desktop coordinates
	// Windows desktop coordinates start at the top left of the primary monitor
	// Subtract this offset to get the desktop coordinates
	static int g_desktopX = 0;
	static int g_desktopY = 0;

	// Mouse state tracking
	static bool g_currentMouseState[5] = {false};
	static bool g_previousMouseState[5] = {false};

	static bool g_isPre24H2 = false;

	// Desktop locking detection
	struct DesktopCloser
	{
		void operator()(HDESK h) const noexcept
		{
			if (h) {
				CloseDesktop(h);
			}
		}
	};
	using unique_desktop = std::unique_ptr<std::remove_pointer_t<HDESK>, DesktopCloser>;

	struct HandleCloser
	{
		void operator()(HANDLE h) const noexcept
		{
			if (h) {
				CloseHandle(h);
			}
		}
	};
	using unique_handle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>;

	// Monitor enumeration callback
	BOOL CALLBACK MonitorEnumProc(
		HMONITOR monitorHandle, [[maybe_unused]] HDC monitorDeviceContext, [[maybe_unused]] LPRECT monitorRectangle,
		[[maybe_unused]] LPARAM lParam
	)
	{
		std::vector<MonitorInfo> *monitorVector = reinterpret_cast<std::vector<MonitorInfo> *>(lParam);

		MONITORINFOEX monitorInfoEx;
		monitorInfoEx.cbSize = sizeof(MONITORINFOEX);

		if (GetMonitorInfo(monitorHandle, &monitorInfoEx)) {
			MonitorInfo currentMonitorInfo;
			currentMonitorInfo.x = monitorInfoEx.rcMonitor.left;
			currentMonitorInfo.y = monitorInfoEx.rcMonitor.top;

			// Monitor width/height
			currentMonitorInfo.width = monitorInfoEx.rcMonitor.right - monitorInfoEx.rcMonitor.left;
			currentMonitorInfo.height = monitorInfoEx.rcMonitor.bottom - monitorInfoEx.rcMonitor.top;

			// Work are top-left X/Y
			currentMonitorInfo.workX = monitorInfoEx.rcWork.left;
			currentMonitorInfo.workY = monitorInfoEx.rcWork.top;

			// Work area width/height
			currentMonitorInfo.workWidth = monitorInfoEx.rcWork.right - monitorInfoEx.rcWork.left;
			currentMonitorInfo.workHeight = monitorInfoEx.rcWork.bottom - monitorInfoEx.rcWork.top;

			monitorVector->push_back(currentMonitorInfo);
		}

		return TRUE;
	}

	// Callback function for EnumWindows to locate the proper WorkerW window
	BOOL CALLBACK EnumWindowsProc(HWND windowHandle, [[maybe_unused]] LPARAM lParam)
	{
		// Look for a child window named "SHELLDLL_DefView" in each top-level window.
		HWND shellViewWindow = FindWindowEx(windowHandle, NULL, L"SHELLDLL_DefView", NULL);
		if (shellViewWindow != NULL) {
			// If found, get the WorkerW window that is a sibling of the found window.
			g_workerWindowHandle = FindWindowEx(NULL, windowHandle, L"WorkerW", NULL);
			return FALSE; // Stop enumeration since we have found the desired window.
		}
		return TRUE;
	}

	bool Initialize()
	{
		// Set the process DPI awareness to get physical pixel coordinates.
		// This must be done before any windows are created.
		HRESULT dpiAwarenessResult = SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
		if (FAILED(dpiAwarenessResult)) {
			// Continue if needed, but coordinate values may be scaled.
		}

		// Locate the Progman window (the desktop window)
		g_progmanWindowHandle = FindWindow(L"Progman", NULL);
		if (!g_progmanWindowHandle) {
			return false;
		}

		// Send message 0x052C to Progman to force creation of a WorkerW window
		LRESULT result = 0;
		SendMessageTimeout(
			g_progmanWindowHandle,
			0x052C, // Undocumented message to trigger WorkerW creation
			0,
			0,
			SMTO_NORMAL,
			1000,
			reinterpret_cast<PDWORD_PTR>(&result)
		);

		// Try to locate the Shell view (desktop icons) and WorkerW child directly under Progman
		g_shellViewWindowHandle = FindWindowEx(g_progmanWindowHandle, NULL, L"SHELLDLL_DefView", NULL);
		g_workerWindowHandle = FindWindowEx(g_progmanWindowHandle, NULL, L"WorkerW", NULL);

		// Fallback for pre-24H2 builds where the WorkerW is a sibling window
		if (g_workerWindowHandle == NULL) {
			g_isPre24H2 = true;

			EnumWindows(EnumWindowsProc, 0);
		}

		if (g_workerWindowHandle == NULL) {
			return false;
		}

		return true;
	}

	void Cleanup()
	{
		if (g_engineWindowHandle) {
			// Restore the desktop wallpaper
			wchar_t wallpaperPath[MAX_PATH] = {0};
			if (SystemParametersInfo(SPI_GETDESKWALLPAPER, MAX_PATH, wallpaperPath, 0)) {
				// Reapply the wallpaper to force a refresh.
				SystemParametersInfo(SPI_SETDESKWALLPAPER, 0, wallpaperPath, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
			}
		}

		g_progmanWindowHandle = NULL;
		g_workerWindowHandle = NULL;
		g_shellViewWindowHandle = NULL;
		g_engineWindowHandle = NULL;
	}

	std::vector<MonitorInfo> EnumerateMonitors()
	{
		std::vector<MonitorInfo> monitorInfoVector;
		EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, reinterpret_cast<LPARAM>(&monitorInfoVector));

		// Convert to desktop coordinates starting at 0,0
		g_desktopX = INT_MAX;
		g_desktopY = INT_MAX;

		for (auto &monitor : monitorInfoVector) {
			if (monitor.x < g_desktopX) {
				g_desktopX = monitor.x;
			}
			if (monitor.y < g_desktopY) {
				g_desktopY = monitor.y;
			}
		}

		for (auto &monitor : monitorInfoVector) {
			monitor.x -= g_desktopX;
			monitor.y -= g_desktopY;
		}

		return monitorInfoVector;
	}

	MonitorInfo GetWallpaperTarget(int monitorIndex)
	{
		std::vector<MonitorInfo> monitors = EnumerateMonitors();

		if (monitorIndex < 0 || monitorIndex >= static_cast<int>(monitors.size())) {
			MonitorInfo info;
			info.x = 0;
			info.y = 0;
			info.width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
			info.height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
			return info;
		}
		else {
			return monitors[monitorIndex];
		}
	}

	void ConfigureWallpaperWindow(void *windowHandle, const MonitorInfo &monitor)
	{
		g_engineWindowHandle = static_cast<HWND>(windowHandle);

		if (!g_engineWindowHandle || !g_progmanWindowHandle) {
			return;
		}

		if (g_isPre24H2) {
			// Reparent the window to the custom WorkerW window.
			// This attaches the window as a child of your WorkerW,
			// which should place it behind desktop icons if your WorkerW is set up that way.
			SetParent(g_engineWindowHandle, g_workerWindowHandle);

			// Adjust window styles so that it behaves like a wallpaper.
			// For example, you may remove the title bar or border:
			LONG_PTR style = GetWindowLongPtr(g_engineWindowHandle, GWL_STYLE);
			style &= ~(WS_OVERLAPPEDWINDOW); // Remove common overlapped window styles.
			style |= WS_CHILD; // Make it a child window.
			SetWindowLongPtr(g_engineWindowHandle, GWL_STYLE, style);
		}
		else {
			// Prepare the engine window to be a layered child of Progman
			LONG_PTR style = GetWindowLongPtr(g_engineWindowHandle, GWL_STYLE);
			style &= ~(WS_OVERLAPPEDWINDOW); // Remove decorations
			style |= WS_CHILD; // Child style required for SetParent
			SetWindowLongPtr(g_engineWindowHandle, GWL_STYLE, style);

			LONG_PTR exStyle = GetWindowLongPtr(g_engineWindowHandle, GWL_EXSTYLE);
			exStyle |= WS_EX_LAYERED; // Make it a layered window for 24H2
			SetWindowLongPtr(g_engineWindowHandle, GWL_EXSTYLE, exStyle);
			SetLayeredWindowAttributes(g_engineWindowHandle, 0, 255, LWA_ALPHA);

			// Reparent the engine window directly to Progman
			SetParent(g_engineWindowHandle, g_progmanWindowHandle);

			// Ensure correct Z-order: below icons but above the system wallpaper
			if (g_shellViewWindowHandle) {
				SetWindowPos(
					g_engineWindowHandle, g_shellViewWindowHandle, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE
				);
			}
			if (g_workerWindowHandle) {
				SetWindowPos(
					g_workerWindowHandle, g_engineWindowHandle, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE
				);
			}
		}

		// Reparent the engine window to WorkerW
		g_selectedMonitor = monitor;

		// Resize/reposition the engine window to match its new parent.
		// g_progmanWindowHandle spans the entire virtual desktop in modern builds
		SetWindowPos(
			g_engineWindowHandle,
			NULL,
			monitor.x,
			monitor.y,
			monitor.width,
			monitor.height,
			SWP_NOZORDER | SWP_NOACTIVATE
		);

		RedrawWindow(g_engineWindowHandle, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
	}

	struct FullscreenOcclusionData
	{
		MonitorInfo monitor;
		std::vector<RECT> occludedRects;
	};

	static bool IsInvisibleWin10BackgroundAppWindow(HWND hWnd)
	{
		int CloakedVal;
		HRESULT hRes = DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &CloakedVal, sizeof(CloakedVal));
		if (hRes != S_OK) {
			CloakedVal = true;
		}
		return CloakedVal ? true : false;
	}

	double
	ComputeOcclusionFraction(const std::vector<RECT> &occludedRects, const MonitorInfo &monitor, int sampleStep = 100)
	{
		int occludedCount = 0;
		int totalSamples = 0;

		for (int y = monitor.y; y < monitor.y + monitor.height; y += sampleStep) {
			for (int x = monitor.x; x < monitor.x + monitor.width; x += sampleStep) {
				totalSamples++;
				bool isOccluded = false;

				for (const RECT &rect : occludedRects) {
					if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom) {
						isOccluded = true;
						break;
					}
				}
				if (isOccluded)
					occludedCount++;
			}
		}

		if (totalSamples == 0)
			return 0.0;
		return static_cast<double>(occludedCount) / static_cast<double>(totalSamples);
	}

	static bool IsWindowOpaque(HWND hWnd)
	{
	    LONG_PTR exStyle = GetWindowLongPtr(hWnd, GWL_EXSTYLE);

	    if (exStyle & WS_EX_TRANSPARENT)
	        return false;

	    if (exStyle & WS_EX_LAYERED) {
	        BYTE alpha = 255;
	        if (GetLayeredWindowAttributes(hWnd, nullptr, &alpha, nullptr) && alpha == 0)
	            return false;
	    }

	    return true;
	}

	static bool IsShellOverlay(HWND hWnd)
	{
	    char cls[256];
	    if (!GetClassNameA(hWnd, cls, 256))
	        return false;

	    // Classes that only the shell uses.  Anything under these names
	    // is not a user application window, so it's safe to skip.
	    if (strcmp(cls, "ShellHandwritingCanvas") == 0)
	        return true;
	    if (strcmp(cls, "XamlExplorerHostIslandWindow") == 0)
	        return true;

	    // `Windows.UI.Core.CoreWindow` is used by both shell overlays and
	    // real UWP apps (e.g. a fullscreen UWP video player, which SHOULD
	    // count as occlusion).  Distinguish by owning process.
	    if (strcmp(cls, "Windows.UI.Core.CoreWindow") == 0) {
	        DWORD pid = 0;
	        GetWindowThreadProcessId(hWnd, &pid);
	        if (pid == 0)
	            return false;

	        unique_handle proc {OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
	        if (!proc)
	            return false;

	        WCHAR path[MAX_PATH];
	        DWORD len = std::size(path);
	        if (!QueryFullProcessImageNameW(proc.get(), 0, path, &len))
	            return false;

	        static const WCHAR *kShellProcs[] = {
	            L"TextInputHost.exe",
	            L"ShellExperienceHost.exe",
	            L"StartMenuExperienceHost.exe",
	            L"SearchHost.exe",
	        };
	        const WCHAR *exe = PathFindFileNameW(path);
	        for (auto *p : kShellProcs) {
	            if (_wcsicmp(exe, p) == 0)
	                return true;
	        }
	        return false;
	    }

	    return false;
	}

	BOOL CALLBACK FullscreenWindowEnumProc(HWND hwnd, LPARAM lParam)
	{
	    FullscreenOcclusionData *occlusionData = reinterpret_cast<FullscreenOcclusionData *>(lParam);

	    if (hwnd == g_engineWindowHandle || hwnd == g_workerWindowHandle)
	        return TRUE;

	    if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
	        return TRUE;

	    if (GetShellWindow() == hwnd)
	        return TRUE;

	    char g_szClassName[256];
	    GetClassNameA(hwnd, g_szClassName, 256);

	    if (strcmp(g_szClassName, "WorkerW") == 0)
	        return TRUE;

	    if (strcmp(g_szClassName, "CEF-OSC-WIDGET") == 0)
	        return TRUE;

	    if (IsInvisibleWin10BackgroundAppWindow(hwnd))
	        return TRUE;

	    if (!IsWindowOpaque(hwnd))
	        return TRUE;

	    if (IsShellOverlay(hwnd))
	        return TRUE;

	    RECT windowRect;
	    if (GetWindowRect(hwnd, &windowRect)) {
	        // convert window rect to desktop coordinates
			windowRect.left -= g_desktopX;
			windowRect.right -= g_desktopX;
			windowRect.top -= g_desktopY;
			windowRect.bottom -= g_desktopY;

			// Build a rectangle for the target monitor.
			RECT monitorRect;
			monitorRect.left = occlusionData->monitor.x;
			monitorRect.top = occlusionData->monitor.y;
			monitorRect.right = occlusionData->monitor.x + occlusionData->monitor.width;
			monitorRect.bottom = occlusionData->monitor.y + occlusionData->monitor.height;

			// Calculate the intersection of the window's rectangle with the monitor's rectangle.
			RECT intersectionRect;
			if (IntersectRect(&intersectionRect, &windowRect, &monitorRect)) {
				// store the occluded area
				occlusionData->occludedRects.push_back(intersectionRect);
			}
	    }

	    return TRUE;
	}

	bool IsMonitorOccluded(const MonitorInfo &monitor, double threshold)
	{
		FullscreenOcclusionData occlusionData;
		occlusionData.monitor = monitor;

		EnumWindows(FullscreenWindowEnumProc, reinterpret_cast<LPARAM>(&occlusionData));

		double occlusionFraction = ComputeOcclusionFraction(occlusionData.occludedRects, monitor);
		return occlusionFraction >= threshold;
	}

	static bool IsSecureDesktop()
	{
		unique_desktop desktop {OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS)};
		if (!desktop) // couldn’t query ⇒ assume we’re secure to be conservative
			return true;

		DWORD bytes = 0;
		if (!GetUserObjectInformationW(desktop.get(), UOI_NAME, nullptr, 0, &bytes) &&
			GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
			return true; // genuine failure
		}

		if (bytes == 0)
			return true; // shouldn’t happen, but stay conservative

		std::vector<wchar_t> name((bytes / sizeof(wchar_t)) + 1, L'\0');
		if (!GetUserObjectInformationW(desktop.get(), UOI_NAME, name.data(), bytes, &bytes))
			return true;

		return _wcsicmp(name.data(), L"Default") != 0;
	}

	bool IsDesktopLocked()
	{
		if (IsSecureDesktop())
			return true;

		HWND hwnd = GetForegroundWindow();
		if (!hwnd)
			return false;

		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid == 0)
			return false;

		unique_handle proc {OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
		if (!proc)
			return false;

		WCHAR path[MAX_PATH];
		DWORD len = std::size(path);
		if (!QueryFullProcessImageNameW(proc.get(), 0, path, &len))
			return false;

		return _wcsicmp(PathFindFileNameW(path), L"LockApp.exe") == 0;
	}

	static int GetVirtualKeyForMouseButton(int button)
	{
		switch (button) {
		case 0:
			return VK_LBUTTON;
		case 1:
			return VK_RBUTTON;
		case 2:
			return VK_MBUTTON;
		case 3:
			return VK_XBUTTON1;
		case 4:
			return VK_XBUTTON2;
		default:
			return 0;
		}
	}

	void UpdateMouseState()
	{
		// Save previous state
		for (int i = 0; i < 5; i++) {
			g_previousMouseState[i] = g_currentMouseState[i];
		}

		// Update current state
		for (int i = 0; i < 5; i++) {
			int vk = GetVirtualKeyForMouseButton(i);
			if (vk != 0) {
				g_currentMouseState[i] = (GetAsyncKeyState(vk) & 0x8000) != 0;
			}
			else {
				g_currentMouseState[i] = false;
			}
		}
	}

	bool IsMouseButtonPressed(int button)
	{
		if (button < 0 || button >= 5)
			return false;
		return g_currentMouseState[button] && !g_previousMouseState[button];
	}

	bool IsMouseButtonDown(int button)
	{
		if (button < 0 || button >= 5)
			return false;
		return g_currentMouseState[button];
	}

	bool IsMouseButtonReleased(int button)
	{
		if (button < 0 || button >= 5)
			return false;
		return !g_currentMouseState[button] && g_previousMouseState[button];
	}

	bool IsMouseButtonUp(int button)
	{
		if (button < 0 || button >= 5)
			return false;
		return !g_currentMouseState[button];
	}

	bool GetRelativeCursorPos(POINT *p)
	{
		if (!GetCursorPos(p))
			return false;

		// Convert to desktop coordinates
		p->x -= g_desktopX;
		p->y -= g_desktopY;

		// Convert to window coordinates
		p->x -= g_selectedMonitor.x;
		p->y -= g_selectedMonitor.y;

		return true;
	}

	int GetMouseX()
	{
		POINT p;
		if (GetRelativeCursorPos(&p)) {
			return p.x;
		}
		return 0;
	}

	int GetMouseY()
	{
		POINT p;
		if (GetRelativeCursorPos(&p)) {
			return p.y;
		}
		return 0;
	}

	Vector2Platform GetMousePosition()
	{
		POINT p;
		if (GetRelativeCursorPos(&p)) {
			return {static_cast<float>(p.x), static_cast<float>(p.y)};
		}
		return {0.0f, 0.0f};
	}

	bool SupportsDynamicWallpaper()
	{
		return true;
	}

	bool SupportsMultiMonitor()
	{
		return true;
	}

	void ShowAlert(const std::string &title, const std::string &message)
	{
		MessageBoxA(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
	}

} // namespace lumin

#endif // _WIN32
