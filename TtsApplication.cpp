// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright © Microsoft Corporation. All rights reserved

#include "globals.h"

#if defined _M_IX86
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")
#elif defined _M_IA64
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='ia64' publicKeyToken='6595b64144ccf1df' language='*'\"")
#elif defined _M_X64
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")
#else
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

/////////////////////////////////////////////////////////////////////////
//  SAPI5 TtsApplication
//
//  Dialog box application used to manually verify SAPI5 TTS methods
//
/////////////////////////////////////////////////////////////////////////
// CLI helper: speak text using SAPI (usable from tests or CLI helpers)
#include "TtsApplication.h"
#include <sapi.h>
#include <sphelper.h>
#include <atlbase.h>
#include <atlcom.h>
#include <string>
#include <stdexcept>
#include "TtsCli.h"
#include <vector>

void SpeakText(const std::wstring &text,
			   const std::wstring &voice,
			   float rate,
			   int volume,
			   const std::wstring & /*format*/)
{
	HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	if (FAILED(hr))
		throw std::runtime_error("Failed to initialize COM");

	CComPtr<ISpVoice> cpVoice;
	hr = cpVoice.CoCreateInstance(CLSID_SpVoice);
	if (FAILED(hr))
	{
		CoUninitialize();
		throw std::runtime_error("Failed to create SAPI voice instance");
	}

	// Set voice (if provided)
	if (!voice.empty() && voice != L"Default")
	{
		CComPtr<IEnumSpObjectTokens> cpEnum;
		hr = SpEnumTokens(SPCAT_VOICES, NULL, NULL, &cpEnum);
		if (SUCCEEDED(hr) && cpEnum)
		{
			while (true)
			{
				CComPtr<ISpObjectToken> cpToken;
				if (cpEnum->Next(1, &cpToken, NULL) != S_OK)
					break;

				CSpDynamicString desc;
				SpGetDescription(cpToken, &desc);
				if (desc && wcsstr(desc, voice.c_str()))
				{
					cpVoice->SetVoice(cpToken);
					break;
				}
			}
		}
	}

	// Set volume and rate
	cpVoice->SetVolume(volume);
	cpVoice->SetRate(static_cast<LONG>((rate - 1.0f) * 10)); // e.g., 1.2 → +2

	// Format parameter not used for simple playback

	// Speak text
	hr = cpVoice->Speak(text.c_str(), SPF_DEFAULT, NULL);
	if (FAILED(hr))
	{
		cpVoice.Release();
		CoUninitialize();
		{
			// Build a more detailed error message for debugging/logging:
			// - HRESULT value
			// - truncated text snippet (to avoid huge messages)
			// - requested voice, rate and volume
			std::wstring snippet = text;
			if (snippet.size() > 200)
				snippet = snippet.substr(0, 200) + L"...";

			auto toUtf8 = [](const std::wstring &s) -> std::string {
				if (s.empty()) return std::string();
				int size = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
				if (size <= 0) return std::string();
				std::string out(size - 1, '\0');
				WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &out[0], size, nullptr, nullptr);
				return out;
			};

			std::string snippetUtf8 = toUtf8(snippet);
			std::string voiceUtf8 = toUtf8(voice);

			std::string msg = "Failed to speak text (HRESULT=" + std::to_string(static_cast<long>(hr)) + ")";
			msg += " text=\"" + snippetUtf8 + "\"";
			msg += " voice=\"" + (voiceUtf8.empty() ? "Default" : voiceUtf8) + "\"";
			msg += " rate=" + std::to_string(rate);
			msg += " volume=" + std::to_string(volume);

			throw std::runtime_error(msg);
		}
	}

	cpVoice.Release();
	CoUninitialize();
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------
// Description:         Program entry point.
// Arguments:
//  HINSTANCE [in]      Program instance handle.
//  HINSTANCE [in]      Unused in Win32.
//  LPSTR [in]          Program command line.
//  int [in]            Command to pass to ShowWindow().
// Returns:
//  int                 0 if all goes well.

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPSTR lpszCmdLine, int nCmdShow)
{
	int ret = 0;

	int argc;
	LPWSTR *argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argvW)
		return 1;

	if (argc > 1)
	{
		// Convert LPWSTR* to char* (UTF-8) for RunCli
		std::vector<std::string> argv(argc);
		std::vector<char *> argvPtrs(argc);

		for (int i = 0; i < argc; ++i)
		{
			int size_needed = WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, nullptr, 0, nullptr, nullptr);
			if (size_needed > 0)
			{
				argv[i].resize(size_needed - 1); // exclude null terminator
				WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, &argv[i][0], size_needed, nullptr, nullptr);
				argvPtrs[i] = &argv[i][0]; // non-const pointer for RunCli
			}
			else
			{
				argvPtrs[i] = nullptr;
			}
		}

		ret = RunCli(argc, argvPtrs.data());
		LocalFree(argvW);
		return ret; // exit after CLI
	}

	LocalFree(argvW);

	// --- Existing GUI code ---
	HWND hWnd = NULL;
	HWND hChild = NULL;
	WNDCLASSEX wc;
	HRESULT hr = S_OK;

	InitCommonControls();

	HMODULE hMod = LoadLibrary(TEXT("riched20.dll"));
	if (!hMod)
	{
		MessageBox(NULL, _T("Couldn't find riched32.dll. Shutting down!"), _T("Error - Missing dll"), MB_OK);
	}

	if (hMod)
	{
		CoInitialize(NULL);

		ZeroMemory(&wc, sizeof(wc));
		wc.cbSize = sizeof(wc);
		wc.style = CS_HREDRAW | CS_VREDRAW;
		wc.lpfnWndProc = ChildWndProc;
		wc.hInstance = hInst;
		wc.hIcon = LoadIcon(hInst, (LPCTSTR)IDI_APPICON);
		wc.hCursor = LoadCursor(NULL, IDC_CROSS);
		wc.hbrBackground = GetSysColorBrush(COLOR_3DFACE);
		wc.lpszMenuName = NULL;
		wc.lpszClassName = CHILD_CLASS;
		wc.hIconSm = LoadIcon(hInst, (LPCTSTR)IDI_APPICON);

		if (RegisterClassEx(&wc))
		{
			CTTSApp DlgClass(hInst);
			hr = DlgClass.InitSapi();

			if (SUCCEEDED(hr))
			{
				DialogBoxParam(hInst, MAKEINTRESOURCE(IDD_MAIN), NULL, (DLGPROC)CTTSApp::DlgProcMain, (LPARAM)&DlgClass);
			}
			else
			{
				MessageBox(NULL, _T("Error initializing TtsApplication. Shutting down."), _T("Error"), MB_OK);
			}
		}

		FreeLibrary(hMod);
	}

	CoUninitialize();
	return 0;
}
