# Полный план реализации программного комплекса DockStation на Repka Pi 5

**Статус:** мастер-план реализации; последовательность разработки согласована, открытые платформенные, сетевые, серверные и аппаратные детали требуют отдельной проверки и решения.  
**Редакция:** 1.46.  
**Дата:** 2026-11-14.  
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

В текущем лабораторном Operator-срезе реализовано автоматическое восстановление ранее установленного SSH/API-соединения после неожиданной потери связи: первая попытка выполняется через 5 секунд, следующие неудачные попытки — с интервалами 5, 10, 20 и 30 секунд (верхний предел — 30 секунд); при успехе запрашивается свежий read-only снимок. Ручное отключение и переключение профиля отменяют запланированные повторы; отменить уже выполняющуюся попытку через UI нельзя. Если профиль сохранён при отсутствии активного соединения, он становится целью следующей попытки; сохранение настроек во время активного сеанса не меняет параметры уже работающего туннеля. Физические команды не повторяются. Host-тесты проверяют сохранение и обновление профиля, восстановление клиента после перезапуска loopback Gateway, планирование ограниченного backoff и отмену ожидающих попыток. Ручный Windows Operator ↔ C++ Gateway сценарий в Ubuntu 24.04.5 LTS x86_64 VM выполнен 2026-11-14; проверены health/diagnostics, значения системных ресурсов, восстановление API-обмена после перезапуска Gateway и loopback-only bind. Подробности и ограничения — в docs/30_REPKA_NETWORK_IMPLEMENTATION_PLAN.md. Проверка восстановления самого SSH-процесса ранее проводилась отдельно на Python VM-прототипе.

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

**Файлы:** корневой `CMakeLists.txt`; `.github/workflows/linux-cmake.yml`; `repka_gateway/main.cpp`; `src/application/linux/application_bootstrap.hpp/.cpp`; `src/platform/linux/runtime_environment.hpp/.cpp`; `src/platform/linux/system_diagnostics.hpp/.cpp`; `src/platform/linux/network_diagnostics.hpp/.cpp`; `src/platform/logging/logger.hpp/.cpp`; `src/platform/linux/runtime_config.hpp/.cpp`; `src/platform/linux/command_line.hpp/.cpp`; `src/platform/linux/runtime_config_file.hpp/.cpp`; в дальнейшем — `repka/dockstation-gateway.service.in` и `repka/gateway.conf.example`.

**Согласованная система сборки:** CMake используется для отдельной Linux-цели. Текущая корневая конфигурация собирает в статическую библиотеку `dockstation_linux_runtime` восемь компонентов: `LinuxApplicationBootstrap`, `RuntimeEnvironment`, `SystemDiagnostics`, `NetworkDiagnostics`, `PlatformLogger`, `RuntimeConfig`, `CommandLineParser` и `RuntimeConfigFileLoader`. Для потокобезопасного PlatformLogger CMake явно находит и подключает `Threads::Threads`; зависимость транзитивно передаётся потребителям статической библиотеки. Отдельная исполняемая цель `dockstation_linux_bootstrap` использует `repka_gateway/main.cpp`. В CTest зарегистрировано 10 проверок, в том числе `test_application_bootstrap`, `test_runtime_environment`, `test_system_diagnostics`, `test_network_diagnostics`, `test_logger`, `test_runtime_config`, `test_command_line` и `test_runtime_config_file`. В штатной точке входа `main()` передаёт ServiceRunner в ApplicationBootstrap; ServiceRunner запускает C++ read-only HTTP API на `127.0.0.1`. Bootstrap-only режим без ServiceRunner сохранён для самостоятельной проверки окружения и жизненного цикла. Он не запускает HTTP API. Существующие `tests/` и `tools/` не меняются.

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

Загрузчик внешней конфигурации RuntimeConfig реализован как отдельный read-only компонент. C++ HTTP/API-сервер уже подключён к штатной точке входа через ServiceRunner и вручную проверен в Ubuntu VM. Добавлены systemd unit template, пример конфигурации и CMake install-правила; CI проверяет unit через `systemd-analyze verify` и запускает установленный Gateway от имени отдельного непривилегированного пользователя. Фактический lifecycle через systemd manager вручную проверен в пользовательской Ubuntu 24.04.5 LTS x86_64 VM: enable/start/stop/restart, journal и автозапуск после reboot пройдены. Production-адаптеры физических устройств, постоянное хранилище, источники событий, реальный Timer и композиция доменных служб остаются отдельными задачами; они не имитируются текущим Bootstrap.

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

