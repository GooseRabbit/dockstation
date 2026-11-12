# Полный план реализации программного комплекса DockStation на Repka Pi 5

**Статус:** мастер-план реализации; последовательность разработки согласована, открытые платформенные, сетевые, серверные и аппаратные детали требуют отдельной проверки и решения.  
**Редакция:** 1.17.  
**Дата:** 2026-11-12.  
**Вычислительная платформа:** Repka Pi 5.  
**Предполагаемая ОС:** официальная Repka OS; образ и версия проверяются перед развёртыванием.

## 1. Назначение и принцип разработки

Задача — довести проект от первого сетевого подключения Repka к Windows до рабочего программного комплекса станции: Linux-служба, графическое приложение оператора, постоянное хранение, ESP32-мост, телеметрия, диагностика, безопасность, механизмы, зарядка, получение материалов БАС и серверный обмен.

Это план будущей реализации, а не утверждение, что функции уже существуют или что все технические решения выбраны.

Рабочая единица разработки: **этап → файл → функция → отдельная проверка → интеграция**. Одна задача закрывает одну функцию или минимальную связную группу. Сначала контракт, затем реализация, затем проверка ошибок и граничных условий. Без больших переписываний.

## 2. Документация, которой подчиняется план

Основные источники: docs/01_PROJECT_PASSPORT.md, 02_REQUIREMENTS.md, 03_SYSTEM_ARCHITECTURE.md, 04_OPERATIONAL_ALGORITHMS.md, 05_SOFTWARE_ARCHITECTURE.md, 06_INTERFACES_AND_DATA_CONTRACTS.md, 07_DATA_TRANSFER_AND_SECURITY.md, 08_WEATHER_SUBSYSTEM.md, 09_SAFETY_AND_FAILURES.md, 10_HARDWARE_REGISTER.md, 11_TESTING_AND_ACCEPTANCE.md, 12_DECISIONS_AND_OPEN_QUESTIONS.md, 18_MODULE_REGISTER.md, 23_OPERATOR_APPLICATION.md, 24_STATION_IDENTITY_AND_CRYPTOGRAPHIC_SECURITY.md и 26_CODE_FUNCTION_CATALOG.md.

Сетевой первый этап подробно расписан в docs/30_REPKA_NETWORK_IMPLEMENTATION_PLAN.md. Точный каталог функций и их контрактов — в docs/26_CODE_FUNCTION_CATALOG.md.

Если этот план расходится с согласованной спецификацией или решением владельца, действует профильный документ. Предложение из плана не становится обязательным требованием автоматически.

## 3. Архитектура продукта

| Узел | Ответственность |
|---|---|
| Repka Pi 5 | Linux-служба, сеть, файлы, сервер, телеметрия, журналы, камеры и вычислительные задачи |
| ESP32 | Непосредственный опрос подключённых датчиков, управление исполнительной электроникой и локальные блокировки |
| Windows Operator Application | Мониторинг, диагностика, отображение, запросы пользователя и управление разрешёнными операциями |
| Удалённый сервер | Пользователи, права, реестр станций, задания и удалённый обмен; отдельный сервисный контур |

Windows-клиент не обращается к GPIO, драйверам и силовым выходам. Repka передаёт типизированные запросы локальному контроллеру, но не отменяет его блокировки. Потеря Интернета, Windows-клиента или процесса Repka не должна отключать локальные аппаратные защиты ESP32.

### 3.1. Сетевые каналы

Полное приложение должно поддерживать три маршрута из docs/23: SERVER, WIFI_LOCAL и WIRED_LOCAL. Все три используют одну модель данных и команд.

Первый прототип: Ethernet + SSH-туннель + read-only API, привязанный к 127.0.0.1. Это временный маршрут разработки. Перед прямым LAN/VPN-доступом обязательны защищённый транспорт, проверка сертификатов, аутентификация и авторизация.

### 3.2. Технологические позиции

| Вопрос | Рабочая позиция | Статус |
|---|---|---|
| Плата | Repka Pi 5 | Выбрана для текущего этапа |
| ОС | Repka OS | Основной кандидат; версию проверить на плате |
| Служба Repka | C++17 с переиспользованием текущего ядра | Предварительная позиция |
| Сборка | CMake для отдельной Linux-цели | СОГЛАСОВАНО владельцем 2026-11-12 |
| Windows-клиент | Python + PySide6 + Qt Quick/QML | Рекомендация docs/23; подтвердить прототипом |
| API первого прототипа | REST/HTTP + JSON через SSH-туннель | Предложение |
| Рабочий API | Защищённый API с проверкой идентичности и полномочий | Обязательно; реализация требует решений docs/24 |
| Локальная БД | Абстрактный интерфейс; SQLite — кандидат | Требует оценки |
| Repka—ESP32 | Абстрактный транспорт; USB/UART — кандидаты | Не выбраны |
| Обновление данных | Периодический опрос сначала; SSE/WebSocket при необходимости | Предварительный порядок |
| Сервер | Отдельный backend/API | Технология не выбрана |

Не утверждать заранее HTTP-библиотеку, СУБД, протокол ESP32, формат всех сообщений или криптографические алгоритмы.

## 4. Виртуальная лаборатория

До получения физической Repka программная логика проверяется на двух виртуальных машинах и с помощью небольших тестовых адаптеров:

- **Linux VM:** Gateway, API, прикладные службы и тестовые адаптеры отсутствующих внешних устройств.
- **Windows VM или основной Windows:** графическое приложение оператора.
- **Изолированная виртуальная сеть:** соединение Windows ↔ Linux без публикации сервисов в Интернет.
- **Тестовые адаптеры:** проверяют контракты взаимодействия с отсутствующими устройствами и могут подтвердить только приём тестового запроса.

Отдельный полнофункциональный симулятор станции с моделью датчиков, ESP32, механики, аккумуляторов и погоды в текущий план не входит. Если физического устройства нет, тестовый адаптер может вернуть явно помеченный результат приёма запроса (например, TEST_ADAPTER_ACCEPTED), но не должен создавать фиктивные показания датчиков, положение механизмов или подтверждение физического выполнения. Все такие ответы должны явно показывать тестовый источник и отсутствие физического исполнения.

Виртуалки позволяют проверить SSH/API, JSON-контракты, GUI, потерю и восстановление сети, тайм-ауты, сохранение/восстановление и программную реакцию на ошибки. Они не подтверждают GPIO/UART/CSI, электрическую совместимость, реальное движение механизмов, зарядку, климатическую пригодность или тепловой режим. Linux VM x86_64 не заменяет сборку ARM64 и проверку на Repka.

Существующие tests/ и tools/ с приложением проверки компилируемости не переписываются и не меняют назначение. Новые проверки планируются отдельно: verification/repka_gateway/, verification/api_contract/, verification/operator_app/, verification/integration/, verification/scenarios/ и verification/hardware/.

## 5. Этапы реализации

## Этап 0. Подготовка платформы и сети

**Файлы:** repka/README.md, repka/check_network.sh, deployment/dev/README.md, deployment/dev/linux_vm/README.md, deployment/dev/windows_vm/README.md, deployment/dev/network/README.md.

**Функции check_network.sh:**
- require_command() — наличие системной команды.
- read_os_release() — имя и версия ОС.
- check_architecture() — архитектура и разрядность.
- check_ethernet_link() — наличие интерфейса и физический линк.
- read_ip_configuration() — назначенные IP-адреса.
- check_default_route() — маршрут по умолчанию.
- check_dns_resolution() — разрешение контрольного имени.
- check_ssh_service() — состояние SSH.
- print_diagnostics_report() — PASS/WARN/FAIL с причинами.

Скрипт только читает состояние; не устанавливает пакеты, не меняет firewall, не редактирует сеть и не включает SSH автоматически.

