.. zephyr:board:: esp32s3_lcd_1_47b

Overview
********

The `ESP32-S3-LCD-1.47B`_ is a compact development board designed by Waveshare.
It is based on the bare Espressif ESP32-S3R8 chip and features a 1.47 inch
172x320 color LCD, a QMI8658 6-axis IMU, a microSD card slot, an addressable
RGB LED and a USB Type-C connector wired to the SoC's native USB Serial/JTAG
interface (there is no USB-UART bridge chip on the board).

Hardware
********

- ESP32-S3R8 microcontroller with 8MB of in-package octal PSRAM
- 16MB of on-board QSPI flash
- USB Type-C connector (native USB Serial/JTAG; also usable by the USB device
  controller)
- BOOT button (GPIO0) and RESET button
- 172x320 1.47 inch color IPS LCD (ST7789T-class controller, SPI, write-only),
  PWM-dimmable backlight
- QST QMI8658 6-axis IMU (I2C address 0x6b, data-ready on INT2/GPIO12; INT1 is
  routed to GPIO13 but unused by the driver)
- microSD card slot (SDMMC, 4-bit bus, external pull-ups on all lines)
- WS2812B addressable RGB LED (GPIO38)
- Lithium battery charger (ETA6098) with battery voltage sensing on GPIO1
  through a 200k/100k divider
- TXD/RXD (UART0, GPIO43/GPIO44) solder pads
- GPIO2-GPIO11 free on the rear castellated header

See also the `ESP32-S3-LCD-1.47B wiki`_ and `schematic`_.

.. include:: ../../../espressif/common/soc-esp32s3-features.rst
   :start-after: espressif-soc-esp32s3-features

Supported Features
==================

.. zephyr:board-supported-hw::

Display backlight
=================

The LCD backlight is not enabled automatically: GPIO46 drives the gate of a
low-side N-FET with a pull-down, so the panel stays dark until the ``pwm-led0``
device (LEDC channel 0) is driven. Applications using the display should set a
backlight duty cycle through the LED or PWM API.

System requirements
*******************

.. include:: ../../../espressif/common/system-requirements.rst
   :start-after: espressif-system-requirements

Programming and Debugging
*************************

.. zephyr:board-supported-runners::

.. include:: ../../../espressif/common/building-flashing.rst
   :start-after: espressif-building-flashing

Debugging
=========

.. include:: ../../../espressif/common/openocd-debugging.rst
   :start-after: espressif-openocd-debugging

Note that GPIO39-GPIO42 (the JTAG MTCK/MTDO/MTDI/MTMS pins) are wired to the
LCD on this board, so debugging is only possible over the built-in USB
Serial/JTAG interface, not an external JTAG probe.

References
**********

.. target-notes::

.. _ESP32-S3-LCD-1.47B:
    https://www.waveshare.com/esp32-s3-lcd-1.47b.htm

.. _ESP32-S3-LCD-1.47B wiki:
    https://www.waveshare.com/wiki/ESP32-S3-LCD-1.47B

.. _schematic:
    https://files.waveshare.com/wiki/ESP32-S3-LCD-1.47B/ESP32-S3-LCD-1.47B_schematic_diagram.pdf
