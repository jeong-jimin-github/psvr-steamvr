# PSVR SteamVR driver (headset only)

OpenVR driver for the original PlayStation VR (CUH-ZVR / processing unit, USB `054C:09AF`). Controllers are intentionally not implemented.

## What it does

- Sends USB control commands so the processing unit actually shows HDMI (power on, VR mode, tracking LEDs)
- Reads the BMI055 IMU over HID and fuses it into 3DoF orientation
- Presents the `SIE HMD` 1920×1080 side-by-side display to SteamVR in extended mode

Positional tracking is a neck model only (no camera). Yaw will drift; press the headset mute button (or volume+ and volume- together) to recenter.

## Requirements

- Windows x64, SteamVR, PSVR processing unit on USB + HDMI
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
