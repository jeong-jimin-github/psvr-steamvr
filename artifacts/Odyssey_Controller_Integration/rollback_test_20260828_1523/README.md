# PSVR SteamVR driver (Gear VR controller support)

OpenVR driver for the original PlayStation VR (CUH-ZVR / processing unit, USB `054C:09AF`) with a Samsung Gear VR BLE controller exposed as a right-hand SteamVR controller.

## What it does

- Sends USB control commands so the processing unit actually shows HDMI (power on, VR mode, tracking LEDs)
- Reads the BMI055 IMU over HID and fuses it into 3DoF orientation
- Presents the `SIE HMD` 1920×1080 side-by-side display to SteamVR in extended mode
- Discovers the Gear VR controller by its `OculusThreemote` BLE service, maintains a direct GATT session, parses buttons/touchpad/IMU, and publishes a 3DoF right-hand OpenVR controller

Positional tracking is a neck model only (no camera). Yaw will drift; press the headset mute button (or volume+ and volume- together) to recenter.

## Requirements

- Windows x64, SteamVR, PSVR processing unit on USB + HDMI
- Bluetooth LE adapter and Gear VR controller in discovery mode (hold **Home** until its LED blinks)
- **PS VR Control (MI_05)** bound to WinUSB — already present on this machine as `PS VR Control (WinUSB)` / GUID `{A6B9C7D4-2B0F-4E48-9E4B-77C2E9D5A3F1}`
- **PS VR sensors (MI_04)** left on the stock HID driver
- Headset display extended (not duplicated), RGB 4:4:4 if the image is black in VR mode

## Build / install

```powershell
.\scripts\build.ps1
.\scripts\install.ps1
```

Then start SteamVR. USB smoke test without SteamVR:

```powershell
.\build\dist\psvr\bin\win64\psvr_ctl.exe vr
.\build\dist\psvr\bin\win64\psvr_ctl.exe sensors
```

Controller discovery/stream test:

```powershell
.\build\dist\psvr\bin\win64\psvr_ctl.exe gearvr-scan
.\build\dist\psvr\bin\win64\psvr_ctl.exe gearvr
```

Windows' generic Bluetooth Settings can list the controller yet fail its classic pairing flow. The driver uses the controller's BLE GATT service directly; no HID profile is required. Hold Home to wake/re-advertise it before starting SteamVR.
