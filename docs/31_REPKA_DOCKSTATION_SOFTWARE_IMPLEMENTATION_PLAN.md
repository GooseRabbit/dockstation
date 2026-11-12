# Полный план реализации программного комплекса DockStation на Repka Pi 5

**Статус:** мастер-план реализации; последовательность разработки согласована, открытые платформенные, сетевые, серверные и аппаратные детали требуют отдельной проверки и решения.  
**Редакция:** 0.6.  
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
| Сборка | CMake для отдельной Linux-цели | Предложение |
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

**Файлы:** deployment/repka/README.md, deployment/repka/check_network.sh, deployment/dev/README.md, deployment/dev/linux_vm/README.md, deployment/dev/windows_vm/README.md, deployment/dev/network/README.md.

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

**Файлы:** CMakeLists.txt; apps/repka_gateway/CMakeLists.txt; apps/repka_gateway/main.cpp; src/platform/linux/runtime_environment.hpp/.cpp; src/platform/linux/system_diagnostics.hpp/.cpp; src/platform/linux/network_diagnostics.hpp/.cpp; src/platform/logging/logger.hpp/.cpp; deployment/repka/dockstation-gateway.service.

**main.cpp:**
- parse_command_line() — аргументы запуска.
- load_runtime_config() / validate_runtime_config() — загрузка и проверка настроек.
- initialize_logging() — журналирование без секретов.
- initialize_platform_adapters() — создание Linux-адаптеров.
- initialize_core_services() — связывание компонентов.
- install_shutdown_handlers() — SIGTERM/SIGINT.
- run_gateway() — основной цикл.
- shutdown_gateway() — штатная остановка и освобождение ресурсов.

**runtime_environment.cpp:**
- read_os_version(), read_kernel_version(), read_hardware_architecture(), read_software_build_info(), check_runtime_dependencies().

**system_diagnostics.cpp:**
- read_uptime(), read_memory_status(), read_storage_status(), read_cpu_temperature(), read_cpu_load(), collect_system_diagnostics().

**network_diagnostics.cpp:**
- read_network_interfaces(), read_interface_addresses(), read_default_route(), resolve_dns_name(), check_tcp_reachability(), collect_network_diagnostics().

**logger.cpp:**
- initialize_logger(), write_log_record(), redact_sensitive_fields(), rotate_log_files(), flush_log().

Недоступная температура или неизвестный статус обозначаются явно, не подменяются нулём.

**Результат:** отдельное Linux-приложение собирается, запускается, выдаёт диагностику и корректно завершается. Старый host-testbench не меняется.

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

**Файлы:** deployment/repka/dockstation-gateway.service; deployment/repka/config/README.md; install_gateway.sh; backup/README.md; release/README.md; src/platform/linux/watchdog_service.hpp/.cpp; process_manager.hpp/.cpp.

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

**Ограничение:** это план. Функции не считаются реализованными до появления кода, проверок и, где требуется, аппаратных испытаний.
