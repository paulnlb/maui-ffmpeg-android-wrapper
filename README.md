Integrating FFmpeg into a .NET MAUI Android app looks like this:

```
MAUI C#
   ↓  (P/Invoke)
Your custom wrapper .so
   ↓
FFmpeg libraries (.so)
```

You **do not call FFmpeg directly from C#**. You write a **small C wrapper** that calls FFmpeg internally.

---

# 1. What you actually need

FFmpeg is not one library - it is several. These are some (but not all) of them:

```
libavformat
libavcodec
libavutil
libswresample
```

Android apps load them as `.so` files.

Typical result:

```
libavcodec.so
libavformat.so
libavutil.so
libswresample.so
```

Your wrapper will become:

```
libffmpegwrapper.so
```

---

# 2. Download or build FFmpeg for Android

You need FFmpeg compiled for Android ABIs.

Most common ABIs:

```
arm64-v8a
armeabi-v7a
x86_64
x86
```

After building or downloading, you should have something like:

```
ffmpeg-android/
 ├── arm64-v8a/
 │   ├── libavcodec.so
 │   ├── libavformat.so
 │   ├── libavutil.so
 │   └── libswresample.so
```

You can build ffmpeg libraries from source using the https://github.com/Javernaut/ffmpeg-android-maker script.

Example usage of ffmpeg-android-maker on Fedora linux:

```sh
git clone https://github.com/Javernaut/ffmpeg-android-maker.git

export ANDROID_SDK_HOME="/var/home/username/Android/Sdk/"
export ANDROID_NDK_HOME="/var/home/username/Android/Sdk/ndk/30.0.14904198/" # or whatever version you installed

sudo dnf install nasm # the script required this dependency in my case

cd ffmpeg-android-maker
./ffmpeg-android-maker.sh
```

The produced libs will be stored in the `output` directory.
Copy `lib` and `include` directories from it to your working directory for the next step.

---

# 3. Write the C wrapper

Create:

```
ffmpeg_wrapper.c
```

Implement (or vibe code as I did lol) one or many wrapper functions for your needs. An example implementation for extracting, transcoding and resampling an audio piece from a video can be found in the repo.

Important idea: Your wrapper exposes **you custom C functions** while hiding the FFmpeg API under the hood.

---

# 4. CMake project

Structure:

```
native/
 ├── CMakeLists.txt
 ├── ffmpeg_wrapper.c
 └── lib/
     └── arm64-v8a/
         ├── libavcodec.so
         ├── libavformat.so
         ├── libavutil.so
         └── libswresample.so
     └── armeabi-v7a/
         ├── libavcodec.so
         ├── libavformat.so
         ├── libavutil.so
         └── libswresample.so
     └── x86/
         ├── libavcodec.so
         ├── libavformat.so
         ├── libavutil.so
         └── libswresample.so
     └── x86_64/
         ├── libavcodec.so
         ├── libavformat.so
         ├── libavutil.so
         └── libswresample.so
 └── include/
     └── arm64-v8a/
         ├── libavcodec
         ├── libavformat
         ├── libavutil
         └── libswresample
     └── armeabi-v7a/
         ├── libavcodec
         ├── libavformat
         ├── libavutil
         └── libswresample
     └── x86/
         ├── libavcodec
         ├── libavformat
         ├── libavutil
         └── libswresample
     └── x86_64/
         ├── libavcodec.so
         ├── libavformat.so
         ├── libavutil.so
         └── libswresample.so
```

CMake:

```cmake
cmake_minimum_required(VERSION 3.10)

project(ffmpegwrapper)

include_directories(${CMAKE_SOURCE_DIR}/include/${ANDROID_ABI})

add_library(ffmpegwrapper SHARED ffmpeg_wrapper.c)

add_library(avcodec SHARED IMPORTED)
set_target_properties(avcodec PROPERTIES
    IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/lib/${ANDROID_ABI}/libavcodec.so)

add_library(avformat SHARED IMPORTED)
set_target_properties(avformat PROPERTIES
    IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/lib/${ANDROID_ABI}/libavformat.so)

add_library(avutil SHARED IMPORTED)
set_target_properties(avutil PROPERTIES
    IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/lib/${ANDROID_ABI}/libavutil.so)

add_library(swresample SHARED IMPORTED)
set_target_properties(swresample PROPERTIES
    IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/lib/${ANDROID_ABI}/libswresample.so)

target_link_libraries(
    ffmpegwrapper
    avcodec
    avformat
    avutil
    swresample
    log
)
```

This says:

```
build libffmpegwrapper.so
and link against FFmpeg libraries
```

---

# 5. Compile the wrapper

```sh
# install CMake and compiler for Fedora
sudo dnf install cmake gcc-c++
```

Using Android NDK:

```
cmake -S . -B build \
 -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
 -DANDROID_ABI=arm64-v8a \
 -DANDROID_PLATFORM=android-21


cmake --build build
```

Result:

```
libffmpegwrapper.so
```

---

# 6. Place libraries into the MAUI project

Inside your MAUI project:

```
Platforms/Android/lib/arm64-v8a/
```

Copy:

```
libavcodec.so
libavformat.so
libavutil.so
libswresample.so
libffmpegwrapper.so
```

Also, register the libs in your .csproj

```xml
<ItemGroup>
    <AndroidNativeLibrary Include="Platforms\Android\lib\arm64-v8a\libffmpegwrapper.so" />
    <AndroidNativeLibrary Include="Platforms\Android\lib\arm64-v8a\libavcodec.so" />
    <AndroidNativeLibrary Include="Platforms\Android\lib\arm64-v8a\libavformat.so" />
    <AndroidNativeLibrary Include="Platforms\Android\lib\arm64-v8a\libavutil.so" />
    <AndroidNativeLibrary Include="Platforms\Android\lib\arm64-v8a\libswresample.so" />
</ItemGroup>
```

Android loads dependencies automatically.

```
libffmpegwrapper.so
    ↓
libavcodec.so
libavformat.so
libavutil.so
...
```

Everything must be in the **same ABI folder**.

---

# 7. Create a C# binding

Create:

```
FfmpegNativeWrapper.cs
```

```csharp
public static partial class FfmpegNativeWrapper
{
    [LibraryImport("ffmpegwrapper", StringMarshalling = StringMarshalling.Utf8)]
    public static partial int extract_audio(
        int start_time_sec,
        int end_time_sec,
        string source_path,
        int sample_rate,
        string output_format,
        string output_path
    );
}
```

❗❗❗**Note**: for Android, it's critical to set string marshalling encoding to **Utf8**.

---

# 8. Use it from MAUI

```csharp
await Task.Run(() =>
        {

            var exitCode = FfmpegNativeWrapper.extract_audio(
                (int)startTime.TotalSeconds,
                (int)endTime.TotalSeconds,
                sourcePath,
                16000,
                AudioFormats.Wave,
                $"unix://{_socketListener.Endpoint}"
            );
        });
```