**Результат:** Linux и Windows VM соединены в изолированной сети; SSH по ключу проверен; версия ОС, архитектура и инструменты известны.

## Этап 1. Linux Gateway и системная диагностика

**Файлы:** корневой `CMakeLists.txt`; `.github/workflows/linux-cmake.yml`; `repka_gateway/main.cpp`; `src/application/linux/application_bootstrap.hpp/.cpp`; `src/platform/linux/runtime_environment.hpp/.cpp`; `src/platform/linux/system_diagnostics.hpp/.cpp`; `src/platform/linux/network_diagnostics.hpp/.cpp`; `src/platform/logging/logger.hpp/.cpp`; `src/platform/linux/runtime_config.hpp/.cpp`; `src/platform/linux/command_line.hpp/.cpp`; `src/platform/linux/runtime_config_file.hpp/.cpp`; в дальнейшем — `repka/dockstation-gateway.service`.

**Согласованная система сборки:** CMake используется для отдельной Linux-цели. Текущая корневая конфигурация собирает в статическую библиотеку `dockstation_linux_runtime` восемь компонентов: `LinuxApplicationBootstrap`, `RuntimeEnvironment`, `SystemDiagnostics`, `NetworkDiagnostics`, `PlatformLogger`, `RuntimeConfig`, `CommandLineParser` и `RuntimeConfigFileLoader`. Для потокобезопасного PlatformLogger CMake явно находит и подключает `Threads::Threads`; зависимость транзитивно передаётся потребителям статической библиотеки. Отдельная исполняемая цель `dockstation_linux_bootstrap` использует `repka_gateway/main.cpp`. В CTest зарегистрированы восемь host-проверок: `test_application_bootstrap`, `test_runtime_environment`, `test_system_diagnostics`, `test_network_diagnostics`, `test_logger`, `test_runtime_config`, `test_command_line`, `test_runtime_config_file`. Bootstrap пока работает в bootstrap-only режиме без callback прикладной службы: проверяет окружение и завершается, явно сообщая, что HTTP API не запущен. Он не подменяет временный Python Gateway и не создаёт конкурирующий HTTP-сервер. Существующие `tests/` и `tools/` не меняются.

Команды из корня репозитория:

- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` — конфигурация проекта;
- `cmake --build build --parallel` — сборка;
- `ctest --test-dir build --output-on-failure` — запуск зарегистрированных проверок;
- для дополнительной host-проверки с санитайзерами: добавить при конфигурации `-DDOCKSTATION_ENABLE_SANITIZERS=ON`. Санитайзеры по умолчанию выключены, чтобы не навязывать их целевой сборке Repka.

**command_line.cpp:**
- parse_command_line() — разбор параметров процесса, включая явный `--config-file PATH`; фиксирует маску явно заданных CLI overrides и не читает файлы/не запускает службы.
- apply_command_line_overrides() — накладывает только явно заданные CLI-значения на загруженную базовую конфигурацию.
- command_line_help() — справка по поддерживаемым параметрам.

**runtime_config_file.cpp:**
- parse_runtime_config_text() — строгий разбор строк `key=value`, пустых строк и комментариев; проверяет известные ключи, дубликаты, типы и диапазоны.
- load_runtime_config_file() — безопасно читает явно заданный обычный файл не более 64 КиБ; различает NOT_FOUND, READ_ERROR и INVALID.
- Поддерживаемые ключи: `api_port`, `log_level`, `log_file`, `log_stderr`, `log_max_file_size_bytes`, `log_max_rotated_files`. Кавычки и inline-комментарии не имеют специальной семантики.
- Приоритет: значения по умолчанию → файл → явно заданные CLI-параметры. После объединения Bootstrap выполняет `validate_runtime_config()` и только затем инициализирует Logger. Файл необязателен и не ищется автоматически.

**LinuxApplicationBootstrap (реализованный текущий lifecycle-каркас):**
- `main()` передаёт аргументы в `run_application()` и возвращает код завершения ОС.
- `run_application()` разбирает CLI; --help и ошибки завершают процесс до инициализации Logger.
- RuntimeConfig повторно валидируется через существующий контракт до инициализации PlatformLogger.
- `install_shutdown_handlers()` устанавливает SIGINT/SIGTERM; обработчик только устанавливает `sig_atomic_t`-флаг.
- проверяются базовые runtime-зависимости; недоступность обязательного `uname` останавливает запуск, частичная недоступность procfs допускает DEGRADED.
- собираются read-only SystemDiagnostics и NetworkDiagnostics; DNS/TCP-пробы автоматически не запускаются.
- зарегистрированная служба получает проверенную конфигурацию и флаг завершения через ServiceRunner; ненулевой код службы отражается как SERVICE_FAILURE.
- при отсутствии ServiceRunner выполняется только bootstrap-проверка с явным сообщением, что HTTP API не запущен.
- обработчики сигналов восстанавливаются, Logger flush/shutdown проверяются отдельно.

Загрузчик внешней конфигурации RuntimeConfig реализован как отдельный read-only компонент; создание физических адаптеров, связывание core services, HTTP/API-сервер и полноценный run_gateway() остаются следующими отдельными задачами; они не имитируются текущим Bootstrap.

**runtime_environment.cpp:**
- read_os_version(), read_kernel_version(), read_hardware_architecture(), read_software_build_info(), check_runtime_dependencies().

**system_diagnostics.cpp:**
- read_uptime(), read_memory_status(), read_storage_status(), read_cpu_temperature(), read_cpu_load(), collect_system_diagnostics().

Ограничения источников данных:
- `read_storage_status()` описывает только файловую систему, содержащую корневой путь `/`; отдельный SSD и его mount point не предполагаются и не проверяются этой функцией.
- `read_cpu_temperature()` принимает только Linux thermal-zone датчик, тип которого явно содержит CPU, SoC или package. Если такой датчик не найден или не читается, результат UNAVAILABLE; температура неизвестной зоны не подставляется.
- `read_cpu_load()` возвращает load average за 1, 5 и 15 минут и количество задач из `/proc/loadavg`. Это не процент использования процессорного времени; в интерфейсе показатель нужно так и называть.
- Каждый показатель имеет независимый статус и причину недоступности. Ошибка одного источника не должна превращать весь диагностический снимок в фиктивный нулевой результат.

**network_diagnostics.cpp:**
- read_network_interfaces(), read_interface_addresses(), read_default_route(), resolve_dns_name(), check_tcp_reachability(), collect_network_diagnostics().

**logger.cpp:**
- initialize_logger(), write_log_record(), redact_sensitive_fields(), rotate_log_files(), flush_log().

**runtime_config.cpp:**
- validate_runtime_config() — проверка типизированных настроек процесса Gateway без чтения файла и запуска служб.
- initialize_runtime_logger() — сначала проверяет RuntimeConfig, затем инициализирует PlatformLogger; при невалидной конфигурации Logger не запускается.

Недоступная температура или неизвестный статус обозначаются явно, не подменяются нулём.

**Реализованный первый срез этапа 1 — RuntimeEnvironment:** `src/platform/linux/runtime_environment.hpp/.cpp` содержит `read_os_version()`, `read_kernel_version()`, `read_hardware_architecture()`, `read_software_build_info()` и `check_runtime_dependencies()`. Проверка размещена в `verification/repka_gateway/test_runtime_environment.cpp`; ранее на host Linux x86_64 были пройдены 16 проверок.

**Реализованный второй срез — SystemDiagnostics:** `src/platform/linux/system_diagnostics.hpp/.cpp` содержит шесть функций из перечня выше. Проверка размещена в `verification/repka_gateway/test_system_diagnostics.cpp`; ранее host-сборка GCC 14.2.0 с AddressSanitizer и UndefinedBehaviorSanitizer прошла, все 24 проверки пройдены. Целевая сборка ARM64 и испытание на Repka Pi 5 не выполнялись.

**Реализованный третий срез — NetworkDiagnostics:** `src/platform/linux/network_diagnostics.hpp/.cpp` содержит шесть функций из перечня выше. Добавлена проверочная программа `verification/repka_gateway/test_network_diagnostics.cpp`; её сценарии используют localhost/loopback и не обращаются к внешним узлам. Код и CMake/CTest-регистрация добавлены в репозиторий, но host-сборка и тест NetworkDiagnostics прошли в GitHub Actions в составе полного набора 8/8 CTest с AddressSanitizer/UndefinedBehaviorSanitizer. Целевая проверка на Repka Pi 5 не выполнялась.

Особенности контракта: DNS/NSS вызывается синхронно и не имеет собственного тайм-аута; TCP timeout 1 мс…60 с ограничивает этап подключения после DNS-разрешения. `collect_network_diagnostics()` не делает внешних сетевых запросов. Неудачная TCP-проверка с доступным результатом диагностики имеет AVAILABLE и `reachable=false`, с причиной отказа/тайм-аута.

**Реализованный четвёртый срез — системный Logger:** добавлены `src/platform/logging/logger.hpp/.cpp` и `verification/repka_gateway/test_logger.cpp`. Журнал формируется в JSON Lines с UTC-временем, уровнем DEBUG/INFO/WARNING/ERROR, компонентом, сообщением и полями. Поддерживаются stderr и необязательный файл, фильтр минимального уровня, ротация ограниченного числа архивов, flush/shutdown и маскирование значений полей с чувствительными именами (password/token/secret/authorization/private key/credential/API key). Для файлового sink родительский каталог должен существовать и быть доступен процессу; автоматическое создание каталогов не выполняется. Logger дополнительно отклоняет неизвестные уровни и пути с NUL, маскирует варианты имён чувствительных полей с подчёркиваниями, дефисами и пробелами, сообщает об ошибках проверки файла, flush и close; перед проверкой размера сбрасывает файловый буфер, чтобы учесть предыдущие записи; исчезновение открытого файла приводит к UNAVAILABLE как при записи, так и при ротации; расчёт порога ротации защищён от переполнения. Секреты запрещено помещать в свободный текст message. Это системный журнал приложения, а не доменный EventLogger и не аудит действий пользователей. Linux-сборка и тест Logger прошли в GitHub Actions в составе полного набора 8/8 CTest с AddressSanitizer/UndefinedBehaviorSanitizer.

Корневой CMake включает LinuxApplicationBootstrap и семь платформенных компонентов, собирает исполняемую цель `dockstation_linux_bootstrap` и регистрирует восемь проверочных программ в CTest. Linux-сборка в GitHub Actions прошла; все восемь из восьми CTest прошли с AddressSanitizer и UndefinedBehaviorSanitizer. Целевая сборка ARM64 и испытание на Repka Pi 5 не выполнялись. HTTP API остаётся в отдельном Python VM-прототипе; C++ Bootstrap пока не запускает сетевую службу.

**Реализованный пятый срез — RuntimeConfig:** добавлены `src/platform/linux/runtime_config.hpp/.cpp` и `verification/repka_gateway/test_runtime_config.cpp`. Контракт задаёт безопасные значения по умолчанию (API port 8080, bind address фиксирован как `127.0.0.1`) и проверяет диапазон порта, параметры PlatformLogger, наличие хотя бы одного sink, корректность уровня, размера файла и количества архивов. `validate_runtime_config()` не открывает файлы/сокеты и не запускает службы; открытие настроенного файлового sink выполняется только при последующем вызове `initialize_runtime_logger()`. Добавлена `initialize_runtime_logger()`: она выполняет валидацию до инициализации PlatformLogger; при ошибке конфигурации журналирование не запускается, при успешной валидации результат инициализации Logger возвращается отдельно. Файл `.conf` читается только по явно заданному `--config-file`; автоматический системный путь и переменные окружения не используются. Порядок настроек: значения по умолчанию → файл → явно заданные параметры CLI. Итоговый RuntimeConfig валидируется до инициализации Logger. Секретные параметры и SSD/storage-настройки в этот контракт не включены.

**Реализованный шестой срез — CommandLineParser:** добавлены `src/platform/linux/command_line.hpp/.cpp` и `verification/repka_gateway/test_command_line.cpp`. Парсер поддерживает только параметры, уже представленные в RuntimeConfig: `--api-port`, `--log-level`, `--log-file`, `--log-stderr`/`--no-log-stderr`, `--log-max-file-size-bytes`, `--log-max-rotated-files` и отдельный `--help`. Повторные/неизвестные параметры, неверные значения и отсутствие значения отклоняются. Парсер возвращает маску явных CLI overrides; при указанном `--config-file` межполевую проверку откладывает до объединения настроек. Сам парсер не читает файлы и не запускает службы. Адрес привязки остаётся фиксированным `127.0.0.1`.

**Реализованный седьмой срез — RuntimeConfigFileLoader:** добавлены `src/platform/linux/runtime_config_file.hpp/.cpp` и `verification/repka_gateway/test_runtime_config_file.cpp`. Формат — строгий текстовый `.conf` с ASCII-ключами `key=value`, пустыми строками и комментариями, начинающимися с `#`; допускается UTF-8 BOM. Поддерживаются `api_port`, `log_level`, `log_file`, `log_stderr`, `log_max_file_size_bytes`, `log_max_rotated_files`. Неизвестные и повторные ключи, пустые значения, некорректные типы/диапазоны и NUL отклоняются; файл ограничен 65536 байтами и должен быть обычным файлом. Результаты NOT_FOUND, READ_ERROR и INVALID различаются. Кавычки и inline-комментарии не имеют специальной семантики. Файл не содержит секретов и не ищется автоматически. Приоритет: defaults → file → явно заданные CLI overrides. Bootstrap проверяет итоговую конфигурацию до запуска Logger. Unit-тест проверяет разбор, ошибки, ограничение размера, чтение файла и приоритет CLI; тест Bootstrap проверяет загрузку файла и передачу итоговой конфигурации службе.

**Ожидаемый результат всего этапа 1:** отдельное Linux-приложение собирается, запускается, выдаёт диагностику и корректно завершается. Это целевой результат этапа, а не текущий статус: на данном срезе есть статическая библиотека, bootstrap-only исполняемая цель и восемь зарегистрированных проверочных программ. HTTP/API-служба ещё не подключена; Python Gateway остаётся отдельным VM-прототипом. Старый host-testbench не меняется.

## Этап 3A. Географическая карта и подготовка маршрутов в Operator

Развивать карту как часть Windows Operator, а не отдельное приложение. Помимо отображения станции, текущих позиций БАС, фактических треков и планов предусмотреть подготовку маршрута:
- редактирование точек на карте;
- ввод/вставка координат;
- импорт файлов через отдельные адаптеры форматов;
- предварительный просмотр и отчёт проверки;
- назначение маршрута станции, БАС и миссии;
- сохранение маршрута как плана/кандидата до отдельного подтверждения.

Предварительные функции:
- create_route_draft() — создать черновик маршрута;
- add_route_waypoint(), update_route_waypoint(), remove_route_waypoint(), reorder_route_waypoints() — редактирование точек;
- parse_route_source() — распознать/разобрать выбранный источник;
- import_route_file() — импортировать файл через адаптер формата;
- validate_route_candidate() — проверить структуру, координаты, единицы, обязательные параметры и назначение;
- preview_route_on_map() — показать предварительный маршрут;
- assign_route_target() — связать план с station_id, drone_id и mission_id;
- save_route_draft() — сохранить проверенный черновик/план.

Это планируемые функции, а не существующий код. Перечень форматов (кандидаты: GeoJSON, KML, GPX, CSV и форматы планировщиков конкретных автопилотов) должен быть утверждён после проверки совместимости. Импорт/сохранение не равны передаче маршрута автопилоту или разрешению на полёт.


