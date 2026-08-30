# PSVR SteamVR driver (Gear VR / Samsung Odyssey controller support)

OpenVR driver for the original PlayStation VR (CUH-ZVR / processing unit, USB `054C:09AF`) with experimental Gear VR and Samsung Odyssey/WMR controller support.

## What it does

- Sends USB control commands so the processing unit actually shows HDMI (power on, VR mode, tracking LEDs)
- Reads the BMI055 IMU over HID and fuses it into 3DoF orientation
- Presents the `SIE HMD` 1920×1080 side-by-side display to SteamVR in extended mode
- Discovers the Gear VR controller by its `OculusThreemote` BLE service
- Reads Samsung Odyssey/WMR controller buttons and IMU over Windows Raw Input
- Experimental PS Camera optical-position path for Odyssey controllers

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

## PS Camera + Odyssey test without a PSVR headset

`psvr_odyssey_optical_test.exe` is intentionally independent of the PSVR headset and processing unit. You only need the Windows-visible PlayStation Camera and one or two paired Samsung Odyssey/WMR motion controllers.

Build first, then list video devices:

```powershell
.\build\dist\psvr\bin\win64\psvr_odyssey_optical_test.exe --list
```

Run the automatic 30-second test:

```powershell
.\build\dist\psvr\bin\win64\psvr_odyssey_optical_test.exe --seconds 30
```

The output reports camera format, adaptive LED threshold/noise, left/right blob counts, stereo matches, controller clusters, controller Raw Input packet counts, camera-space XYZ, quality and visible LED count. Move the controller while watching XYZ; a working stereo path should change continuously rather than staying at a fixed HMD-relative position.

If the PS Camera driver exposes both sensors in one wide frame but it is not recognized automatically:

```powershell
.\build\dist\psvr\bin\win64\psvr_odyssey_optical_test.exe --force-sbs --seconds 30
```

If the driver exposes the two sensors as two separate video devices, find their indices using `--list`, then run for example:

```powershell
.\build\dist\psvr\bin\win64\psvr_odyssey_optical_test.exe --camera 1 --camera-right 2 --seconds 30
```

If stereo disparity has the wrong sign, add `--swap-eyes`.

To save the current left/right grayscale views with crosses on detected blobs:

```powershell
.\build\dist\psvr\bin\win64\psvr_odyssey_optical_test.exe --force-sbs --dump-prefix optical
```

This writes/updates `optical-left.pgm` and `optical-right.pgm` while the test runs.

### Calibration

Without a calibration file the test uses an approximate 85-degree pinhole FOV and 85 mm stereo baseline. This is sufficient for acquisition/movement smoke tests but the XYZ scale is not guaranteed to be accurate.

Copy the example and edit it when measured camera calibration is available:

```powershell
Copy-Item .\psvr_camera_calibration.example.txt .\psvr_camera_calibration.txt
```

The test automatically loads `psvr_camera_calibration.txt` from the current directory, or use `--calibration <file>`.

### Optional Odyssey LED brightness attempt

The WMR LED-control packet supports intensity `1..399`. Windows often owns the Bluetooth HID output side and allows only read access. The normal optical test therefore does not require output access.

To make a best-effort intensity-399 attempt before the test:

```powershell
.\build\dist\psvr\bin\win64\psvr_odyssey_optical_test.exe --try-led-max --force-sbs
```

If Windows denies the output handle, the program prints `SKIP` and continues camera + Raw Input tracking normally.

## Gear VR controller test

```powershell
.\build\dist\psvr\bin\win64\psvr_ctl.exe gearvr-scan
.\build\dist\psvr\bin\win64\psvr_ctl.exe gearvr
```

Windows' generic Bluetooth Settings can list the Gear VR controller yet fail its classic pairing flow. The driver uses the controller's BLE GATT service directly; no HID profile is required. Hold Home to wake/re-advertise it before starting SteamVR.
