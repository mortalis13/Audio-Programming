- audio-filter-effects - based on the book "Designing Audio Effect Plugins in C++"

To build a VST plugin from an example from the book "Designing Audio Effect Plugins in C++":

- copy VST_SDK from the book source code (or download latest VST SDK)
- run copy_vst2_to_vst3_sdk.bat
- create folder /VST_SDK/vst3sdk/myprojects
- copy vstgui4 from ASPiK_SDK to /myprojects
- copy example to /myprojects, for example /myprojects/IIRFilters/
- modify CMakeLists.txt => set(UNIVERSAL_SDK_BUILD FALSE) and set(VST_SDK_BUILD TRUE), other types to FALSE (AAX, AU)
- execute "cmake .." from the /myprojects/[project]/win_build
- ensure the [UserProfile]\AppData\Local\Programs\Common\VST3 is created
- open .sln in Visual Studio and build (or run msbuild project.sln from command line)
- the final .vst3 folder will be in [project]/win_build/VST3/Debug/

Same procedure works for a new project created with ASPiKreator.exe