## Этап 2. Read-only API на Repka

**Файлы:** src/communications/network_types.hpp; local_api_server.hpp/.cpp; api_router.hpp/.cpp; api_routes.hpp/.cpp; api_serialization.hpp/.cpp; api_error.hpp/.cpp.

**Функции local_api_server.cpp:**
- start_local_api_server() — запуск только на 127.0.0.1.
- stop_local_api_server() — остановка и закрытие соединений.
- get_local_api_status() — фактическое состояние и адрес привязки.
- validate_server_limits() — размеры запросов, число соединений и тайм-ауты.

**api_router.cpp:**
- parse_http_request(), validate_http_request(), dispatch_api_request(), build_http_response(), reject_unsupported_request().

**api_routes.cpp:**
- handle_health_request() — GET /api/v1/health.
- handle_system_diagnostics_request() — GET /api/v1/system/diagnostics.
- handle_not_found(), handle_method_not_allowed().

**api_serialization.cpp:**
- serialize_health_response(), serialize_system_diagnostics(), serialize_error_response().

**api_error.cpp:**
- map_internal_error(), validate_error_payload().

На этом этапе API не принимает команды и не слушает Ethernet IP напрямую. HTTP разрешён только на loopback через SSH-туннель.

**Результат:** Windows получает два GET-ответа через туннель; неизвестные маршруты и неподдерживаемые методы отклоняются.

## Этап 3. Первоначальное графическое Operator Application

**Сразу после подтверждения сетевого соединения начинаем графическое Windows-приложение.** Это не консольная утилита и не макет: оно подключается к реальному API, показывает диагностику и разрывы связи. Физические команды пока не исполняются.

**Файлы:**
- operator/pyproject.toml.
- operator/src/dockstation_operator/main.py и application.py.
- config/settings.py.
- connection/connection_manager.py и ssh_tunnel.py.
- api/station_api_client.py и api_models.py.
- models/diagnostics_model.py.
- viewmodels/main_viewmodel.py, connection_viewmodel.py, diagnostics_viewmodel.py.
- ui/Main.qml, ConnectionPage.qml, DiagnosticsPage.qml, EventsPage.qml.

Python + PySide6 + QML — рекомендуемый кандидат из docs/23; пригодность подтверждается первым прототипом.

**main.py / application.py:**
- main(), create_application(), load_application_settings(), initialize_application_services(), show_main_window(), shutdown_application().

**ssh_tunnel.py:**
- find_ssh_executable(), validate_ssh_profile(), verify_known_host(), start_ssh_tunnel(), wait_for_tunnel_ready(), get_ssh_tunnel_status(), stop_ssh_tunnel().

**connection_manager.py:**
- validate_connection_config(), connect_to_station(), disconnect_from_station(), get_connection_status(), check_connection_health(), handle_connection_loss(), reconnect_connection().

Повторное подключение относится только к восстановлению канала и не повторяет физических команд.

**station_api_client.py:**
- get_health(), get_system_diagnostics(), validate_api_response(), map_api_error(), cancel_pending_request().

**diagnostics_model.py / ViewModel:**
- apply_diagnostics(), mark_diagnostics_stale(), clear_connection_state(), refresh_diagnostics(), format_diagnostic_value(), append_client_event().

**Работа над дизайном начинается вместе с первым GUI, а не откладывается до завершения функций.** До расширения набора экранов подготовить информационную архитектуру, пользовательские сценарии, карту навигации, черновые макеты ключевых экранов и базовую дизайн-систему. Затем реализовать визуальный прототип в QML и проверить его на типовых разрешениях Windows. Владелец проекта утверждает направление до масштабирования интерфейса.

**Первый интерфейс:** экран подключения; система (ОС, ядро, архитектура, uptime, CPU/RAM, накопитель, доступная температура); сеть (интерфейсы, адреса, маршрут, DNS, API); события клиента. Отсутствующие датчики показываются как NOT_CONFIGURED/UNAVAILABLE. Секреты и приватные ключи не копируются в настройки приложения; SSH host key проверяется строго.

**Результат:** GUI запускается на Windows, соединяется с Linux VM, обновляет диагностику и корректно показывает отключение и устаревшие данные. Подготовлены и согласованы первые макеты, визуальные правила и навигация; реализация не подменяет фактические данные декоративными индикаторами.

## Этап 4. Тестовые адаптеры и виртуальные сценарии

Полноценный симулятор физической станции не создаём. Вместо него добавляем минимальные тестовые адаптеры только там, где реальное устройство или внешний сервис пока отсутствует. Адаптер реализует тот же программный контракт, что и будущий реальный адаптер, но не имитирует физические показания и результаты.

**Функции и правила тестового адаптера:**
- принять тестовый запрос и вернуть TEST_ADAPTER_ACCEPTED;
- обозначить источник ответа как TEST_ADAPTER;
- явно указывать, что физическое действие не выполнялось;
- возвращать NOT_CONFIGURED/UNAVAILABLE для отсутствующих датчиков и устройств;
- не формировать фиктивные значения температуры, заряда, положения крышки, движения двигателя и иных физических состояний;
- не переводить операцию в COMPLETED без фактического подтверждения от реального исполнителя и обратной связи.

**Виртуальные сценарии:** остановка и перезапуск Gateway; потеря и восстановление SSH/API-соединения; неверный SSH host key; неправильный ключ; тайм-аут; некорректный ответ API; устаревшие данные; повтор запроса; отсутствие физического адаптера; тестовый запрос, принятый адаптером без исполнения. Проверки контрактов, ошибок и границ остаются обязательными.

Новые тестовые адаптеры размещать рядом с соответствующим приложением/интерфейсом либо в согласованном каталоге verification/; не создавать отдельную универсальную подсистему симуляции.

**Результат:** бизнес-логика работает через реальные контракты; при отсутствии оборудования запрос может быть принят тестовым адаптером, но физическое выполнение не изображается и не подтверждается.

## Этап 5. Постоянное хранение и интеграция текущего ядра

**Файлы:** src/platform/persistence/database.hpp/.cpp; schema_migrations.hpp/.cpp; event_repository.hpp/.cpp; station_state_repository.hpp/.cpp; operation_repository.hpp/.cpp; recovery_repository.hpp/.cpp; configuration_repository.hpp/.cpp; src/platform/time/system_clock.hpp/.cpp; monotonic_clock.hpp/.cpp; timer_service.hpp/.cpp; src/platform/ids/operation_id_provider.hpp/.cpp.

SQLite — кандидат, но не утверждённый выбор. Проверить атомарность, восстановление после потери питания, блокировки и ресурс накопителя.

**Функции:**
- open_database(), verify_database_schema(), apply_schema_migrations(), run_storage_transaction().
- append_event_once(), read_source_cursor(), commit_event_and_cursor().
- read_station_snapshot(), commit_station_transition().
- save_operation_intent(), update_operation_record(), list_unfinished_operations().
- read_recovery_marker(), write_recovery_marker().
- read_configuration_record(), write_configuration_record().
- reserve_next_operation_id().
- get_monotonic_time(), schedule_timer(), cancel_timer(), dispatch_expired_timers().

**Переиспользуем:** StationStateManager F01–F17 и ConfigurationManager CFG-F00…CFG-F08 из текущего main. Их логическую реализацию не переписываем. Добавляем production-хранилища, decoder payload, реальные источники событий, Timer и composition root. Тестовая политика конфигурации не становится рабочей политикой автоматически.

**Результат:** snapshot, события, cursor и operation lifecycle сохраняются после перезапуска; восстановление не возобновляет физические действия без сверки.

## Этап 6. Диагностика, SafetyManager, MissionManager и погода

