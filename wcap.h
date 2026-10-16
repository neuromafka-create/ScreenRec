#pragma once

#define UNICODE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_DEPRECATE

#include <initguid.h>
#include <windows.h>

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <intrin.h>
#include <stdatomic.h>

#define WCAP_TITLE L"ScreenRec"
#define WCAP_URL   L"https://github.com/neuromafka-create/ScreenRec"

#define WCAP_CONFIG_TITLE "ScreenRec"

#define SCREENREC_MODE_MONITOR 1
#define SCREENREC_MODE_WINDOW 2
#define SCREENREC_MODE_SCREEN_REGION 3
#define SCREENREC_MODE_WINDOW_REGION 4

#ifdef _DEBUG
#define Assert(Cond) do { if (!(Cond)) __debugbreak(); } while (0)
#else
#define Assert(Cond) (void)(Cond)
#endif
#define HR(hr) do { HRESULT _hr = (hr); Assert(SUCCEEDED(_hr)); } while (0)

// calculates ceil(X * Num / Den)
#define MUL_DIV_ROUND_UP(X, Num, Den) (((X) * (Num) - 1) / (Den) + 1)

// caclulates ceil(X / Y)
#define DIV_ROUND_UP(X, Y) ( ((X) + (Y) - 1) / (Y) )

// MF works with 100nsec units
#define MF_UNITS_PER_SECOND 10000000ULL

#include <stdio.h>
#define StrFormat(Buffer, ...) _snwprintf(Buffer, _countof(Buffer), __VA_ARGS__)
