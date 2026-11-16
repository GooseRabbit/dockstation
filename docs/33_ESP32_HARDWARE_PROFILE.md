# Предварительный аппаратный профиль ESP32 для DockStation

**Статус:** документированный кандидат; физически не подтверждён.
**Редакция:** 0.1.
**Дата:** 2026-11-16.
**Связанные решения:** DEC-062, DEC-064, DEC-065, DEC-066.

## 1. Назначение и правило безопасности

Документ собирает подтверждённые внешними документами сведения о плате ESP32, предоставленной владельцем по FCC ID `2A54N-ESP32`, и отделяет их от неизвестных параметров конкретного физического экземпляра.

Запись FCC и приложенный к ней user manual позволяют уточнить вероятный модуль и распиновку, но не заменяют осмотр/измерение платы владельца. Поэтому документ имеет статус **candidate**, а не «аппаратный профиль подтверждён».

До закрытия критериев §6 запрещено:
- указывать неподтверждённую Flash capacity в `sdkconfig`;
- создавать окончательную production partition table;
- включать Flash Encryption/eFuses или provisioning;
- задавать GPIO16/GPIO17 в `ControllerUartConfig`;
- устанавливать `hardware_profile_confirmed=true`;
- прошивать на физическую плату текущий CI image.

## 2. Что подтверждают документы

| Параметр | Наблюдение в источнике | Статус для DockStation |
|---|---|---|
| FCC ID | `2A54N-ESP32`, ESP32 Development Board, grantee Shenzhen HiLetgo E-Commerce Co., Ltd; FCC application от 2022-04-21 | Подтверждено для предоставленного FCC ID |
| Модуль на схеме/manual | В user manual, приложенном к той же FCC application, показан ESP-WROOM-32; datasheet Espressif описывает модуль и выводы U2RXD/U2TXD | Документированный кандидат; точное исполнение установленного модуля не прочитано с физической маркировки |
| Семейство SoC | ESP32 classic family implied by FCC/manual and ESP-WROOM-32 description | Семейство-кандидат; SoC stepping / package revision не подтверждены |
| Тип платы | Manual содержит 38-pin board pin definition с USB и кнопками EN/IO0 | Кандидат по документации; ревизия фактической PCB и непрерывность дорожек до header не измерены |
| SPI Flash | Официальный datasheet стандартного ESP32-WROOM-32 указывает интегрированную SPI Flash 4 MB | **Ожидается 4 MB при подтверждении точной модели установленного модуля; фактическая плата ещё не проверена через `flash_id`** |
| PSRAM | Нет подтверждения установленной PSRAM | **UNKNOWN; не считать наличие PSRAM** |
| UART2 | Manual указывает GPIO17 = TX2/U2TXD (board pin 28), GPIO16 = RX2/U2RXD (board pin 27) | Документированный pin-map candidate; физическая проверка не выполнена |
| UART0 | Manual указывает GPIO1 = TX0 (pin 35), GPIO3 = RX0 (pin 34) | Не использовать для штатного Repka—ESP32 канала; оставить сервисному USB-UART/console пути |
| Flash pins | Manual помечает GPIO6–GPIO11 как SPI Flash signal pins | Не назначать их прикладной периферии |
| Рабочий ESP-IDF target | `esp32` | Временный family-level build target; не является production board approval |

