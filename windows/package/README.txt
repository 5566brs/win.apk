Win.apk preview
===============

An early preview of Win.apk, an unofficial Windows port of Mac.apk. It runs
Android apps as ordinary Windows programs.

What works: Android apps written only in Java or Kotlin, without native
code. They open in their own window and respond to the mouse, the mouse
wheel and the keyboard. Escape is Android's Back button.

What does not work yet: games and other apps that use OpenGL, apps with
native (.so) libraries, sound, and saving data in databases.

How to run an app
-----------------
1. Unzip this folder anywhere, for example to C:\Win.apk.
2. Drag an .apk file onto "Run APK.bat".
3. The first start of each app takes a while (up to a minute) because its
   code is converted once. Later starts are faster.

Close the app's window to quit.

Where things are
----------------
App data:  %LOCALAPPDATA%\Win.apk\android
Last log:  %LOCALAPPDATA%\Win.apk\logs\last-run.log

If an app does not start or misbehaves, the last log says why. Please
attach it when you report a problem.

Java is included (Eclipse Temurin 21, in the jre folder); nothing else
needs to be installed.

License: AGPL-3.0, like Mac.apk. Source: the repository this build came
from. Win.apk is not affiliated with the Mac.apk project.