Корневой CMake включает LinuxApplicationBootstrap и семь платформенных компонентов, собирает исполняемую цель `dockstation_linux_bootstrap` и регистрирует 10 проверок CTest. Актуальный GitHub Actions run [43516203836](https://github.com/GooseRabbit/dockstation/actions/runs/43516203836) завершился SUCCESS: Linux build, 10/10 CTest с ASan/UBSan, 31/31 C++ host programs и 130 pytest. Целевая сборка ARM64 и испытание на Repka Pi 5 не выполнялись. C++ Bootstrap запускает read-only HTTP API через ServiceRunner; Python Gateway остаётся отдельным временным VM-прототипом.

**Реализованный пятый срез — RuntimeConfig:** добавлены `src/platform/linux/runtime_config.hpp/.cpp` и `verification/repka_gateway/test_runtime_config.cpp`. Контракт задаёт безопасные значения по умолчанию (API port 8080, bind address фиксирован как `127.0.0.1`) и проверяет диапазон порта, параметры PlatformLogger, наличие хотя бы одного sink, корректность уровня, размера файла и количества архивов. `validate_runtime_config()` не открывает файлы/сокеты и не запускает службы; открытие настроенного файлового sink выполняется только при последующем вызове `initialize_runtime_logger()`. Добавлена `initialize_runtime_logger()`: она выполняет валидацию до инициализации PlatformLogger; при ошибке конфигурации журналирование не запускается, при успешной валидации результат инициализации Logger возвращается отдельно. Файл `.conf` читается только по явно заданному `--config-file`; автоматический системный путь и переменные окружения не используются. Порядок настроек: значения по умолчанию → файл → явно заданные параметры CLI. Итоговый RuntimeConfig валидируется до инициализации Logger. Секретные параметры и SSD/storage-настройки в этот контракт не включены.

**Реализованный шестой срез — CommandLineParser:** добавлены `src/platform/linux/command_line.hpp/.cpp` и `verification/repka_gateway/test_command_line.cpp`. Парсер поддерживает только параметры, уже представленные в RuntimeConfig: `--api-port`, `--log-level`, `--log-file`, `--log-stderr`/`--no-log-stderr`, `--log-max-file-size-bytes`, `--log-max-rotated-files` и отдельный `--help`. Повторные/неизвестные параметры, неверные значения и отсутствие значения отклоняются. Парсер возвращает маску явных CLI overrides; при указанном `--config-file` межполевую проверку откладывает до объединения настроек. Сам парсер не читает файлы и не запускает службы. Адрес привязки остаётся фиксированным `127.0.0.1`.

**Реализованный седьмой срез — RuntimeConfigFileLoader:** добавлены `src/platform/linux/runtime_config_file.hpp/.cpp` и `verification/repka_gateway/test_runtime_config_file.cpp`. Формат — строгий текстовый `.conf` с ASCII-ключами `key=value`, пустыми строками и комментариями, начинающимися с `#`; допускается UTF-8 BOM. Поддерживаются `api_port`, `log_level`, `log_file`, `log_stderr`, `log_max_file_size_bytes`, `log_max_rotated_files`. Неизвестные и повторные ключи, пустые значения, некорректные типы/диапазоны и NUL отклоняются; файл ограничен 65536 байтами и должен быть обычным файлом. Результаты NOT_FOUND, READ_ERROR и INVALID различаются. Кавычки и inline-комментарии не имеют специальной семантики. Файл не содержит секретов и не ищется автоматически. Приоритет: defaults → file → явно заданные CLI overrides. Bootstrap проверяет итоговую конфигурацию до запуска Logger. Unit-тест проверяет разбор, ошибки, ограничение размера, чтение файла и приоритет CLI; тест Bootstrap проверяет загрузку файла и передачу итоговой конфигурации службе.

**Результат этапа 1:** отдельное Linux-приложение собирается, запускается, выдаёт read-only системную и сетевую диагностику и корректно завершается. Исполняемая цель подключает HTTP-службу через ServiceRunner; её два GET-маршрута и совместимость с Operator описаны в этапе 2. C++ Gateway вручную проверен на Ubuntu 24.04.5 LTS x86_64 VM. Старый host-testbench не меняется; ручной systemd manager lifecycle в пользовательской Ubuntu VM проверен. Целевая сборка ARM64 и аппаратная проверка Repka Pi 5 остаются открытыми. Установочный systemd unit и CI-проверка непривилегированного запуска реализованы.

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

**Текущий статус:** C++ API реализован; `GET /api/v1/health` и `GET /api/v1/system/diagnostics` вручную вернули HTTP 200 на Ubuntu 24.04.5 LTS x86_64 VM. Windows Operator получил те же данные через SSH local-forward; проверены совпадение RAM/корневой файловой системы с округлением, автоматическое восстановление API-обмена после перезапуска Gateway и bind только на `127.0.0.1:8080`. Подробный протокол приведён в docs/30_REPKA_NETWORK_IMPLEMENTATION_PLAN.md. Это не ARM64/Repka Pi 5, systemd или аппаратное испытание.


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

**Файлы текущей реализации:**
- `operator/pyproject.toml` — пакет, зависимости и отдельные GUI entry points.
- `operator/src/dockstation_operator/main.py` — сохранённый Tkinter entry point `dockstation-operator`.
- `operator/src/dockstation_operator/qml_app.py` — QML entry point `dockstation-operator-qml`.
- `operator/src/dockstation_operator/config/settings.py` и `config/station_registry.py` — профиль соединения и локальный реестр лабораторных профилей.
- `operator/src/dockstation_operator/connection/connection_manager.py` и `connection/ssh_tunnel.py` — управление SSH/API-сеансом и OpenSSH local-forward.
- `operator/src/dockstation_operator/api/station_api_client.py` — проверенный read-only API-клиент и opt-in тестовый PING.
- `operator/src/dockstation_operator/models/diagnostics_model.py` — последний диагностический снимок, состояние актуальности и события клиента.
- `operator/src/dockstation_operator/ui/qml_bridge.py` — мост Qt/QML, профили, диагностика, фоновые операции и автоматическое восстановление связи.
- `operator/src/dockstation_operator/ui/qml/Main.qml` — текущий семивкладочный QML-интерфейс.

В прототипе пока нет отдельных `application.py`, `api_models.py`, ViewModel-каталога и раздельных QML-файлов страниц: соответствующая логика находится в перечисленных существующих файлах. Не создавать эти модули только ради совпадения с предварительным эскизом; выделение выполнять при появлении конкретной необходимости.

Python + PySide6 + QML — выбранный для текущего прототипа технологический вариант; продуктовая архитектура и все целевые сценарии Operator ещё не завершены.

**Точки входа:**
- `main.py:main()` — сохраняет прежний Tkinter entry point.
- `qml_app.py:main()` — запускает текущий QML-вариант через PySide6 и `QmlOperatorBridge`.
- Отдельного `application.py` в текущей реализации нет; жизненный цикл QML-прототипа пока сосредоточен в `qml_app.py`.

**`qml_app.py`:**
- `main()` — создаёт `QGuiApplication`, `QQmlApplicationEngine`, регистрирует `QmlOperatorBridge` и загружает `ui/qml/Main.qml`.

**`ssh_tunnel.py`:**
- `find_ssh_executable()`, `find_ssh_keygen_executable()`, `verify_known_host()`, `build_ssh_command()`, `is_local_port_available()`.
- `SshTunnel.start()`, `wait_for_process_or_stop()`, `stop()`.

**`connection_manager.py`:**
- `connect_to_station()`, `disconnect_from_station()`, `get_connection_status()`, `get_tunnel_status()`, `check_connection_health()`, `get_system_diagnostics()`, `handle_connection_loss()`, `reconnect_connection()`.
- `reconnect_connection()` повторно использует сохранённый профиль и создаёт новый SSH/API-сеанс. Ограниченные автоматические интервалы повторов и отмену ожидания координирует QML bridge.

**`station_api_client.py`:**
- `get_health()`, `get_system_diagnostics()`, `validate_health_response()`, `validate_diagnostics_response()`, `submit_test_adapter_request()`, `validate_test_adapter_response()`, `map_api_error()`.

**`diagnostics_model.py`:**
- `apply_diagnostics()`, `mark_diagnostics_stale()`, `clear_connection_state()`, `append_client_event()`, `diagnostics_age_seconds()`, `format_diagnostic_value()`.

**`qml_bridge.py` и `Main.qml`:**
- `connectToStation()`, `poll()`, `refresh()`, `disconnectFromStation()`, `_schedule_reconnect()`, `_reconnect_operation()`, `_on_operation_finished()`.
- `QmlOperatorBridge` предоставляет QML статусы соединения, актуальность снимка, системные метрики, сетевые показатели, события и локальные профили. Длительные SSH/API-операции выполняются в фоновом потоке.
- `Main.qml` содержит семь вкладок и таймер периодического опроса. Полноэкранный просмотр камеры открывается только по явному действию; видеопоток остаётся недоступным до реализации реального camera pipeline.

Повторное подключение относится только к восстановлению канала и не повторяет физические команды.

**Работа над дизайном начинается вместе с первым GUI, а не откладывается до завершения функций.** В docs/23 подготовлены пользовательские сценарии, информационная архитектура, карта навигации, макеты ключевых экранов и проект дизайн-системы. Основное визуальное направление владелец согласовал 2026-11-13; подробная информационная архитектура и полный набор продуктовых сценариев остаются предметом дальнейшей проверки и уточнения. Первый QML-прототип реализован и проверен на типовом Windows-экране; дальнейшее развитие ведётся по фактическим результатам использования.

**Текущий QML-срез:** экран лабораторного SSH-подключения, обзор опубликованной Linux/API-диагностики, сеть, события клиента, камера-заглушка и отдельная вкладка «Система». API и Operator отображают только реально собранные показатели: ОС, ядро, архитектуру, uptime, RAM, корневую файловую систему, CPU/SoC temperature при наличии явно маркированного термодатчика, load average, интерфейсы/адреса, default routes, DNS и статус SSH-службы. Системные ресурсы передаются в `platform.resources`; каждый показатель имеет независимый статус, а недоступные числовые значения остаются null и не подменяются нулём. Load average не обозначается процентом загрузки CPU. Полная целевая система экранов и границы первого среза описаны в docs/23 §9.4 и §9.6. В VM-срез не включаются продуктовая авторизация, карта, БАС, физические датчики/команды, реальный видеопоток, передача файлов и администрирование. Отсутствующие датчики будущей конфигурации должны отображаться честно, если они появятся в данных контракта. Секреты и приватные ключи не копируются в настройки приложения; SSH host key проверяется строго.

**Текущее состояние:** сохранён прежний Tkinter entry point `dockstation-operator`; отдельный QML entry point `dockstation-operator-qml` реализован в PySide6/QML. Первый QML-срез содержит семь вкладок: подключение, климат, датчики/крышка, сеть, логи, камера и система. Владелец проверял приложение на Windows; в GitHub Actions на Ubuntu 24.04 проверялись headless-загрузка QML, editable install, GUI entry point и наличие QML-ресурса в пакете (93 pytest в прогоне 43378693068). Это не подтверждает полноценную приёмку Windows runtime или реального SSH-подключения. Автоматическое восстановление SSH/API добавлено в текущий код, но требует актуального CI и сквозного испытания с остановкой/повторным запуском Gateway в Ubuntu VM. **Результат этапа** остаётся открытым до успешной проверки актуальной QML-версии на Windows и фактического reconnect-сценария.

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

**Первый реализованный VM-вертикальный срез (DEC-061):** в существующем временном Python Gateway добавлен отключённый по умолчанию маршрут `POST /api/v1/test/adapter/request`. Windows Operator API client отправляет только `PING`; Gateway вызывает `TestRequestService`, затем `TestRequestAdapter`, и возвращает `TEST_ADAPTER_ACCEPTED`, `source=TEST_ADAPTER`, `physical_action_performed=false`. Маршрут включается только явным `--enable-test-adapter`. Добавлена сквозная host-проверка через реальный loopback HTTP-сервер и Operator API client.

Это подтверждает только программный путь в VM-прототипе. Это не C++ Gateway, не запуск через LinuxApplicationBootstrap, не production API и не аппаратная интеграция. Физические показания и выполнение не имитируются; штатные GET-маршруты диагностики не изменены.

**Результат этапа:** бизнес-логика работает через реальные программные контракты; тестовый адаптер подтверждает только приём безопасного PING без физического исполнения.

## Этап 5. Постоянное хранение и интеграция текущего ядра

**Цель:** подключить реальные адаптеры хранения к уже реализованным логическим контрактам StationStateManager и ConfigurationManager, не переписывая их чистую бизнес-логику. Обеспечить атомарность записей, защиту от повторной обработки, сохранение курсоров источников и безопасное восстановление после перезапуска.

**Фактическая граница на редакцию 1.36:** логические функции F01–F17 и CFG-F00…CFG-F08, а также перечисленные ниже интерфейсы портов уже существуют и покрыты host-тестами с fake-адаптерами. Production-адаптеров постоянного хранилища пока нет: каталога `src/platform/persistence/` в текущем дереве ещё нет. SQLite остаётся кандидатом, но не утверждённым выбором.

### 5.1. Карта архитектурных слоёв

```
StationStateManager / ConfigurationManager
        |
        | существующие логические порты (interfaces)
        v
Persistence adapters / Clock / Timer / ID providers
        |
        | одна согласованная транзакционная граница
        v
Выбранное постоянное хранилище
        |
        +-- snapshot и история переходов
        +-- операции и их lifecycle
        +-- принятые события и курсоры источников
        +-- маркер восстановления
        +-- запись конфигурации станции
```

Ядро не должно знать, используется ли внутри SQLite, другой движок или файловый адаптер. Один физический адаптер может реализовать несколько логических портов, чтобы связанные изменения фиксировались одной транзакцией. Это не требует объединять публичные интерфейсы ядра или создавать один монолитный класс.

**Планируемые файлы адаптера (пока не созданы):** `src/platform/persistence/database.hpp/.cpp`, `schema_migrations.hpp/.cpp`, адаптеры репозиториев для событий/состояния/операций/recovery/configuration; `src/platform/time/system_clock.hpp/.cpp`, `monotonic_clock.hpp/.cpp`, `timer_service.hpp/.cpp`; реализация поставщиков ID в `src/platform/ids/`. Это ориентир для будущих минимальных файлов, а не утверждение, что каталоги или классы уже существуют.

Системный журнал Gateway (stderr → journald) остаётся отдельным от долговременных данных SSM. `EventRepository` хранит нормализованные события, необходимые для дедупликации и доказательства изменений; он не становится хранилищем сырых сетевых/аппаратных логов. Конфигурация процесса Linux Gateway из `/etc/dockstation/gateway.conf` (порт и параметры журналирования) также не является конфигурацией станции CFG-F00…CFG-F08.

### 5.2. Карта существующих портов и требуемых адаптеров

| Область | Существующий логический контракт | Что должен обеспечить постоянный адаптер |
|---|---|---|
| Намерение операции — F07 | `StateRepository::save_operation_intent()` (`src/core/operations.hpp`) | Уникальный `operation_id`; атомарная новая запись `ActionRequest` и `OperationRecord` со статусом REQUESTED и revision=1; точный повтор возвращает AlreadySavedSame, тот же ID с другими полями — Conflict |
| Lifecycle операции — F09/F11 | `OperationUpdateRepository::find_operation()`, `find_by_id()`, `commit_operation_update()` (`src/core/operations.hpp`) | Условная фиксация по ожидаемой revision/записи операции; одной транзакцией сохранить новый lifecycle, исходное событие для дедупликации и новый cursor источника |
| Глобальное состояние — F10 | `StationStateRepository::commit_transition()` (`src/core/state_transitions.hpp`) | Сверить ожидаемые state/revision и все поля cursor; проверить уникальность event_id/transition_id; одной транзакцией сохранить новый snapshot, историю переходов, исходный Event и продвинутый cursor. Частичная запись запрещена |
| Чтение состояния — F16 | `StationStateSnapshotReader::read_snapshot()` (`src/core/state_transitions.hpp`) | Возвращать state, revision и transition_id из одного согласованного снимка; отсутствие, повреждение и ошибка чтения должны различаться |
| История переходов — F17 | `TransitionHistoryReader::read_transition_history()` (`src/core/state_transitions.hpp`) | Возвращать полный набор подходящих записей в порядке revision либо явно сообщать Incomplete/Error; нельзя выдавать частичную историю за полную |
| Дедупликация — F03/F13 | `EventRepository::find_by_id()` (`src/core/event_processing.hpp`) | Читать ранее сохранённый Event и отличать NotFound от Error. Самостоятельного append-контракта у этого интерфейса нет: запись события выполняется внутри атомарных методов профильных репозиториев F09/F10 |
| Начало восстановления — F14 | `RecoveryRepository::load_snapshot()`, `mark_recovery_in_progress()` (`src/core/recovery.hpp`) | Прочитать последний подтверждённый snapshot и атомарно сравнить его state/revision с ожидаемым снимком, устойчиво установить идемпотентный маркер RECOVERY_IN_PROGRESS |
| Загрузка конфигурации — CFG-F07 | `SettingsRepository::read_current()`, `ConfigurationDecoderPort::decode()` (`src/configuration/configuration_ports.hpp`) | Вернуть байты одной сохранённой записи; обеспечить срок жизни backing buffer для декодера и всех созданных им View/Snapshot; CFG-F07 выполняет read → decode → F05 → F06 и публикует snapshot только при VALID |
| Выдача ID операций/переходов | `OperationIdProvider::next_operation_id()` (`src/core/operations.hpp`); `TransitionIdProvider::next_transition_id()` (`src/core/global_transition_processor.hpp`) | Обеспечить неповторяемость ID в пределах истории станции и после reboot. Формат и алгоритм генерации не выбраны; до их выбора не встраивать UUID, счётчик или иной вариант в ядро |
| Время и таймеры | Логическая граница Clock/Timer описана в docs/17 и этапе 5; production-реализации пока нет | Реализовать монотонное время, планирование/отмену таймеров и доставку истёкших событий через общий event pipeline. Отдельно определить поведение таймеров при reboot |

`OperationRepository` из предварительного списка файлов этапа 5 пока не представлен отдельным интерфейсом в `src/core`: чтение/обновление существующей операции входит в `OperationUpdateRepository`, а список незавершённых операций понадобится для восстановления. Не создавать дублирующий порт до уточнения состава методов и границы ответственности.

### 5.3. Какие изменения обязаны быть атомарными

Атомарность здесь — не просто «записать несколько таблиц подряд». Для каждого перечисленного случая после сбоя должен существовать либо полный старый результат, либо полный новый результат, но не смесь.

| Функция | Единица атомарной фиксации | Запрет при неопределённом/ошибочном результате |
|---|---|---|
| F07 — сохранить намерение | Новая операция REQUESTED + revision=1 + полный ActionRequest по operation_id | Не передавать запрос ActionDispatcher, пока долговременное сохранение не подтверждено |
| F09/F11 — обновить операцию | Проверка ожидаемой операции/revision и cursor + новый lifecycle/revision + Event + продвижение cursor | Не продвигать статус или cursor частично; не повторять физическую команду при неизвестном результате |
| F10 — подтвердить переход | Проверка ожидаемого snapshot/revision и cursor + уникальность ID + новый snapshot + история + Event + продвижение cursor | Не публиковать StateChanged до подтверждённого commit; при неизвестном результате не выполнять commit вслепую повторно |
| F14 — начать recovery | Проверка snapshot + устойчивый маркер RECOVERY_IN_PROGRESS | Ошибка или конфликт не должны трактоваться как успешное начало восстановления |
| CFG-F07 — загрузить конфигурацию | Один согласованный прочитанный record; затем независимые decode/validation этапы | Не публиковать частичный или невалидный snapshot |

F03 выполняет предварительный read-only поиск event_id, а F13 передаёт событие профильному обработчику. Такая предварительная проверка не защищает от конкурентной гонки сама по себе: F09/F10 обязаны повторно проверить event_id и cursor внутри транзакции вместе с профильной записью. Одна лишь успешная проверка `find_by_id()` не разрешает побочный эффект.

### 5.4. Пробелы контрактов, которые нужно закрыть до реализации адаптера

1. **Чтение сохранённого cursor и доверенное подтверждение source session.** Read-only порт `SourceCursorReader` и проверка формы результата уже реализованы (§5.4.1). Отдельный контракт `SourceSessionVerifier` + `SourceSessionRepository` согласован на уровне архитектуры и описан в §5.4.2. Production-адаптеры постоянного хранения, реестр зарегистрированных источников, composition-root wiring и атомарная реализация пока отсутствуют. Ключ чтения — точный непустой `source`; первая/новая сессия подтверждается отдельно и никогда не выводится из входящего события.
2. **Полный список незавершённых операций.** F15 получает массив `unfinished_operations` во входе `RecoveryReports`, но текущий `RecoveryRepository` не предоставляет метода чтения такого списка. Нужен согласованный read-контракт, вероятно связанный с будущим OperationRepository.
3. **Завершение recovery.** F14 умеет устойчиво установить RECOVERY_IN_PROGRESS, а F15 только возвращает результат сверки. Сейчас нет порта/функции, которая атомарно фиксирует итог успешной сверки и снимает/закрывает recovery marker. До реализации нужно определить допустимые исходы, правила для неполных/небезопасных отчётов и связь с согласованным переходом SSM; нельзя просто очищать маркер по тайм-ауту или по факту запуска процесса.
4. **Сбор отчётов для F15.** `RecoveryReportRequester::request_fresh_reports()` подтверждает только принятие запроса; фактический агрегатор полных `RecoveryReports` и его связь с `unfinished_operations` являются отдельной интеграционной частью.
5. **Уникальные ID после reboot.** Провайдеры ID уже являются интерфейсами, но реализаций нет. Нужно выбрать и проверить стратегию, гарантирующую отсутствие повторного operation_id/transition_id после аварийного выключения; формат ID остаётся открытым.
6. **Событие, породившее F07.** Текущий `save_operation_intent(ActionRequest)` сохраняет только намерение операции и не принимает исходное Event/cursor. Не расширять контракт молча. До интеграции входящих команд определить, должна ли регистрация команды и её source cursor входить в одну транзакцию с созданием REQUESTED.
7. **Запись конфигурации.** Существующий `SettingsRepository` и CFG-F07 обеспечивают только read-only загрузку. CFG-F00…F08 не реализуют save/apply, а docs/28 оставляет запись, миграции и авторизацию изменения вне первого среза. Не добавлять `write_configuration_record()` без отдельного контракта, политики и аудита.
8. **Время после reboot.** Для тайм-аутов и календарных timestamp нужно отдельно зафиксировать, что измеряется монотонными часами, что хранится как UTC-время и как пересчитываются/обрабатываются незавершённые таймеры после перезапуска. Нельзя продолжать countdown на основании неподтверждённого значения часов.

### 5.4.1. Контракт чтения SourceCursor

**Статус: СОГЛАСОВАНО владельцем и реализовано как логический read-only порт. Production adapter постоянного хранения ещё не реализован.**

Текущий факт: StationEventPipeline::handle(event, validation_policy, evidence) принимает SourceSessionEvidence, а не произвольный cursor. Pipeline сначала выполняет чистые F01/F02 preflight-проверки, затем через SourceSessionCoordinator вызывает verifier → SourceCursorReader → SourceSessionRepository. Только Confirmed/AlreadyCurrent запускают дальнейшую F03/F04/F13 обработку. Coordinator передаёт подтверждённый cursor внутрь handle_event и профильного обработчика. F09/F11 и F10 внутри атомарного commit сравнивают ожидаемый cursor с сохранённым и одновременно продвигают его при записи операции/перехода, события и результата. Production reader/repository adapters постоянного хранилища и реальный transport context пока отсутствуют.

**Реализованный минимальный read-only интерфейс** (production adapter пока отсутствует):

```cpp
enum class SourceCursorReadStatus : std::uint8_t {
    Found,    // Для source есть сохранённый cursor.
    NotFound, // Для source ещё нет сохранённой записи.
    Error     // Чтение не удалось или результат нельзя подтвердить.
};

struct SourceCursorReadResult {
    SourceCursorReadStatus status{SourceCursorReadStatus::Error};
    SourceCursor cursor{};
};

class SourceCursorReader {
public:
    virtual ~SourceCursorReader() = default;

    [[nodiscard]] virtual SourceCursorReadResult read_source_cursor(
        std::string_view source) noexcept = 0;
};

// Проверяет согласованность формы ответа до использования cursor вызывающим слоем.
[[nodiscard]] bool is_valid_source_cursor_read_result(
    std::string_view requested_source,
    const SourceCursorReadResult& result) noexcept;
```

**Семантика:**
- Ключом поиска является точное непустое значение `source`. Один результат содержит `source`, optional `source_instance` и optional `last_sequence` из одной согласованной записи, не из разных чтений.
- `Found`: `cursor.source` обязан в точности совпадать с запрошенным ключом. `source_instance` и `last_sequence` возвращаются без нормализации. Отсутствующий `last_sequence` допустим только для ещё не продвинутого, заранее установленного/подтверждённого курсора.
- `NotFound`: записи для source нет; payload результата должен быть пустым/default. Это не разрешение молча подставить курсор из произвольного входящего события.
- `is_valid_source_cursor_read_result()` принимает только непустой ключ, точный `cursor.source` при `Found` и пустой payload при `NotFound`/`Error`. Неизвестный enum и противоречивые сочетания отклоняются. Структурно корректный `Error` всё равно блокирует обработку и не превращается в `NotFound`, `NEW` или нулевой sequence.
- Порт ничего не создаёт, не сбрасывает и не продвигает. Единственная фиксация нового `last_sequence` остаётся частью уже предусмотренной атомарной операции F09/F10.
- Строки внутри `SourceCursor` — невладеющие `string_view`. Адаптер обязан гарантировать срок жизни backing memory как минимум до завершения обработки события и вызванного профильного commit; параллельное чтение не должно инвалидировать уже возвращённый снимок.
- Для одного source конкурентные события могут прочитать один и тот же cursor. Только один commit может пройти compare-and-advance; второй получает `SourceCursorChanged` / `ConcurrentSourceUpdate`, затем вызывающий слой повторно читает cursor и заново проверяет событие. Сам репозиторий и F13 не делают слепой retry.

**Обработка отсутствующего cursor и смены сессии — открытая зависимость:**
- Если `read_source_cursor(source)` вернул `NotFound`, вызывающий слой должен получить доверенное подтверждение регистрации источника и текущей сессии, прежде чем строить исходный `SourceCursor`. Без такого подтверждения событие не обрабатывается.
- Если сохранённый `source_instance` отличается от входящего, F04 по текущему контракту возвращает `UNORDERED / SourceInstanceMismatch`. Нельзя автоматически сбрасывать sequence или заменять instance на значение из события: это может скрыть запоздавшее сообщение старой сессии.
- Нужен отдельный согласуемый контракт доверенного подтверждения первой/новой сессии, который атомарно устанавливает `source_instance` и очищает `last_sequence`. Он не входит в read-only `SourceCursorReader` и не предлагается к реализации этим первым срезом.

**Текущая проверка интерфейса:** `verification/regression/test_source_cursor_reader.cpp` зарегистрирован в CTest как `source_cursor_reader`; 25 host-проверок используют только тестовую in-memory реализацию, владеющую строками. Они покрывают default Error, Found с точными полями, допустимый курсор без sequence для подтверждённой сессии, различие NotFound/Error, пустой payload, отклонение чужого source/пустого ключа/неизвестного enum/противоречивого ответа и сохранение ранее выданных string_view после повторного чтения. Это не production adapter и не проверка постоянного хранилища.

**Остаётся проверить на production adapter и интеграции:**
1. Чтение source/instance/sequence из одной реальной записи хранилища.
2. Отсутствие изменения cursor при read; видимость нового cursor после успешного F09/F10 commit и неизменность после rollback/conflict.
3. Поведение после закрытия и повторного открытия процесса/хранилища.
4. Конкурентный commit по устаревшему expected_cursor и последующее перечитывание фактически сохранённого значения.
5. Действительность string_view до полного завершения профильного handler и commit; параллельные чтения не инвалидируют предыдущий результат.
6. Смена source_instance без отдельного подтверждения остаётся UNORDERED; отсутствие source_sequence остаётся UNORDERED.

Этот срез закрывает только публичный read-only порт и проверку формы его ответа. Он не создаёт production adapter, не подключает чтение cursor к F13/composition root и не решает регистрацию неизвестного источника, подтверждение новой сессии, декодирование payload, транспорт, формат ID или выбор физического хранилища. Продвижение cursor по-прежнему выполняют только атомарные F09/F10 commit-порты.

### 5.4.2. Доверенная регистрация источника и подтверждение source session

**Статус: архитектура и публичные C++-сигнатуры согласованы владельцем 2026-11-14. Логические порты, validators и SourceSessionCoordinator реализованы; Coordinator интегрирован в StationEventPipeline. Production verifier/registry/storage adapters, реальный transport context и production composition root ещё не реализованы.**

#### Разделение идентификаторов

В обработке событий различаются три независимых значения:

- **Station ID** — постоянный идентификатор физической станции. Он не является секретом, паролем или самостоятельным доказательством подлинности устройства.
- **source** — логический идентификатор зарегистрированного источника данных внутри станции, например контроллера ESP32.
- **source_instance** — идентификатор одного конкретного запуска/сеанса этого источника. Он должен оставаться стабильным в пределах сеанса и изменяться при новом запуске, если источник поддерживает такую идентичность.

`source_instance` не является ключом, сертификатом или доказательством подлинности. Простая подмена этого поля во входящем событии не подтверждает новую сессию.

#### Два разных этапа доверия

**1. Регистрация источника.** До приёма событий логический `source` должен существовать в доверенном реестре и быть связан с конкретным Station ID и доверенной ссылкой на аппаратную/криптографическую идентичность контроллера. Для записи источника учитываются состояние `ACTIVE`, `DISABLED` или `REVOKED` и ревизия регистрации. Первое входящее событие не создаёт регистрацию и не может самостоятельно добавить неизвестный контроллер в доверенный список.

Создание регистрации, привязка ключа/сертификата, физическое подтверждение владения устройством и аудит относятся к процессу provisioning из docs/24 и docs/25. Конкретная процедура, алгоритмы, носитель ключа и физический механизм активации остаются открытыми.

**2. Подтверждение текущей сессии.** После установления канала отдельный verifier проверяет, что источник зарегистрирован и активен, предъявленная идентичность соответствует привязке регистрации, доказательство владения действительно для этого источника, а challenge/session proof свежий и не воспроизводит ранее принятую сессию. Точный протокол challenge-response, криптография и транспорт не выбираются этим контрактом.

Для этих двух обязанностей предусмотрены отдельные логические порты:

- **`SourceSessionVerifier`** — проверяет доверие к источнику и конкретной новой сессии. Кандидатные статусы результата: `Verified`, `UnknownSource`, `Rejected`, `Unavailable`, `Error`. При `Verified` возвращается подтверждённая связка `source`, `source_instance` и ревизия регистрации, для которой выполнялась проверка. Результат не извлекается из тела Event и не может быть создан по одному лишь значению Station ID или `source`. `VerifiedSourceSession` является внутренним доверенным значением, но не bearer credential; только доверенный координатор передаёт его в repository непосредственно из результата verifier-а. Его нельзя принимать из Event, API или пользовательского ввода.
- **`SourceSessionRepository`** — фиксирует уже подтверждённую сессию и её стартовый cursor. Кандидатные статусы: `Confirmed`, `AlreadyCurrent`, `CursorChanged`, `RegistrationChanged`, `Error`. Метод принимает ожидаемый результат `SourceCursorReader` (`Found` либо `NotFound`) и подтверждённые verifier-ом данные. `Error`, неизвестный статус или структурно некорректный результат чтения блокируют фиксацию.

Verifier отвечает за проверку доказательства и связывание peer identity с регистрацией. Repository отвечает за согласованную транзакцию и не реализует криптографию. Публичные C++-сигнатуры ниже утверждены владельцем; рабочая координация этих портов вынесена в отдельный SourceSessionCoordinator.

**Утверждённые публичные сигнатуры и реализованный логический контракт:**

```cpp
class SourceSessionEvidence final {
public:
    SourceSessionEvidence(const SourceSessionEvidence&) noexcept = default;
    SourceSessionEvidence& operator=(const SourceSessionEvidence&) noexcept = default;
    [[nodiscard]] std::string_view source() const noexcept;
    [[nodiscard]] std::string_view source_instance() const noexcept;
    [[nodiscard]] std::string_view peer_identity_evidence() const noexcept;
    [[nodiscard]] std::string_view session_proof() const noexcept;
private:
    friend class dockstation::communications::AuthenticatedEventTransportAdapter;
    SourceSessionEvidence(std::string_view, std::string_view,
                          std::string_view, std::string_view) noexcept;
};

enum class SourceSessionVerificationStatus : std::uint8_t {
    Verified, UnknownSource, Rejected, Unavailable, Error
};

// Trusted verifier output, not a bearer credential.
// Only the trusted coordinator passes it to SourceSessionRepository.
struct VerifiedSourceSession {
    std::string_view source;
    std::string_view source_instance;
    std::string_view registration_revision;
};

struct SourceSessionVerificationResult {
    SourceSessionVerificationStatus status{
        SourceSessionVerificationStatus::Error
    };
    VerifiedSourceSession session{};
};

class SourceSessionVerifier {
public:
    virtual ~SourceSessionVerifier() = default;
    [[nodiscard]] virtual SourceSessionVerificationResult
    verify_source_session(
        const SourceSessionEvidence& evidence) noexcept = 0;
};

enum class SourceSessionConfirmationStatus : std::uint8_t {
    Confirmed, AlreadyCurrent, CursorChanged, RegistrationChanged, Error
};

struct SourceSessionConfirmationResult {
    SourceSessionConfirmationStatus status{
        SourceSessionConfirmationStatus::Error
    };
    SourceCursor cursor{};
};

class SourceSessionRepository {
public:
    virtual ~SourceSessionRepository() = default;
    [[nodiscard]] virtual SourceSessionConfirmationResult
    confirm_source_session(
        const SourceCursorReadResult& expected_cursor,
        const VerifiedSourceSession& verified_session) noexcept = 0;
};
```

Результат `Confirmed` возвращает новый cursor с подтверждённым instance и пустым last_sequence. `AlreadyCurrent` возвращает без изменений полный прежний cursor, включая last_sequence. При CursorChanged, RegistrationChanged и Error payload пуст. `is_valid_source_session_verification_result()` и `is_valid_source_session_confirmation_result()` проверяют согласованность status/payload и точную привязку к исходному доказательству/ожидаемому cursor. Все строки являются невладеющими `string_view`; verifier и repository обязаны обеспечивать их действительность на срок использования результата.

#### Реализованный SourceSessionCoordinator

Утверждённый публичный контракт:

```cpp
enum class SourceSessionCoordinatorStatus : std::uint8_t {
    Confirmed, AlreadyCurrent, InvalidEvidence, UnknownSource, Rejected,
    Unavailable, VerificationError, CursorReadError, CursorChanged,
    RegistrationChanged, RepositoryError
};

struct SourceSessionCoordinatorResult {
    SourceSessionCoordinatorStatus status{
        SourceSessionCoordinatorStatus::VerificationError
    };
    SourceCursor cursor{};
};

class SourceSessionCoordinator final {
public:
    SourceSessionCoordinator(
        SourceSessionVerifier& verifier,
        SourceCursorReader& cursor_reader,
        SourceSessionRepository& repository) noexcept;

    [[nodiscard]] SourceSessionCoordinatorResult confirm(
        const Event& event,
        const SourceSessionEvidence& evidence) noexcept;
};
```

Порядок выполнения в `confirm()` фиксирован:

1. Проверить обязательные поля `SourceSessionEvidence`.
2. Сверить точные `event.source` и `event.source_instance` с `evidence.source` и `evidence.source_instance`. Несовпадение или отсутствие instance возвращает `InvalidEvidence` до вызова любого порта.
3. Вызвать `SourceSessionVerifier`. Только структурно корректный `Verified` продолжает последовательность; `UnknownSource`, `Rejected`, `Unavailable`, `Error` и неизвестные/противоречивые ответы закрывают обработку.
4. Прочитать курсор для подтверждённого source через `SourceCursorReader`. Ошибка, неизвестный статус или противоречивый payload возвращают `CursorReadError`; `Error` не превращается в `NotFound`.
5. Передать результат чтения и полученный непосредственно от verifier-а `VerifiedSourceSession` в `SourceSessionRepository`. Только корректные `Confirmed` и `AlreadyCurrent` возвращают cursor. Конфликты и ошибки возвращаются без cursor; автоматического retry нет.

Координатор не владеет адаптерами, не реализует криптографию или постоянное хранение и не продвигает `last_sequence`. Он уже встроен в `StationEventPipeline`: вызывающий транспортный адаптер передаёт Event и SourceSessionEvidence из одного аутентифицированного контекста, а Pipeline сам получает подтверждённый cursor и передаёт его в `handle_event()`/профиль. Production channel adapter и production composition root ещё не реализованы.

#### Порядок входного Pipeline и повторная проверка канала

До вызова Coordinator Pipeline выполняет чистые F01 (структура Event) и F02 (известность event_type). Невалидный Event или неизвестный тип возвращается без попытки session confirmation: event_result заполнен, session_status пуст, verifier/reader/repository не вызываются и source cursor не меняется. После подтверждения сессии handle_event() повторяет F01/F02 для защиты от обхода общего входа, затем выполняет F03/F04 и профильную маршрутизацию.

SourceSessionVerifier вызывается для каждого события. session_proof подтверждает установление текущего канала, а peer_identity_evidence должен позволять убедиться, что именно этот канал всё ещё открыт и аутентифицирован. Повторная проверка на том же непрерывно открытом канале должна быть идемпотентной; повтор proof на новом/закрытом канале или вне исходного channel binding отклоняется. Новый канал проходит новое challenge/session establishment. Это обязательное поведение будущего production verifier/transport adapter; fake host tests криптографическую реализацию не доказывают.

#### Атомарные правила фиксации cursor

`SourceSessionRepository` обязан в одной транзакции проверить актуальность регистрации и сравнить ожидаемое состояние cursor:

1. Для ожидаемого `NotFound` запись cursor должна всё ещё отсутствовать. Только при активной регистрации и `Verified`-сессии создаётся начальная запись: точный `source`, подтверждённый `source_instance`, `last_sequence` отсутствует.
2. Для `Found` сравнивается весь снимок: `source`, `source_instance` и `last_sequence`. Одного совпадения `source` недостаточно.
3. Если сохранённый `source_instance` совпадает с подтверждённым, результат — `AlreadyCurrent`. Существующий `last_sequence` сохраняется без сброса. Повторное подтверждение того же сеанса не должно возвращать sequence к начальному значению.
4. Если verifier подтвердил другую свежую `source_instance`, результат — `Confirmed`: новая instance устанавливается, а `last_sequence` очищается атомарно в той же транзакции.
5. Если реестр изменил ревизию или источник больше не `ACTIVE` между проверкой verifier-а и фиксацией, repository возвращает `RegistrationChanged` и не меняет cursor.
6. Если ожидаемый cursor изменился конкурентно, возвращается `CursorChanged`. Нельзя перезаписывать новое значение или выполнять слепой retry. Координатор должен перечитать состояние и заново выполнить проверку/подтверждение в соответствии с политикой.
7. `Unavailable`, `Error`, неизвестные статусы, ошибки транзакции и любые противоречивые результаты закрывают обработку события. Они не трактуются как `NotFound`, `Confirmed` или успешная сессия.

Источник, который не предоставляет `source_instance`, не может пройти текущий session-gated Pipeline: `SourceSessionEvidence` требует непустой instance, а Coordinator возвращает `InvalidEvidence` до вызова verifier/reader/repository. Это намеренный fail-closed барьер, а не разрешение обойти подтверждение сессии. Для legacy-источника потребуется отдельно согласовать аутентифицированный механизм сессии либо адаптацию источника; generic F04, допускающий cursor без instance при прямом вызове, сам по себе не разрешает такой путь к F13. Для источника с `source_instance`, но без `source_sequence`, F04 продолжает возвращать `UNORDERED / SequenceMissing`; календарное время не используется как замена порядкового номера. Формат sequence, начальное значение, запрет/обработка переполнения и способ генерации свежего `source_instance` (случайный boot token либо стойкий счётчик) остаются предметом отдельного выбора.

#### Привязка событий к подтверждённому каналу

После `Confirmed` или допустимого `AlreadyCurrent` адаптер входящего канала должен связывать Event с контекстом подтверждённой сессии. Значения `event.source` и `event.source_instance` сверяются с этим контекстом до F13; тело события не может сменить источник, зарегистрировать устройство или объявить новую сессию. События прежней сессии после подтверждённого переключения остаются `UNORDERED / SourceInstanceMismatch` и не достигают профильного commit.

`SourceSessionVerifier`/`SourceSessionRepository` не заменяют `validate_event()`, `check_event_freshness()`, duplicate check или атомарный compare-and-advance F09/F10. Подтверждение сессии устанавливает границу текущего источника; номера событий продвигаются только существующими commit-портами.

#### Не входит в этот срез

Не выбираются и не реализуются: схема и физическое хранилище реестра; формат аппаратной идентичности; способ хранения ключей; онлайн/офлайн provisioning; физическая кнопка, перемычка или сервисный ключ активации; challenge-response wire format; TLS/mTLS или иной конкретный защищённый канал; криптографические алгоритмы; транспорт Repka—ESP32; формат и генератор `source_instance`; production adapter и composition-root wiring. Эти решения нельзя подменять тестовыми заглушками или выводить из текущего VM Gateway.

**Проверка:** host harness `verification/regression/test_source_session_contract.cpp` покрывает SSR-01…SSR-18 через production `SourceSessionCoordinator`. Fake verifier/reader/repository моделируют только внешние адаптеры, CAS и ревизию регистрации; они не доказывают реальную криптографию, provisioning, постоянство данных после отключения питания или работу ESP32. Подробная матрица ограничений остаётся в `verification/regression/source_session_contract_scenarios.md`.

### 5.4.3. Доверенная граница входа событий

**Статус: ограничение API-типа реализовано; production ingress и transport отсутствуют.**

Аудит текущего дерева показал:

- C++ `LocalApiServer` принимает HTTP-запросы без тела и обслуживает только GET health/system diagnostics. Это read-only диагностика, не вход событий.
- Python Gateway по умолчанию также read-only. Его отдельно включаемый тестовый route принимает только PING и возвращает `physical_action_performed=false`; это не production event/command channel.
- `src/hardware/interfaces/controller_protocol.hpp` задаёт только логическую структуру сообщений Repka/ESP32. Wire representation и физический транспорт не определены; декодера входящих байтов и receive path нет.
- Нормализованный `Event` не создаётся текущим communications runtime. `StationEventPipeline` пока используется в host verification/contract tests; production composition root, связывающий transport → Event → session evidence → F13, отсутствует.

Следовательно, HTTP-заголовок, JSON payload, `Event.source`, `source_instance`, client timestamp или обычный TCP peer address не являются доказательством доверия. Это только входные значения, пока их происхождение не связано с отдельно подтверждённым аутентифицированным каналом. `received_at` — время приёма, а не средство аутентификации.

**Изменение границы C++ API:** `SourceSessionEvidence` стал непрозрачным final value object с закрытыми полями и закрытым конструктором. Обычный application/API код может только читать значения через getters; он не может создавать объект через aggregate initialization или default construction. Единственный production friend зарезервирован за `dockstation::communications::AuthenticatedEventTransportAdapter`. Этот класс пока не реализован: он обозначает точку расширения для будущего доверенного receive adapter, а не имитирует аутентификацию. Host tests создают фикстуры через отдельный `verification::SourceSessionEvidenceTestAccess`; compile-time `static_assert` фиксирует отсутствие public default constructor и aggregate initialization. C++ access control защищает от случайного вызова/неправильного использования API, но не является песочницей против вредоносного native-кода, уже исполняющегося в том же процессе.

**Контракт будущего ingress adapter:**

1. Один владеющий объект receive frame/session context удерживает исходные байты и строки до полного возврата `StationEventPipeline::handle()`. Нельзя создавать временные `string_view` на локальный JSON buffer и освобождать его до окончания F13.
2. Event и SourceSessionEvidence формируются из одного принятого сообщения и одного контекста конкретного активного канала. Не должно быть независимого публичного API вида `handle(event_from_json, evidence_from_json)`.
3. `source` и `source_instance` связываются с уже аутентифицированной идентичностью/сессией канала. Значения из payload можно сверить с каналом, но нельзя принимать их как самостоятельное доказательство.
4. `peer_identity_evidence` и `session_proof` должны происходить из транспортного handshake/channel context, а не из строки, присланной самим Event.
5. Adapter вызывает Pipeline синхронно, пока receive context и backing storage действительны. При асинхронной обработке кадр и все backing buffers передаются как владеющий объект.
6. Закрытие канала, повторное подключение, новое challenge/session establishment и lifetime peer identity должны быть видимы verifier-у. Повторная проверка внутри одного непрерывно открытого канала допустима; повтор proof на новом канале — нет.
7. Transport decoder сначала проверяет framing, размер, encoding и структуру сообщения, затем формирует нормализованный Event. Payload не может подменить состояние канала или напрямую создать VerifiedSourceSession.

**Не делать сейчас:** не подключать LocalApiServer/Python test route к StationEventPipeline; не принимать evidence из HTTP или пользовательского JSON; не считать loopback, SSH port-forward или source ID самостоятельной аутентификацией устройства; не выбирать TLS/mTLS, алгоритмы, challenge format или физический Repka—ESP32 транспорт без отдельного решения по docs/24 и docs/07.

**Следующий реальный срез:** после выбора физического канала и wire protocol спроектировать receive adapter/decoder и только затем реализовать его как production friend. Отдельно подключить production verifier/registry/storage и composition root. До этого Pipeline остаётся логически проверенным на fake ports, но не принимает реальные события со станции.

### 5.5. Выбор физического хранилища

SQLite остаётся кандидатом, не решением. Сравнение и испытания должны учитывать:
- транзакционность и условную запись с проверкой revision/cursor;
- восстановление после принудительного завершения процесса и потери питания;
- поведение при конкурентных чтениях/записях и блокировках;
- проверку целостности, миграции схемы и реакцию на неизвестную/повреждённую версию;
- ресурс и характеристики целевого накопителя, частоту записей и износ;
- резервное копирование, восстановление и политику хранения;
- возможность безопасно хранить чувствительные параметры; криптографический механизм не выбирать до согласования docs/24.

До выбора backend нельзя называть конкретную БД, формат файла, политику WAL, каталог данных или шифрование утверждёнными.

### 5.6. Порядок реализации небольшими срезами

1. **Уточнить оставшиеся контракты из §5.4:** список незавершённых операций, завершение recovery, выдача ID, атомарная связь исходного Event/cursor с F07 и поведение таймеров при reboot. Порт чтения cursor уже согласован и реализован; production adapter остаётся частью последующего подключения хранения.
2. **Выбрать и проверить backend:** сравнить кандидаты, сделать минимальный опыт транзакций/восстановления на Ubuntu VM и зафиксировать критерии; не переписывать ядро и не менять его интерфейсы на этом шаге.
3. **Создать основу persistence:** открыть/закрыть хранилище, проверить схему, выполнить миграции и предоставить узкий механизм транзакций с явными статусами ошибок.
4. **Подключить чтение:** snapshot, transition history, event lookup, source cursor и сохранённую конфигурацию; проверить корректные NOT_FOUND/UNAVAILABLE/READ_ERROR/INVALID состояния.
5. **Подключить запись намерений F07 и ID providers:** сохранить REQUESTED до передачи ActionDispatcher; отдельно проверить уникальность после пересоздания адаптера/перезапуска процесса.
6. **Подключить атомарные F09/F11 и F10:** сначала operation lifecycle, затем global snapshot/history/event/cursor; покрыть конкурентные revision/cursor конфликты и неизвестный результат commit.
7. **Подключить восстановление F14/F15:** загрузить snapshot, устойчиво отметить recovery, получить полный список незавершённых операций и свежие отчёты, выполнить сверку; только после отдельного согласованного контракта реализовать завершение recovery. Никаких автоматических повторов физической команды.
8. **Подключить CFG-F07 read adapter:** сохранить существующий порядок read → decode → validate → snapshot и проверить lifetime не владеющих памятью представлений. Запись конфигурации — отдельная последующая задача.
9. **Реализовать Clock/Timer:** системные часы и таймерный диспетчер, интегрированный с F13; до этого F11 продолжает принимать уже сформированное событие timeout, но сам таймер не запускает.
10. **Собрать composition root:** один набор согласованных адаптеров передаётся профильным процессорам и загрузчикам; Linux bootstrap открывает хранилище до запуска SSM и закрывает его после штатной остановки. Переход в ОЖИДАНИЕ остаётся зависимым от проверенной конфигурации, полной стартовой диагностики, безопасного исходного состояния и завершённой/не требуемой процедуры recovery.

### 5.7. Проверки и критерии готовности

Существующие host-тесты F07/F09/F10/F11/F14/F15/F16/F17 и CFG-F07 сохраняются как проверка логики на fake-портах; переписывать их ради выбранной БД не нужно. Для production-адаптера дополнительно потребуются:
- contract-тесты всех реализуемых портов на выбранном backend;
- тест «записать → закрыть адаптер/процесс → открыть заново → прочитать те же данные»;
- отказ/аварийное завершение до, во время и после каждой атомарной транзакции; проверка отсутствия частичных записей;
- повторы event_id (точный дубликат и конфликт содержимого), повтор operation_id и transition_id;
- конкурентные обновления одной операции, snapshot и cursor;
- отсутствующая, повреждённая и неподдерживаемая схема/конфигурация;
- незавершённая операция после reboot: только сверка, без повторного dispatch;
- полный и неполный recovery report: неполнота/UNKNOWN не должна давать разрешение на переход в ОЖИДАНИЕ;
- проверка срока жизни байтов/строк, на которые ссылаются ConfigurationSnapshot, Event и другие View-типы.

**Результат этапа 5:** существующие контракты подключены к выбранному долговременному backend; snapshot, события, source cursors, история и lifecycle операций переживают перезапуск; конфигурация загружается и валидируется из сохранённой записи; восстановление не снимает блокировки и не возобновляет физические действия без достоверной сверки. Хостовые fake-тесты сами по себе не считаются доказательством надёжности физического накопителя.

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

**Согласованный физический канал (DEC-062):** внутренний канал Repka Pi 5 ↔ ESP32 — проводной полнодуплексный UART, точка-точка, логические уровни 3,3 В. Исходный профиль линии — 115200 бод, 8 бит данных, без чётности, 1 стоп-бит (8N1), без аппаратного RTS/CTS. Это начальный профиль для реализации и последующей аппаратной проверки, а не результат измерения на установленной плате.

Предварительное соединение:
- Repka Pi 5: 40-pin GPIO, контакт 8 — UART0_TX; контакт 10 — UART0_RX; контакт 6 — GND.
- На Repka включить вариант распиновки 2 в `repka-config` (он включает UART0; вариант 3 также включает UART1, но для данного соединения не нужен).
- ESP32: для стандартного ESP32-WROOM-32 использовать UART2: GPIO16 — U2RXD/RX, GPIO17 — U2TXD/TX. Это назначение условно: перед первой прошивкой необходимо определить фактический SoC/модуль и убедиться, что GPIO16/17 доступны на конкретной плате и не заняты PSRAM или разводкой платы. Если вариант ESP32 иной, назначить другую проверенную свободную пару GPIO в едином board profile до первой прошивки.
- Соединять TX с RX, RX с TX и GND с GND. Платы питаются от своих источников; не соединять их выходы питания 3,3 В и не подавать 5 В на GPIO.
- Использовать короткий внутренний кабель, по возможности вдали от проводов моторов, катушек беспроводной зарядки и силовых преобразователей. Это прямой логический UART 3,3 В, не RS-232.
- Не использовать для этого канала сервисный разъём Repka DEBUG UART (JST-SH): это отдельный отладочный порт. USB не является штатным транспортом обмена Repka ↔ ESP32; он может оставаться средством первоначальной прошивки/восстановления ESP32 через установленный на dev-board USB-UART мост, если он присутствует.

Ограничение идентификации ESP32: сообщённая владельцем маркировка `2A53N-ESP32` не имеет точного совпадения в доступном индексе FCC и не позволяет надёжно установить модель платы, модуль, ревизию SoC, объём flash/PSRAM или распиновку. Поэтому GPIO16/17 и target сборки пока являются условным профилем для классического ESP32-WROOM-32, а не подтверждённым фактом о конкретном экземпляре. До фиксации первой production-прошивки нужно проверить полную маркировку модуля и платы, схему/документацию производителя и доступные контакты. Официальный datasheet Espressif подтверждает назначение GPIO16 как U2RXD и GPIO17 как U2TXD именно для ESP32-WROOM-32.

Прямой UART выбран как минимальный первый физический вариант для короткой внутренней линии. Если после компоновки станции кабель окажется длинным либо испытания выявят помехи/ошибки передачи, допускается RS-485 с трансиверами на обоих концах; прикладной формат и логические сообщения при этом не должны меняться. CRC кадра, если будет включена, обнаруживает случайное повреждение, но не заменяет предусмотренную docs/07 и docs/24 аутентификацию, целостность и конфиденциальность.

**Результат:** один логический контракт используется через тестовый host-адаптер и затем через реальный UART-адаптер ESP32; тестовый адаптер подтверждает только программный контракт, а локальные блокировки остаются на стороне контроллера. Потеря канала приводит к UNKNOWN/UNAVAILABLE, а не к предположению о безопасном состоянии. Повторная доставка/повтор команды не должны повторять физическое действие.

**Политика прошивки ESP32 — цель «одна плановая прошивка» (DEC-063):**
- общая логика протокола, проверка кадров и переносимые структуры отделяются от ESP-IDF/драйверов GPIO/UART; параметры платы и номера GPIO находятся в одном проверяемом board profile, а не разбрасываются по коду;
- после подтверждения модели ESP32, pinout, набора датчиков и набора начальных функций собирается законченная прошивка для точного ESP32 target; именно её планируется залить при первичном вводе платы в состав станции;
- в штатной эксплуатации параметры, пороги и разрешённые режимы должны изменяться версионируемой конфигурацией от Repka и локальной проверкой на ESP32, без перепрошивки ради обычной настройки;
- прошивка должна иметь ограниченные буферы/кадры, проверку длины и версии, тайм-ауты, watchdog и безопасное поведение при потере связи/некорректном сообщении; критические локальные блокировки не должны зависеть от доступности Repka;
- добавление новой аппаратной функции или исправление дефекта прошивки может потребовать обновления кода; абсолютное обещание «никогда не перепрошивать» технически некорректно. Цель — отсутствие регулярной ручной перепрошивки после первичной приёмки. Безопасный OTA-механизм не входит в этот срез и может рассматриваться отдельно после выбора модели, криптографической схемы и процедуры отката.

**Сборка и проверки без симулятора (DEC-064):** в VirtualBox на обычной Linux VM выполняются host unit/contract tests переносимой логики и реальная кросс-сборка прошивки выбранным ESP-IDF toolchain под подтверждённый ESP32 target. Это проверяет компиляцию для реальной архитектуры ESP32, но не эмулирует исполнение на микроконтроллере. Не добавлять QEMU/ESP32 simulator, виртуальные GPIO/UART, эмуляцию периферии или программную модель платы. Обычные test doubles/fakes допускаются только для изолированных unit/contract tests логики и интерфейсов; они не имитируют работу физического ESP32 и не считаются аппаратной проверкой. После закупки оборудования отдельно выполняются физическая прошивка, проверка уровней/связи и аппаратные тесты.

**Начальные host-срезы ControllerProtocol реализованы:** `src/hardware/interfaces/controller_protocol.hpp` содержит логические типы сообщений, общий конверт, словарь статусов качества и типизированные тела начального read-only обмена. `HELLO`, `HELLO_ACK`, `HEARTBEAT` и `REQUEST_SNAPSHOT` используют пустое тело; `TELEMETRY_SNAPSHOT` содержит caller-owned список типизированных измерений; `ERROR` содержит код, серьёзность, описание и необязательную известную причину. `validate_controller_message()` проверяет общий конверт, `validate_controller_message_payload()` — соответствие типа телу и его поля. Host-проверка находится в `verification/repka_gateway/test_controller_protocol.cpp` и зарегистрирована в CTest как `controller_protocol`.

Это типизированный логический контракт, а не wire-кодек. Точный формат временных меток, совместимость версий, порядок/повторы, сериализация, кадрирование, CRC и криптографическая защита wire-канала ещё требуют отдельного решения/реализации. Физический UART выбран, но его GPIO на конкретной ESP32-плате не подтверждены; физическое управление не реализовано. Host-проверки и кросс-сборка не являются обменом с физическим ESP32.

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

**Файлы текущего deployment-среза:** `repka/dockstation-gateway.service.in`, `repka/gateway.conf.example`, `CMakeLists.txt` (install rules), `repka/README.md`, `verification/repka_gateway/test_systemd_deployment.sh` и `.github/workflows/linux-cmake.yml`. Будущие компоненты резервного копирования/релизов и watchdog остаются отдельными задачами.

**Функции:**
- check_service_dependencies(), report_service_health(), register_watchdog_heartbeat(), detect_service_stall().
- restart_allowed_module(), validate_restart_conditions(), request_orderly_shutdown().
- create_backup(), verify_backup(), restore_backup().
- validate_release_manifest(), verify_release_signature(), install_release(), verify_installed_version().

**Реализованный первый deployment-срез:** CMake устанавливает бинарный файл Gateway, unit template и пример конфигурации; systemd запускает процесс от отдельного пользователя `dockstation`, с пустым набором capabilities, read-only ограничениями файловой системы и журналированием в journald. CI проверяет синтаксис unit, install layout, права на конфигурацию и бинарный файл, запускает Gateway от имени `dockstation`, проверяет оба GET endpoint, loopback bind и завершение по SIGTERM. CI не запускает systemd manager. Ручной lifecycle `systemctl enable/start/stop/restart`, проверка journal и автозапуска после перезагрузки Ubuntu VM пройдены владельцем в Ubuntu 24.04.5 LTS x86_64 VM; подробный протокол внесён в docs/30.

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

Не фиксировать без проверки оборудования/согласования: точную модель ESP32-платы/модуля, SoC target и фактическую пару GPIO (физический UART выбран по DEC-062); формат кадров и криптографическую защиту wire-обмена; метеостанцию; датчики и драйверы; батарею/BMS и профиль зарядки; БАС, автопилот и протокол; камеру и калибровку; production-СУБД; серверный API; криптографические параметры из docs/24; удалённое автоматическое обновление.

Для невыбранных устройств сначала создаются интерфейсы и минимальные тестовые адаптеры. Они подтверждают только программный приём запроса и не подменяют физические данные или состояние оборудования. Реальный драйвер пишется после проверки документации и электрической совместимости.

## 8. Особенности Repka Pi 5

Официальная документация Repka указывает для Pi 5 RK3588, Ethernet, USB, PCIe, 40-pin GPIO с альтернативными UART/I2C/SPI/PWM, CSI и DSI. Для DEC-062 используется UART0 на 40-pin GPIO: контакт 8 TX, контакт 10 RX, контакт 6 GND; в `repka-config` нужен вариант распиновки 2 (UART0). Сервисный JST-SH DEBUG UART — отдельный интерфейс и не используется для связи с ESP32. Перед физическим подключением всё равно сверяются маркировка контактов, выбранный pinout и электрические характеристики конкретной платы.

Обязательные правила:
- не считать 40-pin разъём взаимозаменяемым с другой моделью Repka/Raspberry Pi;
- сверять pinout и режимы через официальную документацию и repka-config;
- проверять электрические пределы каждой линии до подключения;
- не писать аппаратный драйвер по виртуальной распиновке;
- сначала проверить программный контракт с тестовым адаптером, затем — на плате с безопасной нагрузкой.

Официальные документы:
- https://repka-pi.ru/docs/109 — характеристики, 40-pin GPIO и электрические параметры.
- https://repka-pi.ru/docs/108 — настройки pinout; таблица вариантов подтверждает включение UART0 в варианте 2 и UART1 в варианте 3.
- https://documentation.espressif.com/esp32-wroom-32_datasheet_en.html — назначение U2RXD/U2TXD и GPIO16/GPIO17 для ESP32-WROOM-32.
- https://fccid.io/2A53N-ESP32 — точная запись FCC ID `2A53N-ESP32` в доступном индексе не найдена; маркировка не используется как доказательство конкретного board profile.
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

1. **Ручная проверка Windows Operator ↔ Linux Gateway в VM выполнена.** 2026-11-13 проверен временный Python Gateway, включая остановку/перезапуск и отдельное восстановление SSH-процесса. 2026-11-14 проверен C++ Gateway: health/diagnostics, системные показатели, автоматическое восстановление API-обмена после перезапуска Gateway и loopback-only bind. Подробности и границы приведены в `docs/30_REPKA_NETWORK_IMPLEMENTATION_PLAN.md`. Repka Pi 5/ARM64 и аппаратные каналы не проверялись.
2. **Systemd deployment проверен вручную в Ubuntu VM.** После CMake install и `systemd-analyze verify` выполнены `systemctl enable --now`, restart, stop и start; служба работала как `dockstation:dockstation`, health и diagnostics вернули HTTP 200, listener был ограничен `127.0.0.1:8080`, журнал содержал запуск API. После reboot служба имела `enabled` и `active`, а journal текущей загрузки подтвердил запуск unit; Windows Operator снова показывал Connected без ручного ввода учётных данных во время проверки. Автоматический CI run 43520279910 независимо подтвердил install layout и запуск от непривилегированного пользователя, но не выполняет systemd manager lifecycle. Production-хранилища, реальные источники событий, Timer, composition root и адаптеры устройств остаются отдельными задачами.
3. Первый безопасный сквозной VM-запрос через Operator → временный Python Gateway → TestRequestService → TestRequestAdapter → Operator реализован и покрыт host integration test (DEC-061); test-adapter подтверждает только приём PING, не физическое выполнение. C++ loopback-only read-only API прошёл актуальный CI run 43516203836 (10/10 CTest, 31/31 C++ host programs, 130 pytest) и отдельную ручную VM-проверку с Windows Operator. C++ API не содержит test-only PING и физических/управляющих маршрутов.
4. Визуальное направление и структура Windows Operator согласованы владельцем 2026-11-13: семь вкладок — Подключение, Климат, Датчики, Сеть, Логи, Камера и Система (docs/23 rev 0.21). Реализован PySide6/QML shell с мостом к SSH/API; вкладка «Система» отображает только опубликованные системные показатели из Gateway API: CPU/SoC temperature при наличии явно маркированного датчика, RAM, корневую файловую систему и load average. Владелец вручную проверил экран с C++ Gateway: значения RAM и корневой файловой системы совпали с API после округления. Недоступная температура остаётся `UNAVAILABLE`; физические датчики и камера показывают отсутствие данных до появления соответствующего API и оборудования. Проверка выполнена в VM и не является проверкой Repka Pi 5 или аппаратной интеграцией.
5. Карта интеграции StationStateManager/ConfigurationManager с постоянным хранением зафиксирована в этапе 5. Порт SourceCursorReader и его CTest (25 host-проверок), а также логические SourceSessionVerifier/SourceSessionRepository с validators и SSR-01…SSR-18 host harness реализованы. Production registry/verifier/storage adapters, подключение к F13/composition root, привязка события к аутентифицированному каналу и транзакционные проверки совместно с F09/F10 commit остаются открытыми. Выбор backend — отдельный следующий шаг по критериям §5.5.
6. Продолжить подключение реальных ESP32 и аппаратных подсистем после утверждения интерфейсов и наличия оборудования.
7. Отдельно провести исследование варианта встроенного VPN/защищённой overlay-сети; реализацию не начинать до выбора архитектуры и согласования владельцем.

Контракты DiagnosticsManager согласованы владельцем проекта (DEC-058/059; docs/21 и docs/22). Это подготовительный результат и не меняет очередность мастер-плана: реализацию DiagnosticsManager, включая первый вертикальный срез SDA-F00 и последующие функции, начинать только на предусмотренном этапе после выполнения предшествующих зависимостей. Открытые аппаратные/режимные параметры не закрываются этим согласованием.

Следующий технический результат — при наличии Repka Pi 5 выполнить ARM64-сборку, установить C++ Gateway на целевую плату и повторить сетевые испытания на Repka OS. Текущая проверка systemd относится только к Ubuntu 24.04.5 LTS x86_64 VM и не заменяет приёмку целевой платы. Первый пользовательский результат по интерфейсу — согласованный визуальный прототип; дальнейшее развитие Operator продолжается вместе с функциями приложения.

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
| 1.18 | 2026-11-12 | По DEC-061 реализован первый безопасный VM-вертикальный срез Operator → Python Gateway → TestRequestService → TestRequestAdapter → Operator. Test-only POST отключён по умолчанию, принимает только PING и подтверждает отсутствие физического действия. Добавлена сквозная host-проверка; C++ Gateway, production API и аппаратные проверки остаются отдельными задачами. |
| 1.19 | 2026-11-12 | Полный host CI после реализации test-adapter VM-среза прошёл: 8/8 CTest, 31/31 C++ host tests, 92 Python pytest с новым сквозным сценарием, compileall и shell checks. Зафиксирован прогон 43293944730; Windows VM/SSH reconnect и целевая/аппаратная проверка остаются открытыми. |
| 1.20 | 2026-11-12 | Подготовлено предложение по Windows Operator: сценарии, информационная архитектура, карта экранов, структурные макеты и дизайн-система (docs/23 rev 0.9). Предложение ожидает согласования владельца; QML-прототип и Windows runtime/reconnect проверки не выполнялись. |
| 1.21 | 2026-11-12 | Уточнена граница первого QML-среза: лабораторное подключение, только опубликованные API-диагностика/сеть/события. Не опубликованные API показатели и физические функции не имитируются. |
| 1.22 | 2026-11-13 | Владелец согласовал шесть вкладок первого Operator-прототипа. Обновлена docs/23 rev 0.12; добавлены PySide6/QML entry point, bridge к существующим SSH/API-модулям и визуальная заглушка камеры. QML smoke test и CI подготовлены; фактический результат прогона ожидается. |
| 1.23 | 2026-11-13 | Первый QML shell и мост к существующим SSH/API-модулям добавлены. GitHub Actions run 43378417832 SUCCESS: CMake build, 8/8 CTest, 31/31 C++ host programs, Python compileall, 93 pytest включая headless QML load, shell syntax и network helper tests. Windows runtime и реальный VM/SSH reconnect остаются открытыми. |
| 1.24 | 2026-11-13 | Расширен CI проверкой editable install `./operator[qml]`, GUI entry point `dockstation-operator-qml` и наличия QML-ресурса в пакете. Run 43378693068 SUCCESS: все проверки прошли, 93 pytest. Windows runtime и реальный VM/SSH reconnect остаются открытыми. |
| 1.25 | 2026-11-13 | Python VM Gateway публикует `platform.resources` (RAM, корневая файловая система, CPU/SoC temperature, load average); Operator API client валидирует поля, QML bridge форматирует их для вкладки «Система». Добавлены host-тесты сборщиков, контракта и QML bridge; фактический прогон после изменений ещё не выполнен. C++ production API и аппаратные проверки остаются отдельными задачами. |
| 1.26 | 2026-11-13 | Operator получил автоматическое восстановление неожиданно потерянного SSH/API-соединения: первый интервал 5 с, далее 5/10/20/30 с с пределом 30 с; ConnectionManager сохраняет профиль при временном отказе, QML bridge после восстановления запрашивает свежий снимок. Добавлены host-тесты сохранения и обновления профиля, повторного подключения к перезапущенному loopback Gateway, планирования backoff и отмены ожидающих попыток. Актуальный CI и реальное SSH/Windows VM испытание ещё не подтверждены. |
| 1.27 | 2026-11-13 | Сверено описание этапа Operator с фактическим деревом исходников и текущим семивкладочным QML-прототипом; удалены ссылки на несуществующие application.py/api_models.py/ViewModel/QML-page файлы как на реализованные. Обновлены корневой и Operator README, уточнены критерии reconnect-приёмки, исправлена ссылка на CI run и записано pytest-предупреждение с исправленным импортом сервиса. GitHub Actions обновлён до checkout@v7 и setup-python@v7 (Node 24). Все эти изменения ожидают нового CI-прогона. |
| 1.28 | 2026-11-13 | Обновлена спецификация журнала QML Operator: внутренняя прокрутка, выделение/копирование, копирование всего буфера и UTF-8 экспорт; согласованы статусы качества данных в docs/06. Зафиксированы успешные ручные VM-проверки Windows Operator ↔ Python Gateway, без распространения результата на Repka Pi 5/ESP32. |
| 1.29 | 2026-11-13 | Добавлен первый host-срез ControllerProtocol: типы логических сообщений, общий конверт, словарь статусов качества и fail-closed проверка обязательных метаданных; 13 проверок локально прошли. Тела сообщений, версия совместимости, сериализация, кадрирование и транспорт не выбраны; полный CMake/CI-прогон после изменения ожидает проверки. |
| 1.30 | 2026-11-13 | Зафиксирован полный успешный GitHub Actions run 43485099448 после добавления ControllerProtocol: Linux build, 9/9 CTest с ASan/UBSan, 31/31 C++ host test programs, 115 pytest, QML package entry point и shell checks. Уточнены результаты ручных VM-проверок Operator ↔ Python Gateway и статус следующего этапа: ControllerCodec ожидает определения логических тел и отдельного утверждения сериализации/кадрирования. |
| 1.31 | 2026-11-13 | Добавлены типизированные тела read-only ControllerProtocol: пустые HELLO/HELLO_ACK/HEARTBEAT/REQUEST_SNAPSHOT, TELEMETRY_SNAPSHOT с caller-owned типизированными измерениями и ERROR со структурированной серьёзностью; реализована отдельная fail-closed проверка payload. Формат wire, timestamps, сериализация, кадрирование и транспорт остаются открытыми. |
| 1.32 | 2026-11-13 | Подтверждён полный host CI для C++ read-only Gateway (run 43499328759): сборка всех Linux targets, 10/10 CTest с ASan/UBSan, 31/31 C++ host programs и 130 pytest. Сквозной процессный тест проверяет оба GET-контракта через Operator validator, отказ POST/неизвестного маршрута и SIGTERM shutdown. Ручной C++ Gateway VM-тест, Repka ARM64 и аппаратные проверки остаются открытыми. |
| 1.33 | 2026-11-14 | Сверен мастер-план с фактической реализацией C++ Gateway и ручным испытанием Windows Operator ↔ C++ Gateway в Ubuntu 24.04.5 LTS x86_64 VM. Уточнены bootstrap/HTTP API, 10 CTest и последний успешный CI run 43516203836; зафиксированы health/diagnostics HTTP 200, совпадение системных показателей, восстановление API-обмена после перезапуска Gateway и loopback-only bind. systemd, ARM64/Repka Pi 5 и аппаратные испытания остаются открытыми. |
| 1.34 | 2026-11-14 | Добавлены C++ Gateway systemd unit template, пример конфигурации и CMake install. CI run 43520279910 SUCCESS: 10/10 CTest, 31/31 C++ host tests, install layout/systemd-analyze проверки, права dockstation и запуск не-root Gateway с health/diagnostics, loopback-only bind и SIGTERM shutdown. Ручные `systemctl` lifecycle и reboot проверка в пользовательской Ubuntu VM остаются открытыми. |
| 1.35 | 2026-11-14 | Зафиксирована ручная проверка полного systemd lifecycle C++ Gateway в Ubuntu 24.04.5 LTS x86_64 VM: установка, enable/start, restart, stop/start, active/running под dockstation, HTTP 200 health/diagnostics, loopback-only bind, journal и автозапуск после reboot (enabled/active, journal текущей загрузки). Windows Operator после загрузки снова отображал Connected. ARM64/Repka Pi 5 и аппаратные проверки остаются открытыми. |
| 1.36 | 2026-11-14 | Развёрнута карта интеграции существующих портов SSM/ConfigurationManager с постоянным хранилищем: атомарные единицы F07/F09/F10/F11/F14, read-порты F16/F17/CFG-F07, границы EventRepository, идентификаторы, курсоры и Timer. Явно выделены отсутствующие контракты чтения cursor, списка незавершённых операций и завершения recovery; сохранены открытыми backend, формат ID, запись конфигурации и политика таймеров после reboot. Добавлены последовательность небольших срезов и критерии adapter-level тестов; код и публичные интерфейсы не менялись. |
| 1.37 | 2026-11-14 | Подготовлено предложение публичного read-only интерфейса SourceCursorReader с семантикой Found/NotFound/Error, проверками целостности результата, сроком жизни string_view и поведением при конкурирующем commit. Отдельно оставлено открытым доверенное подтверждение первой/новой source_instance. Добавлены сценарии verification-контракта; код и публичные интерфейсы не менялись. |
| 1.38 | 2026-11-14 | SourceCursorReader согласован владельцем и добавлен в `src/core/event_processing.hpp`: read-only статусы Found/NotFound/Error и стабильный снимок SourceCursor; добавлена fail-closed проверка формы результата в `event_processing.cpp`. В CTest зарегистрирован отдельный verification harness с 25 проверками. Обновлены docs/06 и docs/26. Production adapter, интеграция с F13/composition root, транзакционные проверки с F09/F10 и доверенная процедура первой/новой source_instance остаются открытыми. |
| 1.39 | 2026-11-14 | По согласованию владельца добавлен §5.4.2: разделены постоянная регистрация logical source и доверенное подтверждение source session через SourceSessionVerifier/SourceSessionRepository; описаны статусы, атомарные правила обновления cursor, проверка ревизии регистрации и привязка событий к подтверждённому каналу. Добавлена спецификация SSR-01…SSR-18 в verification/regression. Публичные C++-сигнатуры требуют отдельного согласования; код, production adapters, криптографический протокол и аппаратная проверка не выполнялись. |
| 1.40 | 2026-11-14 | Владелец утвердил публичные SourceSessionVerifier/SourceSessionRepository сигнатуры. Добавлены src/core/source_session.hpp/.cpp и fail-closed validators для evidence, Verified session и confirmation result. Добавлен CTest host harness SSR-01…SSR-18 на fake verifier/reader/repository; документация 06/26 и regression README обновлена. Production registry/verifier/storage, composition-root/event-adapter wiring, криптография, provisioning и аппаратная проверка остаются открытыми. |
| 1.41 | 2026-11-14 | Реализован утверждённый SourceSessionCoordinator: проверка привязки Event к evidence, последовательность verifier → cursor reader → transactional repository, fail-closed статусы и возврат cursor только при Confirmed/AlreadyCurrent. SSR-01…SSR-18 переведены с тестовой копии оркестрации на production coordinator с fake ports. Обновлены CMake, docs/06, docs/26 и regression-сценарии. Production adapters, аутентифицированный transport context и F13 composition-root integration остаются открытыми. |
| 1.42 | 2026-11-14 | Укреплены fail-closed validators: отклоняются пробельные source/source_instance/registration_revision, некорректный Found cursor и успешное подтверждение при expected Error. StationEventPipeline выполняет F01/F02 до любых session-side effects и возвращает пустой session_status при раннем отказе; добавлены тесты точной передачи cursor в профиль и повторной проверки той же активной сессии. Синхронизированы интерфейсы Pipeline в docs/06, docs/26, docs/31 и docs/18; обновлены README и regression-инструкции. Production transport/verifier/storage остаются открытыми. |
| 1.43 | 2026-11-14 | Добавлен явный отказ session-gated Pipeline для источников без source_instance; generic F04 без instance не считается разрешением обхода аутентификации. Документация legacy-совместимости уточнена. Полный CI run 43608617035 для HEAD 22933ce SUCCESS: 13/13 CTest, 31/31 C++ host programs, 134 pytest, Linux build, Gateway install/non-root/systemd checks, QML и shell/network tests. |
| 1.44 | 2026-11-14 | Проведён аудит фактического входного пути: C++ LocalApiServer read-only, Python test route принимает только PING, ControllerProtocol не задаёт wire format/transport, production Event ingress/composition root отсутствуют. SourceSessionEvidence стал opaque final type с закрытым конструктором; создание зарезервировано за будущим AuthenticatedEventTransportAdapter. Добавлены test-only factory и compile-time ограничения. Документирован контракт ingress adapter и запрет принимать доверие из Event/API payload; реальная аутентификация и transport остаются отдельными будущими задачами. |
| 1.45 | 2026-11-14 | По DEC-062 согласован внутренний физический канал Repka Pi 5 ↔ ESP32: UART 3,3 В, 115200 8N1; документированы перекрёстное соединение TX/RX, земля, Repka UART0 и предварительная пара GPIO16/17 для ESP32-WROOM-32 с обязательной проверкой конкретной платы. Зафиксирована цель одной плановой первичной прошивки, настройка через версионируемую конфигурацию, host-проверки и кросс-сборка в VirtualBox без симулятора; фактический ESP32 target/GPIO и wire-кодек остаются неподтверждёнными/нереализованными.
| 1.46 | 2026-11-14 | В журнал решений добавлены DEC-063/064: цель одной плановой прошивки ESP32 с обычной настройкой через проверяемую конфигурацию и обязательная разработка без симулятора. Мастер-план явно связывает с ними требования к firmware profile, VirtualBox host tests и кросс-сборке под подтверждённый ESP32 target.

**Ограничение:** это план. Функции не считаются реализованными до появления кода, проверок и, где требуется, аппаратных испытаний.



## 13. Результат полного host CI

Полный workflow `.github/workflows/linux-cmake.yml` [43608617035](https://github.com/GooseRabbit/dockstation/actions/runs/43608617035) завершился SUCCESS на Ubuntu 24.04.5 x86_64 для HEAD `22933ce86484c45d47322ba3940a98119afb66d4`.

| Проверка | Результат |
|---|---:|
| CMake configure и сборка всех Linux targets, включая C++ Gateway | PASS |
| CTest с ASan/UBSan | 13/13 PASS |
| C++ host unit/contract программы StationStateManager и ConfigurationManager | 31/31 PASS |
| Установочная раскладка C++ Gateway, unit и примера конфигурации | PASS |
| `systemd-analyze verify` для установленного unit | PASS |
| Доступность конфигурации и бинарного файла для `dockstation` без права записи | PASS |
| Запуск C++ Gateway от имени `dockstation`, health и diagnostics | PASS |
| Listener ограничен `127.0.0.1:8080`; завершение по SIGTERM | PASS |
| Python compileall | PASS |
| Python pytest: Operator, Python Gateway и integration | 134 PASS |
| QML package entry point и ресурсы | PASS |
| Shell syntax/behavior и network helper tests | PASS |

Сквозной тест запускает собранный C++ Gateway отдельным процессом. Он получает health и system diagnostics, проверяет JSON существующим Operator API validator, подтверждает 404 для неизвестного маршрута и 405 для POST, затем отправляет SIGTERM и проверяет успешное завершение процесса.

Отдельный deployment CI-шаг устанавливает unit и конфигурацию, проверяет unit через `systemd-analyze verify`, права непривилегированной учётной записи, запуск C++ Gateway от имени `dockstation`, оба GET endpoint, loopback-only listener и завершение процесса по SIGTERM. Этот шаг не использует systemd manager и не подтверждает `systemctl enable/start/stop/restart` или запуск после перезагрузки.

Это подтверждает host-проверки на Ubuntu 24.04.5 x86_64. Это не ARM64/Repka Pi 5 deployment, не ручной lifecycle systemd в пользовательской VM и не испытание ESP32/физической станции.



---

## Дополнение: обязательная привязка F13 к подтверждённой сессии

**Реализовано на уровне core:** StationEventPipeline выполняет F01/F02 до SourceSessionCoordinator, затем после Confirmed/AlreadyCurrent запускает общий F03/F04/F13. Публичный входной метод принимает SourceSessionEvidence, а не произвольный SourceCursor. Некорректный Event или неизвестный тип не вызывают session verifier/reader/repository и не меняют cursor. При отказе подтверждения сессии поиск события и профильная обработка не запускаются; точный статус сохраняется, а при раннем F01/F02 результат session confirmation отсутствует.

**Проверка:** CTest station_event_pipeline проверяет успешное подтверждение, блокировку при отказе, несовпадение instance, ошибки cursor/registration, ранний отказ F01/F02 без изменения cursor и точную передачу cursor профильному обработчику. Контрактный тест в tests/ проверяет все пять маршрутов и захват переданного cursor.

**Ограничения:** тесты используют fake-адаптеры. Реальный verifier, доверенное создание SourceSessionEvidence внутри аутентифицированного transport adapter, постоянное хранилище и атомарность production-транзакций должны быть реализованы отдельно. Подтверждение сессии и commit события пока не объединены в одну транзакцию: неуспешная обработка первого события может оставить новую сессию подтверждённой с пустым last_sequence. C++ Gateway runtime ещё не подключён к этому core pipeline.