**DiagnosticsManager:**
- diagnostic_validation.cpp: validate_diagnostic_context(), validate_diagnostic_result(), aggregate_self_test_results().
- diagnostic_plan.cpp: build_startup_diagnostic_plan(), execute_diagnostic_check(), run_startup_self_test(), publish_self_test_report().
- checks_runtime.cpp: check_runtime_health(), check_configuration_integrity(), check_clock_service(), check_event_logger(), check_local_storage(), check_watchdog_status().
- checks_equipment.cpp: check_power_manager_status(), check_safety_manager_status(), check_ble_adapter_status(), check_ble_scan_capability(), check_camera_pipeline_status(), check_meteo_service_status(), check_sensor_channel_status(), check_motor_service_status(), check_wireless_power_service_status(), check_drone_integration_status().

Стартовая диагностика не запускает крышку, очистку и зарядку.

**SafetyManager — src/core/safety_manager.cpp:**
- collect_safety_snapshot(), evaluate_operation_permission(), is_permission_current(), request_safe_stop(), record_blocking_reason().

**Геопозиционирование и маршруты БАС (данные для Operator Map):**
- получать координаты станции из утверждённой конфигурации/реестра;
- получать координаты БАС из доверенного источника телеметрии;
- раздельно предоставлять фактический трек и запланированный маршрут с идентификаторами станции, БАС и миссии;
- передавать время измерения/формирования, время получения, систему координат и качество/актуальность каждой позиции;
- не дополнять отсутствующие координаты вымышленными значениями и не превращать план маршрута в фактический трек.

**MissionManager — src/core/mission_manager.cpp:**
- validate_mission(), validate_mission_target(), check_mission_expiry(), check_manual_confirmation(), select_next_mission(), reserve_mission(), mark_mission_dispatched(), complete_mission(), revoke_mission(), stop_automation(), check_local_mission_quota(), set_local_mission_quota(), check_storage_reserve(), check_inspection_cycle_limit().

**WeatherManager / MeteoService:**
- validate_weather_measurement(), filter_weather_measurements(), evaluate_weather_status(), evaluate_weather_permission(), read_weather_measurements(), get_meteo_service_status().

Погодные пороги и конкретная модель метеостанции не назначаются без выбора и согласования.

**Результат:** UNKNOWN не трактуется как ALLOW; миссия не выбирается без адресата, срока, разрешения и достаточного места.

## Этап 7. Мост Repka—ESP32

**Файлы:** src/hardware/interfaces/controller_transport.hpp; controller_protocol.hpp; controller_codec.hpp/.cpp; src/hardware/esp32/esp32_bridge.hpp/.cpp; src/platform/linux/serial_transport.hpp/.cpp; verification/integration/controller_test_adapter/.

**Функции:**
- open_controller_transport(), close_controller_transport().
- read_controller_frame(), write_controller_frame().
- encode_controller_message(), decode_controller_message(), validate_controller_message().
- perform_controller_handshake(), request_controller_snapshot().
- send_controller_request(), process_controller_response().
- monitor_controller_link(), handle_controller_link_loss(), reconnect_controller().

USB/UART, пины и формат кадров остаются открытыми до проверки электрических характеристик и утверждения протокола. Потеря канала приводит к UNKNOWN/UNAVAILABLE, а не к предположению о безопасном состоянии. Повторная доставка не повторяет физическое действие.

**Результат:** один контракт проверен с тестовым адаптером и затем используется реальным ESP32; тестовый адаптер подтверждает только приём запроса, а локальные блокировки остаются на стороне контроллера.

## Этап 8. Физические подсистемы

**Крышка и приводы — src/mechanics/lid_manager.cpp, motor_service.cpp:**
- check_lid_permission(), request_open_lid(), request_close_lid(), get_lid_position(), monitor_lid_motion(), stop_lid_motion().
- read_motor_feedback(), move_actuator(), stop_actuator().

**Самоочистка — src/mechanics/cleaning_manager.cpp:**
- check_cleaning_conditions(), start_cleaning(), monitor_cleaning(), detect_cleaning_fault(), verify_cleaning_result(), evaluate_cleaning_retry(), stop_cleaning().

**Зарядка — src/charging/charging_manager.cpp, wireless_power_service.cpp, bms_interface.cpp:**
- check_charging_conditions(), start_charging(), get_charging_status(), stop_charging(), verify_charging_completion(), classify_charging_fault().
- enable_power_transfer(), disable_power_transfer(), get_wireless_power_status().
- read_battery_status(), request_charge_authorization().

**Электропитание — src/power/power_manager.cpp:**
- read_power_status(), check_power_budget(), switch_to_backup_power(), restore_normal_power(), classify_power_fault().

Обогрев, вентиляция и антиобледенение добавляются после выбора аппаратуры и режимов. Каждая опасная операция требует актуального ALLOW, обратной связи, тайм-аута и безопасной остановки.

## Этап 9. Обнаружение БАС, камера и посадка

Зависит от выбора БАС, автопилота, камеры и протокола.

**DetectionManager — src/detection/detection_manager.cpp:**
- process_detection(), authenticate_drone(), match_registered_drone(), confirm_visual_match(), select_target_drone(), get_detection_status().

**BleService — src/detection/ble_service.cpp:**
- start_ble_scan(), stop_ble_scan(), get_ble_scan_results(), get_ble_service_status(). BLE не является самостоятельной аутентификацией.

**DeviceAuthManager — src/detection/device_auth_manager.cpp:**
- verify_device_authentication(), get_device_trust_status().

**LandingManager — src/landing/landing_manager.cpp:**
- detect_landing_marker(), estimate_relative_pose(), assess_pose_quality(), build_landing_guidance(), send_landing_guidance(), stop_guidance_on_tracking_loss(), collect_landing_evidence(), confirm_landing().

**CameraService — src/camera/camera_service.cpp:**
- open_camera_device(), close_camera_device(), get_camera_status().
- open_camera_stream(), close_camera_stream().
- start_camera_recording(), stop_camera_recording(), get_recording_status().

Для текущего прототипа используется одна обычная USB-веб-камера. Просмотр оператором и запись независимы: поток доступен при выключенной записи; запись запускается явной командой оператора и продолжается до команды остановки. После остановки файл должен быть корректно завершён и сохранён через настроенное хранилище. Автоматическая непрерывная запись не включается.

**VisionService / AutopilotInterface:**
- capture_frame(), get_camera_status().
- get_autopilot_status(), transmit_authorized_mission(), transmit_landing_guidance(), check_autopilot_link().

Не утверждать MAVLink или иной протокол до выбора БАС и изучения документации автопилота. Посадочные команды проверяются отдельно на стенде и в полётных испытаниях.

## Этап 10. Файлы и передача материалов

**DataManager — src/data/data_manager.cpp:**
- receive_mission_materials(), verify_and_store_materials(), check_data_readiness_for_next_mission(), bind_material_to_mission().

**DroneStorageManager — src/data/drone_storage_manager.cpp:**
- connect_to_drone_storage(), get_file_manifest(), read_file_chunk(), get_drone_storage_status().

**FileTransferManager — src/data/file_transfer_manager.cpp:**
- create_transfer_job(), receive_file_chunk(), resume_transfer(), finalize_file_transfer().

**DataIntegrityManager — src/data/data_integrity_manager.cpp:**
- verify_chunk_checksum(), calculate_file_checksum(), verify_completed_file(), verify_manifest_consistency().

**LocalStorageManager — src/data/local_storage_manager.cpp:**
- check_available_capacity(), reserve_storage(), save_file_atomically(), get_stored_file(), delete_local_copy().

Локальное хранилище использует абстрактный `StorageBackend`. Реализация для SSD — отдельный необязательный backend, выбираемый конфигурацией. Отсутствие SSD не должно препятствовать запуску и работе остальных служб; функции, требующие постоянной записи (включая сохранение видео), возвращают явную недоступность. Фактическое сохранение подтверждается только после успешного завершения записи.

