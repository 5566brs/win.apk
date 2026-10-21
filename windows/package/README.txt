Win.apk preview
===============

An early preview of Win.apk, an unofficial Windows port of Mac.apk. It runs
Android apps as ordinary Windows programs on Windows 10/11 x64.

What works
----------
- Apps written in Java or Kotlin. They open in their own window and respond
  to the mouse, the mouse wheel and the keyboard. Escape (or Alt+Left, or
  the mouse's back button) is Android's Back.
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

Installing Win.apk
------------------
Win.apk-Setup.exe installs it for your user (no administrator rights): Win.apk
goes into the Start menu, and .apk files open with it. Or unzip
Win.apk-windows-x64.zip anywhere and start Win.apk.exe from that folder.

Adding and running apps
-----------------------
Open Win.apk: it shows your app library.
- Add an app: drag an .apk (or an .apkm, .xapk or .apks bundle) onto the
  window, click "Add app...", or double-click the .apk in Explorer.
  Installing takes up to a minute, once per app.
- Each installed app gets its own icon in the library, in the Start menu
  (Start > Win.apk Apps) and on the desktop.
- Double-click an app to open it. Right-click for more: a new desktop
  shortcut, the last log, or removing the app.

In an app, the mouse is your finger and the wheel scrolls. Android's Back
is Escape, Alt+Left, or the mouse's back side button. Close the app's window to quit. Apps from F-Droid
(f-droid.org) are a good place to start: they are free, open source, and
usually Java-only.

Where things are
----------------
Installed apps:  %LOCALAPPDATA%\Win.apk\library
App data:        %LOCALAPPDATA%\Win.apk\android
Last log:        %LOCALAPPDATA%\Win.apk\logs\last-run.log

Removing an app from the library keeps what it saved; uninstalling Win.apk
keeps the apps too. Delete %LOCALAPPDATA%\Win.apk to start from scratch.

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
