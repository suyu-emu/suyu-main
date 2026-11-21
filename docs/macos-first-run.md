# suyu on your Mac — first run

These instructions are for the finished application ZIP. A source-code or
build-preparation ZIP is not an application and cannot be installed this way.

1. Double-click `suyu-macos-arm64.zip` in Downloads. A folder containing
   `suyu.app` appears.
2. Open Finder. Drag `suyu.app` into **Applications** in the sidebar.
3. Open Applications and double-click **suyu**.
4. If asked to choose a mode, select **Gamer**. You can remember this choice.
5. If a welcome window appears, its setup steps are optional. Click **Finish**
   to view the empty application. You do not need to link an account or install
   Steam just to open it.
6. An empty game list is expected. This download contains only the application
   and its supporting libraries. Games, firmware and encryption keys are not
   supplied. Opening the app does not prove that a particular game works.

The intended package includes Qt and MoltenVK. You should not need Homebrew,
Xcode, CMake, a Vulkan SDK, Rosetta or Terminal to open the finished arm64 app.
Apple's own system frameworks come with macOS and are not redistributed.

## If macOS blocks the first launch

The prepared build process uses an ad hoc signature, which is not Apple
notarization. After attempting to open the trusted application:

1. Click **Done** or dismiss the warning.
2. Open the **Apple menu → System Settings → Privacy & Security**.
3. Scroll to Security. Find the message about suyu and click **Open Anyway**.
4. Confirm **Open** and enter your Mac login password if asked.

This creates an exception for that application. You should then be able to open
it normally. You do not need to turn off Gatekeeper or use Terminal.

If the message says the app **will damage your computer**, or says it is
**damaged**, stop and obtain a checked replacement package. A blocked unknown
developer and a damaged application need different handling.

Apple's instructions: https://support.apple.com/en-my/102445

## If something does not work

- **No Open Anyway button:** try opening the app again, then return to Privacy
  & Security. A managed work Mac may require help from its administrator.
- **Missing library or immediate crash:** provide the builder with the exact
  message, your macOS version, and the package's verification report. A properly
  packaged app should not ask you to install a missing developer library.
- **Vulkan warning or no graphics device:** the builder needs to investigate
  MoltenVK on your Mac. Keep Vulkan selected; a Metal rewrite is not part of
  this build.

Your reported target is a MacBook Pro with M4 Pro and 24 GB RAM, running macOS
27.0.1. A successful build-host check still needs confirmation on that exact
Mac and macOS version.