**EncryptionManager — src/data/encryption_manager.cpp:**
- encrypt_local_file(), decrypt_local_file(), encrypt_sensitive_configuration(), verify_key_reference().

**UploadManager — src/data/upload_manager.cpp:**
- upload_file_metadata(), upload_file_chunk(), confirm_server_receipt().

**TransferQueueManager — src/data/transfer_queue_manager.cpp:**
- enqueue_transfer(), get_next_pending_transfer(), update_transfer_status(), schedule_transfer_retry(), restore_pending_transfers().

Оригиналы файлов на БАС не удаляются. Частичный файл не считается завершённым; потеря сервера не уничтожает локальные данные; повторы не создают дубликаты.

## Этап 11. Station ID, пользователи и сервер

До реализации согласовать открытые вопросы docs/24: размещение доверенного сервиса, первичную регистрацию, Station ID, ключи/сертификаты, отзыв, офлайн-регистрацию и политику паролей. UUIDv4, mTLS, TLS 1.3 и Argon2id в документах пока имеют статус предложений.

**StationIdentityManager — src/identity/station_identity_manager.cpp:**
- validate_station_id_format(), generate_station_id(), validate_station_identity_record(), find_registration_conflicts(), validate_provisioning_context(), create_device_key_reference(), build_enrollment_request(), validate_enrollment_response(), persist_station_identity(), complete_station_enrollment(), verify_station_certificate(), begin_credential_rotation(), apply_credential_rotation(), revoke_station_credential(), retire_station(), validate_offline_enrollment_voucher(), synchronize_pending_enrollment(), recover_station_identity().

**UserAccessManager — src/access/user_access_manager.cpp (локальная станция):**
- verify_access_grant_signature(), validate_station_access_grant(), apply_signed_grant_revocation().
- authorize_station_resource(), authorize_command(), revoke_local_sessions_for_grant().

Центральный сервер DockStation ведёт единственный авторитетный реестр OwnerSpace, пользователей, членства, владельцев станций и делегированных grants. Он выполняет create_user_account(), authenticate_user(), create_owner_space(), add/update/revoke_owner_space_member(), assign/transfer_station_ownership(), list_accessible_stations(), grant/revoke_station_access(), authorize_station_resource(), authorize_command(), issue_signed_station_access_grant(), publish_signed_grant_revocation() и record_access_audit_event(). Эти серверные операции не размещаются в локальном UserAccessManager Repka Pi; backend/API/DB будут специфицированы отдельно.

Обязательная модель доступа (DEC-055): пространство владения представляет частного владельца или организацию; одно пространство может иметь несколько станций и индивидуальных пользователей; пользователь может состоять в нескольких пространствах. Каждая станция принадлежит ровно одному OwnerSpace. Внешний сервисный специалист получает только явно делегированный доступ к конкретным станциям, функциям и сроку.

Сервер проверяет OwnerSpace и station-scoped полномочия при каждом чтении/изменении ресурса: списки, телеметрия, история, журналы, файлы, видео, конфигурация, миссии, команды, результаты, подписки и экспорты. OwnerSpace/роль/permissions из тела запроса не считаются доверенными; принадлежность станции определяется по серверному реестру. Неавторизованный Station ID не раскрывает данные или метаданные. При неизвестном/просроченном праве доступ запрещается.

Прямые Wi-Fi/проводные маршруты не обходят эту модель: станция проверяет локально подписанные полномочия в пределах срока их действия и применяет полученные подписанные отзывы. Полностью отключённая станция не может получить отзыв мгновенно; поэтому максимальный срок автономного grant и разрешённые офлайн-операции должны быть отдельно ограничены. Передача станции отзывает прежние привязки, делегирования, сессии и токены; Station ID сохраняется. Детальные требования и сценарии испытаний — docs/23, docs/24, docs/11 TEST-SW-113…123.

**CommunicationService — src/communications/communication_service.cpp:**
- connect_to_server(), connect_to_local_wifi(), connect_to_wired_service(), disconnect(), send_message(), receive_message(), get_connection_status().

**ServerApi — src/communications/server_api.cpp:**
- synchronize_missions(), upload_material_chunk(), request_material_receipt(), publish_station_telemetry().

Правила владения, tenant isolation, server-side authorization, локальной проверки подписанных grants и ограничений offline-доступа определены в docs/24; пользовательские сценарии — в docs/23. Детальные контракты серверного API/БД, миграции, очереди, подписание/отзыв grants и реализация аудита остаются отдельной серверной работой. Клиентская функция не заменяет серверную проверку полномочий.

## Этап 12. Полная версия Windows Operator Application

Первый GUI создаётся на этапе 3; здесь приложение расширяется до полного набора docs/23.

**ConnectionManager:**
- connect_via_server(), connect_via_wifi(), connect_via_wire(), disconnect_active_connection(), get_active_connection_status().

**StationApiClient:**
- get_station_snapshot(), subscribe_to_station_updates(), send_station_command(), query_command_result(), get_measurement_history(), get_module_status(), get_station_logs().

**TelemetryStore:**
- update_measurement(), mark_measurement_stale(), get_measurement_history(), apply_station_snapshot(), clear_station_data().

**CommandCenter:**
- build_command_request(), submit_command(), track_operation_status(), confirm_dangerous_action(), query_unknown_outcome(), reject_unsupported_command().

**SessionManager:**
- login(), logout(), refresh_session(), clear_local_session().

**CameraViewer:**
- open_camera_stream(), close_camera_stream(), request_camera_frame().
- start_camera_recording(), stop_camera_recording(), get_recording_status().

**AuditViewer:**
- load_audit_events(), filter_audit_events(), export_audit_events().

**MissionAndDroneControls:**
- load_registered_drones(), select_target_drone(), set_local_mission_quota(), set_transfer_completion_criterion(), set_inspection_cycle_limit(), request_module_restart(), request_station_restart(), request_emergency_exit_confirmation().

**StationDroneMap / RouteViewModel:**
- load_station_geolocation(), load_drone_positions(), load_flown_track(), load_planned_route(), subscribe_to_position_updates(), mark_position_stale(), select_map_station(), select_map_drone(), set_route_layer_visibility().
- Отображать станцию, все доступные привязанные БАС, фактические треки и предстоящие маршруты раздельно. Сохранять связь данных с station_id, drone_id и mission_id.
- Показывать время и качество координат; при потере связи оставлять только последнее известное положение с явным признаком STALE.
- Картографический движок, поставщик карт, офлайн-режим и конкретные форматы геоданных выбираются отдельно.

### Экраны полного приложения

1. Авторизация и выбор станции.
2. Главная панель и предупреждения.
3. Все датчики с единицами, качеством и временем.
4. Модули и оборудование.
5. Команды и lifecycle операций.
6. Миссии и зарегистрированные БАС.
7. Файловая передача и очередь.
8. Камера при наличии оборудования и прав.
9. Журнал и аудит.
10. Пользователи, привязки и полномочия.
11. Подключения и сервисная диагностика.
12. Интерактивная карта станции и привязанных БАС: актуальные/последние известные позиции, фактически пройденные треки и предстоящие маршруты разными слоями.

Станция перепроверяет полномочия, адресата, срок, защиту от повтора и SafetyManager перед каждой физической операцией. Потерянный ответ означает OUTCOME_UNKNOWN и сверку, а не автоматический повтор.

## Этап 13. Надёжность и развёртывание

**Файлы:** repka/dockstation-gateway.service; repka/config/README.md; install_gateway.sh; backup/README.md; release/README.md; src/platform/linux/watchdog_service.hpp/.cpp; process_manager.hpp/.cpp.

