#include "TtsCli.h"
#include "TtsApplication.h" // for SpeakText()
#include <windows.h>
#include <string>
#include <stdexcept>
#include <vector>
#include <sstream>
#include <atlbase.h>
#include <sapi.h>

int RunCli(int argc, char *argv[])
{
    if (argc < 2)
        return 1; // nothing to say

    // Default values
    std::wstring voice = L"Default";
    float rate = 1.0f;
    int volume = 100;
    std::wstring outputFile; // empty means play directly
    std::wstring text;

    // Parse flags
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        if (arg == "--voice" && i + 1 < argc)
        {
            int len = MultiByteToWideChar(CP_UTF8, 0, argv[++i], -1, nullptr, 0);
            std::wstring wstr(len - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, &wstr[0], len);
            voice = wstr;
        }
        else if (arg == "--rate" && i + 1 < argc)
            rate = std::stof(argv[++i]);
        else if (arg == "--volume" && i + 1 < argc)
            volume = std::stoi(argv[++i]);
        else if (arg == "--output" && i + 1 < argc)
        {
            int len = MultiByteToWideChar(CP_UTF8, 0, argv[++i], -1, nullptr, 0);
            std::wstring wstr(len - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, &wstr[0], len);
            outputFile = wstr;
        }
        else
        {
            // Append to text
            int len = MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, nullptr, 0);
            std::wstring wstr(len - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, &wstr[0], len);
            if (!text.empty())
                text += L" ";
            text += wstr;
        }
    }
    try
    {
        if (!outputFile.empty())
        {
            // Save to .wav
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

            // Set voice
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

            cpVoice->SetVolume(volume);
            cpVoice->SetRate(static_cast<LONG>((rate - 1.0f) * 10));

            // Set output to file
            CComPtr<ISpStream> cpStream;
            CSpStreamFormat format;
            hr = SPBindToFile(outputFile.c_str(), SPFM_CREATE_ALWAYS, &cpStream, &format.FormatId(), format.WaveFormatExPtr());
            if (FAILED(hr))
            {
                cpVoice.Release();
                CoUninitialize();
                throw std::runtime_error("Failed to open output file for writing");
            }

            cpVoice->SetOutput(cpStream, TRUE);
            cpVoice->Speak(text.c_str(), SPF_DEFAULT, NULL);
            cpVoice->SetOutput(NULL, TRUE);

            cpStream.Release();
            cpVoice.Release();
            CoUninitialize();
        }
        else
        {
            // Play directly
            SpeakText(text, voice, rate, volume, L"");
        }
    }
    catch (const std::exception &e)
    {
        MessageBoxA(NULL, e.what(), "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    return 0;
}
