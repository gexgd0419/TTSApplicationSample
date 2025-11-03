This is a modified version of the [TtsApplication][1] sample project in [Windows-classic-samples][2].

This is a sub-project of [NaturalVoiceSAPIAdapter][3], for testing the SAPI 5 TTS engine.

This version adds:

- Simplified Chinese translation
- More detailed information for phoneme/viseme events
- Ability to open UTF-8 text files
- Ability to enter multiple lines in the text box

- Added very simple CLI to application with abality to speak text, and accept args for type of voice, rate, volume, and format

Example
'''
TtsApplication.exe --voice "Microsoft Andrew Online (Natural)" --volume 100 --rate 1.0 "Hello world from CLI"
'''

Click here to view the original [Readme.txt][4].

[1]: https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/Win7Samples/winui/speech/ttsapplication
[2]: https://github.com/microsoft/Windows-classic-samples
[3]: https://github.com/gexgd0419/NaturalVoiceSAPIAdapter
[4]: https://github.com/gexgd0419/TTSApplicationSample/blob/master/Readme.txt