**Функции:**
- check_service_dependencies(), report_service_health(), register_watchdog_heartbeat(), detect_service_stall().
- restart_allowed_module(), validate_restart_conditions(), request_orderly_shutdown().
- create_backup(), verify_backup(), restore_backup().
- validate_release_manifest(), verify_release_signature(), install_release(), verify_installed_version().

Автоматическое удалённое обновление ПО/прошивки не входит без отдельного согласования. Первоначально — контролируемая установка и процедура восстановления.

## Этап 14. Сквозные испытания

Проверки новых приложений размещать в verification/, не переписывая старый testbench.

Обязательные сценарии:
1. Gateway стартует после загрузки VM/Repka.
2. Windows подключается по SSH-туннелю и отображает диагностику.
3. Непроверенный host key отклоняется.
4. Остановленный API не отображается как активная станция.
5. Потеря сети переводит данные в DISCONNECTED/STALE.
6. Некорректный ответ API не применяется.
7. Перезапуск Gateway не теряет подтверждённое состояние.
8. Повтор события не дублирует побочный эффект.
9. Секреты отсутствуют в журналах.
10. UNKNOWN/INVALID датчик не отображается как нормальный.
11. DENY/UNKNOWN SafetyManager блокирует опасную операцию.
12. Потеря ESP32 блокирует новые опасные действия.
13. Неверный Station ID, просроченная или повторная команда отклоняется.
14. Потерянный ответ на физическую команду приводит к OUTCOME_UNKNOWN.
15. Крышка, очистка и зарядка проходят отдельные аппаратные проверки.
16. Файлы сохраняются локально и передаются с проверкой целостности.
17. Конфликтующие команды нескольких операторов сериализуются.
18. Видеопоток доступен оператору при выключенной записи.
19. Запись начинается и останавливается только по явным командам; после остановки проверяется завершение файла.
20. При отсутствии/отказе SSD просмотр камеры и остальные службы продолжают работать, а запись не сообщает об успешном сохранении.

Каждый протокол фиксирует условия, версии, ожидаемый/фактический результат, логи и вывод. Виртуальный успех не заменяет аппаратную приёмку.

## 6. Уже реализованная часть репозитория

- StationStateManager F01–F17: логические функции ядра реализованы; нужны production-хранилища, источники событий, payload decoder, Timer и composition root.
- ConfigurationManager CFG-F00…CFG-F08: переносимая логика чтения/проверки/snapshot/revision реализована; нужны production-политики, decoder и repository.
- Точные контракты и назначения функций приведены в docs/26_CODE_FUNCTION_CATALOG.md.

Наличие ядра не означает, что уже есть готовое запускаемое приложение или интеграция на Repka.

## 7. Что не выбираем заранее

Не фиксировать без выбора оборудования/согласования: модель ESP32 и транспорт, протокол кадров и GPIO; метеостанцию; датчики и драйверы; батарею/BMS и профиль зарядки; БАС, автопилот и протокол; камеру и калибровку; production-СУБД; серверный API; криптографические параметры из docs/24; удалённое автоматическое обновление.

Для невыбранных устройств сначала создаются интерфейсы и минимальные тестовые адаптеры. Они подтверждают только программный приём запроса и не подменяют физические данные или состояние оборудования. Реальный драйвер пишется после проверки документации и электрической совместимости.

## 8. Особенности Repka Pi 5

Официальная документация Repka указывает для Pi 5 RK3588, Ethernet, USB, PCIe, 40-pin GPIO с альтернативными UART/I2C/SPI/PWM, CSI и DSI. Перед использованием интерфейсов нужно сверить распиновку, pin multiplexing и электрические характеристики платы.

Обязательные правила:
- не считать 40-pin разъём взаимозаменяемым с другой моделью Repka/Raspberry Pi;
- сверять pinout и режимы через официальную документацию и repka-config;
- проверять электрические пределы каждой линии до подключения;
- не писать аппаратный драйвер по виртуальной распиновке;
- сначала проверить программный контракт с тестовым адаптером, затем — на плате с безопасной нагрузкой.

Официальные документы:
- https://repka-pi.ru/docs/109 — характеристики, интерфейсы и электрические параметры.
- https://repka-pi.ru/docs/108 — настройки pinout, DSI, CSI и SPI.
- https://repka-pi.ru/docs/65 — GPIO и логические уровни.

## 9. Правила пофункциональной реализации

1. Перед задачей читать актуальные документы, исходник и зависимости из main.
2. Одна задача — одна функция или минимальная связная группа.
3. Существующие tests/ и tools/ не переписывать.
4. Новые проверки приложений размещать в verification/.
5. Не выдавать симуляцию за физический статус.
6. Для функции определить входы, выходы, ошибки, тайм-ауты и побочные эффекты.
7. GUI и сеть не входят в real-time контур.
8. Опасная операция требует устойчиво сохранённого намерения, operation_id, актуального ALLOW и подтверждения исполнителя.
9. Потеря ответа на физическую команду означает OUTCOME_UNKNOWN до сверки.
10. В отчёте перечислять только реально выполненные проверки.
11. При изменении контракта обновлять профильную документацию и каталог функций.

## 10. Критерии готовности рабочего продукта

Рабочий комплекс должен иметь: устойчивый запуск Gateway; постоянное хранение и восстановление; три защищённых канала оператора; идентичность станции, пользователей и права; телеметрию всех зарегистрированных датчиков; геопозицию станции и БАС, фактические треки, запланированные маршруты, создание/импорт маршрутов с проверкой и корректной маркировкой актуальности; журналы и диагностику; локальные проверки безопасности; интеграцию с ESP32; механизмы, зарядку, файлы и миссии в выбранной комплектации; корректную работу при потере питания/сети/модулей; виртуальные и аппаратные протоколы приёмки.

Наличие GUI, API или тестового адаптера само по себе не доказывает готовность станции.

## 11. Ближайшие задачи

1. Проверить текущий Windows Operator ↔ Linux Gateway: остановка Gateway, отображение потери связи/устаревших данных, запуск Gateway и восстановление обмена.
2. Подготовить инфраструктурные компоненты Linux-приложения: runtime/startup/shutdown, загрузка и проверка конфигурации, журналирование и интерфейсы внешних устройств.
3. Реализовать первый безопасный сквозной запрос через Operator → Gateway → прикладной сервис → тестовый адаптер → Operator. Результат тестового адаптера не считать физическим выполнением.
4. Продолжить дизайн Windows-приложения: структура экранов, навигация, визуальная концепция, макеты главной панели и диагностики, дизайн-система; после согласования — последовательная реализация в QML.
5. Составить карту интеграции StationStateManager F01–F17 и ConfigurationManager CFG-F00…CFG-F08 с production-хранилищами, Timer, источниками событий и composition root.
6. Продолжить подключение реальных ESP32 и аппаратных подсистем после утверждения интерфейсов и наличия оборудования.
7. Отдельно провести исследование варианта встроенного VPN/защищённой overlay-сети; реализацию не начинать до выбора архитектуры и согласования владельцем.

Контракты DiagnosticsManager согласованы владельцем проекта (DEC-058/059; docs/21 и docs/22). Это подготовительный результат и не меняет очередность мастер-плана: реализацию DiagnosticsManager, включая первый вертикальный срез SDA-F00 и последующие функции, начинать только на предусмотренном этапе после выполнения предшествующих зависимостей. Открытые аппаратные/режимные параметры не закрываются этим согласованием.

Первый следующий технический результат — проверенное восстановление соединения и сквозной программный запрос. Первый пользовательский результат по интерфейсу — согласованный визуальный прототип, развиваемый вместе с функциями приложения.

## 12. История изменений

