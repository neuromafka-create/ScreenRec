// ScreenRec — локальный рекордер экрана. Лицензия MIT. Захват и кодирование взяты из wcap.

#include "wcap.h"
#include "wcap_config.h"
#include "wcap_audio_capture.h"
#include "wcap_screen_capture.h"
#include "wcap_encoder.h"

#include <dxgi1_6.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <wincodec.h>
#include <commctrl.h>

#pragma comment (lib, "ntdll")
#pragma comment (lib, "kernel32")
#pragma comment (lib, "user32")
#pragma comment (lib, "gdi32")
#pragma comment (lib, "msimg32")
#pragma comment (lib, "dxgi")
#pragma comment (lib, "d3dcompiler")
#pragma comment (lib, "d3d11")
#pragma comment (lib, "dwmapi")
#pragma comment (lib, "shell32")
#pragma comment (lib, "shlwapi")
#pragma comment (lib, "mfplat")
#pragma comment (lib, "mfuuid")
#pragma comment (lib, "mfreadwrite")
#pragma comment (lib, "evr")
#pragma comment (lib, "strmiids")
#pragma comment (lib, "ksuser")
#pragma comment (lib, "mmdevapi")
#pragma comment (lib, "ole32")
#pragma comment (lib, "wmcodecdspuuid")
#pragma comment (lib, "avrt")
#pragma comment (lib, "uxtheme")
#pragma comment (lib, "OneCore")
#pragma comment (lib, "CoreMessaging")
#pragma comment (lib, "windowscodecs")
#pragma comment (lib, "comctl32")

#if defined(_M_AMD64)
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
#endif

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

#define WM_WCAP_ALREADY_RUNNING (WM_USER+1)
#define WM_WCAP_STOP_CAPTURE    (WM_USER+2)
#define WM_WCAP_TRAY_TITLE      (WM_USER+3)
#define WM_WCAP_COMMAND         (WM_USER+4)
#define WM_WCAP_PICKED          (WM_USER+5)
#define WM_WCAP_CANCEL_UI       (WM_USER+6)
#define WM_WCAP_RESTORE         (WM_USER+7)
#define WM_WCAP_CLOSE_WINDOW    (WM_USER+8)

#define TIMER_AUDIO  1
#define TIMER_VIDEO  2
#define TIMER_FOLLOW 3
#define TIMER_FLASH  4

#define HOT_RECORD_WINDOW        1
#define HOT_RECORD_MONITOR       2
#define HOT_RECORD_REGION        3
#define HOT_RECORD_WINDOW_REGION 4
#define HOT_TOGGLE               5
#define HOT_SHOT                 6

#define CMD_MONITOR       1
#define CMD_WINDOW        2
#define CMD_REGION        3
#define CMD_WINDOW_REGION 4
#define CMD_SHOT          5
#define CMD_FOLDER        6
#define CMD_SETTINGS      7
#define CMD_QUIT          8

#define PHASE_HIDDEN 0
#define PHASE_PICK   1
#define PHASE_ARMED  2
#define PHASE_RUBBER 3

#define BTN_NONE     0
#define BTN_RECORD   1
#define BTN_PAUSE    2
#define BTN_SHOT     3
#define BTN_MINIMIZE 4
#define BTN_TRAY     5
#define BTN_CLOSE    6
#define BTN_MODE     7
#define BTN_SETTINGS 11

#define FOLD_NONE    0
#define FOLD_TASKBAR 1
#define FOLD_TRAY    2

static WCHAR gConfigPath[MAX_PATH];
static LARGE_INTEGER gTickFreq;
static HICON gIcon1;
static HICON gIcon2;
static UINT WM_TASKBARCREATED;
static HFONT gFont;
static UINT gFontDpi;

static HWND gWindow;
static HWND gFrame;
static HWND gToolbar;
static HWND gTaskbar;
static HWND gShade;
static HWND gTip;
static int gFold;
static BOOL gSettingsOpen;
static HHOOK gMouseHook;
static HHOOK gKeyHook;

static Config gConfig;
static AudioCapture gAudio;
static ScreenCapture gCapture;
static Encoder gEncoder;

static int gPhase;
static int gMode;
static HWND gTarget;
static HMONITOR gMonitor;
static POINT gCropOffset;
static SIZE gCropSize;
static int gLockedW;
static int gLockedH;

static BOOL gRecording;
static BOOL gPaused;
static BOOL gStarting;
static UINT64 gPauseTotal;
static UINT64 gPauseStart;
static UINT64 gAudioIgnoreBefore;
static DWORD gRecordingLimitFramerate;
static DWORD gRecordingDroppedFrames;
static UINT64 gRecordingLastFrame;
static UINT64 gRecordingNextEncode;
static UINT64 gRecordingNextTooltip;
static EXECUTION_STATE gRecordingState;
static WCHAR gRecordingPath[MAX_PATH];
static WCHAR gRecordingBase[MAX_PATH];
static int gShotIndex;
static BOOL gShotRequested;
static BOOL gBorderFlash;

static BOOL gDragging;
static int gDragHit;
static POINT gDragAnchor;
static RECT gDragStart;
static HWND gDragWindow;
static int gPressed;
static int gHover;
static BOOL gRubberDrag;
static POINT gRubberStart;
static RECT gRubberLimit;

static void ShowNotification(LPCWSTR Message, LPCWSTR Title, DWORD Flags);
static void UpdateTrayTitle(LPCWSTR Title);
static void UpdateTrayIcon(HICON Icon);
static void StopRecording(void);
static void StartArmed(void);
static void DismissUi(void);
static void CommandMode(int Mode);
static void RequestShot(void);
static void PlaceToolbar(void);
static void MoveFrame(RECT Outer, BOOL ClickThrough);
static void ClearFold(void);
static void RestoreOverlay(void);
static void SyncTaskbar(void);
static void EnsureTooltip(void);
static void EncodeCapturedAudio(void);
static UINT64 AdjustTime(UINT64 Time);
static int BorderPx(void);

static void ShowNotification(LPCWSTR Message, LPCWSTR Title, DWORD Flags)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = gWindow,
		.uFlags = NIF_INFO | NIF_TIP,
		.dwInfoFlags = Flags,
	};
	StrCpyNW(Data.szTip, WCAP_TITLE, _countof(Data.szTip));
	StrCpyNW(Data.szInfo, Message, _countof(Data.szInfo));
	StrCpyNW(Data.szInfoTitle, Title ? Title : WCAP_TITLE, _countof(Data.szInfoTitle));
	Shell_NotifyIconW(NIM_MODIFY, &Data);
}

static void UpdateTrayTitle(LPCWSTR Title)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = gWindow,
		.uFlags = NIF_TIP,
	};
	StrCpyNW(Data.szTip, Title, _countof(Data.szTip));
	Shell_NotifyIconW(NIM_MODIFY, &Data);
}

static void UpdateTrayIcon(HICON Icon)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = gWindow,
		.uFlags = NIF_ICON,
		.hIcon = Icon,
	};
	Shell_NotifyIconW(NIM_MODIFY, &Data);
}

static void AddTrayIcon(HWND Window)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = Window,
		.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP,
		.uCallbackMessage = WM_WCAP_COMMAND,
		.hIcon = gIcon1,
	};
	StrCpyNW(Data.szTip, WCAP_TITLE, _countof(Data.szTip));
	Shell_NotifyIconW(NIM_ADD, &Data);
}

static void RemoveTrayIcon(HWND Window)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = Window,
	};
	Shell_NotifyIconW(NIM_DELETE, &Data);
}

static void OpenRecordingsFolder(void)
{
	ShellExecuteW(NULL, L"open", gConfig.OutputFolder, NULL, NULL, SW_SHOWNORMAL);
}

static void ShowFileInFolder(LPCWSTR Filename)
{
	SFGAOF Flags;
	PIDLIST_ABSOLUTE List;
	if (Filename[0] && SUCCEEDED(SHParseDisplayName(Filename, NULL, &List, 0, &Flags)))
	{
		HR(SHOpenFolderAndSelectItems(List, 0, NULL, 0));
		CoTaskMemFree(List);
	}
}

static UINT64 AdjustTime(UINT64 Time)
{
	UINT64 Pause = gPauseTotal;
	if (gPaused)
	{
		LARGE_INTEGER Now;
		QueryPerformanceCounter(&Now);
		Pause += (UINT64)Now.QuadPart - gPauseStart;
	}
	if (Time > Pause)
	{
		return Time - Pause;
	}
	return Time;
}

static void SanitizeTopic(WCHAR* Text)
{
	WCHAR* Start = Text;
	while (*Start == L' ' || *Start == L'.')
	{
		Start++;
	}
	if (Start != Text)
	{
		MoveMemory(Text, Start, (lstrlenW(Start) + 1) * sizeof(WCHAR));
	}
	for (WCHAR* P = Text; *P; P++)
	{
		if (*P == L'\\' || *P == L'/' || *P == L':' || *P == L'*' || *P == L'?' ||
			*P == L'"' || *P == L'<' || *P == L'>' || *P == L'|')
		{
			*P = L'_';
		}
	}
	int Length = lstrlenW(Text);
	while (Length > 0 && (Text[Length - 1] == L' ' || Text[Length - 1] == L'.'))
	{
		Text[--Length] = 0;
	}
}

static BOOL BuildOutputPaths(void)
{
	SYSTEMTIME Time;
	GetLocalTime(&Time);

	WCHAR Topic[64];
	StrCpyNW(Topic, gConfig.FileTopic, _countof(Topic));
	SanitizeTopic(Topic);

	WCHAR Stamp[32];
	StrFormat(Stamp, L"%04u%02u%02u_%02u%02u%02u",
		Time.wYear, Time.wMonth, Time.wDay, Time.wHour, Time.wMinute, Time.wSecond);

	WCHAR Name[160];
	if (Topic[0] && gConfig.TopicAtEnd)
	{
		StrFormat(Name, L"%ls_%ls", Stamp, Topic);
	}
	else if (Topic[0])
	{
		StrFormat(Name, L"%ls_%ls", Topic, Stamp);
	}
	else
	{
		StrCpyW(Name, Stamp);
	}

	StrCpyW(gRecordingBase, gConfig.OutputFolder);
	if (!PathAppendW(gRecordingBase, Name))
	{
		return FALSE;
	}
	if (lstrlenW(gRecordingBase) + 4 >= MAX_PATH)
	{
		return FALSE;
	}
	StrCpyW(gRecordingPath, gRecordingBase);
	StrCatW(gRecordingPath, L".mp4");
	return TRUE;
}

static BOOL BuildShotPath(WCHAR* Path, int Index)
{
	WCHAR Suffix[16];
	StrFormat(Suffix, L"_%02d.png", Index);
	if (lstrlenW(gRecordingBase) + lstrlenW(Suffix) >= MAX_PATH)
	{
		return FALSE;
	}
	StrCpyW(Path, gRecordingBase);
	StrCatW(Path, Suffix);
	return TRUE;
}

static BOOL SavePng(ID3D11Texture2D* Texture, RECT Rect, LPCWSTR Path)
{
	BOOL Ok = FALSE;
	ID3D11Device* Device = NULL;
	ID3D11DeviceContext* Context = NULL;
	ID3D11Multithread* Guard = NULL;
	ID3D11Texture2D* Copy = NULL;
	IWICImagingFactory* Factory = NULL;
	IWICStream* Stream = NULL;
	IWICBitmapEncoder* Png = NULL;
	IWICBitmapFrameEncode* Bitmap = NULL;
	BOOL Mapped = FALSE;

	int Width = Rect.right - Rect.left;
	int Height = Rect.bottom - Rect.top;
	if (Width <= 0 || Height <= 0)
	{
		return FALSE;
	}

	ID3D11Texture2D_GetDevice(Texture, &Device);
	ID3D11Device_GetImmediateContext(Device, &Context);
	if (SUCCEEDED(ID3D11Device_QueryInterface(Device, &IID_ID3D11Multithread, (void**)&Guard)))
	{
		ID3D11Multithread_Enter(Guard);
	}

	D3D11_TEXTURE2D_DESC Desc;
	ID3D11Texture2D_GetDesc(Texture, &Desc);
	if (Rect.left < 0) Rect.left = 0;
	if (Rect.top < 0) Rect.top = 0;
	if (Rect.right > (LONG)Desc.Width) Rect.right = (LONG)Desc.Width;
	if (Rect.bottom > (LONG)Desc.Height) Rect.bottom = (LONG)Desc.Height;
	Width = Rect.right - Rect.left;
	Height = Rect.bottom - Rect.top;
	if (Width <= 0 || Height <= 0)
	{
		goto Done;
	}

	D3D11_TEXTURE2D_DESC Staging =
	{
		.Width = (UINT)Width,
		.Height = (UINT)Height,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = Desc.Format,
		.SampleDesc = { 1, 0 },
		.Usage = D3D11_USAGE_STAGING,
		.CPUAccessFlags = D3D11_CPU_ACCESS_READ,
	};
	if (FAILED(ID3D11Device_CreateTexture2D(Device, &Staging, NULL, &Copy)))
	{
		goto Done;
	}

	D3D11_BOX Box =
	{
		.left = (UINT)Rect.left,
		.top = (UINT)Rect.top,
		.right = (UINT)Rect.right,
		.bottom = (UINT)Rect.bottom,
		.front = 0,
		.back = 1,
	};
	ID3D11DeviceContext_CopySubresourceRegion(Context, (ID3D11Resource*)Copy, 0, 0, 0, 0, (ID3D11Resource*)Texture, 0, &Box);

	D3D11_MAPPED_SUBRESOURCE Map;
	if (FAILED(ID3D11DeviceContext_Map(Context, (ID3D11Resource*)Copy, 0, D3D11_MAP_READ, 0, &Map)))
	{
		goto Done;
	}
	Mapped = TRUE;

	if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void**)&Factory)))
	{
		goto Done;
	}
	if (FAILED(IWICImagingFactory_CreateStream(Factory, &Stream)))
	{
		goto Done;
	}
	if (FAILED(IWICStream_InitializeFromFilename(Stream, Path, GENERIC_WRITE)))
	{
		goto Done;
	}
	if (FAILED(IWICImagingFactory_CreateEncoder(Factory, &GUID_ContainerFormatPng, NULL, &Png)))
	{
		goto Done;
	}
	if (FAILED(IWICBitmapEncoder_Initialize(Png, (IStream*)Stream, WICBitmapEncoderNoCache)))
	{
		goto Done;
	}
	if (FAILED(IWICBitmapEncoder_CreateNewFrame(Png, &Bitmap, NULL)))
	{
		goto Done;
	}
	if (FAILED(IWICBitmapFrameEncode_Initialize(Bitmap, NULL)))
	{
		goto Done;
	}
	if (FAILED(IWICBitmapFrameEncode_SetSize(Bitmap, (UINT)Width, (UINT)Height)))
	{
		goto Done;
	}
	WICPixelFormatGUID Format = GUID_WICPixelFormat32bppBGRA;
	if (FAILED(IWICBitmapFrameEncode_SetPixelFormat(Bitmap, &Format)) || !IsEqualGUID(&Format, &GUID_WICPixelFormat32bppBGRA))
	{
		goto Done;
	}
	UINT ImageBytes = Map.RowPitch * (UINT)Height;
	if (FAILED(IWICBitmapFrameEncode_WritePixels(Bitmap, (UINT)Height, Map.RowPitch, ImageBytes, (BYTE*)Map.pData)))
	{
		goto Done;
	}
	if (FAILED(IWICBitmapFrameEncode_Commit(Bitmap)) || FAILED(IWICBitmapEncoder_Commit(Png)))
	{
		goto Done;
	}
	Ok = TRUE;

