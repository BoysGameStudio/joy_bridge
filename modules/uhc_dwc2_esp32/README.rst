uhc_dwc2_esp32 — ESP32-S3 DWC2 USB host controller driver (Zephyr module)
========================================================================

A native USB host-controller (UHC) driver for the ESP32-S3 on-chip USB-OTG
(DesignWare DWC2) core, driven in host mode. Upstream Zephyr ships no DWC2 *host*
driver for this core (``drivers/usb/uhc`` has only max3421e / NXP mcux / virtual),
so this fills the gap — any ESP32-S3 Zephyr project that wants USB host can use it.

Implements the Zephyr UHC API (``struct uhc_api``) over the vendored Espressif
host USB HAL (``usb_dwc_hal``) + USB PHY, modelled on ``uhc_max3421e``: a worker
thread woken by the OTG ISR decodes HAL port/channel events and drives the USB
host stack. Full-Speed only; single-active transfer engine.

Using it
--------

1. Add the module before ``find_package(Zephyr ...)`` in the app ``CMakeLists.txt``::

       list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/modules/uhc_dwc2_esp32)

   (or copy / git-submodule it elsewhere and point the path at it; or add a west
   manifest entry).

2. Enable it::

       CONFIG_UHC_DRIVER=y
       CONFIG_UHC_DWC2=y

3. Enable the OTG node in host mode in your board overlay::

       &usb_otg {
           status = "okay";
       };

   Its compatible is ``espressif,esp32-usb-otg`` (includes ``snps,dwc2.yaml`` ->
   reg/interrupts/clocks). Keep the UDC (device-mode) controller off so nothing
   else binds the OTG core; the driver brings the PHY up in host mode.

4. Bind the USB host stack to it via devicetree::

       USBH_CONTROLLER_DEFINE(my_uhs, DEVICE_DT_GET(DT_NODELABEL(usb_otg)));

Hardware notes
--------------

- **VBUS:** the internal PHY host mode does NOT source VBUS, and the DevKitC
  native USB-C VBUS is Schottky-isolated from 5 V — power the device externally /
  direct-attach it. Zephyr USBH has no hub support, so the device must be on the
  root port.
- ``CONFIG_UHC_DWC2_DIAG_LATENCY`` adds an interrupt-IN re-arm latency probe
  (off by default).

The hard-won transfer-engine fixes (connect debounce, host-mode init delay,
synchronous bus reset, periodic frame-list scheduling, reconfigure-on-target-change,
silent control-transfer retry) live inside ``src/uhc_dwc2_esp32.c`` and are
documented in the joy_bridge ``AGENTS.md`` "USB host" section. **Do not regress.**
