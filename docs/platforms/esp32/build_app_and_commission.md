# ESP32 Application Usage Guide

Below guide mentions the details about how to build, flash, monitor, commission
the esp32 application.

---

-   [Supported target chips](#supported-target-chips)
-   [Building an example application](#building-an-example-application)
-   [Set up the environment variables](#set-up-the-environment-variables)
-   [Build, flash and monitor an example](#build-flash-and-monitor-an-example)
-   [Commissioning](#commissioning)
    -   [Building Standalone chip-tool](#building-standalone-chip-tool)
    -   [Commissioning the WiFi devices](#commissioning-the-wifi-devices-esp32-esp32c3-esp32s3-esp32c6)
    -   [Commissioning the Thread device](#commissioning-the-thread-device-esp32h2)
    -   [Commissioning Parameters](#commissioning-parameters)
-   [Flashing app using script](#flashing-app-using-script)

---

## Supported target chips

All the Matter demo application is intended to work on: the
[ESP32-DevKitC](https://www.espressif.com/en/products/hardware/esp32-devkitc/overview),
the
[ESP32-WROVER-KIT_V4.1](https://www.espressif.com/en/products/hardware/esp-wrover-kit/overview),
the [M5Stack](http://m5stack.com), the
[ESP32C3-DevKitM](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/hw-reference/esp32c3/user-guide-devkitm-1.html),
[ESP32-Ethernet-Kit](https://docs.espressif.com/projects/esp-idf/en/latest/hw-reference/get-started-ethernet-kit.html)
and the ESP32S3.

All the applications support variants of ESP32, ESP32C3, ESP32S3 chips.

ESP32H2 and ESP32C6 are only supported and tested with lighting-app,
lit-icd-app, and all-clusters-app.

Note: M5Stack Core 2 display is not supported in the tft component, while other
functionality can still work fine.

## Building an example application

### Set up the environment variables

-   ESP-IDF

    Tools path SHALL be added to the PATH environment variable to make the tools
    usable from the command line. ESP-IDF provides another script which does
    that.

    ```
    $ cd path/to/esp-idf
    $ source export.sh
    ```

-   Matter

    Activate the Matter environment. Below command needs to be executed after
    sourcing `esp-idf/export.sh`.

    ```
    $ cd path/to/connectedhomeip
    $ source scripts/activate.sh
    ```

-   Ccache

    Enable Ccache for faster IDF builds. It is recommended to have Ccache
    installed for faster builds.

    ```
    $ export IDF_CCACHE_ENABLE=1
    ```

### Build, flash and monitor an example

-   Change to example application directory

    ```
    $ cd examples/<app-name>/esp32
    ```

-   Set the Matter target to build

    ```
    $ idf.py set-target (target chip)
    ```

-   Configuration Options

    To build the default configuration (`sdkconfig.defaults`) skip this step.

    To build a specific configuration (example `m5stack` or `esp32h2` or
    `esp32c6`):

    ```
    $ rm sdkconfig
    $ idf.py -D 'SDKCONFIG_DEFAULTS=sdkconfig_m5stack.defaults' build
    ```

    Note: If using a specific device configuration, it is highly recommended to
    start off with one of the defaults and customize on top of that. Certain
    configurations have different constraints that are customized within the
    device specific configuration (eg: main app stack size).

    To customize the configuration, run menuconfig:

    ```
    $ idf.py menuconfig
    ```

-   Build the application

    ```
    $ idf.py build
    ```

-   Flash the application

    After building the application, to flash it connect your device via USB.
    Then run the following command to erase the whole flash, flash the demo
    application onto the device and then monitor its output.

    Note that sometimes you might have to press and hold the boot button on the
    device while it's trying to connect before flashing. For ESP32-DevKitC
    devices this is labeled in the
    [functional description diagram](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/hw-reference/esp32/get-started-devkitc.html#functional-description)

    ```
    $ idf.py -p (PORT) erase_flash
    $ idf.py -p (PORT) flash monitor
    ```

    Please replace `(PORT)` with the correct USB device name for your system
    (like `/dev/ttyUSB0` on Linux or `/dev/tty.usbserial-101` on Mac).

    Note: Some users might have to install the
    [VCP driver](https://www.silabs.com/products/development-tools/software/usb-to-uart-bridge-vcp-drivers)
    before the device shows up on /dev/tty.

-   Quit the monitor by pressing `Ctrl+]`. Note: You can see a menu of various
    monitor commands by hitting Ctrl+t Ctrl+h while the monitor is running.

### Troubleshooting Environment Setup Issues

If you have already built an example and later change the ESP-IDF version, you
may encounter environment setup issues. To resolve this, follow these steps:

1. Clean the build artifacts:

    ```
    cd examples/<app-name>/esp32
    rm -rf build sdkconfig managed_components dependencies.lock
    ```

2. Close the terminal, open a new one, and source the ESP-IDF and Matter
   environments:

    ```
    cd path/to/esp-idf
    source export.sh

    cd path/to/connectedhomeip
    source scripts/activate.sh
    ```

3. Rebuild the example application:

    ```
    cd examples/<app-name>/esp32
    idf.py build
    ```

## Commissioning

Below apps can be used for commissioning the application running on ESP32:

-   [Python Based Device Controller](https://github.com/project-chip/connectedhomeip/tree/master/src/controller/python)
-   [Standalone chip-tool](https://github.com/project-chip/connectedhomeip/tree/master/examples/chip-tool)
-   [iOS chip-tool App](https://github.com/project-chip/connectedhomeip/tree/master/src/darwin/CHIPTool)
-   [Android chip-tool App](https://github.com/project-chip/connectedhomeip/tree/master/examples/android/CHIPTool)

### Building Standalone chip-tool

```
$ cd path/to/connectedhomeip
$ scripts/examples/gn_build_example.sh examples/chip-tool out/debug
```

Run the built executable and pass it the discriminator and pairing code of the
remote device, as well as the network credentials to use.

#### Commissioning the WiFi devices (ESP32, ESP32C3, ESP32S3, ESP32C6)

```
$ out/debug/chip-tool pairing ble-wifi 12345 MY_SSID MY_PASSWORD 20202021 3840
```

#### Commissioning the Thread device (ESP32H2)

-   For ESP32-H2, firstly start OpenThread Border Router, you can either use
    [Raspberry Pi OpenThread Border Router](../openthread/openthread_border_router_pi.md)
    OR
    [ESP32 OpenThread Border Router](../../../examples/thread-br-app/esp32/README.md)

-   Get the active operational dataset.

    ```
    $ ot-ctl> dataset active -x
    ```

-   Commissioning the Thread device

    ```
    $ ./out/debug/chip-tool pairing ble-thread 12345 hex:<operational-dataset> 20202021 3840
    ```

#### Commissioning the Ethernet device (ESP32-Ethernet-Kit)

```
$ out/debug/chip-tool pairing onnetwork 12345 20202021
```

Note: In order to commission an ethernet device, from all-clusters-app enable
these config options: select `ESP32-Ethernet-Kit` under `Demo->Device Type` and
select `On-Network` rendezvous mode under `Demo->Rendezvous Mode`. Currently
default ethernet board supported is IP101, if you want to use other types of
ethernet board then you can extend the `ESPEthernetDriver` class in your
application and override the current implementation under
`ESPEthernetDriver::Init`

#### Commissioning Parameters

-   Node Id : 12345 (you can assign any node id)
-   Discriminator : 3840 (Test discriminator)
-   Setup Pin Code : 20202021 (Test setup pin code)

If you want to use different values for discriminator and setup pin code please
follow [Using ESP32 Factory Data Provider guide](factory_data.md)

## Flashing app using script

-   Follow these steps to use `${app_name}.flash.py`.

    -   First set IDF target, run set-target with one of the commands.

        ```
        $ idf.py set-target esp32
        $ idf.py set-target esp32c3
        $ idf.py set-target esp32c6
        ```

    -   Execute below sequence of commands

        ```
        $ export ESPPORT=/dev/tty.SLAB_USBtoUART
        $ idf.py build
        $ idf.py flashing_script
        $ python ${app_name}.flash.py
        ```
