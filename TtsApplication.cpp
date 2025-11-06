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

// Speak text using SAPI5, suppressing non-critical errors
void SpeakText(const std::wstring &text,
               const std::wstring &voice,
               float rate,
               int volume,
               const std::wstring & /*format*/)
{
    // Initialize COM in multithreaded mode to avoid STA popups
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)  // ignore if already initialized differently
        throw std::runtime_error("Failed to initialize COM");

    // Clear any existing COM error info to prevent popups
    ::SetErrorInfo(0, nullptr);

    CComPtr<ISpVoice> cpVoice;
    hr = cpVoice.CoCreateInstance(CLSID_SpVoice);
    if (FAILED(hr))
    {
        CoUninitialize();
        throw std::runtime_error("Failed to create SAPI voice instance");
    }

    // Select voice if specified
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
    cpVoice->SetRate(static_cast<LONG>((rate - 1.0f) * 10));  // 1.2 → +2

    // Speak asynchronously to avoid blocking popups
    hr = cpVoice->Speak(text.c_str(), SPF_ASYNC | SPF_PURGEBEFORESPEAK, NULL);

    // Ignore some non-critical HRESULTs
    if (FAILED(hr) && hr != 0x80045002) // SPERR_NOT_FOUND
    {
        cpVoice.Release();
        CoUninitialize();
        std::wstring snippet = text.substr(0, 200);
        throw std::runtime_error("Failed to speak text");
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

    // Parse CLI arguments
    int argc;
    LPWSTR *argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argvW) return 1;

    if (argc > 1)
    {
        std::vector<std::string> argv(argc);
        std::vector<char *> argvPtrs(argc);

        for (int i = 0; i < argc; ++i)
        {
            int size_needed = WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, nullptr, 0, nullptr, nullptr);
            if (size_needed > 0)
            {
                argv[i].resize(size_needed - 1);
                WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, &argv[i][0], size_needed, nullptr, nullptr);
                argvPtrs[i] = &argv[i][0];
            }
            else
            {
                argvPtrs[i] = nullptr;
            }
        }

        ret = RunCli(argc, argvPtrs.data());
        LocalFree(argvW);
        return ret;
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
