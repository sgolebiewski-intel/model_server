::
:: Copyright (c) 2024 Intel Corporation
::
:: Licensed under the Apache License, Version 2.0 (the "License");
:: you may not use this file except in compliance with the License.
:: You may obtain a copy of the License at
::
::      http:::www.apache.org/licenses/LICENSE-2.0
::
:: Unless required by applicable law or agreed to in writing, software
:: distributed under the License is distributed on an "AS IS" BASIS,
:: WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
:: See the License for the specific language governing permissions and
:: limitations under the License.
::
@echo on
setlocal EnableExtensions DisableDelayedExpansion
set "vsDevPath=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Professional\Common7\Tools\vsdevcmd.bat"
:: Setting PATH environment variable based on default windows node settings: Added ovms_windows specific python settings and c:/opt and removed unused Nvidia and OCL specific tools.
:: When changing the values here you can print the node default PATH value and base your changes on it.
set "setPath=C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Tools\MSVC\14.29.30133\bin\HostX86\x86;c:\opt;C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\Common7\IDE\VC\VCPackages;C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\Common7\IDE\CommonExtensions\Microsoft\TestWindow;C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer;C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\MSBuild\Current\bin\Roslyn;C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\Team Tools\Performance Tools;C:\Program Files (x86)\Microsoft Visual Studio\Shared\Common\VSPerfCollectionTools\vs2019\;C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\Common7\Tools\devinit;C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x86;C:\Program Files (x86)\Windows Kits\10\bin\x86;C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\\MSBuild\Current\Bin;C:\Windows\Microsoft.NET\Framework\v4.0.30319;C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\Common7\IDE\;C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\Common7\Tools\;C:\Program Files\Common Files\Oracle\Java\javapath;C:\Windows\system32;C:\Windows;C:\Windows\System32\Wbem;C:\Windows\System32\WindowsPowerShell\v1.0\;C:\Utils\;C:\Program Files\Git\cmd;C:\Program Files\Git\mingw64\bin;C:\Program Files\Git\usr\bin;C:\Ninja;C:\Program Files\CMake\bin;C:\Program Files\7-zip;C:\opt\Python39\Scripts\;C:\opt\Python39\;C:\opencl\install\;C:\opencl\;C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
set "buildCommand=bazel build --config=windows --jobs=%NUMBER_OF_PROCESSORS% --verbose_failures --define CLOUD_DISABLE=1 --define MEDIAPIPE_DISABLE=1 --define PYTHON_DISABLE=1 //src:ovms 2>&1 | tee build.log"
set "envPath=environment.log"

:: Start VS 2019 Developer command line
%vsDevPath%

:: Set proper PATH environment variable: Remove other python paths and add c:\opt with bazel to PATH
set "PATH=%setPath%"

:: Log all environment variables
set > %envPath%

:: Start bazel build
%buildCommand%
endlocal