| Редакция | Дата | Изменения |
|---|---|---|
| 0.1 | 2026-11-11 | Полная дорожная карта Gateway, Windows GUI, VM-лаборатории, ядра, ESP32, оборудования, файлов, сервера и приёмки. |
| 0.2 | 2026-11-11 | Этап 11 дополнен реализацией OwnerSpace, multi-tenant изоляции, делегированных station grants, object-level авторизации каждого запроса и отзыва доступа при передаче станции. |
| 0.3 | 2026-11-12 | Добавлена системная работа над дизайном Windows Operator; отдельный полный симулятор заменён минимальными тестовыми адаптерами без фиктивных физических состояний; добавлено предварительное исследование встроенного VPN без решения о реализации. |
| 0.4 | 2026-11-12 | Включена интерактивная карта станции и привязанных БАС, отображение фактических треков и предстоящих маршрутов; добавлены требования к геопозиционным данным и актуальности. |
| 0.5 | 2026-11-12 | Добавлены создание маршрутов на карте, ввод координат, файловый импорт, предварительная проверка и функции подготовки маршрута; конкретные форматы оставлены открытыми. |
| 0.6 | 2026-11-12 | Зафиксированы USB-веб-камера прототипа, независимый просмотр и ручная запись; добавлен необязательный SSD-backend, выбираемый конфигурацией, и сценарии проверки отсутствующего накопителя. |
| 0.7 | 2026-11-12 | Реализован первый Linux platform slice RuntimeEnvironment: сведения об ОС, ядре, архитектуре, сборке и доступности системных источников; 16 host-проверок пройдены, целевая проверка на Repka ещё не выполнялась. |
| 0.8 | 2026-11-12 | Владелец согласовал CMake для отдельной Linux-цели; добавлена корневая конфигурация сборки RuntimeEnvironment и его verification-теста. Исполняемая цель Gateway отложена до появления main/bootstrap. |
| 0.9 | 2026-11-12 | Реализован Linux SystemDiagnostics: uptime, RAM, корневая файловая система, явно идентифицированная CPU/SoC-температура и load average; добавлены отдельные host-проверки и CMake/CTest-цель. |
| 1.0 | 2026-11-12 | Добавлен третий Linux platform slice NetworkDiagnostics: интерфейсы, IPv4/IPv6-адреса, default routes, DNS и TCP; CMake/CTest и host-проверка добавлены. Фактический запуск нового теста ожидает доступного checkout. |
| 1.1 | 2026-11-12 | Добавлен системный PlatformLogger: JSONL, уровни, stderr/файл, ротация и маскирование чувствительных полей; добавлены CMake/CTest и отдельная проверочная программа. Запуск тестов ожидает host-сборки. |
| 1.2 | 2026-11-12 | Добавлены типизированный RuntimeConfig и валидатор безопасных настроек процесса Gateway; добавлены CMake/CTest и отдельная проверочная программа. Формат внешнего файла конфигурации не выбран и загрузчик не реализован. |
| 1.3 | 2026-11-12 | RuntimeConfig интегрирован с PlatformLogger через validate-before-initialize; добавлена проверка отказа при невалидных настройках и успешной инициализации файлового sink. Сборка и новые проверки ещё не запускались. |
| 1.4 | 2026-11-12 | Проведён статический аудит Linux-срезов; исправлены проверки входов Logger, обработка ошибок файлового sink, утечка addrinfo при успешном TCP connect и строгость разбора маршрутов. Добавлен CommandLineParser и его CTest-программа; фактическая сборка/тесты ожидают запуска. |
| 1.5 | 2026-11-12 | Добавлены проверки Logger для недопустимого уровня, NUL в пути и удаления открытого файла; уточнён каталог функций. Испытания остаются НЕ ПРОВЕРЕНЫ до host-сборки. |
| 1.6 | 2026-11-12 | Разделы программных проверок в docs/11 перенумерованы без дублирования; сценарии Logger и CommandLineParser сверены с фактическими тестами. Испытания остаются НЕ ПРОВЕРЕНЫ до host-сборки. |
| 1.7 | 2026-11-12 | Дополнительно обработано исчезновение открытого log-файла при ротации/записи, добавлена защита расчёта размера от переполнения и тест недопустимого семейства DNS. Сборка и тесты всё ещё не запускались. |
| 1.8 | 2026-11-12 | Исправлено маскирование чувствительных имён полей с пробелами (`private key`, `API key`); добавлены проверки обоих вариантов. Сборка и тесты всё ещё не запускались. |
| 1.9 | 2026-11-12 | Уточнён состав текущей CMake-цели и шести CTest-программ; ожидаемый результат этапа отделён от фактически реализованного среза. CLI дополнительно отклоняет следующий option-токен вместо значения; добавлен регрессионный критерий TEST-PLAT-028. Сборка и тесты всё ещё не запускались. |
| 1.10 | 2026-11-12 | Для статической Linux-библиотеки явно подключена системная потоковая зависимость `Threads::Threads`; Logger сбрасывает файловый буфер до проверки размера, чтобы ротация учитывала все предыдущие записи. Добавлен регрессионный тест и критерий TEST-PLAT-029. Сборка и тесты всё ещё не запускались. |
| 1.11 | 2026-11-12 | Уточнено описание PlatformLogger в реестре и мастер-плане: проверка порога ротации выполняется после сброса буфера файлового потока. Сборка и тесты всё ещё не запускались. |

| 1.12 | 2026-11-12 | Добавлен LinuxApplicationBootstrap: отдельная точка входа, validate-before-initialize Logger, read-only runtime/system/network диагностика, SIGINT/SIGTERM через sig_atomic_t, ServiceRunner и штатное завершение; добавлены исполняемая CMake-цель, седьмая CTest-программа и критерии TEST-PLAT-030…036. Bootstrap-only режим явно не запускает HTTP API. Локальная сборка не выполнялась; результат GitHub Actions пока не подтверждён. |

| 1.13 | 2026-11-12 | Добавлен GitHub Actions workflow для CMake-сборки Linux-цели и выполнения семи CTest-программ с AddressSanitizer/UndefinedBehaviorSanitizer. Доступный статус выполнения пока не получен. |
| 1.14 | 2026-11-12 | Добавлен RuntimeConfigFileLoader и его интеграция с Bootstrap; расширены тесты ошибок и приоритета CLI. GitHub Actions: Linux-сборка успешна, 8/8 CTest пройдены с ASan/UBSan. |
| 1.15 | 2026-11-12 | Расширен Linux CI: добавлены 31 host C++ unit/contract тест, Python compileall/pytest и shell syntax/behavior tests; исправлены устаревшие пути после переноса каталогов. |
| 1.16 | 2026-11-12 | Полный расширенный GitHub Actions прогон SUCCESS: CMake build, 8/8 CTest, 31/31 C++ host тест, Python compileall, 76 pytest и shell проверки. Обновлены host-проверочные статусы; целевые ARM64/Repka и аппаратные проверки остаются открытыми. |
| 1.17 | 2026-11-12 | Контракты DiagnosticsManager согласованы владельцем (DEC-058/059); подготовительная документация обновлена. Реализация не начата и не перенесена вперёд мастер-плана; аппаратные и режимные вопросы остаются открытыми. |

**Ограничение:** это план. Функции не считаются реализованными до появления кода, проверок и, где требуется, аппаратных испытаний.



## 13. Результат полного host CI

Полный workflow `.github/workflows/linux-cmake.yml` [43221524755](https://github.com/GooseRabbit/dockstation/actions/runs/43221524755) завершился успешно на Ubuntu 24.04 x86_64: Linux CMake build; 8/8 CTest с ASan/UBSan; 31/31 C++ host unit/contract программ; Python compileall; 76 pytest; синтаксические и поведенческие shell-тесты.

Это не является подтверждением ARM64-сборки, работы на Repka Pi 5, ESP32/Arduino, Windows GUI runtime, реального VM reconnect или аппаратных узлов. Их статус остаётся НЕ ПРОВЕРЕНО.