Done:
	if (Mapped)
	{
		ID3D11DeviceContext_Unmap(Context, (ID3D11Resource*)Copy, 0);
	}
	if (Bitmap) IWICBitmapFrameEncode_Release(Bitmap);
	if (Png) IWICBitmapEncoder_Release(Png);
	if (Stream) IWICStream_Release(Stream);
	if (Factory) IWICImagingFactory_Release(Factory);
	if (Copy) ID3D11Texture2D_Release(Copy);
	if (Guard)
	{
		ID3D11Multithread_Leave(Guard);
		ID3D11Multithread_Release(Guard);
	}
	if (Context) ID3D11DeviceContext_Release(Context);
	if (Device) ID3D11Device_Release(Device);
	return Ok;
}

static UINT DpiOfMonitor(HMONITOR Monitor)
{
	UINT X = 96;
	UINT Y = 96;
	HMODULE Lib = LoadLibraryW(L"shcore.dll");
	if (Lib)
	{
		typedef HRESULT (WINAPI *Fn)(HMONITOR, int, UINT*, UINT*);
		Fn GetDpi = (Fn)GetProcAddress(Lib, "GetDpiForMonitor");
		if (GetDpi)
		{
			GetDpi(Monitor, 0, &X, &Y);
		}
		FreeLibrary(Lib);
	}
	return X;
}

static int BorderFor(UINT Dpi)
{
	int Border = MulDiv(4, (int)Dpi, 96);
	if (Border < 3)
	{
		Border = 3;
	}
	return Border;
}

static int BorderPx(void)
{
	UINT Dpi = 96;
	if (gFrame)
	{
		Dpi = GetDpiForWindow(gFrame);
	}
	return BorderFor(Dpi);
}

static RECT ExpandBorder(RECT Inner, int Border)
{
	Inner.left -= Border;
	Inner.top -= Border;
	Inner.right += Border;
	Inner.bottom += Border;
	return Inner;
}

static RECT VisualInterior(void)
{
	RECT Outer;
	GetWindowRect(gFrame, &Outer);
	int Border = BorderPx();
	Outer.left += Border;
	Outer.top += Border;
	Outer.right -= Border;
	Outer.bottom -= Border;
	return Outer;
}

static void NormalizeInterior(RECT* Interior, const RECT* Limit, BOOL KeepSize)
{
	int Width = Interior->right - Interior->left;
	int Height = Interior->bottom - Interior->top;
	if (!KeepSize)
	{
		if (Width < 0)
		{
			LONG Swap = Interior->left;
			Interior->left = Interior->right;
			Interior->right = Swap;
			Width = -Width;
		}
		if (Height < 0)
		{
			LONG Swap = Interior->top;
			Interior->top = Interior->bottom;
			Interior->bottom = Swap;
			Height = -Height;
		}
		Width &= ~1;
		Height &= ~1;
		if (Width < 64) Width = 64;
		if (Height < 64) Height = 64;
	}
	int MaxW = Limit->right - Limit->left;
	int MaxH = Limit->bottom - Limit->top;
	if (MaxW < 2) MaxW = 2;
	if (MaxH < 2) MaxH = 2;
	if (Width > MaxW) Width = MaxW & ~1;
	if (Height > MaxH) Height = MaxH & ~1;
	if (Width < 2) Width = MaxW;
	if (Height < 2) Height = MaxH;
	if (Interior->left < Limit->left) Interior->left = Limit->left;
	if (Interior->top < Limit->top) Interior->top = Limit->top;
	if (Interior->left + Width > Limit->right) Interior->left = Limit->right - Width;
	if (Interior->top + Height > Limit->bottom) Interior->top = Limit->bottom - Height;
	Interior->right = Interior->left + Width;
	Interior->bottom = Interior->top + Height;
}

static void UpdateFrameRegion(void)
{
	RECT Client;
	GetClientRect(gFrame, &Client);
	int Border = BorderPx();
	if (Client.right <= Border * 2 || Client.bottom <= Border * 2)
	{
		return;
	}
	HRGN Outer = CreateRectRgn(0, 0, Client.right, Client.bottom);
	HRGN Inner = CreateRectRgn(Border, Border, Client.right - Border, Client.bottom - Border);
	CombineRgn(Outer, Outer, Inner, RGN_DIFF);
	DeleteObject(Inner);
	SetWindowRgn(gFrame, Outer, TRUE);
}

static void UpdateToolbarRegion(void)
{
	RECT Client;
	GetClientRect(gToolbar, &Client);
	HRGN Round = CreateRoundRectRgn(0, 0, Client.right + 1, Client.bottom + 1, Client.bottom, Client.bottom);
	SetWindowRgn(gToolbar, Round, TRUE);
}

static void EnsureTooltip(void)
{
	if (gTip || !gToolbar)
	{
		return;
	}
	INITCOMMONCONTROLSEX Controls = { sizeof(Controls), ICC_BAR_CLASSES };
	InitCommonControlsEx(&Controls);
	gTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASS, NULL,
		WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		gToolbar, NULL, GetModuleHandleW(NULL), NULL);
	if (!gTip)
	{
		return;
	}
	SetWindowDisplayAffinity(gTip, WDA_EXCLUDEFROMCAPTURE);
	TOOLINFOW Info;
	ZeroMemory(&Info, sizeof(Info));
	Info.cbSize = sizeof(Info);
	Info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
	Info.hwnd = gToolbar;
	Info.uId = (UINT_PTR)gToolbar;
	Info.lpszText = LPSTR_TEXTCALLBACKW;
	SendMessageW(gTip, TTM_ADDTOOLW, 0, (LPARAM)&Info);
}

