# Créditos / Credits

ds5haptics se apoya en la investigación y el trabajo de otras personas. Gracias a todas.
ds5haptics builds on the research and work of others. Thank you all.

| Proyecto / Project | Qué se usó / What was used | Licencia / License |
|---|---|---|
| [SAxense](https://github.com/egormanga/SAxense) — Egor Vorontsov (Sdore) · [apps.sdore.me/SAxense](https://apps.sdore.me/SAxense) | Formato del reporte de háptica por Bluetooth (0x32). `tests/test_packet.cpp` reproduce su estructura y su CRC para comparar. *Bluetooth haptics report format; the test reproduces its layout and CRC for comparison.* | MPL-2.0 |
| [DS5Dongle](https://github.com/awalol/DS5Dongle) — awalol | Formato del reporte de parlante + háptica (0x36, Opus) y la idea del DualSense USB virtual. *Speaker + haptics report format and the virtual wired DualSense idea.* | MIT |
| [nondebug/dualsense](https://github.com/nondebug/dualsense) | Descriptores USB y HID del DualSense. *DualSense USB/HID descriptors.* | — |
| Driver `hid-playstation` de Linux / Linux `hid-playstation` driver | Estructura de los reportes de entrada y salida (como referencia, sin copiar código). *Input/output report layout (reference only, no code copied).* | GPL-2.0 |
| [libopus](https://opus-codec.org/) — Xiph.Org | Compresión del audio del parlante. Se descarga y compila al construir; el `.exe` la incluye. *Speaker audio codec, built in.* | BSD-3-Clause |
| [usbip-win2](https://github.com/vadimgrn/usbip-win2) — vadimgrn | Cliente USB/IP con drivers firmados (se instala aparte). *Signed USB/IP client, installed separately.* | BSD-2-Clause (desde / since 0.9.7.0) |
| [HidHide](https://github.com/nefarius/HidHide) — Nefarius Software Solutions | Oculta el mando real a los juegos (se instala aparte). *Hides the real controller from games, installed separately.* | Ver su repositorio / see its repository |
| [Linux kernel USB/IP docs](https://docs.kernel.org/usb/usbip_protocol.html) | Protocolo USB/IP. *USB/IP protocol.* | — |

## Aviso / Disclaimer

ds5haptics es un proyecto independiente, sin relación con Sony Interactive Entertainment ni con
Microsoft. PlayStation, DualSense y DualShock son marcas de Sony Interactive Entertainment; Xbox es
marca de Microsoft. Se usan solo para describir con qué es compatible el programa.

*ds5haptics is an independent project, not affiliated with or endorsed by Sony Interactive
Entertainment or Microsoft. PlayStation, DualSense and DualShock are trademarks of Sony Interactive
Entertainment; Xbox is a trademark of Microsoft. They are used only to describe compatibility.*
