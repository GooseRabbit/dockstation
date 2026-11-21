# Аппаратный профиль ESP32 для DockStation

**Статус:** raw UART2 ESP32 → обычный Linux физически проверен; программный путь DHT11/DHT22 через USB/UART0 реализован, но USB → Repka ещё не принят; secure ControllerLink не принят.
**Редакция:** 0.8.
**Дата:** 2026-11-21.
**Связанные решения:** DEC-062, DEC-064, DEC-065, DEC-066.

## 1. Назначение и правило безопасности

Документ фиксирует аппаратные сведения и результаты физической проверки прототипа. Владелец подтвердил работу физической ESP32 в связке с обычным Linux-хостом. Проверка не выполнялась на Repka Pi; она не подтверждает готовность Repka UART, защищённого ControllerLink/TLS или production provisioning.

До отдельной приёмки Repka и production security запрещено:
- считать текущую проверку обычного Linux-хоста подтверждением Repka Pi;
- включать `hardware_profile_confirmed=true` для защищённого ControllerLink без завершённой проверки его полного профиля;
- включать Flash Encryption/eFuses или выполнять production provisioning;
- считать прототипную partition table окончательной production-разметкой.

4 MB Flash уже подтверждены физически и явно задаются в `firmware/esp32/sdkconfig.defaults`.

## 2. Что подтверждают документы

| Параметр | Наблюдение в источнике | Статус для DockStation |
|---|---|---|
| FCC ID | `2A54N-ESP32`, ESP32 Development Board, grantee Shenzhen HiLetgo E-Commerce Co., Ltd; FCC application от 2022-04-21 | Подтверждено для предоставленного FCC ID |
| Модуль на схеме/manual | В user manual, приложенном к той же FCC application, показан ESP-WROOM-32; datasheet Espressif описывает модуль и выводы U2RXD/U2TXD | Документированный кандидат; точное исполнение установленного модуля не прочитано с физической маркировки |
| Семейство SoC | ESP32 classic family implied by FCC/manual and ESP-WROOM-32 description | Семейство-кандидат; SoC stepping / package revision не подтверждены |
| Тип платы | Manual содержит 38-pin board pin definition с USB и кнопками EN/IO0 | Кандидат по документации; ревизия фактической PCB и непрерывность дорожек до header не измерены |
| SPI Flash | `esptool flash-id` на физической плате ESP32-D0WD-V3 revision 3.1 | **Физически подтверждено: 4 MB; CI конфигурация также фиксирует 4 MB** |
| PSRAM | Нет подтверждения установленной PSRAM | **UNKNOWN; не считать наличие PSRAM** |
| UART2 prototype | GPIO17 = TX, GPIO16 = RX, 115200 8N1 | **Физически проверен ESP32 → обычный Linux host; Repka не проверялась** |
| UART0 | Manual указывает GPIO1 = TX0 (pin 35), GPIO3 = RX0 (pin 34) | Основной прототипный путь DHT11/DHT22 через onboard USB-UART bridge и console; не использовать для secure ControllerLink из-за совместного bootloader/logging пути |
| Flash pins | Manual помечает GPIO6–GPIO11 как SPI Flash signal pins | Не назначать их прикладной периферии |
| Рабочий ESP-IDF target | `esp32` | Временный family-level build target; не является production board approval |