static void PlaceToolbar(void)
{
	if (!gToolbar || gPhase != PHASE_ARMED || gFold != FOLD_NONE)
	{
		if (gToolbar)
		{
			ShowWindow(gToolbar, SW_HIDE);
		}
		return;
	}

	RECT Frame;
	GetWindowRect(gFrame, &Frame);
	UINT Dpi = GetDpiForWindow(gFrame);
	int Width = MulDiv(680, (int)Dpi, 96);
	int Height = MulDiv(44, (int)Dpi, 96);
	int X = Frame.left + ((Frame.right - Frame.left) - Width) / 2;
	int Y = Frame.top - Height / 2;

	HMONITOR Monitor = MonitorFromRect(&Frame, MONITOR_DEFAULTTONEAREST);
	MONITORINFO Info = { .cbSize = sizeof(Info) };
	if (GetMonitorInfoW(Monitor, &Info))
	{
		int MaxW = Info.rcMonitor.right - Info.rcMonitor.left - 16;
		if (Width > MaxW) Width = MaxW;
		if (X < Info.rcMonitor.left + 8) X = Info.rcMonitor.left + 8;
		if (X + Width > Info.rcMonitor.right - 8) X = Info.rcMonitor.right - 8 - Width;
		if (Y < Info.rcMonitor.top + 4) Y = Info.rcMonitor.top + MulDiv(8, (int)Dpi, 96);
		if (Y + Height > Info.rcMonitor.bottom - 4) Y = Info.rcMonitor.bottom - 4 - Height;
	}

	HWND Z = gSettingsOpen ? HWND_NOTOPMOST : HWND_TOPMOST;
	SetWindowPos(gToolbar, Z, X, Y, Width, Height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
	UpdateToolbarRegion();
	EnsureTooltip();
	InvalidateRect(gToolbar, NULL, FALSE);
}

static void MoveFrame(RECT Outer, BOOL ClickThrough)
{
	LONG ExStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
	if (ClickThrough)
	{
		ExStyle |= WS_EX_TRANSPARENT;
	}
	SetWindowLongW(gFrame, GWL_EXSTYLE, ExStyle);
	int Width = Outer.right - Outer.left;
	int Height = Outer.bottom - Outer.top;
	if (Width < 8) Width = 8;
	if (Height < 8) Height = 8;
	UINT Show = gFold == FOLD_NONE ? SWP_SHOWWINDOW : 0;
	HWND Z = gSettingsOpen ? HWND_NOTOPMOST : HWND_TOPMOST;
	SetWindowPos(gFrame, Z, Outer.left, Outer.top, Width, Height, SWP_NOACTIVATE | SWP_FRAMECHANGED | Show);
	UpdateFrameRegion();
	InvalidateRect(gFrame, NULL, FALSE);
	if (ClickThrough || gFold != FOLD_NONE)
	{
		ShowWindow(gToolbar, SW_HIDE);
	}
	else
	{
		PlaceToolbar();
	}
}

static BOOL GetExtendedBounds(HWND Window, RECT* Out)
{
	if (!IsWindow(Window))
	{
		return FALSE;
	}
	if (FAILED(DwmGetWindowAttribute(Window, DWMWA_EXTENDED_FRAME_BOUNDS, Out, sizeof(*Out))))
	{
		return GetWindowRect(Window, Out);
	}
	return Out->right > Out->left && Out->bottom > Out->top;
}

static BOOL GetTargetBounds(RECT* Out)
{
	if (!IsWindow(gTarget) || IsIconic(gTarget))
	{
		return FALSE;
	}
	if (gConfig.OnlyClientArea)
	{
		RECT Client;
		if (!GetClientRect(gTarget, &Client))
		{
			return FALSE;
		}
		POINT TopLeft = { 0, 0 };
		ClientToScreen(gTarget, &TopLeft);
		Out->left = TopLeft.x;
		Out->top = TopLeft.y;
		Out->right = TopLeft.x + Client.right;
		Out->bottom = TopLeft.y + Client.bottom;
		return Out->right > Out->left && Out->bottom > Out->top;
	}
	return GetExtendedBounds(gTarget, Out);
}

static void RememberCrop(void)
{
	if (gMode != SCREENREC_MODE_WINDOW_REGION)
	{
		return;
	}
	RECT Bounds;
	if (!GetTargetBounds(&Bounds))
	{
		return;
	}
	RECT Inner = VisualInterior();
	gCropOffset.x = Inner.left - Bounds.left;
	gCropOffset.y = Inner.top - Bounds.top;
	gCropSize.cx = Inner.right - Inner.left;
	gCropSize.cy = Inner.bottom - Inner.top;
}

static RECT MonitorLimit(void)
{
	MONITORINFO Info = { .cbSize = sizeof(Info) };
	if (!GetMonitorInfoW(gMonitor, &Info))
	{
		Info.rcMonitor = (RECT){ 0, 0, 1920, 1080 };
	}
	return Info.rcMonitor;
}

static void SyncRegionRect(void)
{
	if (!gRecording || gMode != SCREENREC_MODE_SCREEN_REGION)
	{
		return;
	}
	MONITORINFO Info = { .cbSize = sizeof(Info) };
	if (!GetMonitorInfoW(gMonitor, &Info))
	{
		return;
	}
	RECT Inner = VisualInterior();
	Inner.right = Inner.left + gLockedW;
	Inner.bottom = Inner.top + gLockedH;
	NormalizeInterior(&Inner, &Info.rcMonitor, TRUE);
	gCapture.Rect.left = Inner.left - Info.rcMonitor.left;
	gCapture.Rect.top = Inner.top - Info.rcMonitor.top;
	gCapture.Rect.right = gCapture.Rect.left + (Inner.right - Inner.left);
	gCapture.Rect.bottom = gCapture.Rect.top + (Inner.bottom - Inner.top);
}

static RECT DragLimit(void)
{
	if (gMode == SCREENREC_MODE_WINDOW_REGION)
	{
		RECT Bounds;
		if (GetTargetBounds(&Bounds))
		{
			return Bounds;
		}
	}
	return MonitorLimit();
}

static void ApplyDrag(POINT Screen)
{
	int Dx = Screen.x - gDragAnchor.x;
	int Dy = Screen.y - gDragAnchor.y;
	RECT Outer = gDragStart;
	switch (gDragHit)
	{
		case HTCAPTION: OffsetRect(&Outer, Dx, Dy); break;
		case HTLEFT: Outer.left += Dx; break;
		case HTRIGHT: Outer.right += Dx; break;
		case HTTOP: Outer.top += Dy; break;
		case HTBOTTOM: Outer.bottom += Dy; break;
		case HTTOPLEFT: Outer.left += Dx; Outer.top += Dy; break;
		case HTTOPRIGHT: Outer.right += Dx; Outer.top += Dy; break;
		case HTBOTTOMLEFT: Outer.left += Dx; Outer.bottom += Dy; break;
		case HTBOTTOMRIGHT: Outer.right += Dx; Outer.bottom += Dy; break;
		default: break;
	}

	int Border = BorderPx();
	RECT Inner = Outer;
	Inner.left += Border;
	Inner.top += Border;
	Inner.right -= Border;
	Inner.bottom -= Border;
	RECT Limit = DragLimit();
	BOOL KeepSize = gDragHit == HTCAPTION || gRecording;
	NormalizeInterior(&Inner, &Limit, KeepSize);
	MoveFrame(ExpandBorder(Inner, Border), FALSE);
	SyncRegionRect();
}

static void FinishDrag(void)
{
	gDragging = FALSE;
	gDragHit = 0;
	if (gDragWindow)
	{
		ReleaseCapture();
		gDragWindow = NULL;
	}
	RememberCrop();
	SyncRegionRect();
}

static int HitTestFrame(int X, int Y)
{
	if (gPhase != PHASE_ARMED || gRecording)
	{
		return HTCLIENT;
	}
	if (gMode != SCREENREC_MODE_SCREEN_REGION && gMode != SCREENREC_MODE_WINDOW_REGION)
	{
		return HTCLIENT;
	}

	RECT Client;
	GetClientRect(gFrame, &Client);
	int Left = X;
	int Right = Client.right - 1 - X;
	int Top = Y;
	int Bottom = Client.bottom - 1 - Y;
	int Corner = MulDiv(22, (int)GetDpiForWindow(gFrame), 96);
	BOOL NearLeft = Left < Corner;
	BOOL NearRight = Right < Corner;
	BOOL NearTop = Top < Corner;
	BOOL NearBottom = Bottom < Corner;
	if (NearTop && NearLeft) return HTTOPLEFT;
	if (NearTop && NearRight) return HTTOPRIGHT;
	if (NearBottom && NearLeft) return HTBOTTOMLEFT;
	if (NearBottom && NearRight) return HTBOTTOMRIGHT;
	if (Top <= Bottom && Top <= Left && Top <= Right) return HTTOP;
	if (Bottom < Top && Bottom <= Left && Bottom <= Right) return HTBOTTOM;
	if (Left <= Right) return HTLEFT;
	return HTRIGHT;
}

static HCURSOR CursorForHit(int Hit)
{
	switch (Hit)
	{
		case HTTOPLEFT:
		case HTBOTTOMRIGHT: return LoadCursor(NULL, IDC_SIZENWSE);
		case HTTOPRIGHT:
		case HTBOTTOMLEFT: return LoadCursor(NULL, IDC_SIZENESW);
		case HTTOP:
		case HTBOTTOM: return LoadCursor(NULL, IDC_SIZENS);
		case HTLEFT:
		case HTRIGHT: return LoadCursor(NULL, IDC_SIZEWE);
		case HTCAPTION: return LoadCursor(NULL, IDC_SIZEALL);
		default: return LoadCursor(NULL, IDC_ARROW);
	}
}

static void DragBegin(HWND Window, int Hit, POINT Screen)
{
	gDragging = TRUE;
	gDragHit = Hit;
	gDragAnchor = Screen;
	gDragWindow = Window;
	GetWindowRect(gFrame, &gDragStart);
	SetCapture(Window);
}

static void RemoveHooks(void)
{
	if (gMouseHook)
	{
		UnhookWindowsHookEx(gMouseHook);
		gMouseHook = NULL;
	}
	if (gKeyHook)
	{
		UnhookWindowsHookEx(gKeyHook);
		gKeyHook = NULL;
	}
}

static BOOL IsCapturable(HWND Window)
{
	if (!Window || !IsWindowVisible(Window) || IsIconic(Window))
	{
		return FALSE;
	}
	if (Window == gFrame || Window == gToolbar || Window == gWindow || Window == gShade || Window == gTaskbar)
	{
		return FALSE;
	}
	DWORD Pid = 0;
	GetWindowThreadProcessId(Window, &Pid);
	if (Pid == GetCurrentProcessId())
	{
		return FALSE;
	}
	if (GetWindowLongW(Window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
	{
		return FALSE;
	}
	DWORD Cloaked = 0;
	if (SUCCEEDED(DwmGetWindowAttribute(Window, DWMWA_CLOAKED, &Cloaked, sizeof(Cloaked))) && Cloaked)
	{
		return FALSE;
	}
	WCHAR Class[64];
	if (GetClassNameW(Window, Class, _countof(Class)))
	{
		if (lstrcmpW(Class, L"Progman") == 0 || lstrcmpW(Class, L"WorkerW") == 0 ||
			lstrcmpW(Class, L"Shell_TrayWnd") == 0 || lstrcmpW(Class, L"Shell_SecondaryTrayWnd") == 0)
		{
			return FALSE;
		}
	}
	RECT Rect;
	if (!GetWindowRect(Window, &Rect))
	{
		return FALSE;
	}
	if (Rect.right - Rect.left < 32 || Rect.bottom - Rect.top < 32)
	{
		return FALSE;
	}
	return TRUE;
}

static void UpdatePick(void)
{
	POINT Cursor;
	GetCursorPos(&Cursor);
	HWND Window = WindowFromPoint(Cursor);
	if (Window)
	{
		Window = GetAncestor(Window, GA_ROOT);
	}
	if (!IsCapturable(Window))
	{
		ShowWindow(gFrame, SW_HIDE);
		return;
	}

	HWND Previous = gTarget;
	gTarget = Window;
	RECT Bounds;
	BOOL HaveBounds = GetTargetBounds(&Bounds);
	gTarget = Previous;
	if (!HaveBounds)
	{
		ShowWindow(gFrame, SW_HIDE);
		return;
	}
	HMONITOR Monitor = MonitorFromWindow(Window, MONITOR_DEFAULTTONEAREST);
	int Border = BorderFor(DpiOfMonitor(Monitor));
	MoveFrame(ExpandBorder(Bounds, Border), TRUE);
}

static void FollowTarget(void)
{
	if (gDragging || gPhase != PHASE_ARMED)
	{
		return;
	}
	if (gMode != SCREENREC_MODE_WINDOW && gMode != SCREENREC_MODE_WINDOW_REGION)
	{
		return;
	}
	if (!IsWindow(gTarget))
	{
		ShowNotification(L"Окно закрыто.", WCAP_TITLE, NIIF_INFO);
		if (gRecording)
		{
			StopRecording();
		}
		DismissUi();
		return;
	}

	RECT Bounds;
	if (!GetTargetBounds(&Bounds))
	{
		return;
	}

	RECT Inner;
	if (gMode == SCREENREC_MODE_WINDOW)
	{
		Inner = Bounds;
	}
	else
	{
		Inner.left = Bounds.left + gCropOffset.x;
		Inner.top = Bounds.top + gCropOffset.y;
		Inner.right = Inner.left + gCropSize.cx;
		Inner.bottom = Inner.top + gCropSize.cy;
		NormalizeInterior(&Inner, &Bounds, TRUE);
		gCropOffset.x = Inner.left - Bounds.left;
		gCropOffset.y = Inner.top - Bounds.top;
		gCropSize.cx = Inner.right - Inner.left;
		gCropSize.cy = Inner.bottom - Inner.top;
	}

	int Border = BorderPx();
	RECT Outer = ExpandBorder(Inner, Border);
	RECT Current;
	GetWindowRect(gFrame, &Current);
	if (Current.left != Outer.left || Current.top != Outer.top || Current.right != Outer.right || Current.bottom != Outer.bottom)
	{
		MoveFrame(Outer, FALSE);
	}
}

static LRESULT CALLBACK PickMouseProc(int Code, WPARAM WParam, LPARAM LParam)
{
	if (Code == HC_ACTION && gPhase == PHASE_PICK && (WParam == WM_LBUTTONDOWN || WParam == WM_RBUTTONDOWN))
	{
		MSLLHOOKSTRUCT* Info = (MSLLHOOKSTRUCT*)LParam;
		HWND Window = WindowFromPoint(Info->pt);
		if (Window)
		{
			Window = GetAncestor(Window, GA_ROOT);
		}
		PostMessageW(gWindow, WParam == WM_LBUTTONDOWN ? WM_WCAP_PICKED : WM_WCAP_CANCEL_UI, 0, (LPARAM)Window);
		return 1;
	}
	return CallNextHookEx(NULL, Code, WParam, LParam);
}

static LRESULT CALLBACK PickKeyProc(int Code, WPARAM WParam, LPARAM LParam)
{
	if (Code == HC_ACTION && (gPhase == PHASE_PICK || gPhase == PHASE_RUBBER) && (WParam == WM_KEYDOWN || WParam == WM_SYSKEYDOWN))
	{
		KBDLLHOOKSTRUCT* Info = (KBDLLHOOKSTRUCT*)LParam;
		if (Info->vkCode == VK_ESCAPE)
		{
			PostMessageW(gWindow, WM_WCAP_CANCEL_UI, 0, 0);
			return 1;
		}
	}
	return CallNextHookEx(NULL, Code, WParam, LParam);
}

static void BeginPick(int Mode)
{
	gMode = Mode;
	gTarget = NULL;
	gPhase = PHASE_PICK;
	gConfig.LastMode = (DWORD)Mode;
	Config_Save(&gConfig, gConfigPath);
	RemoveHooks();
	gMouseHook = SetWindowsHookExW(WH_MOUSE_LL, PickMouseProc, NULL, 0);
	gKeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, PickKeyProc, NULL, 0);
	SetTimer(gWindow, TIMER_FOLLOW, 40, NULL);
	if (!gMouseHook)
	{
		ShowNotification(L"Не удалось начать выбор окна.", WCAP_TITLE, NIIF_WARNING);
	}
	UpdatePick();
}

static void BeginRegion(int Mode)
{
	POINT Mouse;
	GetCursorPos(&Mouse);
	HMONITOR Monitor = MonitorFromPoint(Mouse, MONITOR_DEFAULTTONEAREST);
	MONITORINFO Info = { .cbSize = sizeof(Info) };
	if (!GetMonitorInfoW(Monitor, &Info))
	{
		ShowNotification(L"Неизвестный монитор.", WCAP_TITLE, NIIF_WARNING);
		return;
	}

	gMonitor = Monitor;
	gMode = Mode;
	gTarget = NULL;
	gPhase = PHASE_ARMED;
	RemoveHooks();

	int Border = BorderFor(DpiOfMonitor(Monitor));
	if (Mode == SCREENREC_MODE_MONITOR)
	{
		MoveFrame(Info.rcMonitor, FALSE);
	}
	else
	{
		int MaxW = Info.rcMonitor.right - Info.rcMonitor.left;
		int MaxH = Info.rcMonitor.bottom - Info.rcMonitor.top;
		int Width = 1280;
		int Height = 720;
		if (Width > MaxW) Width = MaxW;
		if (Height > MaxH) Height = MaxH;
		Width &= ~1;
		Height &= ~1;
		if (Width < 2) Width = MaxW;
		if (Height < 2) Height = MaxH;
		RECT Inner;
		Inner.left = Info.rcMonitor.left + (MaxW - Width) / 2;
		Inner.top = Info.rcMonitor.top + (MaxH - Height) / 2;
		Inner.right = Inner.left + Width;
		Inner.bottom = Inner.top + Height;
		MoveFrame(ExpandBorder(Inner, Border), FALSE);
	}

	gConfig.LastMode = (DWORD)Mode;
	Config_Save(&gConfig, gConfigPath);
	SetTimer(gWindow, TIMER_FOLLOW, 50, NULL);
}

static void ArmWindow(HWND Window)
{
	if (!IsCapturable(Window))
	{
		ShowNotification(L"Это окно нельзя записать.", WCAP_TITLE, NIIF_WARNING);
		return;
	}
	DWORD Affinity = WDA_NONE;
	if (GetWindowDisplayAffinity(Window, &Affinity) && Affinity != WDA_NONE)
	{
		ShowNotification(L"Окно исключено из захвата.", WCAP_TITLE, NIIF_WARNING);
		return;
	}

	gTarget = Window;
	gMonitor = MonitorFromWindow(Window, MONITOR_DEFAULTTONEAREST);
	RemoveHooks();
	gPhase = PHASE_ARMED;

	RECT Bounds;
	if (!GetTargetBounds(&Bounds))
	{
		ShowNotification(L"Не удалось прочитать размер окна.", WCAP_TITLE, NIIF_WARNING);
		DismissUi();
		return;
	}

	RECT Inner = Bounds;
	if (gMode == SCREENREC_MODE_WINDOW_REGION)
	{
		MONITORINFO Info = { .cbSize = sizeof(Info) };
		RECT Clipped;
		if (GetMonitorInfoW(gMonitor, &Info) && IntersectRect(&Clipped, &Bounds, &Info.rcMonitor))
		{
			Inner = Clipped;
		}
		NormalizeInterior(&Inner, &Bounds, FALSE);
		gCropOffset.x = Inner.left - Bounds.left;
		gCropOffset.y = Inner.top - Bounds.top;
		gCropSize.cx = Inner.right - Inner.left;
		gCropSize.cy = Inner.bottom - Inner.top;
	}

	MoveFrame(ExpandBorder(Inner, BorderFor(DpiOfMonitor(gMonitor))), FALSE);
	gConfig.LastMode = (DWORD)gMode;
	Config_Save(&gConfig, gConfigPath);
	SetTimer(gWindow, TIMER_FOLLOW, 50, NULL);
}

static void PlaceShade(void)
{
	if (!gShade)
	{
		return;
	}
	int X = GetSystemMetrics(SM_XVIRTUALSCREEN);
	int Y = GetSystemMetrics(SM_YVIRTUALSCREEN);
	int Width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	int Height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
	if (Width < 1 || Height < 1)
	{
		X = 0;
		Y = 0;
		Width = GetSystemMetrics(SM_CXSCREEN);
		Height = GetSystemMetrics(SM_CYSCREEN);
	}
	SetWindowPos(gShade, HWND_TOPMOST, X, Y, Width, Height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void HideShade(void)
{
	if (gShade)
	{
		ShowWindow(gShade, SW_HIDE);
	}
}

static POINT ShadePoint(HWND Window, LPARAM LParam)
{
	RECT Place;
	GetWindowRect(Window, &Place);
	POINT Screen;
	Screen.x = Place.left + GET_X_LPARAM(LParam);
	Screen.y = Place.top + GET_Y_LPARAM(LParam);
	return Screen;
}

static RECT ClipRubber(POINT A, POINT B, const RECT* Limit)
{
	RECT Raw;
	Raw.left = A.x < B.x ? A.x : B.x;
	Raw.top = A.y < B.y ? A.y : B.y;
	Raw.right = A.x > B.x ? A.x : B.x;
	Raw.bottom = A.y > B.y ? A.y : B.y;
	RECT Hit;
	if (!IntersectRect(&Hit, &Raw, Limit))
	{
		Hit.left = 0;
		Hit.top = 0;
		Hit.right = 0;
		Hit.bottom = 0;
	}
	return Hit;
}

static int RubberSpan(LONG A, LONG B)
{
	LONGLONG Delta = (LONGLONG)A - (LONGLONG)B;
	if (Delta < 0)
	{
		Delta = -Delta;
	}
	if (Delta > 1000000)
	{
		Delta = 1000000;
	}
	return (int)Delta;
}

static HWND WindowBelowShade(POINT Screen)
{
	LONG ExStyle = GetWindowLongW(gShade, GWL_EXSTYLE);
	SetWindowLongW(gShade, GWL_EXSTYLE, ExStyle | WS_EX_TRANSPARENT);
	SetWindowPos(gShade, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	HWND Found = WindowFromPoint(Screen);
	SetWindowLongW(gShade, GWL_EXSTYLE, ExStyle);
	SetWindowPos(gShade, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	if (!Found || Found == gShade)
	{
		ShowWindow(gShade, SW_HIDE);
		Found = WindowFromPoint(Screen);
		ShowWindow(gShade, SW_SHOWNOACTIVATE);
		SetWindowPos(gShade, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
	if (Found)
	{
		Found = GetAncestor(Found, GA_ROOT);
	}
	return Found;
}

static void ShowRubberPreview(const RECT* Inner)
{
	if (!gFrame)
	{
		return;
	}
	if (Inner->right - Inner->left < 2 || Inner->bottom - Inner->top < 2)
	{
		ShowWindow(gFrame, SW_HIDE);
		return;
	}
	MoveFrame(ExpandBorder(*Inner, BorderFor(DpiOfMonitor(gMonitor))), TRUE);
}

static BOOL BeginRubberLimit(POINT Screen)
{
	if (gMode == SCREENREC_MODE_WINDOW_REGION)
	{
		HWND Window = WindowBelowShade(Screen);
		if (!IsCapturable(Window))
		{
			ShowNotification(L"Это окно нельзя записать.", WCAP_TITLE, NIIF_WARNING);
			return FALSE;
		}
		DWORD Affinity = WDA_NONE;
		if (GetWindowDisplayAffinity(Window, &Affinity) && Affinity != WDA_NONE)
		{
			ShowNotification(L"Окно исключено из захвата.", WCAP_TITLE, NIIF_WARNING);
			return FALSE;
		}
		gTarget = Window;
		gMonitor = MonitorFromWindow(Window, MONITOR_DEFAULTTONEAREST);
		if (!GetTargetBounds(&gRubberLimit))
		{
			gTarget = NULL;
			ShowNotification(L"Не удалось прочитать размер окна.", WCAP_TITLE, NIIF_WARNING);
			return FALSE;
		}
		return TRUE;
	}

	gTarget = NULL;
	gMonitor = MonitorFromPoint(Screen, MONITOR_DEFAULTTONEAREST);
	MONITORINFO Info = { .cbSize = sizeof(Info) };
	if (!GetMonitorInfoW(gMonitor, &Info))
	{
		ShowNotification(L"Неизвестный монитор.", WCAP_TITLE, NIIF_WARNING);
		return FALSE;
	}
	gRubberLimit = Info.rcMonitor;
	return TRUE;
}

static void CommitRubber(POINT Screen)
{
	if (RubberSpan(Screen.x, gRubberStart.x) < 8 || RubberSpan(Screen.y, gRubberStart.y) < 8)
	{
		gTarget = NULL;
		if (gFrame) ShowWindow(gFrame, SW_HIDE);
		return;
	}

	if (gMode == SCREENREC_MODE_WINDOW_REGION)
	{
		RECT Bounds;
		if (!IsWindow(gTarget) || !GetTargetBounds(&Bounds))
		{
			gTarget = NULL;
			if (gFrame) ShowWindow(gFrame, SW_HIDE);
			ShowNotification(L"Окно закрыто.", WCAP_TITLE, NIIF_INFO);
			return;
		}
		gRubberLimit = Bounds;
	}

	RECT Inner = ClipRubber(gRubberStart, Screen, &gRubberLimit);
	if (Inner.right - Inner.left < 8 || Inner.bottom - Inner.top < 8)
	{
		gTarget = NULL;
		if (gFrame) ShowWindow(gFrame, SW_HIDE);
		return;
	}

	NormalizeInterior(&Inner, &gRubberLimit, FALSE);
	if (gMode == SCREENREC_MODE_WINDOW_REGION)
	{
		RECT Bounds;
		if (!GetTargetBounds(&Bounds))
		{
			gTarget = NULL;
			if (gFrame) ShowWindow(gFrame, SW_HIDE);
			return;
		}
		gCropOffset.x = Inner.left - Bounds.left;
		gCropOffset.y = Inner.top - Bounds.top;
		gCropSize.cx = Inner.right - Inner.left;
		gCropSize.cy = Inner.bottom - Inner.top;
	}

	HideShade();
	RemoveHooks();
	gPhase = PHASE_ARMED;
	MoveFrame(ExpandBorder(Inner, BorderFor(DpiOfMonitor(gMonitor))), FALSE);
	SetTimer(gWindow, TIMER_FOLLOW, 50, NULL);
}

static LRESULT CALLBACK ShadeProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	if (Message == WM_ERASEBKGND)
	{
		RECT Client;
		GetClientRect(Window, &Client);
		HBRUSH Brush = CreateSolidBrush(RGB(0, 0, 0));
		FillRect((HDC)WParam, &Client, Brush);
		DeleteObject(Brush);
		return 1;
	}
	else if (Message == WM_MOUSEACTIVATE)
	{
		return MA_NOACTIVATE;
	}
	else if (Message == WM_SETCURSOR)
	{
		SetCursor(LoadCursor(NULL, IDC_CROSS));
		return TRUE;
	}
	else if (Message == WM_LBUTTONDOWN || Message == WM_LBUTTONDBLCLK)
	{
		if (gPhase != PHASE_RUBBER || gRubberDrag)
		{
			return 0;
		}
		POINT Screen = ShadePoint(Window, LParam);
		if (!BeginRubberLimit(Screen))
		{
			return 0;
		}
		gRubberStart = Screen;
		gRubberDrag = TRUE;
		SetCapture(Window);
		return 0;
	}
	else if (Message == WM_MOUSEMOVE)
	{
		if (gPhase != PHASE_RUBBER || !gRubberDrag)
		{
			return 0;
		}
		POINT Screen = ShadePoint(Window, LParam);
		if (gMode == SCREENREC_MODE_WINDOW_REGION)
		{
			RECT Bounds;
			if (GetTargetBounds(&Bounds))
			{
				gRubberLimit = Bounds;
			}
		}
		RECT Inner = ClipRubber(gRubberStart, Screen, &gRubberLimit);
		ShowRubberPreview(&Inner);
		return 0;
	}
	else if (Message == WM_LBUTTONUP)
	{
		if (!gRubberDrag)
		{
			return 0;
		}
		POINT Screen = ShadePoint(Window, LParam);
		gRubberDrag = FALSE;
		if (GetCapture() == Window)
		{
			ReleaseCapture();
		}
		if (gPhase == PHASE_RUBBER)
		{
			CommitRubber(Screen);
		}
		return 0;
	}
	else if (Message == WM_CAPTURECHANGED)
	{
		if (gRubberDrag && (HWND)LParam != Window)
		{
			gRubberDrag = FALSE;
			gTarget = NULL;
			if (gFrame) ShowWindow(gFrame, SW_HIDE);
		}
		return 0;
	}
	else if (Message == WM_RBUTTONUP)
	{
		if (gPhase == PHASE_RUBBER && !gRecording)
		{
			DismissUi();
		}
		return 0;
	}
	return DefWindowProcW(Window, Message, WParam, LParam);
}

static void BeginRubber(int Mode)
{
	gMode = Mode;
	gTarget = NULL;
	gRubberDrag = FALSE;
	gPhase = PHASE_RUBBER;
	gConfig.LastMode = (DWORD)Mode;
	Config_Save(&gConfig, gConfigPath);
	RemoveHooks();
	gKeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, PickKeyProc, NULL, 0);
	if (gFrame) ShowWindow(gFrame, SW_HIDE);
	if (gToolbar) ShowWindow(gToolbar, SW_HIDE);
	if (!gShade)
	{
		RemoveHooks();
		gPhase = PHASE_HIDDEN;
		ShowNotification(L"Не удалось начать обозначение области.", WCAP_TITLE, NIIF_WARNING);
		return;
	}
	PlaceShade();
	LPCWSTR Text = Mode == SCREENREC_MODE_WINDOW_REGION
		? L"Зажмите левую кнопку на окне и обведите область внутри него. Правая кнопка или Esc — отмена."
		: L"Зажмите левую кнопку и обведите область. Правая кнопка или Esc — отмена.";
	if (!gKeyHook)
	{
		Text = L"Зажмите левую кнопку и обведите область. Правая кнопка — отмена.";
	}
	ShowNotification(Text, WCAP_TITLE, NIIF_INFO);
}

static void DismissUi(void)
{
	if (gRecording)
	{
		return;
	}
	ClearFold();
	gPhase = PHASE_HIDDEN;
	gTarget = NULL;
	gBorderFlash = FALSE;
	if (gDragging || gPressed || gRubberDrag || GetCapture() == gShade)
	{
		ReleaseCapture();
	}
	gDragging = FALSE;
	gPressed = 0;
	gRubberDrag = FALSE;
	RemoveHooks();
	KillTimer(gWindow, TIMER_FOLLOW);
	KillTimer(gWindow, TIMER_FLASH);
	if (gFrame) ShowWindow(gFrame, SW_HIDE);
	if (gToolbar) ShowWindow(gToolbar, SW_HIDE);
	HideShade();
}

static void CommandMode(int Mode)
{
	if (gRecording)
	{
		return;
	}
	if (Mode < SCREENREC_MODE_MONITOR || Mode > SCREENREC_MODE_WINDOW_REGION)
	{
		Mode = SCREENREC_MODE_SCREEN_REGION;
	}
	if (gPhase == PHASE_ARMED && gMode == Mode)
	{
		return;
	}
	DismissUi();
	if (Mode == SCREENREC_MODE_WINDOW)
	{
		BeginPick(Mode);
	}
	else if (Mode == SCREENREC_MODE_SCREEN_REGION || Mode == SCREENREC_MODE_WINDOW_REGION)
	{
		BeginRubber(Mode);
	}
	else
	{
		BeginRegion(Mode);
	}
}

static ID3D11Device* CreateDevice(void)
{
	IDXGIAdapter* Adapter = NULL;
	if (gConfig.HardwareEncoder)
	{
		IDXGIFactory* Factory;
		if (SUCCEEDED(CreateDXGIFactory(&IID_IDXGIFactory, (void**)&Factory)))
		{
			IDXGIFactory6* Factory6;
			if (SUCCEEDED(IDXGIFactory_QueryInterface(Factory, &IID_IDXGIFactory6, (void**)&Factory6)))
			{
				DXGI_GPU_PREFERENCE Preference = gConfig.HardwarePreferIntegrated ? DXGI_GPU_PREFERENCE_MINIMUM_POWER : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
				if (FAILED(IDXGIFactory6_EnumAdapterByGpuPreference(Factory6, 0, Preference, &IID_IDXGIAdapter, (void**)&Adapter)))
				{
					Adapter = NULL;
				}
				IDXGIFactory6_Release(Factory6);
			}
			IDXGIFactory_Release(Factory);
		}
	}

	ID3D11Device* Device;
	UINT Flags = 0;
#ifndef NDEBUG
	Flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
	D3D_DRIVER_TYPE Driver = Adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
	if (FAILED(D3D11CreateDevice(Adapter, Driver, NULL, Flags, (D3D_FEATURE_LEVEL[]){ D3D_FEATURE_LEVEL_11_0 }, 1, D3D11_SDK_VERSION, &Device, NULL, NULL)))
	{
		ShowNotification(L"Не удалось создать устройство Direct3D.", L"Ошибка", NIIF_ERROR);
		Device = NULL;
	}
	if (Adapter)
	{
		IDXGIAdapter_Release(Adapter);
	}
	return Device;
}

static RECT MonitorRelativeRect(void)
{
	MONITORINFO Info = { .cbSize = sizeof(Info) };
	GetMonitorInfoW(gMonitor, &Info);
	RECT Screen = gMode == SCREENREC_MODE_MONITOR ? Info.rcMonitor : VisualInterior();
	RECT Relative =
	{
		.left = Screen.left - Info.rcMonitor.left,
		.top = Screen.top - Info.rcMonitor.top,
		.right = Screen.right - Info.rcMonitor.left,
		.bottom = Screen.bottom - Info.rcMonitor.top,
	};
	int MaxW = Info.rcMonitor.right - Info.rcMonitor.left;
	int MaxH = Info.rcMonitor.bottom - Info.rcMonitor.top;
	if (Relative.left < 0) Relative.left = 0;
	if (Relative.top < 0) Relative.top = 0;
	if (Relative.right > MaxW) Relative.right = MaxW;
	if (Relative.bottom > MaxH) Relative.bottom = MaxH;
	return Relative;
}

static void StartRecording(ID3D11Device* Device, HWND AudioWindow, int Width, int Height)
{
	int Error = SHCreateDirectoryExW(NULL, gConfig.OutputFolder, NULL);
	if (Error != ERROR_SUCCESS && Error != ERROR_FILE_EXISTS && Error != ERROR_ALREADY_EXISTS)
	{
		ShowNotification(L"Не удалось создать папку для записей.", L"Нельзя начать запись", NIIF_WARNING);
		ScreenCapture_Stop(&gCapture);
		ID3D11Device_Release(Device);
		return;
	}
	if (!BuildOutputPaths())
	{
		ShowNotification(L"Слишком длинный путь к файлу.", L"Нельзя начать запись", NIIF_WARNING);
		ScreenCapture_Stop(&gCapture);
		ID3D11Device_Release(Device);
		return;
	}

	DWM_TIMING_INFO Info = { .cbSize = sizeof(Info) };
	HR(DwmGetCompositionTimingInfo(NULL, &Info));

	DWORD FramerateNum = Info.rateCompose.uiNumerator;
	DWORD FramerateDen = Info.rateCompose.uiDenominator;
	if (gConfig.VideoMaxFramerate > 0 && gConfig.VideoMaxFramerate * FramerateDen < FramerateNum)
	{
		gRecordingLimitFramerate = gConfig.VideoMaxFramerate;
		FramerateNum = gConfig.VideoMaxFramerate;
		FramerateDen = 1;
	}
	else
	{
		gRecordingLimitFramerate = 0;
	}

	EncoderConfig EncConfig =
	{
		.Width = (DWORD)Width,
		.Height = (DWORD)Height,
		.FramerateNum = FramerateNum,
		.FramerateDen = FramerateDen,
		.Config = &gConfig,
	};

	if (gConfig.CaptureAudio)
	{
		HWND ApplicationWindow = gConfig.ApplicationLocalAudio && AudioCapture_CanCaptureApplicationLocal() ? AudioWindow : NULL;
		if (!AudioCapture_Start(&gAudio, ApplicationWindow))
		{
			ShowNotification(L"Не удалось начать запись звука.", L"Нельзя начать запись", NIIF_WARNING);
			ScreenCapture_Stop(&gCapture);
			ID3D11Device_Release(Device);
			return;
		}
		EncConfig.AudioFormat = gAudio.Format;
	}

	if (!Encoder_Start(&gEncoder, Device, gRecordingPath, &EncConfig))
	{
		ShowNotification(L"Не удалось начать кодирование.", L"Нельзя начать запись", NIIF_WARNING);
		if (gConfig.CaptureAudio)
		{
			AudioCapture_Stop(&gAudio);
		}
		ScreenCapture_Stop(&gCapture);
		ID3D11Device_Release(Device);
		return;
	}

	gRecordingNextTooltip = 0;
	gRecordingNextEncode = 0;
	gRecordingLastFrame = 0;
	gRecordingDroppedFrames = 0;
	gPaused = FALSE;
	gPauseTotal = 0;
	gPauseStart = 0;
	gAudioIgnoreBefore = 0;
	gShotIndex = 0;
	gShotRequested = FALSE;
	gBorderFlash = FALSE;
	gLockedW = Width;
	gLockedH = Height;
	ScreenCapture_Start(&gCapture, gConfig.MouseCursor, gConfig.ShowRecordingBorder, gConfig.IncludeSecondaryWindows);

	if (gConfig.CaptureAudio)
	{
		SetTimer(gWindow, TIMER_AUDIO, 100, NULL);
	}
	SetTimer(gWindow, TIMER_VIDEO, 100, NULL);

	UpdateTrayIcon(gIcon2);
	gRecordingState = SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
	gRecording = TRUE;
	SyncTaskbar();
	InvalidateRect(gFrame, NULL, FALSE);
	InvalidateRect(gToolbar, NULL, FALSE);
	ShowNotification(L"Запись начата.", WCAP_TITLE, NIIF_INFO);
	ID3D11Device_Release(Device);
}

static void StartArmed(void)
{
	if (gRecording || gPhase != PHASE_ARMED || gStarting)
	{
		return;
	}
	gStarting = TRUE;

	HWND AudioWindow = NULL;
	if (gMode == SCREENREC_MODE_WINDOW || gMode == SCREENREC_MODE_WINDOW_REGION)
	{
		if (!IsWindow(gTarget))
		{
			ShowNotification(L"Окно закрыто.", L"Нельзя начать запись", NIIF_WARNING);
			gStarting = FALSE;
			return;
		}
		AudioWindow = gTarget;
	}

	ID3D11Device* Device = CreateDevice();
	if (!Device)
	{
		gStarting = FALSE;
		return;
	}

	BOOL Created = FALSE;
	int Width = 0;
	int Height = 0;
	if (gMode == SCREENREC_MODE_MONITOR || gMode == SCREENREC_MODE_SCREEN_REGION)
	{
		RECT Relative = MonitorRelativeRect();
		Width = Relative.right - Relative.left;
		Height = Relative.bottom - Relative.top;
		Created = ScreenCapture_CreateForMonitor(&gCapture, Device, gMonitor, &Relative);
	}
	else
	{
		Created = ScreenCapture_CreateForWindow(&gCapture, Device, gTarget, gConfig.OnlyClientArea, !gConfig.KeepRoundedWindowCorners);
		if (gMode == SCREENREC_MODE_WINDOW_REGION)
		{
			RECT Inner = VisualInterior();
			Width = Inner.right - Inner.left;
			Height = Inner.bottom - Inner.top;
		}
		else
		{
			Width = gCapture.Rect.right - gCapture.Rect.left;
			Height = gCapture.Rect.bottom - gCapture.Rect.top;
		}
	}

	if (!Created || Width < 2 || Height < 2)
	{
		if (Created)
		{
			ScreenCapture_Stop(&gCapture);
		}
		ID3D11Device_Release(Device);
		ShowNotification(L"Не удалось начать захват.", L"Ошибка", NIIF_WARNING);
		gStarting = FALSE;
		return;
	}

	StartRecording(Device, AudioWindow, Width, Height);
	gStarting = FALSE;
}

static void EncodeCapturedAudio(void)
{
	if (gEncoder.StartTime == 0)
	{
		return;
	}

	AudioCaptureData Data;
	while (AudioCapture_GetData(&gAudio, &Data, gEncoder.StartTime))
	{
		BOOL Drop = gPaused || (gAudioIgnoreBefore != 0 && Data.Time < gAudioIgnoreBefore);
		if (!Drop)
		{
			Data.Time = AdjustTime(Data.Time);
		}

		UINT32 FramesToEncode = Drop ? 0 : (UINT32)Data.Count;
		if (!Drop && Data.Time < gEncoder.StartTime)
		{
			const UINT32 SampleRate = gAudio.Format->nSamplesPerSec;
			const UINT32 BytesPerFrame = gAudio.Format->nBlockAlign;
			UINT64 TimeToSkip = gEncoder.StartTime - Data.Time;
			UINT32 FramesToSkip = (UINT32)((TimeToSkip * SampleRate - 1) / MF_UNITS_PER_SECOND + 1);
			if (FramesToSkip < FramesToEncode)
			{
				Data.Time += FramesToSkip * MF_UNITS_PER_SECOND / SampleRate;
				FramesToEncode -= FramesToSkip;
				if (Data.Samples)
				{
					Data.Samples = (BYTE*)Data.Samples + FramesToSkip * BytesPerFrame;
				}
			}
			else
			{
				FramesToEncode = 0;
			}
		}
		if (FramesToEncode != 0)
		{
			Encoder_NewSamples(&gEncoder, Data.Samples, FramesToEncode, Data.Time, gTickFreq.QuadPart);
		}
		AudioCapture_ReleaseData(&gAudio, &Data);
	}
}

static void StopRecording(void)
{
	if (!gRecording)
	{
		return;
	}

	gRecording = FALSE;
	gPaused = FALSE;
	SyncTaskbar();
	gShotRequested = FALSE;
	gBorderFlash = FALSE;
	SetThreadExecutionState(gRecordingState);

	if (gConfig.CaptureAudio)
	{
		KillTimer(gWindow, TIMER_AUDIO);
		AudioCapture_Flush(&gAudio);
		EncodeCapturedAudio();
		AudioCapture_Stop(&gAudio);
	}
	KillTimer(gWindow, TIMER_VIDEO);
	KillTimer(gWindow, TIMER_FLASH);

	ScreenCapture_Stop(&gCapture);
	Encoder_Stop(&gEncoder);

	UpdateTrayIcon(gIcon1);
	UpdateTrayTitle(WCAP_TITLE);
	InvalidateRect(gFrame, NULL, FALSE);
	InvalidateRect(gToolbar, NULL, FALSE);
	ShowNotification(L"Запись остановлена.", WCAP_TITLE, NIIF_INFO);
	if (gConfig.OpenFolder)
	{
		ShowFileInFolder(gRecordingPath);
	}
}

static void SetPaused(BOOL Paused)
{
	if (!gRecording || gPaused == Paused)
	{
		return;
	}
	LARGE_INTEGER Now;
	QueryPerformanceCounter(&Now);
	if (Paused)
	{
		gPauseStart = (UINT64)Now.QuadPart;
		gPaused = TRUE;
	}
	else
	{
		gPauseTotal += (UINT64)Now.QuadPart - gPauseStart;
		gPaused = FALSE;
		gAudioIgnoreBefore = (UINT64)Now.QuadPart;
	}
	InvalidateRect(gToolbar, NULL, FALSE);
}

static void RequestShot(void)
{
	if (!gRecording)
	{
		return;
	}
	gShotRequested = TRUE;
	gBorderFlash = TRUE;
	InvalidateRect(gFrame, NULL, FALSE);
	SetTimer(gWindow, TIMER_FLASH, 350, NULL);
}

static void SyncTaskbar(void)
{
	if (!gTaskbar)
	{
		return;
	}
	HICON Icon = gRecording ? gIcon2 : gIcon1;
	SendMessageW(gTaskbar, WM_SETICON, ICON_BIG, (LPARAM)Icon);
	SendMessageW(gTaskbar, WM_SETICON, ICON_SMALL, (LPARAM)Icon);
	SetWindowTextW(gTaskbar, gRecording ? L"ScreenRec — запись" : L"ScreenRec");
}

static void ClearFold(void)
{
	gFold = FOLD_NONE;
	if (gTaskbar)
	{
		ShowWindow(gTaskbar, SW_HIDE);
	}
}

static void HideOverlay(void)
{
	gPressed = 0;
	gHover = 0;
	if (gDragging)
	{
		ReleaseCapture();
		gDragging = FALSE;
	}
	if (gFrame) ShowWindow(gFrame, SW_HIDE);
	if (gToolbar) ShowWindow(gToolbar, SW_HIDE);
	HideShade();
}

static void RestoreOverlay(void)
{
	if (gPhase == PHASE_HIDDEN)
	{
		ClearFold();
		return;
	}
	ClearFold();
	if (gPhase == PHASE_RUBBER)
	{
		PlaceShade();
		if (gFrame) ShowWindow(gFrame, SW_HIDE);
		if (gToolbar) ShowWindow(gToolbar, SW_HIDE);
		return;
	}
	if (gFrame)
	{
		ShowWindow(gFrame, SW_SHOWNOACTIVATE);
	}
	PlaceToolbar();
}

static void FoldToTaskbar(void)
{
	if (gPhase != PHASE_ARMED)
	{
		return;
	}
	if (!gTaskbar)
	{
		gFold = FOLD_TRAY;
		HideOverlay();
		return;
	}
	gFold = FOLD_TASKBAR;
	HideOverlay();
	SyncTaskbar();
	ShowWindow(gTaskbar, SW_SHOWMINIMIZED);
}

static void FoldToTray(void)
{
	if (gPhase != PHASE_ARMED)
	{
		return;
	}
	gFold = FOLD_TRAY;
	HideOverlay();
	if (gTaskbar)
	{
		ShowWindow(gTaskbar, SW_HIDE);
	}
}

static void CloseCaptureWindow(void)
{
	if (gRecording)
	{
		StopRecording();
	}
	DismissUi();
}

static void OpenSettings(void)
{
	if (gRecording || gSettingsOpen)
	{
		return;
	}
	gSettingsOpen = TRUE;
	if (gFrame) SetWindowPos(gFrame, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	if (gToolbar) SetWindowPos(gToolbar, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	BOOL ShadeUp = gShade && IsWindowVisible(gShade);
	if (ShadeUp) ShowWindow(gShade, SW_HIDE);
	BOOL Changed = Config_ShowDialog(&gConfig);
	gSettingsOpen = FALSE;
	if (Changed)
	{
		Config_Save(&gConfig, gConfigPath);
		DisableHotKeys();
		EnableHotKeys();
	}
	if (gPhase == PHASE_RUBBER && gFold == FOLD_NONE)
	{
		if (ShadeUp) PlaceShade();
	}
	else if (gPhase == PHASE_ARMED && gFold == FOLD_NONE)
	{
		if (gFrame) SetWindowPos(gFrame, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		PlaceToolbar();
	}
}

static void FireButton(int Button)
{
	if (Button == BTN_RECORD)
	{
		if (gRecording) StopRecording();
		else StartArmed();
	}
	else if (Button == BTN_PAUSE && gRecording)
	{
		SetPaused(!gPaused);
	}
	else if (Button == BTN_SHOT)
	{
		RequestShot();
	}
	else if (Button == BTN_MINIMIZE)
	{
		FoldToTaskbar();
	}
	else if (Button == BTN_TRAY)
	{
		FoldToTray();
	}
	else if (Button == BTN_CLOSE)
	{
		CloseCaptureWindow();
	}
	else if (Button >= BTN_MODE && Button < BTN_MODE + 4)
	{
		if (!gRecording)
		{
			CommandMode(Button - BTN_MODE + 1);
		}
	}
	else if (Button == BTN_SETTINGS)
	{
		OpenSettings();
	}
}

typedef struct
{
	RECT Record;
	RECT Pause;
	RECT Shot;
	RECT Mode[4];
	RECT Settings;
	RECT Minimize;
	RECT Tray;
	RECT Close;
} ToolbarLayout;

static void ToolbarMetrics(ToolbarLayout* Layout)
{
	RECT Client;
	GetClientRect(gToolbar, &Client);
	UINT Dpi = GetDpiForWindow(gToolbar);
	int Margin = MulDiv(8, (int)Dpi, 96);
	int Size = MulDiv(28, (int)Dpi, 96);
	int Gap = MulDiv(6, (int)Dpi, 96);
	int Mode = MulDiv(26, (int)Dpi, 96);
	int ModeGap = MulDiv(4, (int)Dpi, 96);
	int Chrome = MulDiv(22, (int)Dpi, 96);
	int ChromeGap = MulDiv(4, (int)Dpi, 96);
	int Y = (Client.bottom - Size) / 2;
	int Ym = (Client.bottom - Mode) / 2;
	int Yc = (Client.bottom - Chrome) / 2;
	if (Y < 0) Y = 0;
	if (Ym < 0) Ym = 0;
	if (Yc < 0) Yc = 0;

	int X = Margin;
	Layout->Record = (RECT){ X, Y, X + Size, Y + Size };
	X = Layout->Record.right + Gap;
	Layout->Pause = (RECT){ X, Y, X + Size, Y + Size };
	X = Layout->Pause.right + Gap;
	Layout->Shot = (RECT){ X, Y, X + Size, Y + Size };
	X = Layout->Shot.right + MulDiv(14, (int)Dpi, 96);
	for (int Index = 0; Index < 4; Index++)
	{
		Layout->Mode[Index] = (RECT){ X, Ym, X + Mode, Ym + Mode };
		X += Mode + ModeGap;
	}

	int Right = Client.right - Margin;
	Layout->Close = (RECT){ Right - Chrome, Yc, Right, Yc + Chrome };
	Right = Layout->Close.left - ChromeGap;
	Layout->Tray = (RECT){ Right - Chrome, Yc, Right, Yc + Chrome };
	Right = Layout->Tray.left - ChromeGap;
	Layout->Minimize = (RECT){ Right - Chrome, Yc, Right, Yc + Chrome };
	Right = Layout->Minimize.left - MulDiv(10, (int)Dpi, 96);
	Layout->Settings = (RECT){ Right - Mode, Ym, Right, Ym + Mode };
}

static int HitToolbar(POINT Point)
{
	ToolbarLayout Layout;
	ToolbarMetrics(&Layout);
	if (PtInRect(&Layout.Close, Point)) return BTN_CLOSE;
	if (PtInRect(&Layout.Tray, Point)) return BTN_TRAY;
	if (PtInRect(&Layout.Minimize, Point)) return BTN_MINIMIZE;
	if (PtInRect(&Layout.Settings, Point)) return BTN_SETTINGS;
	for (int Index = 0; Index < 4; Index++)
	{
		if (PtInRect(&Layout.Mode[Index], Point)) return BTN_MODE + Index;
	}
	if (PtInRect(&Layout.Record, Point)) return BTN_RECORD;
	if (PtInRect(&Layout.Pause, Point) && gRecording) return BTN_PAUSE;
	if (PtInRect(&Layout.Shot, Point) && gRecording) return BTN_SHOT;
	return BTN_NONE;
}

static void EnsureFont(UINT Dpi)
{
	if (gFont && gFontDpi == Dpi)
	{
		return;
	}
	if (gFont)
	{
		DeleteObject(gFont);
	}
	gFont = CreateFontW(-MulDiv(15, (int)Dpi, 96), 0, 0, 0, FW_NORMAL,
		FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
	gFontDpi = Dpi;
}

static void DrawRecordIcon(HDC Dc, RECT Rect, BOOL Stop)
{
	int Cx = (Rect.left + Rect.right) / 2;
	int Cy = (Rect.top + Rect.bottom) / 2;
	int Radius = MulDiv(9, (int)GetDpiForWindow(gToolbar), 96);
	HBRUSH Red = CreateSolidBrush(RGB(214, 40, 40));
	HBRUSH Old = (HBRUSH)SelectObject(Dc, Red);
	HPEN OldPen = (HPEN)SelectObject(Dc, GetStockObject(NULL_PEN));
	Ellipse(Dc, Cx - Radius, Cy - Radius, Cx + Radius, Cy + Radius);
	if (Stop)
	{
		HBRUSH White = CreateSolidBrush(RGB(255, 255, 255));
		SelectObject(Dc, White);
		int Half = Radius / 2;
		if (Half < 3) Half = 3;
		Rectangle(Dc, Cx - Half, Cy - Half, Cx + Half, Cy + Half);
		SelectObject(Dc, Old);
		DeleteObject(White);
	}
	else
	{
		SelectObject(Dc, Old);
	}
	SelectObject(Dc, OldPen);
	DeleteObject(Red);
}

static void DrawPauseIcon(HDC Dc, RECT Rect, BOOL Resume, BOOL Enabled)
{
	COLORREF Color = Enabled ? RGB(60, 60, 60) : RGB(176, 176, 176);
	HBRUSH Brush = CreateSolidBrush(Color);
	HBRUSH Old = (HBRUSH)SelectObject(Dc, Brush);
	HPEN OldPen = (HPEN)SelectObject(Dc, GetStockObject(NULL_PEN));
	int Cx = (Rect.left + Rect.right) / 2;
	int Cy = (Rect.top + Rect.bottom) / 2;
	int H = MulDiv(10, (int)GetDpiForWindow(gToolbar), 96);
	if (Resume)
	{
		POINT Triangle[3] =
		{
			{ Cx - H / 2, Cy - H },
			{ Cx - H / 2, Cy + H },
			{ Cx + H, Cy },
		};
		Polygon(Dc, Triangle, 3);
	}
	else
	{
		int W = MulDiv(3, (int)GetDpiForWindow(gToolbar), 96);
		if (W < 2) W = 2;
		Rectangle(Dc, Cx - W - 2, Cy - H, Cx - 2, Cy + H);
		Rectangle(Dc, Cx + 2, Cy - H, Cx + 2 + W, Cy + H);
	}
	SelectObject(Dc, Old);
	SelectObject(Dc, OldPen);
	DeleteObject(Brush);
}

static void DrawShotIcon(HDC Dc, RECT Rect, BOOL Enabled)
{
	COLORREF Color = Enabled ? RGB(60, 60, 60) : RGB(176, 176, 176);
	int Cx = (Rect.left + Rect.right) / 2;
	int Cy = (Rect.top + Rect.bottom) / 2;
	int W = MulDiv(14, (int)GetDpiForWindow(gToolbar), 96);
	int H = MulDiv(10, (int)GetDpiForWindow(gToolbar), 96);
	HPEN Pen = CreatePen(PS_SOLID, 2, Color);
	HBRUSH OldBrush = (HBRUSH)SelectObject(Dc, GetStockObject(NULL_BRUSH));
	HPEN OldPen = (HPEN)SelectObject(Dc, Pen);
	RoundRect(Dc, Cx - W, Cy - H + 2, Cx + W, Cy + H + 2, 4, 4);
	Ellipse(Dc, Cx - 4, Cy - 3, Cx + 5, Cy + 6);
	SelectObject(Dc, OldBrush);
	SelectObject(Dc, OldPen);
	DeleteObject(Pen);
}

static void DrawButtonBack(HDC Dc, RECT Rect, int Id, BOOL Danger)
{
	if (gHover != Id && gPressed != Id)
	{
		return;
	}
	COLORREF Color = gPressed == Id ? RGB(220, 220, 220) : RGB(236, 236, 236);
	if (Danger)
	{
		Color = gPressed == Id ? RGB(196, 43, 28) : RGB(232, 17, 35);
	}
	HBRUSH Brush = CreateSolidBrush(Color);
	HBRUSH OldBrush = (HBRUSH)SelectObject(Dc, Brush);
	HPEN OldPen = (HPEN)SelectObject(Dc, GetStockObject(NULL_PEN));
	int Radius = Rect.bottom - Rect.top;
	RoundRect(Dc, Rect.left, Rect.top, Rect.right + 1, Rect.bottom + 1, Radius, Radius);
	SelectObject(Dc, OldBrush);
	SelectObject(Dc, OldPen);
	DeleteObject(Brush);
}

static void DrawMinIcon(HDC Dc, RECT Rect)
{
	int Cx = (Rect.left + Rect.right) / 2;
	int Cy = (Rect.top + Rect.bottom) / 2;
	int Half = MulDiv(5, (int)GetDpiForWindow(gToolbar), 96);
	if (Half < 4) Half = 4;
	HPEN Pen = CreatePen(PS_SOLID, 2, RGB(60, 60, 60));
	HPEN Old = (HPEN)SelectObject(Dc, Pen);
	MoveToEx(Dc, Cx - Half, Cy + Half / 2, NULL);
	LineTo(Dc, Cx + Half + 1, Cy + Half / 2);
	SelectObject(Dc, Old);
	DeleteObject(Pen);
}

static void DrawTrayIcon(HDC Dc, RECT Rect)
{
	int Cx = (Rect.left + Rect.right) / 2;
	int Cy = (Rect.top + Rect.bottom) / 2;
	int S = MulDiv(4, (int)GetDpiForWindow(gToolbar), 96);
	if (S < 3) S = 3;
	HPEN Pen = CreatePen(PS_SOLID, 2, RGB(60, 60, 60));
	HPEN Old = (HPEN)SelectObject(Dc, Pen);
	MoveToEx(Dc, Cx, Cy - S - 1, NULL);
	LineTo(Dc, Cx, Cy + S);
	MoveToEx(Dc, Cx - S, Cy, NULL);
	LineTo(Dc, Cx, Cy + S);
	MoveToEx(Dc, Cx + S, Cy, NULL);
	LineTo(Dc, Cx, Cy + S);
	MoveToEx(Dc, Cx - S - 2, Cy + S + 2, NULL);
	LineTo(Dc, Cx + S + 3, Cy + S + 2);
	SelectObject(Dc, Old);
	DeleteObject(Pen);
}

static void DrawCloseIcon(HDC Dc, RECT Rect)
{
	BOOL Hot = gHover == BTN_CLOSE || gPressed == BTN_CLOSE;
	int Cx = (Rect.left + Rect.right) / 2;
	int Cy = (Rect.top + Rect.bottom) / 2;
	int Half = MulDiv(4, (int)GetDpiForWindow(gToolbar), 96);
	if (Half < 3) Half = 3;
	HPEN Pen = CreatePen(PS_SOLID, 2, Hot ? RGB(255, 255, 255) : RGB(60, 60, 60));
	HPEN Old = (HPEN)SelectObject(Dc, Pen);
	MoveToEx(Dc, Cx - Half, Cy - Half, NULL);
	LineTo(Dc, Cx + Half + 1, Cy + Half + 1);
	MoveToEx(Dc, Cx + Half, Cy - Half, NULL);
	LineTo(Dc, Cx - Half - 1, Cy + Half + 1);
	SelectObject(Dc, Old);
	DeleteObject(Pen);
}

static void DrawSlot(HDC Dc, RECT Rect, int Id, BOOL Selected, BOOL Enabled)
{
	BOOL Hot = Enabled && (gHover == Id || gPressed == Id);
	if (!Selected && !Hot)
	{
		return;
	}
	COLORREF Color = RGB(236, 236, 236);
	if (Selected) Color = Hot ? RGB(196, 218, 240) : RGB(214, 230, 246);
	else if (gPressed == Id) Color = RGB(220, 220, 220);
	HBRUSH Brush = CreateSolidBrush(Color);
	HBRUSH OldBrush = (HBRUSH)SelectObject(Dc, Brush);
	HPEN OldPen = (HPEN)SelectObject(Dc, GetStockObject(NULL_PEN));
	int Radius = Rect.bottom - Rect.top;
	RoundRect(Dc, Rect.left, Rect.top, Rect.right + 1, Rect.bottom + 1, Radius, Radius);
	SelectObject(Dc, OldBrush);
	SelectObject(Dc, OldPen);
	DeleteObject(Brush);
}

static void DrawSeparator(HDC Dc, int X, int Height, UINT Dpi)
{
	HPEN Pen = CreatePen(PS_SOLID, 1, RGB(220, 220, 220));
	HPEN Old = (HPEN)SelectObject(Dc, Pen);
	int Pad = MulDiv(10, (int)Dpi, 96);
	MoveToEx(Dc, X, Pad, NULL);
	LineTo(Dc, X, Height - Pad);
	SelectObject(Dc, Old);
	DeleteObject(Pen);
}

static COLORREF ModeColor(BOOL Selected)
{
	if (gRecording) return Selected ? RGB(20, 96, 168) : RGB(176, 176, 176);
	return Selected ? RGB(20, 96, 168) : RGB(60, 60, 60);
}

static void DrawMonitorIcon(HDC Dc, RECT Rect, COLORREF Color)
{
	int Cx = (Rect.left + Rect.right) / 2;
	int Cy = (Rect.top + Rect.bottom) / 2;
	UINT Dpi = GetDpiForWindow(gToolbar);
	int W = MulDiv(7, (int)Dpi, 96);
	int H = MulDiv(5, (int)Dpi, 96);
	HPEN Pen = CreatePen(PS_SOLID, 2, Color);
	HBRUSH OldBrush = (HBRUSH)SelectObject(Dc, GetStockObject(NULL_BRUSH));
	HPEN Old = (HPEN)SelectObject(Dc, Pen);
	RoundRect(Dc, Cx - W, Cy - H - 2, Cx + W + 1, Cy + H - 2, 3, 3);
	MoveToEx(Dc, Cx, Cy + H - 2, NULL);
	LineTo(Dc, Cx, Cy + H + 1);
	MoveToEx(Dc, Cx - MulDiv(3, (int)Dpi, 96), Cy + H + 1, NULL);
	LineTo(Dc, Cx + MulDiv(4, (int)Dpi, 96), Cy + H + 1);
	SelectObject(Dc, OldBrush);
	SelectObject(Dc, Old);
	DeleteObject(Pen);
}

static void DrawWindowIcon(HDC Dc, RECT Rect, COLORREF Color)
{
	int Cx = (Rect.left + Rect.right) / 2;
	int Cy = (Rect.top + Rect.bottom) / 2;
	UINT Dpi = GetDpiForWindow(gToolbar);
	int W = MulDiv(6, (int)Dpi, 96);
	int H = MulDiv(5, (int)Dpi, 96);
	HPEN Pen = CreatePen(PS_SOLID, 2, Color);
	HBRUSH OldBrush = (HBRUSH)SelectObject(Dc, GetStockObject(NULL_BRUSH));
	HPEN Old = (HPEN)SelectObject(Dc, Pen);
	Rectangle(Dc, Cx - W, Cy - H, Cx + W + 1, Cy + H + 1);
	int Title = Cy - H + MulDiv(3, (int)Dpi, 96);
	MoveToEx(Dc, Cx - W, Title, NULL);
	LineTo(Dc, Cx + W + 1, Title);
	SelectObject(Dc, OldBrush);
	SelectObject(Dc, Old);
	DeleteObject(Pen);
}

static void DrawRegionIcon(HDC Dc, RECT Rect, COLORREF Color)
{
	int Cx = (Rect.left + Rect.right) / 2;
	int Cy = (Rect.top + Rect.bottom) / 2;
	UINT Dpi = GetDpiForWindow(gToolbar);
	int W = MulDiv(6, (int)Dpi, 96);
	int H = MulDiv(5, (int)Dpi, 96);
	int A = MulDiv(3, (int)Dpi, 96);
	HPEN Pen = CreatePen(PS_SOLID, 2, Color);
	HPEN Old = (HPEN)SelectObject(Dc, Pen);
	MoveToEx(Dc, Cx - W, Cy - H + A, NULL);
	LineTo(Dc, Cx - W, Cy - H);
	LineTo(Dc, Cx - W + A + 1, Cy - H);
	MoveToEx(Dc, Cx + W - A, Cy - H, NULL);
	LineTo(Dc, Cx + W, Cy - H);
	LineTo(Dc, Cx + W, Cy - H + A + 1);
	MoveToEx(Dc, Cx - W, Cy + H - A, NULL);
	LineTo(Dc, Cx - W, Cy + H);
	LineTo(Dc, Cx - W + A + 1, Cy + H);
	MoveToEx(Dc, Cx + W - A, Cy + H, NULL);
	LineTo(Dc, Cx + W, Cy + H);
	LineTo(Dc, Cx + W, Cy + H - A - 1);
	SelectObject(Dc, Old);
	DeleteObject(Pen);
}

static void DrawWindowRegionIcon(HDC Dc, RECT Rect, COLORREF Color)
{
	int Cx = (Rect.left + Rect.right) / 2;
	int Cy = (Rect.top + Rect.bottom) / 2;
	UINT Dpi = GetDpiForWindow(gToolbar);
	int W = MulDiv(7, (int)Dpi, 96);
	int H = MulDiv(5, (int)Dpi, 96);
	HPEN Pen = CreatePen(PS_SOLID, 2, Color);
	HBRUSH OldBrush = (HBRUSH)SelectObject(Dc, GetStockObject(NULL_BRUSH));
	HPEN Old = (HPEN)SelectObject(Dc, Pen);
	Rectangle(Dc, Cx - W, Cy - H, Cx + W + 1, Cy + H + 1);
	int In = MulDiv(3, (int)Dpi, 96);
	Rectangle(Dc, Cx - W + In, Cy - H + In + 1, Cx + W - In + 1, Cy + H - 1);
	SelectObject(Dc, OldBrush);
	SelectObject(Dc, Old);
	DeleteObject(Pen);
}

static void DrawSettingsIcon(HDC Dc, RECT Rect, COLORREF Color)
{
	int Cx = (Rect.left + Rect.right) / 2;
	int Cy = (Rect.top + Rect.bottom) / 2;
	UINT Dpi = GetDpiForWindow(gToolbar);
	int W = MulDiv(6, (int)Dpi, 96);
	int Gap = MulDiv(3, (int)Dpi, 96);
	HPEN Pen = CreatePen(PS_SOLID, 2, Color);
	HBRUSH Brush = CreateSolidBrush(Color);
	HBRUSH OldBrush = (HBRUSH)SelectObject(Dc, Brush);
	HPEN Old = (HPEN)SelectObject(Dc, Pen);
	for (int Row = -1; Row <= 1; Row++)
	{
		int Y = Cy + Row * Gap;
		MoveToEx(Dc, Cx - W, Y, NULL);
		LineTo(Dc, Cx + W + 1, Y);
		int Knob = Cx + Row * MulDiv(3, (int)Dpi, 96);
		int R = MulDiv(2, (int)Dpi, 96);
		if (R < 2) R = 2;
		Ellipse(Dc, Knob - R, Y - R, Knob + R + 1, Y + R + 1);
	}
	SelectObject(Dc, OldBrush);
	SelectObject(Dc, Old);
	DeleteObject(Brush);
	DeleteObject(Pen);
}

static void PaintToolbar(HWND Window, HDC Dc)
{
	RECT Client;
	GetClientRect(Window, &Client);
	HBRUSH Background = CreateSolidBrush(RGB(255, 255, 255));
	FillRect(Dc, &Client, Background);
	DeleteObject(Background);

	ToolbarLayout Layout;
	ToolbarMetrics(&Layout);
	UINT Dpi = GetDpiForWindow(Window);
	DrawButtonBack(Dc, Layout.Record, BTN_RECORD, FALSE);
	DrawButtonBack(Dc, Layout.Pause, BTN_PAUSE, FALSE);
	DrawButtonBack(Dc, Layout.Shot, BTN_SHOT, FALSE);
	for (int Index = 0; Index < 4; Index++)
	{
		int Mode = Index + 1;
		BOOL Selected = gMode == Mode && gPhase != PHASE_HIDDEN;
		DrawSlot(Dc, Layout.Mode[Index], BTN_MODE + Index, Selected, !gRecording);
	}
	DrawSlot(Dc, Layout.Settings, BTN_SETTINGS, FALSE, !gRecording);
	DrawButtonBack(Dc, Layout.Minimize, BTN_MINIMIZE, FALSE);
	DrawButtonBack(Dc, Layout.Tray, BTN_TRAY, FALSE);
	DrawButtonBack(Dc, Layout.Close, BTN_CLOSE, TRUE);
	DrawRecordIcon(Dc, Layout.Record, gRecording);
	DrawPauseIcon(Dc, Layout.Pause, gPaused, gRecording);
	DrawShotIcon(Dc, Layout.Shot, gRecording);
	DrawMonitorIcon(Dc, Layout.Mode[0], ModeColor(gMode == SCREENREC_MODE_MONITOR));
	DrawWindowIcon(Dc, Layout.Mode[1], ModeColor(gMode == SCREENREC_MODE_WINDOW));
	DrawRegionIcon(Dc, Layout.Mode[2], ModeColor(gMode == SCREENREC_MODE_SCREEN_REGION));
	DrawWindowRegionIcon(Dc, Layout.Mode[3], ModeColor(gMode == SCREENREC_MODE_WINDOW_REGION));
	DrawSettingsIcon(Dc, Layout.Settings, gRecording ? RGB(176, 176, 176) : RGB(60, 60, 60));
	DrawMinIcon(Dc, Layout.Minimize);
	DrawTrayIcon(Dc, Layout.Tray);
	DrawCloseIcon(Dc, Layout.Close);

	if (Layout.Mode[0].left - Layout.Shot.right > MulDiv(10, (int)Dpi, 96))
	{
		DrawSeparator(Dc, (Layout.Shot.right + Layout.Mode[0].left) / 2, Client.bottom, Dpi);
	}
	if (Layout.Settings.left - Layout.Mode[3].right > MulDiv(18, (int)Dpi, 96))
	{
		DrawSeparator(Dc, Layout.Settings.left - MulDiv(6, (int)Dpi, 96), Client.bottom, Dpi);
	}

	EnsureFont(GetDpiForWindow(Window));
	SelectObject(Dc, gFont);
	SetBkMode(Dc, TRANSPARENT);
	SetTextColor(Dc, RGB(40, 40, 40));

	WCHAR Text[128];
	int Width = 0;
	int Height = 0;
	if (gMode == SCREENREC_MODE_MONITOR)
	{
		RECT Monitor = MonitorLimit();
		Width = Monitor.right - Monitor.left;
		Height = Monitor.bottom - Monitor.top;
	}
	else if (gRecording && gMode == SCREENREC_MODE_WINDOW)
	{
		Width = gLockedW;
		Height = gLockedH;
	}
	else
	{
		RECT Inner = VisualInterior();
		Width = Inner.right - Inner.left;
		Height = Inner.bottom - Inner.top;
		if (Width < 0) Width = 0;
		if (Height < 0) Height = 0;
	}

	if (gRecording && gEncoder.StartTime != 0)
	{
		DWORD Bitrate = 0;
		DWORD Length = 0;
		UINT64 Size = 0;
		Encoder_GetStats(&gEncoder, &Bitrate, &Length, &Size);
		DWORD Sec = Length / 1000;
		StrFormat(Text, L"%d \u00d7 %d   \u2022   %02u:%02u:%02u%ls",
			Width, Height, Sec / 3600, (Sec / 60) % 60, Sec % 60, gPaused ? L"   пауза" : L"");
	}
	else
	{
		StrFormat(Text, L"%d \u00d7 %d", Width, Height);
	}

	RECT TextRect = Client;
	TextRect.left = Layout.Mode[3].right + MulDiv(10, (int)GetDpiForWindow(Window), 96);
	TextRect.right = Layout.Settings.left - MulDiv(12, (int)GetDpiForWindow(Window), 96);
	if (TextRect.right > TextRect.left)
	{
		DrawTextW(Dc, Text, -1, &TextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	}
}

static LRESULT CALLBACK FrameProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	if (Message == WM_ERASEBKGND)
	{
		return 1;
	}
	else if (Message == WM_PAINT)
	{
		PAINTSTRUCT Paint;
		HDC Dc = BeginPaint(Window, &Paint);
		COLORREF Color = RGB(128, 128, 128);
		if (gBorderFlash) Color = RGB(255, 196, 0);
		else if (gRecording) Color = RGB(214, 40, 40);
		HBRUSH Brush = CreateSolidBrush(Color);
		FillRect(Dc, &Paint.rcPaint, Brush);
		DeleteObject(Brush);
		EndPaint(Window, &Paint);
		return 0;
	}
	else if (Message == WM_SETCURSOR)
	{
		if (LOWORD(LParam) == HTCLIENT)
		{
			POINT Point;
			GetCursorPos(&Point);
			ScreenToClient(Window, &Point);
			SetCursor(CursorForHit(HitTestFrame(Point.x, Point.y)));
			return TRUE;
		}
	}
	else if (Message == WM_LBUTTONDOWN)
	{
		int Hit = HitTestFrame(GET_X_LPARAM(LParam), GET_Y_LPARAM(LParam));
		if (Hit != HTCLIENT)
		{
			POINT Screen;
			GetCursorPos(&Screen);
			DragBegin(Window, Hit, Screen);
		}
		return 0;
	}
	else if (Message == WM_MOUSEMOVE)
	{
		if (gDragging && gDragWindow == Window)
		{
			POINT Screen;
			GetCursorPos(&Screen);
			ApplyDrag(Screen);
		}
		return 0;
	}
	else if (Message == WM_LBUTTONUP || Message == WM_CANCELMODE)
	{
		if (gDragging && gDragWindow == Window)
		{
			FinishDrag();
		}
		return 0;
	}
	else if (Message == WM_DPICHANGED)
	{
		UpdateFrameRegion();
		PlaceToolbar();
		return 0;
	}
	return DefWindowProcW(Window, Message, WParam, LParam);
}

static LRESULT CALLBACK ToolbarProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	if (Message == WM_ERASEBKGND)
	{
		return 1;
	}
	else if (Message == WM_PAINT)
	{
		PAINTSTRUCT Paint;
		HDC Dc = BeginPaint(Window, &Paint);
		PaintToolbar(Window, Dc);
		EndPaint(Window, &Paint);
		return 0;
	}
	else if (Message == WM_LBUTTONDOWN)
	{
		POINT Point = { GET_X_LPARAM(LParam), GET_Y_LPARAM(LParam) };
		int Hit = HitToolbar(Point);
		if (Hit)
		{
			gPressed = Hit;
			SetCapture(Window);
			InvalidateRect(Window, NULL, FALSE);
		}
		else if (gMode == SCREENREC_MODE_SCREEN_REGION || gMode == SCREENREC_MODE_WINDOW_REGION)
		{
			POINT Screen;
			GetCursorPos(&Screen);
			DragBegin(Window, HTCAPTION, Screen);
		}
		return 0;
	}
	else if (Message == WM_MOUSEMOVE)
	{
		if (gDragging && gDragWindow == Window)
		{
			POINT Screen;
			GetCursorPos(&Screen);
			ApplyDrag(Screen);
			return 0;
		}
		POINT Point = { GET_X_LPARAM(LParam), GET_Y_LPARAM(LParam) };
		int Hit = HitToolbar(Point);
		if (Hit != gHover)
		{
			gHover = Hit;
			InvalidateRect(Window, NULL, FALSE);
		}
		TRACKMOUSEEVENT Track = { .cbSize = sizeof(Track), .dwFlags = TME_LEAVE, .hwndTrack = Window };
		TrackMouseEvent(&Track);
		return 0;
	}
	else if (Message == WM_MOUSELEAVE)
	{
		gHover = 0;
		InvalidateRect(Window, NULL, FALSE);
		return 0;
	}
	else if (Message == WM_LBUTTONUP)
	{
		if (gDragging && gDragWindow == Window)
		{
			FinishDrag();
			return 0;
		}
		int Pressed = gPressed;
		gPressed = 0;
		ReleaseCapture();
		POINT Point = { GET_X_LPARAM(LParam), GET_Y_LPARAM(LParam) };
		int Hit = HitToolbar(Point);
		if (Pressed && Hit == Pressed)
		{
			FireButton(Pressed);
		}
		return 0;
	}
	else if (Message == WM_CANCELMODE)
	{
		gPressed = 0;
		if (gDragging && gDragWindow == Window)
		{
			FinishDrag();
		}
		return 0;
	}
	else if (Message == WM_SETCURSOR)
	{
		POINT Point;
		GetCursorPos(&Point);
		ScreenToClient(Window, &Point);
		int Hit = HitToolbar(Point);
		BOOL CanMove = gMode == SCREENREC_MODE_SCREEN_REGION || gMode == SCREENREC_MODE_WINDOW_REGION;
		SetCursor(LoadCursor(NULL, (!Hit && CanMove) ? IDC_SIZEALL : IDC_ARROW));
		return TRUE;
	}
	else if (Message == WM_RBUTTONUP)
	{
		if (!gRecording)
		{
			DismissUi();
		}
		return 0;
	}
	else if (Message == WM_NOTIFY)
	{
		NMHDR* Header = (NMHDR*)LParam;
		if (Header->code == TTN_GETDISPINFO)
		{
			NMTTDISPINFO* Info = (NMTTDISPINFO*)LParam;
			POINT Point;
			GetCursorPos(&Point);
			ScreenToClient(Window, &Point);
			int Hit = HitToolbar(Point);
			LPCWSTR Tip = L"";
			if (Hit == BTN_RECORD) Tip = gRecording ? L"Стоп" : L"Запись";
			else if (Hit == BTN_PAUSE) Tip = gPaused ? L"Продолжить" : L"Пауза";
			else if (Hit == BTN_SHOT) Tip = L"Скриншот";
			else if (Hit == BTN_MINIMIZE) Tip = L"Свернуть";
			else if (Hit == BTN_TRAY) Tip = L"Свернуть в трей";
			else if (Hit == BTN_CLOSE) Tip = L"Закрыть окно";
			else if (Hit == BTN_MODE) Tip = L"Весь экран";
			else if (Hit == BTN_MODE + 1) Tip = L"Окно целиком";
			else if (Hit == BTN_MODE + 2) Tip = L"Область экрана";
			else if (Hit == BTN_MODE + 3) Tip = L"Область окна";
			else if (Hit == BTN_SETTINGS) Tip = L"Настройки";
			StrCpyNW(Info->szText, Tip, _countof(Info->szText));
			Info->lpszText = Info->szText;
		}
		return 0;
	}
	return DefWindowProcW(Window, Message, WParam, LParam);
}

static void ToggleFromTray(void)
{
	if (gFold != FOLD_NONE)
	{
		RestoreOverlay();
		return;
	}
	if (gRecording)
	{
		PlaceToolbar();
		return;
	}
	if (gPhase == PHASE_HIDDEN)
	{
		CommandMode((int)gConfig.LastMode);
	}
	else
	{
		DismissUi();
	}
}

static RECT EncodeRectForFrame(const ScreenCaptureFrame* Frame)
{
	RECT Use = Frame->Rect;
	if (gMode == SCREENREC_MODE_WINDOW_REGION && IsWindow(gTarget))
	{
		RECT Bounds;
		if (GetExtendedBounds(gTarget, &Bounds))
		{
			RECT Inner = VisualInterior();
			RECT Desired =
			{
				.left = Inner.left - Bounds.left,
				.top = Inner.top - Bounds.top,
				.right = Inner.left - Bounds.left + gLockedW,
				.bottom = Inner.top - Bounds.top + gLockedH,
			};
			RECT Hit;
			if (IntersectRect(&Hit, &Desired, &Frame->Rect))
			{
				Use = Hit;
			}
		}
	}
	if (gLockedW > 0 && Use.right - Use.left > gLockedW)
	{
		Use.right = Use.left + gLockedW;
	}
	if (gLockedH > 0 && Use.bottom - Use.top > gLockedH)
	{
		Use.bottom = Use.top + gLockedH;
	}
	if (Use.left < 0) Use.left = 0;
	if (Use.top < 0) Use.top = 0;
	if (Use.right > (LONG)Frame->Width) Use.right = (LONG)Frame->Width;
	if (Use.bottom > (LONG)Frame->Height) Use.bottom = (LONG)Frame->Height;
	if (Use.right < Use.left) Use.right = Use.left;
	if (Use.bottom < Use.top) Use.bottom = Use.top;
	return Use;
}

static bool OnCaptureFrame(ScreenCapture* Capture, ScreenCaptureFrame* Frame)
{
	(void)Capture;
	if (Frame == NULL)
	{
		PostMessageW(gWindow, WM_WCAP_STOP_CAPTURE, 0, 0);
		return true;
	}

	RECT Rect = EncodeRectForFrame(Frame);
	if (gShotRequested)
	{
		gShotRequested = FALSE;
		WCHAR Path[MAX_PATH];
		if (Rect.right > Rect.left && Rect.bottom > Rect.top && BuildShotPath(Path, gShotIndex + 1) && SavePng(Frame->Texture, Rect, Path))
		{
			gShotIndex++;
		}
		else
		{
			ShowNotification(L"Не удалось сохранить скриншот.", WCAP_TITLE, NIIF_WARNING);
		}
	}

	if (gPaused || Rect.right <= Rect.left || Rect.bottom <= Rect.top)
	{
		return true;
	}

	UINT64 Time = AdjustTime(Frame->Time);
	BOOL DoEncode = TRUE;
	DWORD LimitFramerate = gRecordingLimitFramerate;
	if (LimitFramerate != 0)
	{
		if (Time * LimitFramerate < gRecordingNextEncode)
		{
			DoEncode = FALSE;
		}
		else
		{
			if (gRecordingNextEncode == 0)
			{
				gRecordingNextEncode = Time * LimitFramerate;
			}
			gRecordingNextEncode += gTickFreq.QuadPart;
		}
	}
	if (Time <= gRecordingLastFrame)
	{
		DoEncode = FALSE;
	}
	gRecordingLastFrame = Time;

	if (DoEncode)
	{
		if (!Encoder_NewFrame(&gEncoder, Frame->Texture, Rect, Time, gTickFreq.QuadPart))
		{
			gRecordingDroppedFrames++;
		}
	}

	if (gConfig.EnableLimitLength || gConfig.EnableLimitSize)
	{
		BOOL Stop = FALSE;
		if (gConfig.EnableLimitLength && gEncoder.StartTime != 0)
		{
			if (Time - gEncoder.StartTime >= (UINT64)gConfig.LimitLength * gTickFreq.QuadPart)
			{
				Stop = TRUE;
			}
		}
		if (gConfig.EnableLimitSize && !Stop)
		{
			UINT64 FileSize;
			DWORD Bitrate, LengthMsec;
			Encoder_GetStats(&gEncoder, &Bitrate, &LengthMsec, &FileSize);
			if (1000 * FileSize >= (995ULL * gConfig.LimitSize) << 20)
			{
				Stop = TRUE;
			}
		}
		if (Stop)
		{
			PostMessageW(gWindow, WM_WCAP_STOP_CAPTURE, 0, 0);
			return true;
		}
	}

	if (gRecordingNextTooltip == 0)
	{
		gRecordingNextTooltip = Time + gTickFreq.QuadPart;
	}
	else if (Time >= gRecordingNextTooltip)
	{
		gRecordingNextTooltip += gTickFreq.QuadPart;
		PostMessageW(gWindow, WM_WCAP_TRAY_TITLE, 0, 0);
	}
	return true;
}

static void ShowTrayMenu(HWND Window)
{
	HMENU Menu = CreatePopupMenu();
	Assert(Menu);
	UINT ModeFlags = (UINT)(gRecording ? MF_DISABLED : 0);
	AppendMenuW(Menu, MF_STRING | ModeFlags | (gMode == SCREENREC_MODE_MONITOR && gPhase != PHASE_HIDDEN ? MF_CHECKED : 0), CMD_MONITOR, L"Запись: Весь экран");
	AppendMenuW(Menu, MF_STRING | ModeFlags | (gMode == SCREENREC_MODE_WINDOW && gPhase != PHASE_HIDDEN ? MF_CHECKED : 0), CMD_WINDOW, L"Запись: Окно целиком");
	AppendMenuW(Menu, MF_STRING | ModeFlags | (gMode == SCREENREC_MODE_SCREEN_REGION && gPhase != PHASE_HIDDEN ? MF_CHECKED : 0), CMD_REGION, L"Запись: Область экрана");
	AppendMenuW(Menu, MF_STRING | ModeFlags | (gMode == SCREENREC_MODE_WINDOW_REGION && gPhase != PHASE_HIDDEN ? MF_CHECKED : 0), CMD_WINDOW_REGION, L"Запись: Область окна");
	AppendMenuW(Menu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(Menu, MF_STRING | (gRecording ? 0 : MF_DISABLED), CMD_SHOT, L"Скриншот");
	AppendMenuW(Menu, MF_STRING, CMD_FOLDER, L"Открыть папку с записями");
	AppendMenuW(Menu, MF_STRING | (gRecording ? MF_DISABLED : 0), CMD_SETTINGS, L"Настройки");
	AppendMenuW(Menu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(Menu, MF_STRING, CMD_QUIT, L"Выход");

	POINT Mouse;
	GetCursorPos(&Mouse);
	SetForegroundWindow(Window);
	int Command = TrackPopupMenu(Menu, TPM_RETURNCMD | TPM_NONOTIFY, Mouse.x, Mouse.y, 0, Window, NULL);
	DestroyMenu(Menu);

	if (Command == CMD_MONITOR) CommandMode(SCREENREC_MODE_MONITOR);
	else if (Command == CMD_WINDOW) CommandMode(SCREENREC_MODE_WINDOW);
	else if (Command == CMD_REGION) CommandMode(SCREENREC_MODE_SCREEN_REGION);
	else if (Command == CMD_WINDOW_REGION) CommandMode(SCREENREC_MODE_WINDOW_REGION);
	else if (Command == CMD_SHOT) RequestShot();
	else if (Command == CMD_FOLDER) OpenRecordingsFolder();
	else if (Command == CMD_SETTINGS)
	{
		OpenSettings();
	}
	else if (Command == CMD_QUIT)
	{
		DestroyWindow(Window);
	}
}

void DisableHotKeys(void)
{
	UnregisterHotKey(gWindow, HOT_RECORD_MONITOR);
	UnregisterHotKey(gWindow, HOT_RECORD_WINDOW);
	UnregisterHotKey(gWindow, HOT_RECORD_REGION);
	UnregisterHotKey(gWindow, HOT_RECORD_WINDOW_REGION);
	UnregisterHotKey(gWindow, HOT_TOGGLE);
	UnregisterHotKey(gWindow, HOT_SHOT);
}

BOOL EnableHotKeys(void)
{
	BOOL Success = TRUE;
	if (gConfig.ShortcutMonitor)
	{
		Success = Success && RegisterHotKey(gWindow, HOT_RECORD_MONITOR, HOT_GET_MOD(gConfig.ShortcutMonitor), HOT_GET_KEY(gConfig.ShortcutMonitor));
	}
	if (gConfig.ShortcutWindow)
	{
		Success = Success && RegisterHotKey(gWindow, HOT_RECORD_WINDOW, HOT_GET_MOD(gConfig.ShortcutWindow), HOT_GET_KEY(gConfig.ShortcutWindow));
	}
	if (gConfig.ShortcutRegion)
	{
		Success = Success && RegisterHotKey(gWindow, HOT_RECORD_REGION, HOT_GET_MOD(gConfig.ShortcutRegion), HOT_GET_KEY(gConfig.ShortcutRegion));
	}
	if (gConfig.ShortcutWindowRegion)
	{
		Success = Success && RegisterHotKey(gWindow, HOT_RECORD_WINDOW_REGION, HOT_GET_MOD(gConfig.ShortcutWindowRegion), HOT_GET_KEY(gConfig.ShortcutWindowRegion));
	}
	if (gConfig.ShortcutToggle)
	{
		Success = Success && RegisterHotKey(gWindow, HOT_TOGGLE, HOT_GET_MOD(gConfig.ShortcutToggle), HOT_GET_KEY(gConfig.ShortcutToggle));
	}
	if (gConfig.ShortcutShot)
	{
		Success = Success && RegisterHotKey(gWindow, HOT_SHOT, HOT_GET_MOD(gConfig.ShortcutShot), HOT_GET_KEY(gConfig.ShortcutShot));
	}
	return Success;
}

static void HandleHotkey(WPARAM Id)
{
	if (Id == HOT_SHOT)
	{
		RequestShot();
		return;
	}
	if (gRecording)
	{
		StopRecording();
		return;
	}
	if (Id == HOT_TOGGLE)
	{
		if (gPhase == PHASE_ARMED) StartArmed();
		else CommandMode((int)gConfig.LastMode);
		return;
	}
	if (Id == HOT_RECORD_MONITOR) CommandMode(SCREENREC_MODE_MONITOR);
	else if (Id == HOT_RECORD_WINDOW) CommandMode(SCREENREC_MODE_WINDOW);
	else if (Id == HOT_RECORD_REGION) CommandMode(SCREENREC_MODE_SCREEN_REGION);
	else if (Id == HOT_RECORD_WINDOW_REGION) CommandMode(SCREENREC_MODE_WINDOW_REGION);
}

static LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	if (Message == WM_CREATE)
	{
		AddTrayIcon(Window);
		return 0;
	}
	else if (Message == WM_DESTROY)
	{
		if (gRecording)
		{
			StopRecording();
		}
		DismissUi();
		if (gFrame) DestroyWindow(gFrame);
		if (gToolbar) DestroyWindow(gToolbar);
		if (gShade) DestroyWindow(gShade);
		if (gTaskbar) DestroyWindow(gTaskbar);
		RemoveTrayIcon(Window);
		PostQuitMessage(0);
		return 0;
	}
	else if (Message == WM_TIMER)
	{
		if (WParam == TIMER_AUDIO && gRecording)
		{
			EncodeCapturedAudio();
		}
		else if (WParam == TIMER_VIDEO && gRecording)
		{
			if (!gPaused)
			{
				LARGE_INTEGER Time;
				QueryPerformanceCounter(&Time);
				Encoder_Update(&gEncoder, AdjustTime((UINT64)Time.QuadPart), gTickFreq.QuadPart);
			}
			InvalidateRect(gToolbar, NULL, FALSE);
		}
		else if (WParam == TIMER_FOLLOW)
		{
			if (gPhase == PHASE_PICK) UpdatePick();
			else FollowTarget();
		}
		else if (WParam == TIMER_FLASH)
		{
			gBorderFlash = FALSE;
			KillTimer(Window, TIMER_FLASH);
			InvalidateRect(gFrame, NULL, FALSE);
		}
		return 0;
	}
	else if (Message == WM_POWERBROADCAST)
	{
		if (WParam == PBT_APMQUERYSUSPEND && gRecording)
		{
			if (LParam & 1) return BROADCAST_QUERY_DENY;
			StopRecording();
		}
		return TRUE;
	}
	else if (Message == WM_WCAP_COMMAND)
	{
		if (LOWORD(LParam) == WM_RBUTTONUP)
		{
			ShowTrayMenu(Window);
		}
		else if (LOWORD(LParam) == WM_LBUTTONUP)
		{
			ToggleFromTray();
		}
		return 0;
	}
	else if (Message == WM_HOTKEY)
	{
		HandleHotkey(WParam);
		return 0;
	}
	else if (Message == WM_WCAP_TRAY_TITLE)
	{
		if (gRecording && gEncoder.StartTime != 0)
		{
			UINT64 FileSize;
			DWORD Bitrate, LengthMsec;
			Encoder_GetStats(&gEncoder, &Bitrate, &LengthMsec, &FileSize);
			DWORD Sec = LengthMsec / 1000;
			WCHAR SizeText[64];
			StrFormatByteSizeW(FileSize, SizeText, _countof(SizeText));
			WCHAR Text[512];
			StrFormat(Text, L"%ls %dx%d\nДлительность: %02u:%02u:%02u%ls\nБитрейт: %u кбит/с\nРазмер: %ls\nПропущено кадров: %u",
				gPaused ? L"Пауза" : L"Запись",
				gEncoder.OutputWidth, gEncoder.OutputHeight,
				Sec / 3600, (Sec / 60) % 60, Sec % 60,
				gPaused ? L" (пауза)" : L"",
				Bitrate, SizeText, gRecordingDroppedFrames);
			UpdateTrayTitle(Text);
		}
		return 0;
	}
	else if (Message == WM_WCAP_STOP_CAPTURE)
	{
		if (gRecording) StopRecording();
		return 0;
	}
	else if (Message == WM_WCAP_ALREADY_RUNNING)
	{
		ShowNotification(L"ScreenRec уже запущен.", NULL, NIIF_INFO);
		return 0;
	}
	else if (Message == WM_WCAP_PICKED)
	{
		if (gPhase == PHASE_PICK)
		{
			ArmWindow((HWND)LParam);
		}
		return 0;
	}
	else if (Message == WM_WCAP_CANCEL_UI)
	{
		if (!gRecording) DismissUi();
		return 0;
	}
	else if (Message == WM_WCAP_RESTORE)
	{
		RestoreOverlay();
		return 0;
	}
	else if (Message == WM_WCAP_CLOSE_WINDOW)
	{
		CloseCaptureWindow();
		return 0;
	}
	else if (Message == WM_TASKBARCREATED)
	{
		AddTrayIcon(Window);
		if (gRecording) UpdateTrayIcon(gIcon2);
		return 0;
	}
	return DefWindowProcW(Window, Message, WParam, LParam);
}

static void ExcludeFromCapture(HWND Window)
{
	SetWindowDisplayAffinity(Window, WDA_EXCLUDEFROMCAPTURE);
}

static LRESULT CALLBACK TaskbarProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	if (Message == WM_SYSCOMMAND)
	{
		UINT Cmd = (UINT)(WParam & 0xFFF0);
		if (Cmd == SC_RESTORE || Cmd == SC_MAXIMIZE)
		{
			ShowWindow(Window, SW_HIDE);
			if (gWindow) PostMessageW(gWindow, WM_WCAP_RESTORE, 0, 0);
			return 0;
		}
		if (Cmd == SC_CLOSE)
		{
			ShowWindow(Window, SW_HIDE);
			if (gWindow) PostMessageW(gWindow, WM_WCAP_CLOSE_WINDOW, 0, 0);
			return 0;
		}
	}
	return DefWindowProcW(Window, Message, WParam, LParam);
}

#ifndef NDEBUG
int WinMain(HINSTANCE Instance, HINSTANCE Previous, LPSTR CommandLine, int Show)
#else
void WinMainCRTStartup(void)
#endif
{
	WNDCLASSEXW MessageClass =
	{
		.cbSize = sizeof(MessageClass),
		.lpfnWndProc = WindowProc,
		.hInstance = GetModuleHandleW(NULL),
		.lpszClassName = L"ScreenRecMessage",
	};

	HWND Existing = FindWindowW(MessageClass.lpszClassName, NULL);
	if (Existing)
	{
		PostMessageW(Existing, WM_WCAP_ALREADY_RUNNING, 0, 0);
		ExitProcess(0);
	}

	if (!ScreenCapture_IsSupported() || !ScreenCapture_CanHideMouseCursor())
	{
		MessageBoxW(NULL, L"Нужна Windows 10 версии 2004 (сборка 19041) или новее.", WCAP_TITLE, MB_ICONEXCLAMATION);
		ExitProcess(0);
	}

	GetModuleFileNameW(NULL, gConfigPath, _countof(gConfigPath));
	PathRenameExtensionW(gConfigPath, L".ini");

	HR(CoInitializeEx(0, COINIT_APARTMENTTHREADED));
	Config_Defaults(&gConfig);
	Config_Load(&gConfig, gConfigPath);
	ScreenCapture_Create(&gCapture, &OnCaptureFrame, false);
	Encoder_Init(&gEncoder);
	QueryPerformanceFrequency(&gTickFreq);

	gIcon1 = LoadIconW(MessageClass.hInstance, MAKEINTRESOURCEW(1));
	gIcon2 = LoadIconW(MessageClass.hInstance, MAKEINTRESOURCEW(2));
	Assert(gIcon1 && gIcon2);
	WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
	Assert(WM_TASKBARCREATED);

	WNDCLASSEXW FrameClass =
	{
		.cbSize = sizeof(FrameClass),
		.lpfnWndProc = FrameProc,
		.hCursor = LoadCursor(NULL, IDC_ARROW),
		.hInstance = MessageClass.hInstance,
		.lpszClassName = L"ScreenRecFrame",
	};
	WNDCLASSEXW ToolbarClass =
	{
		.cbSize = sizeof(ToolbarClass),
		.lpfnWndProc = ToolbarProc,
		.hCursor = LoadCursor(NULL, IDC_ARROW),
		.hInstance = MessageClass.hInstance,
		.lpszClassName = L"ScreenRecToolbar",
	};
	WNDCLASSEXW TaskbarClass =
	{
		.cbSize = sizeof(TaskbarClass),
		.lpfnWndProc = TaskbarProc,
		.hIcon = gIcon1,
		.hCursor = LoadCursor(NULL, IDC_ARROW),
		.hInstance = MessageClass.hInstance,
		.lpszClassName = L"ScreenRecTaskbar",
	};
	WNDCLASSEXW ShadeClass =
	{
		.cbSize = sizeof(ShadeClass),
		.lpfnWndProc = ShadeProc,
		.hCursor = LoadCursor(NULL, IDC_CROSS),
		.hInstance = MessageClass.hInstance,
		.lpszClassName = L"ScreenRecShade",
	};
	RegisterClassExW(&MessageClass);
	RegisterClassExW(&FrameClass);
	RegisterClassExW(&ToolbarClass);
	RegisterClassExW(&TaskbarClass);
	RegisterClassExW(&ShadeClass);

	gFrame = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
		FrameClass.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, MessageClass.hInstance, NULL);
	gToolbar = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		ToolbarClass.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, MessageClass.hInstance, NULL);
	gShade = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		ShadeClass.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, MessageClass.hInstance, NULL);
	if (!gFrame || !gToolbar || !gShade)
	{
		ExitProcess(0);
	}
	SetLayeredWindowAttributes(gShade, 0, 1, LWA_ALPHA);
	ExcludeFromCapture(gFrame);
	ExcludeFromCapture(gToolbar);
	ExcludeFromCapture(gShade);

	gWindow = CreateWindowExW(0, MessageClass.lpszClassName, WCAP_TITLE, WS_POPUP,
		0, 0, 0, 0, NULL, NULL, MessageClass.hInstance, NULL);
	gTaskbar = CreateWindowExW(WS_EX_APPWINDOW, TaskbarClass.lpszClassName, WCAP_TITLE,
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT, 320, 120, NULL, NULL, MessageClass.hInstance, NULL);
	if (!gWindow || !gTaskbar)
	{
		ExitProcess(0);
	}
	ExcludeFromCapture(gTaskbar);
	ShowWindow(gTaskbar, SW_HIDE);
	if (!EnableHotKeys())
	{
		MessageBoxW(NULL,
			L"Не удалось зарегистрировать горячие клавиши.\nИх, возможно, уже заняла другая программа.\nПроверьте настройки.",
			WCAP_TITLE, MB_ICONEXCLAMATION);
	}

	for (;;)
	{
		MSG Message;
		BOOL Result = GetMessageW(&Message, NULL, 0, 0);
		if (Result == 0)
		{
			ExitProcess(0);
		}
		Assert(Result > 0);
		TranslateMessage(&Message);
		DispatchMessageW(&Message);
	}
}
