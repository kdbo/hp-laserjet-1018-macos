# HP LaserJet 1018 for macOS

A native CUPS driver for the **HP LaserJet 1018** on modern macOS, supporting both **Intel (x86_64)** and **Apple Silicon (arm64)** Macs.

The driver provides automatic firmware loading and AirPrint support for the HP LaserJet 1018 on macOS systems where no official driver is available.

## Features

* Native **Intel (x86_64)** support

* Native **Apple Silicon (arm64)** support

* Universal CUPS raster filter containing both architectures

* No Ghostscript required

* No Rosetta required

* Automatic firmware loading after the printer is connected

* Automatic firmware reload when the printer becomes available

* **AirPrint support**

* AirPrint advertisement follows the CUPS printer sharing state

* Packaged installation using a macOS `.pkg`

* **Built-in uninstaller**

## How it works

The printing pipeline uses macOS's built-in PDF-to-raster conversion:

```text
PDF

 ↓

macOS cgpdftoraster

 ↓

rastertozjs

 ↓

HP LaserJet 1018
```

The `rastertozjs` filter is built as a universal Mach-O binary containing:

```text
x86_64

arm64
```

This means the same driver works natively on both Intel and Apple Silicon Macs.

The HP LaserJet 1018 requires firmware to be uploaded to the printer. A LaunchDaemon monitors the printer and automatically performs this operation when necessary.

AirPrint is provided separately through Bonjour:

```text
iPhone / iPad / Mac

        ↓

      Bonjour

        ↓

     AirPrint

        ↓

       CUPS

        ↓

HP LaserJet 1018
```

## Requirements

* macOS 14 or later

* Intel Mac (`x86_64`) **or** Apple Silicon Mac (`arm64`)

* HP LaserJet 1018 connected through USB

* Xcode Command Line Tools when building from source

Install the Command Line Tools with:

```bash
xcode-select --install
```

## Installation

The project is distributed as a macOS installer package.

The package installs the CUPS filter, firmware, PPD and supporting LaunchDaemons required by the driver.

Generated installer packages are build artifacts and are not stored in the Git repository.

## Uninstallation

The package includes a built-in uninstaller.

After installation, run:

```bash
sudo hp1018-uninstall
```

The uninstaller stops and removes the HP LaserJet 1018 firmware and AirPrint LaunchDaemons and removes the files installed by the driver, including the CUPS raster filter, firmware resources and HP1018 application-support files.

The CUPS printer queue is **not automatically removed**. This is intentional, as printer queues are user configuration rather than part of the driver installation.

If required, the printer can be removed separately through:

**System Settings → Printers & Scanners**

## CUPS setup

After installation:

1. Connect the HP LaserJet 1018 through USB.
2. Open **System Settings → Printers & Scanners**.
3. Add the HP LaserJet 1018.
4. Select the supplied HP LaserJet 1018 PPD/driver.
5. Enable **Printer Sharing** if AirPrint access is required.

Multiple HP LaserJet 1018 printers can be configured with separate CUPS queue
names. The firmware and AirPrint services associate each queue with its exact
USB DeviceURI, including the serial number when CUPS provides one.

The firmware LaunchDaemon handles the firmware upload automatically.

## AirPrint

AirPrint support is provided by the included LaunchDaemon:

```text
com.kdbo.hp1018.airprint
```

When printer sharing is enabled, the daemon publishes the printer through Bonjour.

The advertised service uses:

```text
_ipp._tcp,_universal
```

The daemon publishes every shared HP LaserJet 1018 CUPS queue whose configured
USB device is currently connected. Each advertisement points to its own CUPS
queue at `printers/<queue-name>` and includes that queue name in the Bonjour
service name.

### Checking AirPrint

Bonjour can be inspected with:

```bash
dns-sd -B _ipp._tcp,_universal local
```

The expected service name has this form:

```text
<CUPS printer info> AirPrint (<queue-name>)
```

### Disabling AirPrint

AirPrint follows the CUPS printer sharing state.

If printer sharing is disabled, the AirPrint service disappears automatically.

If printer sharing is enabled again, the service is recreated automatically.

No manual `dns-sd` command is required.

## Firmware

The HP LaserJet 1018 requires firmware to be loaded after the printer powers on.

The printer-specific firmware resource is:

```text
sihp1018.dl
```

Firmware loading is handled automatically by:

```text
com.kdbo.hp1018-firmware
```

The firmware watcher is installed as:

```text
/usr/local/libexec/hp1018/watch.sh
```

The LaunchDaemon periodically checks every configured HP LaserJet 1018 queue
and loads firmware only to that queue's exact USB DeviceURI when required.

## Building from source

Clone the repository and enter the project directory:

```bash
git clone <repository-url>

cd hp-laserjet-1018-macos
```

Build the HP LaserJet 1018 CUPS filter with:

```bash
make rastertozjs
```

The resulting `rastertozjs` executable is built for both:

```text
x86_64

arm64
```

The compiled binary is a build artifact and is excluded from Git.

To build the available filters:

```bash
make
```

## Building the installer package

The installer package is generated using Apple's `pkgbuild` and related packaging tools.

The package build directory is temporary and is **not part of the Git repository**.

Generated `.pkg` files are also excluded from Git.

## Project structure

```text
hp-laserjet-1018-macos/

├── PPD/

│   ├── HP-LaserJet_1018-native.ppd
│   ├── HP-LaserJet_1018.ppd
│   └── ...

│
├── foo2zjs/

│   ├── sihp1018.img
│   ├── jbig.c
│   ├── jbig_ar.c
│   ├── *.h
│   ├── *.icm
│   └── ...

│
├── rastertozjs.c

├── Makefile

├── README.md

├── LICENSE

└── .gitignore
```

The `foo2zjs/` directory contains source code and printer-specific resources originating from the foo2zjs project. It is retained as a source base for the current driver and potentially for support for additional printers in the future.

Compiled binaries, installer packages and temporary package-build directories are intentionally excluded from the repository.

## Version history

### Temporary / unreleased
* Added support for multiple HP LaserJet 1018 printers by associating each CUPS queue with its exact USB DeviceURI.
* Firmware loading and AirPrint advertisements are now tracked independently per configured printer queue.

### 1.0.19
* Airprint also checks if the printer is connected, not only if the printer is shared. This will stop airprint when the usb has been unplugged or the printer loses power.
* Firmware will now be loaded when the usb gets reconnected or when the printer  has been powercycled.

### 1.0.18

* Added built-in uninstaller
* Added `hp1018-uninstall` command for removing the HP LaserJet 1018 driver and its supporting files

### 1.0.2

* Added automatic AirPrint support
* Added Bonjour `_ipp._tcp,_universal` advertisement
* AirPrint follows the CUPS printer sharing state
* Added automatic restart/recovery of the AirPrint daemon
* Added dynamic printer information to the AirPrint advertisement
* Added universal `x86_64` and `arm64` CUPS filter

### 1.0.1

Initial packaged macOS driver release with automatic firmware handling.

## Origin

The printing technology used by this project is based on the **foo2zjs** project by Rick Richardson.

The original foo2zjs project provided support for a large number of printers using proprietary raster formats. This project uses relevant foo2zjs code and printer-specific resources as the basis for a modern macOS implementation for the HP LaserJet 1018.

Original project:

http://foo2zjs.rkkda.com/

Firmware images are copyright HP.

## License

GNU General Public License v2 or later.

See [LICENSE](LICENSE) for the full license text.

```
```
