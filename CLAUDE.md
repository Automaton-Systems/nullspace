# Development Setup Guide

## Android Development Environment

### Prerequisites
- Android SDK installed (typically at `~/Android/Sdk`)
- Java 17+ (available from Android Studio's bundled JDK)
- Git submodules initialized

### Quick Start

#### 1. Initial Setup
```bash
# Clone and initialize submodules
git clone https://github.com/plushmonkey/nullspace
cd nullspace
git submodule init && git submodule update

# Copy resources (graphics and sound folders) to Android assets
mkdir -p android/app/src/main/assets
cp -r /path/to/nullspace-resources/graphics android/app/src/main/assets/
cp -r /path/to/nullspace-resources/sound android/app/src/main/assets/
```

#### 2. Configure Android Project
Create `android/local.properties`:
```properties
sdk.dir=/home/username/Android/Sdk
```

Update CMake version in `android/app/build.gradle`:
```gradle
externalNativeBuild {
    cmake {
        version '3.22.1'  // Match your installed CMake version
    }
}
```

#### 3. Build APK
```bash
cd android
export JAVA_HOME="/path/to/jdk"  # e.g., Android Studio's bundled JDK
./gradlew build
```

### Running on Emulator

#### Create and Start Emulator
```bash
# Create AVD configuration
mkdir -p ~/.android/avd/nullspace_dev.avd

# Create AVD ini file (see full config in android/ folder)
# Then start emulator (use -gpu host for better input handling):
~/Android/Sdk/emulator/emulator -avd nullspace_dev -no-snapshot-save -no-audio -no-boot-anim -gpu host &

# Wait for boot
~/Android/Sdk/platform-tools/adb wait-for-device shell 'while [[ -z $(getprop sys.boot_completed) ]]; do sleep 1; done'
```

> **GPU Mode:** Use `-gpu host` for best performance and input handling. If you have GPU driver issues, fall back to `-gpu swiftshader_indirect` (software rendering).

#### Install and Run
```bash
cd android
./gradlew installDebug
~/Android/Sdk/platform-tools/adb shell am start -n com.plushnode.nullspace/.MainActivity
```

> **Note:** Before connecting, ensure an ASSS server is running (see [Server Setup](#server-setup) section). When using the emulator, the app will connect to 10.0.2.2:5000 by default, which maps to localhost:5000 on your host machine.

#### Take Screenshots
```bash
~/Android/Sdk/platform-tools/adb exec-out screencap -p > screenshot.png
```

#### View Logs
```bash
~/Android/Sdk/platform-tools/adb logcat -d | grep nullspace
```

#### Programmatic Control (Automation)

**Coordinate System:**
The emulator runs landscape (2400x1080) with rotation=1 on a portrait display (1080x2400).

**Click Play Button:**
```bash
# Play button coordinates in landscape: (1703, 648)
adb shell input tap 1703 648
```

**Move Camera (Swipe):**
```bash
# Swipe from center (1200, 540) to top-left (800, 340) over 500ms
adb shell input swipe 1200 540 800 340 500
```

**Coordinate Transformation Formula:**
```bash
# From raw touch events (getevent, max value 32767):
portrait_x = (raw_x × 1080) / 32767  
portrait_y = (raw_y × 2400) / 32767

# Transform to landscape for adb input tap:
landscape_x = portrait_y
landscape_y = 1080 - portrait_x
```

**Full Automated Workflow:**
```bash
# Stop, restart, and connect to server
adb shell am force-stop com.plushnode.nullspace
adb shell am start -n com.plushnode.nullspace/.MainActivity
sleep 4
adb shell input tap 1703 648  # Click Play
sleep 3
adb exec-out screencap -p > game.png
```

### Common Paths

**Flatpak Android Studio Java:**
```
/home/username/.local/share/flatpak/app/com.google.AndroidStudio/x86_64/stable/[hash]/files/extra/jbr
```

**Output APKs:**
```
android/app/build/outputs/apk/debug/app-debug.apk
android/app/build/outputs/apk/release/app-release.apk
```

### Troubleshooting

**SDK location not found:**
- Create `android/local.properties` with correct `sdk.dir`

**CMake version mismatch:**
- Check installed version: `ls ~/Android/Sdk/cmake/`
- Update `android/app/build.gradle` to match

**Missing resources:**
- Ensure `graphics/` and `sound/` folders exist in `android/app/src/main/assets/`
- Assets are loaded via Android AssetManager, not filesystem

**Java not found:**
- Set `JAVA_HOME` to Java 17+ before running gradlew
- Android Studio's bundled JDK works well

### Touch Controls

The Android version is spectator-only (no ship controls):
- **Drag** - Move spectate camera
- **Double-tap** - Spectate nearby player
- **Long-press** - Open ESC menu
- **Screen zones** (3x3 grid) - Select ships 1-8 when menu is open

## Desktop Development

TODO

## Server Setup

### ASSS Server (for testing)

#### Clone and Build
```bash
# Clone ASSS server
git clone https://github.com/plushmonkey/asss
cd asss

# Build with CMake
mkdir build && cd build
cmake ..
make

# Server binary will be in build/bin/asss
```

#### Running the Server
```bash
# Start server (from asss/zone directory)
cd ~/path/to/asss/zone
../build/bin/asss

# Server listens on port 5000 (TCP/UDP) by default
# Emulator connects via 10.0.2.2:5000 (maps to host localhost)
```

#### Stop Server
```bash
# Find process
ps aux | grep asss

# Kill by PID
kill <pid>
```

### Connecting to Server

The nullspace app has multiple server configurations in `android/app/src/main/cpp/nullspace_android.cpp`:
- **"emulator"** - 10.0.2.2:5000 (for Android emulator → host localhost)
- **"local"** - 192.168.x.x:5000 (for device on local network)
- Various public servers

**When running in emulator:** Select the "emulator" zone to connect to ASSS running on your host machine.

**Zone selection:** The app defaults to `kServerIndex = 0` (emulator zone). To change, modify the index in nullspace_android.cpp and rebuild.
