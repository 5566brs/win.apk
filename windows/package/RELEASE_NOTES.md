A pre-release for testing: Win.apk runs Android apps on Windows 10/11 x64.

## Getting started
- **Install:** run `Win.apk-Setup.exe`. It installs per user, with no administrator rights, and puts Win.apk in the Start menu. If you'd rather not install, unzip `Win.apk-windows-x64.zip` and start `Win.apk.exe`.
- **Add apps:** drag `.apk` files onto the Win.apk window, or double-click them in Explorer. Each app gets its own icon in the library, the Start menu and the desktop.
- **Included:** Java and ANGLE. `README.txt` has the details.

## What works
- **Java/Kotlin apps:** each opens in its own window, with mouse, wheel and keyboard input. Back is Escape, Alt+Left or the mouse's back button.
- **OpenGL ES:** works from Java (`GLSurfaceView`, libGDX) and from an app's native code through a SurfaceView (SDL games). It runs on ANGLE (Direct3D 11), falling back to WARP or SwiftShader without a GPU.
- **Native libraries:** apps that ship `lib/x86_64` libraries load them natively.
- **Sound from Java:** AudioTrack and SoundPool, for WAV, Ogg and MP3.
- **Storage:** files, SharedPreferences and SQLite.
- **Desktop services:** clipboard, file pickers and game controllers.

In the automated sweep of open-source apps, about 33 of 38 draw their first screen.

## Not yet
- ARM-only native code.
- NativeActivity games.
- MediaPlayer and native audio (OpenSL ES and AAudio).
- Video, camera, WebView and location.
- Google Play services.

## Reporting
Please include the app's name and version, and attach `%LOCALAPPDATA%\Win.apk\logs\last-run.log`.