Источники:
- [FCC ID 2A54N-ESP32 — устройство и filing](https://fccid.io/2A54N-ESP32)
- [FCC User Manual 5836477](https://fccid.io/2A54N-ESP32/User-Manual/User-Manual-5836477) — pin definition в §1.5, последняя страница документа
- [Espressif ESP32-WROOM-32 datasheet](https://documentation.espressif.com/esp32-wroom-32_datasheet_en.html) — для стандартного модуля указывает ESP32-D0WDQ6 и 4 MB SPI Flash; это условный профиль, пока маркировка и физическая Flash не проверены
- [Repka Pi 5 GPIO](https://repka-pi.com/docs/109) и [Repka pinout configuration](https://repka-pi.ru/docs/108)

## 3. Кандидатный UART pin map

DEC-062 уже утвердил для Repka Pi 5 ↔ ESP32: проводной full-duplex UART point-to-point, логические уровни 3,3 В, 115200 baud, 8N1, без hardware/software flow control; TX/RX перекрещиваются, GND общая; USB не используется как штатный межконтроллерный канал.

Документированный кандидат ESP32 из FCC User Manual:

| Сигнал | ESP32 GPIO | Board pin из manual | Направление |
|---|---:|---:|---|
| UART2 TX2 | GPIO17 | 28 | ESP32 → Repka RX |
| UART2 RX2 | GPIO16 | 27 | Repka TX → ESP32 |

Порт ESP-IDF: `UART_NUM_2`. В текущем `ControllerUartConfig` он остаётся логическим default candidate; `tx_gpio` и `rx_gpio` намеренно равны `GPIO_NUM_NC`. Это fail-closed поведение нужно сохранить до физической проверки.

UART0 GPIO1/TX0 и GPIO3/RX0 не назначать для ControllerLink: они относятся к USB serial/console programming path. GPIO6–GPIO11 по документации связаны с SPI Flash; не использовать их для внешней периферии. Учитывать также ограничения boot-strapping GPIO из datasheet.

Перед соединением необходимо проверить:
1. что на установленной плате присутствуют именно контакты 27/28 и они соответствуют manual;
2. что контакты действительно идут на GPIO16/GPIO17, нет другой ревизии/переназначения;
3. уровни обоих UART TX равны 3,3 В и ни одна сторона не выдаёт 5 В;
4. общую землю и отсутствие ошибочного соединения выход-выход;
5. Repka UART0 не используется одновременно консолью ОС после его выделения службе.

## 4. Flash и partition table — не определены

### 4.1. SPI Flash

Официальный datasheet Espressif для стандартного ESP32-WROOM-32 указывает интегрированную SPI Flash объёмом **4 MB**. Поскольку связанный FCC manual указывает именно ESP-WROOM-32, 4 MB — наиболее вероятный профиль платы, но он остаётся условным до подтверждения физической маркировки модуля и read-only `flash_id`. Нельзя переносить это значение на иной suffix/модуль или на другую ревизию платы без проверки.

Текущий GitHub Actions ESP-IDF v5.5.6 build наследует SDK default **2 MB** и формирует соответствующие flash arguments. Это настройка CI toolchain, а не результат чтения физической Flash; она не соответствует условному ожидаемому профилю 4 MB и не должна использоваться для прошивки. Сборка не выполняет flash operation.

Для определения на реальной плате использовать только read-only идентификацию, например `python -m esptool --chip esp32 --port COMx flash_id` (в Windows заменить `COMx` на фактический порт; на Linux — на `/dev/...`). Зафиксировать полный вывод, manufacturer/device ID и обнаруженный размер. Перед командой не выполнять `erase_flash`, `write_flash`, `idf.py flash` или настройку eFuse.

### 4.2. Production partition table

Окончательная таблица разделов не выбирается, пока Flash capacity не подтверждена. Production layout должен отдельно учесть bootloader, partition table, запас по размеру app image, NVS data partition, отдельную `nvs_keys` partition нужного размера и требования Flash Encryption + NVS Encryption. OTA/rollback включать только после выбора стратегии обновления. Нужны также резерв свободного пространства и возможность будущего расширения firmware.

Текущая default ESP-IDF partition table — build scaffold. В ней нет production-provisioned `nvs_keys`; read-only `ControllerPskNvsCredentialStore` не сможет загрузить реальные credentials до безопасного provisioning. Нельзя включать eFuse security или генерировать NVS keys во время обычного старта.

## 5. Текущее состояние реализации

- ESP-IDF v5.5.6 / target `esp32` кросс-собирает прошивку и TLS/ControllerLink components.
- `ControllerUartConfig.port` по умолчанию `UART_NUM_2`; TX/RX GPIO остаются `GPIO_NUM_NC`.
- Baud rate по умолчанию 115200, receive buffer 4096, 8N1/no flow control.
- `app_main` диагностический и default-off: не открывает NVS, UART или TLS, не вызывает runtime start.
- `hardware_profile_confirmed` остаётся `false`.
- CI build только компилирует и линкует; не прошивает физическую плату.

## 6. Критерии подтверждения аппаратного профиля

Профиль можно перевести из candidate в verified только после закрытия всех пунктов:
1. Маркировка модуля и ревизия платы на фактическом устройстве согласуются с FCC/manual candidate (либо составлен отдельный профиль для обнаруженного отличия).
2. Read-only `flash_id` на фактической плате определяет и записывает SPI Flash manufacturer/device/size.
3. Наличие или отсутствие PSRAM установлено; неизвестное значение не трактуется как наличие.
4. 38-pin header / контакты 27 и 28 и UART2 mapping GPIO17/TX, GPIO16/RX сверены с физической платой и электрически проверены.
5. Уровни логики 3,3 В, GND reference и cross-wiring проверены без подключения силовых/моторных нагрузок.
6. Выбрана partition table, размер которой помещается в физическую Flash и содержит требуемые защищённые NVS области; её app partition проходит build size checks.
7. Изменения профиля просмотрены и явно утверждены до включения `hardware_profile_confirmed`, установки GPIO или flash/provisioning операций.

До выполнения этих критериев аппаратный профиль остаётся **CANDIDATE / NOT_CONFIGURED**.

## 7. Следующий безопасный шаг

Когда плата доступна для подключения к ПК, сначала выполнить только:
- посмотреть маркировку модуля и PCB;
- получить фактический COM-порт/serial device;
- выполнить read-only `flash_id`;
- сверить результат с этой матрицей.

До этого не требуется менять прошивку, flash-size config, partition CSV, eFuse или GPIO defaults.
