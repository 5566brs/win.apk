Win.apk preview
===============

An early preview of Win.apk, an unofficial Windows port of Mac.apk. It runs
Android apps as ordinary Windows programs on Windows 10/11 x64.

What works
----------
- Apps written in Java or Kotlin. They open in their own window and respond
  to the mouse, the mouse wheel and the keyboard. Escape is Android's Back.
- Games and apps that draw with OpenGL ES, from Java (GLSurfaceView, libGDX)
  or from their own native code through a SurfaceView (SDL games). Graphics
  go through ANGLE on Direct3D 11; without a usable GPU, a software renderer
  takes over.
- Apps that ship x86_64 native libraries (lib/x86_64/*.so).
- Sound from Java through AudioTrack and SoundPool (WAV, Ogg, MP3), saving
  data (files, SharedPreferences, SQLite), the clipboard, file open/save
  dialogs and game controllers.

What does not work yet
----------------------
- Apps that only have ARM native libraries (lib/arm64-v8a only).
- NativeActivity games (pure C/C++ apps without a Java activity).
- MediaPlayer and sound from native code (OpenSL ES, AAudio), video
  playback, the camera, WebView (web pages inside apps), location.
- Google Play services (sign-in, in-app purchases, maps).

How to run an app
-----------------
1. Unzip this folder anywhere, for example to C:\Win.apk.
2. Drag an .apk file (or an .apkm, .xapk or .apks bundle) onto
   "Run APK.bat".
3. The first start of each app takes a while (up to a minute) because its
   code is converted once. Later starts are faster.

Close the app's window to quit. Apps from F-Droid (f-droid.org) are a good
place to start: they are free, open source, and usually Java-only.

Where things are
----------------
App data:  %LOCALAPPDATA%\Win.apk\android
Last log:  %LOCALAPPDATA%\Win.apk\logs\last-run.log

To start an app from scratch, delete its folder under
%LOCALAPPDATA%\Win.apk.

Reporting a problem
-------------------
If an app does not start or misbehaves, the last log says why. Please note
the app's name and version and attach last-run.log. These lines in the log
are the most useful:
  "native not implemented"            a Windows piece that is missing
  "not implemented on Windows yet"    a system function an app's native code needs
  "Exception" or "FATAL"              the app crashed

Java is included (Eclipse Temurin 21, in the jre folder), and so is ANGLE
(bin\libEGL.dll, bin\libGLESv2.dll, Chromium's build; licenses in
bin\licenses). Nothing else needs to be installed.

License: AGPL-3.0, like Mac.apk. Source: the repository this build came
from. Win.apk is not affiliated with the Mac.apk project.