Источники:
- [FCC ID 2A54N-ESP32 — устройство и filing](https://fccid.io/2A54N-ESP32)
- [FCC User Manual 5836477](https://fccid.io/2A54N-ESP32/User-Manual/User-Manual-5836477) — pin definition в §1.5, последняя страница документа
- [Espressif ESP32-WROOM-32 datasheet](https://documentation.espressif.com/esp32-wroom-32_datasheet_en.html) — справочные сведения о модуле; фактические 4 MB дополнительно подтверждены read-only `esptool flash-id` на ESP32-D0WD-V3 revision 3.1.
- Физическую проверку ESP32 с обычным Linux-хостом подтвердил владелец; Repka в эту проверку не входила.
- [Repka Pi 5 GPIO](https://repka-pi.com/docs/109) и [Repka pinout configuration](https://repka-pi.ru/docs/108)

## 3. UART pin map и границы физической проверки

DEC-062 описывает целевой проводной full-duplex UART point-to-point для Repka Pi 5 ↔ ESP32: уровни 3,3 В, 115200 baud, 8N1, без flow control, TX/RX перекрещиваются, GND общая.

Отдельный прототипный телеметрический канал ESP32 → обычный Linux host физически проверен владельцем. Его конфигурация:

| Сигнал | ESP32 GPIO | Board pin из manual | Направление |
|---|---:|---:|---|
| UART2 TX2 | GPIO17 | 28 | ESP32 → Repka RX |
| UART2 RX2 | GPIO16 | 27 | Repka TX → ESP32 |

Для прототипной телеметрии DHT11/DHT22 прошивка передаёт один и тот же кадр двумя способами:
- UART0 через onboard USB-UART bridge: кадр `DS1,...` выводится штатным ESP-IDF logger в строке вида `I (123) DockStationESP32: DS1,...`; Linux Gateway выделяет кадр из префикса. Это основной путь для подключения ESP32 обычным USB-кабелем к Linux/Repka.
- UART2 (`UART_NUM_2`), GPIO17/TX и GPIO16/RX, 115200 8N1: исходный raw-кадр `DS1,...` остаётся доступен для прямого последовательного подключения к Linux-хосту.

Оба прототипных варианта открыты, не имеют аутентификации/шифрования и не поддерживают команды. Это временный транспорт из `docs/35_PROTOTYPE_UART_DHT22.md`; он не является secure ControllerLink. Secure ControllerLink остаётся default-off (`hardware_profile_confirmed=false`, его UART GPIO не заданы). UART0 не назначать защищённому ControllerLink: он разделяется с USB bootloader/console. Не запускать raw prototype UART2 и secure ControllerLink одновременно на одном UART.

GPIO6–GPIO11 по документации связаны с SPI Flash; не использовать их для внешней периферии. Учитывать также ограничения boot-strapping GPIO из datasheet.

Физическая проверка raw UART2 с обычным Linux-хостом подтверждена владельцем. Программный USB/UART0 путь собирается через ESP-IDF и имеет host-проверку парсера на Linux, но его физическое подключение к Repka ещё не принято.

## 4. Flash и partition table

### 4.1. SPI Flash

Физическая плата ESP32-D0WD-V3 revision 3.1 прошла read-only `esptool flash-id`; обнаружен объём **4 MB SPI Flash**. В `firmware/esp32/sdkconfig.defaults` установлен `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y`. ESP-IDF v5.5.6 CI дополнительно проверяет сгенерированные `flasher_args.json` и `flash_args`, чтобы в публикуемый образ не попала конфигурация 2 MB. Workflow не прошивает физическую плату.

Read-only `flash_id` уже выполнен на используемой физической плате и подтвердил 4 MB. Повторять проверку требуется только при замене платы/модуля или при сомнении в аппаратной конфигурации. Команда для такой повторной идентификации: `python -m esptool --chip esp32 --port COMx flash_id` (в Windows заменить `COMx` на фактический порт; на Linux — на `/dev/...`). Она не стирает Flash и не меняет eFuses.

### 4.2. Production partition table

Текущая ESP-IDF partition table остаётся прототипной. Production layout должен отдельно учесть bootloader, partition table, app slots, NVS data, отдельную `nvs_keys` partition, Flash Encryption + NVS Encryption, OTA/rollback и резерв расширения. Публикуемый бинарник использует именно текущую таблицу из ESP-IDF build; он не включает provisioning credentials и не активирует security eFuses.

Текущая default ESP-IDF partition table — build scaffold. В ней нет production-provisioned `nvs_keys`; read-only `ControllerPskNvsCredentialStore` не сможет загрузить реальные credentials до безопасного provisioning. Нельзя включать eFuse security или генерировать NVS keys во время обычного старта.

## 5. Текущее состояние реализации

- ESP-IDF v5.5.6 / target `esp32` кросс-собирает прошивку и TLS/ControllerLink components.
- Для secure ControllerLink: `ControllerUartConfig.port` по умолчанию `UART_NUM_2`, TX/RX GPIO остаются `GPIO_NUM_NC`; secure path не запускается.
- Для отдельной прототипной телеметрии: основной путь UART0 через onboard USB-UART bridge, 115200, кадр DS1 внутри ESP-IDF log line; дополнительный raw UART2 — GPIO17/TX, GPIO16/RX, 115200 8N1. Физически проверен пока только raw UART2 с обычным Linux-хостом.
- Baud rate по умолчанию 115200, receive buffer 4096, 8N1/no flow control.
- `app_main` генерирует per-boot ID, затем проверяет `hardware_profile_confirmed` и явную валидную UART-конфигурацию **до обращения к NVS**.
- Secure ControllerLink имеет `hardware_profile_confirmed=false`; его NVS/TLS startup не выполняется. Отдельный prototype UART запускается независимо и передаёт телеметрию на обычный Linux host.
- Secure ControllerLink сможет читать заранее provisioned protected NVS credentials и stable controller IDs только после отдельной приёмки Repka/ESP32 профиля и явной UART-конфигурации; provisioning writer в штатном старте отсутствует.
- FreeRTOS polling/lifecycle task `controller_link_polling_task` уже реализована в `app_main.cpp`: она создаётся после прохождения startup readiness checks, с выделенным стеком и приоритетом, и обслуживает runtime с периодом опроса 10 мс.
- Secure ControllerLink не запускается, поскольку `hardware_profile_confirmed=false`; его protected NVS и TLS transport не открываются. Runtime task независимо опрашивает DHT11/DHT22 GPIO25/GPIO26. Кадры передаются через UART0/USB как ESP-IDF log lines и, при успешной инициализации, дублируются raw через UART2 GPIO17/GPIO16.
- После подтверждения аппаратного профиля, явной UART-конфигурации и наличия заранее provisioned credentials/ID task запускает runtime, выполняет polling и повторные попытки подключения с backoff. При запросе снимка передаются восемь логических измерений состояния станции, а также четыре климатических канала двух DHT11/DHT22 (температура и влажность каждого датчика). DHT11/DHT22 GPIO reader сначала использует open-drain start pulse 1,1 мс; при полном отсутствии ответа повторяет запрос с импульсом не менее 18 мс для DHT11, затем применяет соответствующий checksum/range decoder; каждый датчик имеет отдельный sampling scheduler с интервалом не менее двух секунд. Для двух датчиков назначены GPIO25 (датчик 1 DATA) и GPIO26 (датчик 2 DATA); модель каждого определяется по ответному кадру независимо. Каналы опрашиваются независимо и передают только валидные свежие измерения; до успешного чтения сообщают `UNAVAILABLE`, а не фиктивные значения. Физическая проверка ESP32 с обычным Linux-хостом не является проверкой Repka. Для настроенного, но ещё не прочитанного канала передаётся `UNAVAILABLE`; успешное измерение получает время фактического захвата, а старше пяти секунд помечается `STALE`. Недоступные физические входы состояния станции передаются как `UNKNOWN` с качеством `Unknown`; неподтверждённая крышка — `NOT_CONFIGURED`. Получение feedback механизмов и GPIO-концевиков ещё не реализовано.
- UART event queue обрабатывается ограниченными порциями до 8 событий за один runtime poll. Если после порции очередь не пуста, runtime откладывает TLS/ControllerLink poll до следующего цикла; оставшиеся UART-события сначала должны быть проверены на overflow/framing/parity faults.
- На Repka (Linux/OpenSSL) первоначальное TLS-рукопожатие ограничено отдельным монотонным дедлайном 10 секунд. При истечении срока transport manager закрывает TLS и UART и возвращает отдельный `TlsHandshakeTimeout`; ControllerLink service классифицирует его как потерю транспорта и запускает обычный reconnect backoff. На ESP32 TLS/HELLO handshake ограничен тремя секундами. Оба тайм-аута относятся к программной политике и ещё требуют проверки на физической связке.
- ESP32 CI после сборки выполняет `idf.py size` и `idf.py size-components`; сводка размера ELF/секции и разбивка по компонентам доступны в журнале workflow. Это статические показатели сборки, не runtime RAM.
- При разрешённом запуске ControllerLink firmware task пишет при старте и далее раз в 60 секунд: свободную internal 8-bit-capable heap, largest contiguous internal block, minimum-ever free internal heap и stack high-water mark текущей задачи. Это диагностические измерения будущего запуска на плате; текущий default-off образ их не выдаёт, поскольку задача не создаётся.
- CI собирает ELF/BIN, проверяет 4 MB flash arguments, формирует merged flash image и архивирует исходные бинарники с контрольными суммами. CI не прошивает и не запускает физическую плату.

## 6. Что уже подтверждено и что остаётся

Подтверждено:
1. Физическая ESP32-D0WD-V3 revision 3.1 и SPI Flash 4 MB по `esptool flash-id`.
2. Сборочный профиль ESP-IDF v5.5.6 / `esp32` с явным выбором 4 MB.
3. Физическая работа ESP32 prototype firmware с обычным Linux-хостом (по подтверждению владельца).
4. Прототипный raw UART2: GPIO17/TX, GPIO16/RX, 115200 8N1.
5. Программный USB/UART0 путь: кадр DS1 передаётся через ESP-IDF logger и распознаётся Linux Gateway; физическая приёмка USB → Repka ещё не выполнена.

Не подтверждено:
1. Физическое соединение USB/UART0 с Repka Pi и приём телеметрии на ней.
2. Запуск secure ControllerLink/TLS на физической связке и provisioning защищённых NVS credentials.
3. Наличие PSRAM (остаётся UNKNOWN).
4. Production partition layout, Flash Encryption, Secure Boot и OTA strategy.
5. Аппаратная обратная связь концевиков, приводов, safety-stop и физическое управление механизмами.

## 7. Следующий шаг

Следующая аппаратная приёмка — ESP32 ↔ Repka: отдельно подтвердить уровни, перекрёстный UART, Linux UART device/service ownership, затем защищённый ControllerLink и reconnect. До этой проверки обычный Linux host остаётся единственной подтверждённой физической хост-платформой.


## 8. Логика команд и телеметрия состояния ESP32

Текущий программный слой разделяет проверку разрешения команды и физическое
исполнение. Для обычных команд движения ESP32 требует явного неактивного
safety-stop, а также явного feedback для platform/cleaning/drive. Отсутствующий
feedback трактуется как UNKNOWN и запрещает обычную команду. Команды крышки
дополнительно требуют подтверждённого debounce endpoint; запуск очистки требует
известной свободной рабочей зоны. StopMotion и StopCleaning остаются доступными
даже при неизвестных входах или активном safety-stop.

После решения ALLOW команда становится pending. Пока pending не подтверждён
физическим исполнительным слоем, новая обычная команда отклоняется и не
перезаписывает исходный pending-запрос; команда остановки может его заменить.
Разрыв соединения очищает pending-состояние. В будущем pending должен очищаться
только после достоверного аппаратного feedback. Сейчас исполнительного слоя и
физического подтверждения нет; ALLOW/ACCEPTED не означает, что механизм начал
или завершил движение.

На `REQUEST_SNAPSHOT` ESP32 передаёт логические измерения состояния станции и
четыре климатических канала двух DHT11/DHT22. В прошивке назначены GPIO25 и GPIO26;
до первого успешного чтения настроенный канал имеет явный статус
`UNAVAILABLE`. Валидные измерения сопровождаются временем захвата,
а после пяти секунд без обновления получают качество `Stale`. Это программная
реализация acquisition/telemetry, но не физическая приёмка датчиков и проводки.
Недоступные входы состояния станции имеют значение `UNKNOWN` и качество
`Unknown`, а не подменяются значением `false`. Подключение концевиков,
датчиков движения и safety-stop остаётся отдельной задачей.


## 9. Свежесть safety-входов

Программная проверка возраста входов описана в
`docs/36_ESP32_INPUT_FRESHNESS.md`. В аппаратном профиле должны быть явно
заданы и подтверждены максимальные интервалы свежести для положения крышки,
motion feedback, safety-stop и workspace. Сейчас все интервалы по умолчанию
равны нулю, то есть входы не считаются пригодными для разрешения обычных
команд. Это намеренно: конкретные миллисекундные значения нельзя назначать
до измерения реального периода опроса, debounce, jitter и времени реакции
на выбранной плате и схеме.

Перевод hardware profile в VERIFIED требует:
- зафиксировать период получения каждого safety-входа;
- обосновать max-age и подтвердить его измерениями при штатной нагрузке,
  TLS обмене и reconnect;
- проверить fail-closed поведение при пропусках, обрывах и устаревании;
- подтвердить, что timestamp создаётся при фактическом чтении, а не при
  пересылке ранее сохранённого значения.

Программная freshness policy сама по себе не является подтверждением
функциональной безопасности и не заменяет независимый аппаратный emergency-stop.


## Временный прототипный телеметрический UART

На этапе первичной проверки DHT11/DHT22 используется отдельный открытый UART2-поток:
GPIO17 TX, GPIO16 RX, 115200 8N1. Он не активирует защищённый ControllerLink
и не передаёт команды. Полное описание кадра, Linux-настройки и ограничений
находится в docs/35_PROTOTYPE_UART_DHT22.md. Это стендовый транспорт без защиты,
не предназначенный для эксплуатации.
