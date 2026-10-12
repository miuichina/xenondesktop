#include <windows.h>
#include <dwmapi.h>
#include <cstdio>
#include <initializer_list>

int main() {
	auto composition = BOOL(FALSE);
	const auto compositionResult = DwmIsCompositionEnabled(&composition);
	std::printf("DWM composition: hr=%08lx enabled=%d\n", compositionResult, composition);
	auto definition = WNDCLASSW{};
	definition.lpfnWndProc = DefWindowProcW;
	definition.hInstance = GetModuleHandleW(nullptr);
	definition.lpszClassName = L"XenonAcrylicHiddenTest";
	if (!composition || FAILED(compositionResult)) {
		return 77;
	}
	if (!RegisterClassW(&definition)) {
		return 1;
	}
	for (const auto extendedStyle : { DWORD(0), DWORD(WS_EX_LAYERED) }) {
		const auto window = CreateWindowExW(extendedStyle, definition.lpszClassName,
			L"", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, definition.hInstance, nullptr);
		if (!window) {
			return 2;
		}
		const auto acrylic = DWORD(3);
		const auto enabled = DwmSetWindowAttribute(window, 38, &acrylic, sizeof(acrylic));
		const auto margins = MARGINS{ -1, -1, -1, -1 };
		const auto extended = DwmExtendFrameIntoClientArea(window, &margins);
		const auto none = DWORD(1);
		const auto disabled = DwmSetWindowAttribute(window, 38, &none, sizeof(none));
		std::printf("style=%08lx acrylic=%08lx margins=%08lx disable=%08lx\n",
			extendedStyle, enabled, extended, disabled);
		DestroyWindow(window);
		if (enabled == E_INVALIDARG) {
			return 77;
		}
		if (FAILED(enabled) || FAILED(extended) || FAILED(disabled)) {
			return 3;
		}
	}
	UnregisterClassW(definition.lpszClassName, definition.hInstance);
}
