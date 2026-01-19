# ESP32-CalDAV

CalDAV driver for the esp-idf.

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg?logo=opensourceinitiative)](https://www.gnu.org/licenses/gpl-3.0)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.1+-blue.svg)](https://github.com/espressif/esp-idf)
[![Documentation](https://img.shields.io/badge/User%20Guide-PDF-007ec6?longCache=true&style=flat&logo=asciidoctor&colorA=555555)](https://kampi.github.io/ESP32-CalDAV/)

## Table of Contents

- [ESP32-CalDAV](#esp32-caldav)
  - [Table of Contents](#table-of-contents)
  - [Requirements](#requirements)
  - [Installation](#installation)
    - [Using ESP-IDF Component Manager](#using-esp-idf-component-manager)
    - [Manual Installation](#manual-installation)
  - [Kconfig Options](#kconfig-options)
  - [Examples](#examples)
  - [License](#license)
  - [Maintainer](#maintainer)

## Requirements

- **ESP-IDF**: v5.1 or newer

## Installation

### Using ESP-IDF Component Manager

Add to your `main/idf_component.yml`:

```yaml
dependencies:
  esp32-webdav:
    git: https://github.com/Kampi/ESP32-WebDAV.git
```

### Manual Installation

- Clone into your project's `components` directory:

```bash
cd your_project/components
git clone https://github.com/Kampi/ESP32-WebDAV.git
```

## Kconfig Options

Configure via `idf.py menuconfig`:

```sh
Component config → ESP32-WebDAV
```

## Examples

See the [`examples/`](examples/) directory.

## License

This project is licensed under the **GNU General Public License v3.0**.

See [LICENSE](LICENSE) for full text.

## Maintainer

**Daniel Kampert**  
📧 [DanielKampert@kampis-elektroecke.de](mailto:DanielKampert@kampis-elektroecke.de)  
🌐 [www.kampis-elektroecke.de](https://www.kampis-elektroecke.de)

---

**Contributions Welcome!** Please open issues or pull requests on GitHub.
