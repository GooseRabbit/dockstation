# Каталог предполагаемых функций DockStation

**Статус:** предварительный рабочий каталог.  
**Дата:** 2026-11-16.
**Назначение:** показать, на какие отдельные функции может быть разбито программное обеспечение станции и совместно разрабатываемая бортовая часть.

> **Важно:** приведённые функции, их названия, расположение по файлам и их количество могут измениться по мере проектирования, выбора БАС, оборудования, платформы и протоколов. Это предварительный ориентир, а не окончательное техническое задание.

Сигнатуры записаны в условном C++-подобном виде только для наглядности. Язык программирования не выбран. Типы вроде `Result<T>`, `Mission`, `Event`, `Status` и другие названия обозначают данные, структуру которых ещё предстоит определить. Функции со статусом **«уже есть»** присутствуют в текущем рабочем коде; остальные — предполагаемые.

---

## 1. Ядро станции — StationStateManager

### src/core/event_validation.cpp

| Функция | Назначение |
|---|---|
| `constexpr EventValidationResult valid_result() noexcept` **(уже есть)** | Формирует результат успешной проверки события. |
| `constexpr EventValidationResult invalid_result(EventValidationError error, std::string_view field, std::string_view message) noexcept` **(уже есть)** | Формирует результат ошибки с указанием поля и причины. |
| `constexpr bool is_ascii_whitespace(char ch) noexcept` **(уже есть)** | Проверяет, является ли символ обычным пробельным ASCII-символом. |
| `bool has_non_whitespace(std::string_view value) noexcept` **(уже есть)** | Проверяет, что строка содержит хотя бы один непробельный символ. |
| `bool is_known_quality_status(std::string_view value) noexcept` **(уже есть)** | Проверяет, входит ли статус качества данных в предусмотренный перечень. |
| `EventValidationResult validate_event(const Event& event, const EventValidationPolicy& policy = {}) noexcept` **(уже есть)** | Проверяет обязательные поля события, статусы качества и согласованность необязательных пар полей. |

### src/core/event_processing.hpp / src/core/event_processing.cpp

| Функция | Назначение |
|---|---|
| `EventCategory classify_event(const Event& event) noexcept` **(уже есть)** | По точному `event_type` определяет одну из шести категорий из §2.1 документа `06`; неизвестный тип возвращает `Unknown`. Не меняет событие и не выполняет побочных действий. |
| `DuplicateEventResult check_duplicate_event(const Event& event, EventRepository& repository) noexcept` **(уже есть)** | Проверяет сохранённое событие по `event_id`: NEW, DUPLICATE, CONFLICT или REPOSITORY_ERROR. `NOT_FOUND` допускается только с полностью пустым представлением Event; заполненный Event при этом статусе считается ошибкой репозитория. Сравнение точное, кроме поля `received_at`, назначаемого принимающей стороной; репозиторий только читается. Не резервирует ID и не сохраняет результат обработки. |
| `EventFreshnessResult check_event_freshness(const Event& event, const SourceCursor& cursor) noexcept` **(реализована)** | Сравнивает source_sequence в пределах одного источника/сессии; не использует календарное время, не меняет курсор. При новой сессии вызывающий слой предварительно подтверждает её и сбрасывает курсор. |
| `SourceCursorReadResult SourceCursorReader::read_source_cursor(std::string_view source) noexcept` **(порт и SQLite production adapter реализованы)** | Читает единый снимок source/source_instance/last_sequence по точному source. Ничего не создаёт, не сбрасывает и не продвигает; строки результата должны жить до завершения профильной обработки и commit. |
| `bool is_valid_source_cursor_read_result(std::string_view requested_source, const SourceCursorReadResult& result) noexcept` **(реализована)** | Проверяет непустой ключ, точное совпадение source для FOUND, пустой payload для NOT_FOUND/ERROR и отклоняет неизвестный статус. ERROR остаётся ошибкой и блокирует обработку. |


### src/core/source_session.cpp

| Функция / интерфейс | Назначение |
|---|---|
| `SourceSessionVerifier::verify_source_session(const SourceSessionEvidence&) noexcept` **(логический порт реализован)** | Абстрактная граница проверки доверия к источнику/сессии; `RegistrySourceSessionVerifier` реализует storage-neutral policy, а `AuthenticatedEventTransportAdapter` создаёт opaque evidence из live ControllerLink callback. Production registry reader/storage и session repository ещё отсутствуют. |
| `SourceRegistrationReader::find_by_peer_identity(std::string_view) noexcept` **(порт и SQLite production adapter реализованы)** | Ищет регистрацию по непрозрачному binary pair-specific identity; SQLite adapter копирует возвращённые строки в owned buffers до завершения session flow и возвращает Station ID, logical source, registration revision и ACTIVE/DISABLED/REVOKED. |
| `RegistrySourceSessionVerifier::verify_source_session(const SourceSessionEvidence&) noexcept` **(policy implementation реализована)** | Сверяет точный локальный Station ID, source, ACTIVE state и registration revision; проверяет 32-байтовую длину proof. Реализованный ControllerLink ingress adapter получает proof через service exporter в синхронном callback текущей открытой TLS session; verifier не пересчитывает exporter и не доказывает его свежесть самостоятельно. |
| `is_valid_source_registration_lookup_result()` **(реализована)** | Отклоняет неизвестные статусы и противоречивые ответы registry reader; NOT_FOUND/UNAVAILABLE/ERROR допускаются только с пустой записью. |
| `SourceSessionRepository::confirm_source_session(const SourceCursorReadResult&, const VerifiedSourceSession&) noexcept` **(порт и SQLite production adapter реализованы)** | В BEGIN IMMEDIATE транзакции повторно проверяет ACTIVE/revision регистрации и сравнивает полный ожидаемый cursor. Создаёт начальный cursor или атомарно заменяет source_instance с очисткой last_sequence; AlreadyCurrent сохраняет sequence. Конфликт регистрации/cursor не перезаписывается. |
| `SourceSessionEvidence` **(непрозрачный тип реализован)** | Закрытый конструктор и поля, только read-only getters; обычный application/API код не может собрать evidence из payload. Реализованный `AuthenticatedEventTransportAdapter` — единственный production friend; C++ access control не является sandbox против произвольного native-кода внутри процесса. |
| `is_valid_source_session_evidence()` / `is_valid_verified_source_session()` **(реализованы)** | Проверяют наличие обязательных полей источника, сессии, доказательства peer identity и ревизии регистрации. |
| `is_valid_source_session_verification_result()` **(реализована)** | Проверяет точное совпадение Verified source/instance с исходным доказательством; отрицательные статусы обязаны иметь пустой payload; неизвестный статус отклоняется. |
| `is_valid_source_session_confirmation_result()` **(реализована)** | Проверяет семантику Confirmed/AlreadyCurrent относительно ожидаемого полного cursor; ошибки и конфликты обязаны возвращать пустой cursor. |

### src/core/source_session_coordinator.cpp

| Функция / интерфейс | Назначение |
|---|---|
| `SourceSessionCoordinator::SourceSessionCoordinator(SourceSessionVerifier&, SourceCursorReader&, SourceSessionRepository&) noexcept` **(реализован)** | Сохраняет невладеющие ссылки на три порта; адаптеры должны жить дольше координатора. |
| `SourceSessionCoordinator::confirm(const Event&, const SourceSessionEvidence&) noexcept` **(реализована)** | До обращения к портам проверяет точное совпадение Event.source/source_instance с закрытыми getters evidence; затем вызывает verifier, читает текущий cursor и передаёт только VerifiedSourceSession в repository. Возвращает cursor только для Confirmed/AlreadyCurrent; прочие статусы закрывают обработку. |

Host harness SSR-01…SSR-20 вызывает production coordinator с fake verifier/reader/repository; test-only SourceSessionEvidenceTestAccess создаёт тестовые фикстуры, а compile-time static_assert подтверждает отсутствие публичного конструктора и aggregate initialization. Fake ports проверяют только программные границы и моделируют CAS/revision. CTest `source_session_verifier` проверяет policy на fake registry; TLS/PTY сценарий в `test_controller_link_service` отдельно проверяет generic ingress callback и binding evidence. Эти host fixtures не являются production registry/storage, production event projector, Gateway composition root или доказательством физической аутентификации ESP32.

### src/core/event_handling.cpp

| Функция | Назначение |
|---|---|
| `EventHandlingResult handle_event(const Event& event, const EventValidationPolicy& validation_policy, const SourceCursor& source_cursor, EventRepository& event_repository, EventProcessorPort& processor) noexcept` **(реализована; SSM-F13)** | Выполняет F01–F04 в фиксированном порядке и передаёт прошедшее F01 проверку конверта, известное, новое и упорядоченное событие профильному обработчику. Не разбирает payload, не продвигает курсор и не выполняет физические действия. |

### src/core/station_event_pipeline.cpp

| Элемент | Назначение |
|---|---|
| StationEventPipeline::StationEventPipeline(EventRepository&, EventProcessorPort&, EventProcessorPort&, EventProcessorPort&, SourceSessionVerifier&, SourceCursorReader&, SourceSessionRepository&, EventProcessorPort* = nullptr) noexcept | Создаёт пять обязательных маршрутов и SourceSessionCoordinator; опциональный шестой F13-маршрут SUBSYSTEM_STATUS_UPDATED регистрируется только при передаче реального профильного обработчика. Все зависимости невладеющие и должны жить дольше pipeline. |
| StationEventPipeline::is_configured() const noexcept | Сообщает о готовности внутреннего EventProcessorRouter до запуска приёма событий. |
| StationEventPipeline::handle(const Event&, const EventValidationPolicy&, const SourceSessionEvidence&) noexcept (реализована; 46 контрактных проверок в tests/) | Выполняет чистые F01/F02 preflight-проверки, затем подтверждает сессию через SourceSessionCoordinator; только Confirmed/AlreadyCurrent запускают F03/F04 и профиль. Не принимает произвольный SourceCursor и не продвигает sequence; pipeline запрещено копировать/перемещать. Generic AuthenticatedEventTransportAdapter, production Registry/Session adapters, ControllerSubsystemStatusEventProjector и Gateway composition root подключены; без injected F13 processor соответствующий маршрут не регистрируется и известное событие возвращает Unsupported. |
| StationEventPipelineResult | Содержит optional session_status (пустой, если F01/F02 остановили событие до обращения к Coordinator) и optional event_result. |

### src/core/event_processor_router.cpp

| Элемент | Назначение |
|---|---|
| `EventProcessorRoute` | Связывает точные `event_type` и `EventCategory` с невладеющим указателем на один профильный `EventProcessorPort`. |
| `EventProcessorRouter::is_configured() const noexcept` | Сообщает, прошла ли таблица маршрутов проверку: она непуста, содержит точные непустые ключи, известные категории, ненулевые указатели и не содержит повторных пар. |
| `EventProcessorRouter::process(const Event&, EventCategory, const SourceCursor&) noexcept` **(реализована; 38 контрактных проверок)** | Выбирает ровно один профиль по точной паре type/category и передаёт ему исходное событие/курсор. Нет маршрута — UNSUPPORTED; недействительная конфигурация, неоднозначность или несогласованный ответ выбранного профиля — ERROR. Не выполняет F01–F04 и не меняет состояние. |

### src/core/global_transition_processor.cpp

| Функция | Назначение |
|---|---|
| `EventProcessorStatus GlobalTransitionProcessor::process(const Event& event, EventCategory category, const SourceCursor& expected_cursor) noexcept` **(реализована; профильная часть SSM-F13)** | Обрабатывает только три согласованные стартовые пары; читает snapshot, получает доверенный контекст, применяет F05 и передаёт план/доказательства в F10 с исходным курсором. Не разбирает payload, не расширяет матрицу переходов и не управляет оборудованием. |

### src/core/operation_event_processor.cpp

| Элемент | Назначение |
|---|---|
| `OperationUpdateDecoderPort::decode_next_status(const Event& event)` | Логический порт доверенного адаптера: преобразует payload конкретного формата в типизированный `OperationLifecycleStatus`; не меняет событие и не фиксирует состояние. |
| `OperationUpdateEventProcessor::process(const Event& event, EventCategory category, const SourceCursor& expected_cursor) noexcept` **(реализована; профильная связка F13 → F09)** | Обрабатывает только `OPERATION_STATUS_UPDATED / Operation`, требует `VALID`, вызывает адаптер, передаёт исходное событие и курсор в F09 и отображает её результат в общий статус F13. Не меняет глобальное состояние станции и не выполняет физических действий. |

### src/core/subsystem_status_event_processor.hpp/.cpp

| Элемент | Назначение |
|---|---|
| `SubsystemStatusEventDecoderPort::decode(const Event& event)` | Storage- и serialization-neutral порт: преобразует непрозрачный payload SUBSYSTEM_STATUS_UPDATED в типизированный subsystem_id/status/error_code/severity/description/reason_text; все views живут до возврата processor. |
| `SubsystemRegistrationReader::find_subsystem(station_id, subsystem_id)` | Read-only точный поиск регистрации подсистемы в локальной станции; результат должен содержать source, registration_revision и ACTIVE/DISABLED/REVOKED. Дубликаты и противоречивые ответы отклоняются. |
| `SubsystemStatusRepository::commit_subsystem_status()` | Логический атомарный commit port: проверяет текущую ACTIVE-регистрацию/revision, весь expected SourceCursor и event_id; сохраняет статус/отчёт/Event и продвигает cursor одной транзакцией. Физическая БД не выбрана. |
| `SubsystemStatusEventProcessor::process()` | Обрабатывает только SUBSYSTEM_STATUS_UPDATED/Subsystem, VALID schema 1; проверяет порядок, payload, точную активную регистрацию и вызывает commit port. Наблюдательные статусы PASS/DEGRADED/FAIL/UNKNOWN не имеют отдельной запрещающей матрицы; любой принятый report возвращается в F13 как NoStateChange относительно глобального автомата, не управляет оборудованием и не меняет глобальное состояние. Зарегистрирован в production StationEventPipeline при явной передаче профильного обработчика; этот путь доступен только из аутентифицированного ControllerLink ingress и Gateway SQLite composition root. |

CTest: `verification/repka_gateway/test_subsystem_status_event_processor.cpp` / `subsystem_status_event_processor`. Fake adapters проверяют fail-closed контракт и передачу expected registration/cursor; это не проверка production persistence или физической атомарности.

### src/core/operation_timeout_event_processor.cpp

| Элемент | Назначение |
|---|---|
| `OperationTimeoutDecoderPort::decode_timeout(const Event& event)` | Логический порт доверенного адаптера: преобразует непрозрачный payload в типизированные `OperationTimeoutType` и `DispatchCertainty`; формат сериализации не выбирает и событие не меняет. |
| `OperationTimeoutEventProcessor::process(const Event& event, EventCategory category, const SourceCursor& expected_cursor) noexcept` **(реализована; профильная связка F13 → F11; контрактный harness — 65 проверок)** | Принимает только `OPERATION_TIMEOUT / Timer`, требует `VALID`, передаёт исходные Event/SourceCursor в F11 и отображает результат в статус F13. Контрактный harness использует заглушку F11; интегрированная сборка ядра и композиция профилей не проверены. |

### src/core/state_transitions.cpp

| Функция | Назначение |
|---|---|
| `TransitionResult resolve_transition(StationState current_state, const Event& event, const TransitionContext& context = {}) noexcept` **(реализована; SSM-F05)** | Определяет, разрешён ли переход из текущего состояния по поступившему событию. Неизвестные переходы отклоняет. |
| `TransitionCommitResult commit_transition(const TransitionPlan& plan, const TransitionEvidence& evidence, const SourceCursor& expected_cursor, StationStateRepository& repository, StateChangedPublisher& publisher) noexcept` **(реализована)** | Повторно проверяет F01/F04/F05; передаёт ожидаемый SourceCursor для атомарного сравнения и продвижения вместе с snapshot/историей/событием, затем публикует StateChanged только после подтверждённой фиксации. |
| `SnapshotReadResult get_snapshot(StationStateSnapshotReader& reader) noexcept` **(реализована; SSM-F16)** | Однократно читает последний подтверждённый snapshot, проверяет state и revision/transition_id; возвращает optional snapshot только при AVAILABLE и не подставляет состояние при ошибке. |
| `TransitionHistoryResult get_transition_history(const TransitionHistoryFilter& filter, TransitionHistoryReader& reader) noexcept` **(реализована; SSM-F17)** | Читает подтверждённую историю; проверяет фильтр, целостность записей и порядок revision; отличает EMPTY, INCOMPLETE и ошибки. Не меняет состояние. |

### src/core/failure_disposition.cpp

| Функция | Назначение |
|---|---|
| `FailureDispositionResult resolve_failure_disposition(const Failure& failure) noexcept` **(реализована)** | По точному FAIL-коду возвращает предварительную реакцию из `docs/09`, §6; неизвестный код не получает выдуманной реакции. Не меняет состояние и не запускает оборудование. |

### src/core/operations.cpp

| Функция | Назначение |
|---|---|
| `ActionRequestResult create_action_request(const ActionPlan& plan, OperationIdProvider& id_provider)` | Формирует логический запрос из ActionPlan и получает operation_id через OperationIdProvider; не сохраняет запрос и не выполняет физическое действие. |
| `PersistResult persist_operation_intent(const ActionRequest& request, StateRepository& repository)` | Передаёт ActionRequest в StateRepository; только впервые устойчиво сохранённая запись разрешает последующий dispatch, повторная запись или ошибка отправку не разрешают. |
| `DispatchResult dispatch_action_request(const ActionRequest& request, const PersistResult& persistence, ActionDispatcher& dispatcher)` | Передаёт запрос только при точном подтверждении новой записи F07; получатель дедуплицирует по operation_id, а ACCEPTED не означает физическое начало действия. |
| `OperationUpdateResult handle_operation_update(const OperationUpdate& update, const SourceCursor& source_cursor, OperationUpdateRepository& repository)` | Проверяет typed update, качество/повтор/порядок события, operation_id и correlation_id; применяет только разрешённый переход и атомарно фиксирует статус, событие и курсор источника. |
| `OperationTimeoutResult handle_operation_timeout(const OperationTimeout& timeout, const SourceCursor& source_cursor, OperationUpdateRepository& repository) noexcept` **(реализована; отдельный модульный тест; полная интеграционная сборка ожидает проверки)** | По подтверждённому таймерному событию переводит только в FAILED до отправки либо OUTCOME_UNKNOWN после возможной отправки; сохраняет событие и lifecycle атомарно. |


### src/core/recovery.cpp

| Функция | Назначение |
|---|---|
| `RecoveryStartResult begin_recovery(RecoveryRepository& repository, RecoveryReportRequester& report_requester) noexcept` **(реализована; SSM-F14)** | Загружает и проверяет последний snapshot, устойчиво отмечает RECOVERY_IN_PROGRESS и запрашивает свежие отчёты; не выполняет сверку и физические действия. |
| `RecoveryReconciliationResult reconcile_after_restart(const StationStateSnapshot& snapshot, const RecoveryReports& reports) noexcept` **(реализована; SSM-F15)** | Проверяет полноту и качество нормализованных отчётов подсистем, сопоставляет незавершённые операции по operation_id/correlation_id; возвращает EvidenceComplete только для полного непротиворечивого набора. Не выбирает глобальное состояние, не снимает маркер и не возобновляет действия. |

---

## 2. Самодиагностика — DiagnosticsManager

### src/diagnostics/diagnostic_validation.cpp

| Функция | Назначение |
|---|---|
| `DiagnosticContextResult validate_diagnostic_context(const DiagnosticContext& context)` | Проверяет, что конфигурация и контекст позволяют корректно запустить нужные проверки. |
| `DiagnosticResult validate_diagnostic_result(const DiagnosticResult& result)` | Проверяет структуру и допустимые значения результата одной диагностической проверки. |
| `SelfTestSummary aggregate_self_test_results(const std::vector<DiagnosticResult>& results)` | Собирает результаты проверок в общую сводку, не заменяя отдельные статусы одним неоднозначным флагом. |

### src/diagnostics/diagnostic_plan.cpp

| Функция | Назначение |
|---|---|
| `DiagnosticPlan build_startup_diagnostic_plan(const DiagnosticContext& context)` | Формирует список применимых стартовых проверок с учётом установленного и настроенного оборудования. |
| `DiagnosticResult execute_diagnostic_check(CheckId check_id, const DiagnosticContext& context)` | Находит зарегистрированную проверку и запускает её через соответствующую службу. |
| `SelfTestReport run_startup_self_test(const DiagnosticContext& context)` | Координирует выполнение стартового плана, ограничивает ожидание ответов и собирает отчёт. |
| `PublishResult publish_self_test_report(const SelfTestReport& report)` | Передаёт отчёт в журнал и StationStateManager. |

### src/diagnostics/checks_runtime.cpp

| Функция | Назначение |
|---|---|
| `DiagnosticResult check_runtime_health()` | Проверяет доступность и работоспособность критических программных задач. |
| `DiagnosticResult check_configuration_integrity()` | Проверяет наличие, структуру, версию и целостность конфигурации станции. |
| `DiagnosticResult check_clock_service()` | Проверяет доступность монотонного времени и качество календарного времени. |
| `DiagnosticResult check_event_logger()` | Проверяет, может ли система записывать события в журнал. |
| `DiagnosticResult check_local_storage()` | Проверяет доступность локального накопителя и возможность безопасного чтения/записи, если такой тест разрешён. |
| `DiagnosticResult check_watchdog_status()` | Проверяет наличие и состояние watchdog без выполнения опасного тестового сброса. |

### src/diagnostics/checks_equipment.cpp

| Функция | Назначение |
|---|---|
| `DiagnosticResult check_power_manager_status()` | Проверяет доступность PowerManager и его каналов контроля. |
| `DiagnosticResult check_safety_manager_status()` | Проверяет доступность SafetyManager и наличие загруженных правил безопасности. |
| `DiagnosticResult check_ble_adapter_status()` | Проверяет адаптер и службу Bluetooth. Отсутствие обнаруженных БАС само по себе не является отказом адаптера. |
| `DiagnosticResult check_ble_scan_capability()` | Проверяет, может ли BLE-служба выполнить сканирование, если такая проверка включена в профиль. |
| `DiagnosticResult check_camera_pipeline_status()` | Проверяет доступность камеры и получение диагностического кадра. |
| `DiagnosticResult check_meteo_service_status()` | Проверяет доступность службы метеостанции. |
| `DiagnosticResult check_sensor_channel_status(SensorId sensor_id)` | Проверяет конкретный настроенный канал датчика и качество его данных. |
| `DiagnosticResult check_motor_service_status()` | Проверяет доступность службы приводов и обратной связи без фактического движения. |
| `DiagnosticResult check_wireless_power_service_status()` | Проверяет доступность зарядной службы без включения передачи энергии. |
| `DiagnosticResult check_drone_integration_status()` | Проверяет бортовые интерфейсы, если БАС присутствует; отсутствие БАС может давать статус «не применимо/отложено». |

---

## 3. Контроль безопасности — SafetyManager

### src/core/safety_manager.cpp

| Функция | Назначение |
|---|---|
| `SafetySnapshot collect_safety_snapshot()` | Собирает актуальные сведения, необходимые для проверки конкретной операции. |
| `PermissionResult evaluate_operation_permission(const OperationRequest& request, const SafetySnapshot& snapshot)` | Выдаёт ALLOW, DENY или UNKNOWN с причиной для конкретной операции. Проверка выполняется в локальном контуре станции. |
| `bool is_permission_current(const PermissionResult& permission, MonotonicTime now)` | Проверяет, не устарело ли выданное разрешение к моменту физического действия. |
| `SafeStopResult request_safe_stop(const OperationId& operation_id, StopReason reason)` | Запрашивает безопасную остановку конкретной операции при выявленной опасности или отказе. |
| `void record_blocking_reason(const OperationId& operation_id, const SafetyReason& reason)` | Регистрирует причину запрета или блокировки для журнала и отображения оператору. |

---

## 4. Миссии и очередь заданий — MissionManager

### src/core/mission_manager.cpp

| Функция | Назначение |
|---|---|
| `MissionValidationResult validate_mission(const Mission& mission)` | Проверяет обязательные поля и общую структуру задания. |
| `MissionTargetResult validate_mission_target(const Mission& mission, const RegisteredDrone& drone)` | Проверяет, что задание адресовано выбранному зарегистрированному БАС. |
| `MissionExpiryResult check_mission_expiry(const Mission& mission, CalendarTime now)` | Проверяет срок действия задания и наличие отзыва. |
| `ManualConfirmationResult check_manual_confirmation(const Mission& mission)` | Определяет, требуется ли подтверждение специалиста: REQUIRED, NOT_REQUIRED или UNKNOWN. UNKNOWN не разрешает автоматический вылет. |
| `MissionSelectionResult select_next_mission(const MissionQueue& queue, const MissionSelectionContext& context)` | Выбирает следующую заранее разрешённую миссию с учётом ограничений и выбранного БАС. |
| `MissionReservationResult reserve_mission(MissionId mission_id)` | Закрепляет выбранную миссию за текущим циклом, чтобы её не выбрал параллельно другой процесс. |
| `MissionUpdateResult mark_mission_dispatched(MissionId mission_id, const DispatchResult& result)` | Фиксирует результат передачи задания бортовой системе. |
| `MissionUpdateResult complete_mission(MissionId mission_id, const MissionResult& result)` | Сохраняет подтверждённый результат завершённой миссии. |
| `MissionUpdateResult revoke_mission(MissionId mission_id, const AuthorizationContext& authorization)` | Отзывает миссию по разрешённой команде и фиксирует причину. |
| `void stop_automation(StopAutomationReason reason)` | Запрещает автоматический переход к следующим миссиям. Сам по себе не является командой возврата или принудительной посадки уже летящего БАС. |
| `QuotaCheckResult check_local_mission_quota(const LocalMissionQuota& quota)` | Проверяет наличие и остаток явно назначенной локальной квоты на случай недоступности сервера. |
| `QuotaUpdateResult set_local_mission_quota(const LocalMissionQuota& quota, const AuthorizationContext& authorization)` | Устанавливает или изменяет локальную квоту только от имени уполномоченного пользователя. |
| `StorageReserveResult check_storage_reserve(const ExpectedMissionData& expected_data)` | Проверяет достаточность свободного места для материалов следующей миссии. |
| `CycleLimitResult check_inspection_cycle_limit(const CyclePolicy& policy)` | Проверяет, не достигнут ли установленный предел циклов без физического осмотра. |

---

## 5. Погодные данные — WeatherManager и MeteoService

### src/weather/weather_manager.cpp

| Функция | Назначение |
|---|---|
| `MeasurementValidationResult validate_weather_measurement(const WeatherMeasurement& measurement)` | Проверяет диапазон, единицы, временную метку и качество отдельного измерения. |
| `WeatherSnapshot filter_weather_measurements(const std::vector<WeatherMeasurement>& measurements)` | Отделяет актуальные и достоверные измерения от устаревших, ошибочных или конфликтующих. |
| `WeatherStatus evaluate_weather_status(const WeatherSnapshot& snapshot)` | Формирует общую оценку погодной обстановки на основании доступных измерений. |
| `WeatherPermission evaluate_weather_permission(OperationType operation, const WeatherSnapshot& snapshot)` | Формирует погодную оценку отдельно для вылета, приёма, открытия крышки, очистки и других операций. Не заменяет итоговое решение SafetyManager. |

### src/weather/meteo_service.cpp

| Функция | Назначение |
|---|---|
| `MeteoReadResult read_weather_measurements()` | Получает текущие измерения от выбранной метеостанции. |
| `MeteoServiceStatus get_meteo_service_status()` | Возвращает доступность и состояние службы метеостанции. |

---

## 6. Обнаружение и идентификация БАС

### src/detection/detection_manager.cpp

| Функция | Назначение |
|---|---|
| `DetectionResult process_detection(const DetectionCandidate& candidate)` | Обрабатывает информацию о потенциально приближающемся аппарате. |
| `AuthenticationResult authenticate_drone(const DetectionCandidate& candidate)` | Проверяет подлинность обнаруженного БАС выбранным для этой конфигурации способом. |
| `DroneMatchResult match_registered_drone(const AuthenticatedDevice& device, const DroneRegistry& registry)` | Сопоставляет подтверждённую идентичность с реестром зарегистрированных БАС станции. |
| `VisualConfirmationResult confirm_visual_match(const DroneMatch& match, const VisualObservation& observation)` | Сопоставляет визуальное наблюдение с идентифицированным аппаратом и посадочной зоной. |
| `TargetSelectionResult select_target_drone(DroneId drone_id)` | Выбирает БАС для следующего физического цикла. Выбор не заменяет последующую аутентификацию. |
| `DetectionStatus get_detection_status()` | Возвращает текущий статус обнаружения и идентификации. |

### src/detection/ble_service.cpp

| Функция | Назначение |
|---|---|
| `BleScanResult start_ble_scan(const BleScanPolicy& policy)` | Запускает BLE-сканирование по заданной политике. |
| `BleScanResult stop_ble_scan()` | Завершает BLE-сканирование. |
| `std::vector<BleDevice> get_ble_scan_results()` | Возвращает обнаруженные BLE-устройства как кандидатов, но не как уже авторизованные БАС. |
| `BleServiceStatus get_ble_service_status()` | Возвращает состояние BLE-адаптера и службы. |

### src/detection/device_auth_manager.cpp

| Функция | Назначение |
|---|---|
| `AuthenticationResult verify_device_authentication(const DeviceIdentity& identity, const AuthenticationProof& proof)` | Проверяет доказательство подлинности устройства по выбранному контракту. |
| `AuthenticationStatus get_device_trust_status(DeviceId device_id)` | Возвращает статус доверия к устройству в реестре. |

---

## 7. Посадочное позиционирование — LandingManager

### src/landing/landing_manager.cpp

| Функция | Назначение |
|---|---|
| `MarkerDetectionResult detect_landing_marker(const CameraFrame& frame)` | Находит посадочный маркер на изображении. |
| `RelativePose estimate_relative_pose(const MarkerDetection& marker, const CameraCalibration& calibration)` | Рассчитывает относительное положение БАС и посадочной цели. |
| `MeasurementQuality assess_pose_quality(const RelativePose& pose, const CameraFrame& frame)` | Оценивает достоверность измеренного положения. |
| `LandingGuidanceResult build_landing_guidance(const RelativePose& pose, const MeasurementQuality& quality)` | Формирует данные коррекции посадки только при достаточном качестве измерения. |
| `GuidanceDispatchResult send_landing_guidance(const LandingGuidance& guidance)` | Передаёт разрешённые корректировки через интерфейс штатного автопилота. |
| `void stop_guidance_on_tracking_loss(const TrackingStatus& status)` | Прекращает выдачу недостоверных корректировок при потере маркера или сопровождения. |
| `LandingEvidence collect_landing_evidence()` | Собирает доступные независимые признаки касания и безопасного положения БАС. |
| `LandingConfirmation confirm_landing(const LandingEvidence& evidence)` | Формирует результат подтверждения посадки; одного визуального признака недостаточно, если контракт требует независимые признаки. |

### src/landing/vision_service.cpp

| Функция | Назначение |
|---|---|
| `CameraFrame capture_frame()` | Получает кадр с камеры станции. |
| `CameraServiceStatus get_camera_status()` | Возвращает состояние камеры и тракта передачи изображения. |

### src/landing/autopilot_interface.cpp

| Функция | Назначение |
|---|---|
| `AutopilotStatus get_autopilot_status()` | Получает доступные статусы штатного автопилота. |
| `MissionDispatchResult transmit_authorized_mission(const Mission& mission)` | Передаёт заранее разрешённое задание через документированный интерфейс выбранного автопилота. |
| `GuidanceDispatchResult transmit_landing_guidance(const LandingGuidance& guidance)` | Передаёт посадочные корректировки через согласованный интерфейс. |
| `FlightIntegrationStatus check_autopilot_link()` | Проверяет доступность и состояние связи с автопилотом. |

---

## 8. Крышка и механика

### src/mechanics/lid_manager.cpp

| Функция | Назначение |
|---|---|
| `LidPermissionResult check_lid_permission(LidOperation operation, const LidContext& context)` | Проверяет условия и актуальное разрешение на открытие или закрытие крышки. |
| `LidOperationResult request_open_lid(const LidContext& context)` | Запрашивает открытие крышки после проверки условий и разрешения. |
| `LidOperationResult request_close_lid(const LidOperationContext& context)` | Запрашивает закрытие крышки с учётом контекста AFTER_LANDING или AFTER_TAKEOFF. |
| `LidPosition get_lid_position()` | Возвращает подтверждённое положение крышки по обратной связи датчиков. |
| `LidMotionStatus monitor_lid_motion(const LidOperationId& operation_id)` | Контролирует движение, положение, нагрузку и время выполнения операции крышки. |
| `LidStopResult stop_lid_motion(StopReason reason)` | Останавливает привод при препятствии, заклинивании, потере достоверности или иной опасной ситуации. |

### src/mechanics/motor_service.cpp

| Функция | Назначение |
|---|---|
| `MotorFeedback read_motor_feedback(DeviceId device_id)` | Получает состояние привода и доступную обратную связь. |
| `MotorCommandResult move_actuator(DeviceId device_id, const MotionCommand& command)` | Выполняет разрешённую команду движения конкретного механизма. |
| `MotorStopResult stop_actuator(DeviceId device_id, StopReason reason)` | Останавливает конкретный привод и возвращает результат остановки. |

---

## 9. Самоочистка — CleaningManager

### src/mechanics/cleaning_manager.cpp

| Функция | Назначение |
|---|---|
| `CleaningReadiness check_cleaning_conditions()` | Проверяет, свободна ли площадка, безопасно ли положение крышки и разрешена ли очистка. |
| `CleaningStartResult start_cleaning()` | Запускает механизм очистки после получения актуального разрешения SafetyManager. |
| `CleaningProgress monitor_cleaning(const CleaningOperationId& operation_id)` | Контролирует ход очистки и обратную связь механизма. |
| `CleaningFault detect_cleaning_fault(const CleaningProgress& progress)` | Выявляет заклинивание, отсутствие движения или другую неисправность. |
| `CleaningResult verify_cleaning_result(const CleaningEvidence& evidence)` | Фиксирует результат очистки по доступным признакам. |
| `RetryDecision evaluate_cleaning_retry(const CleaningFault& fault, unsigned int attempts)` | Определяет, допускается ли ещё одна попытка; бесконечные повторы запрещены. |
| `CleaningStopResult stop_cleaning(StopReason reason)` | Останавливает очистку при опасности или отказе. |

---

## 10. Беспроводная зарядка

### src/charging/charging_manager.cpp

| Функция | Назначение |
|---|---|
| `ChargingPrecheckResult check_charging_conditions()` | Проверяет положение БАС, состояние BMS, температуры, взаимное положение катушек и доступные признаки посторонних предметов. |
| `ChargingStartResult start_charging()` | Запускает передачу энергии после всех проверок и получения актуального разрешения. |
| `ChargingStatus get_charging_status()` | Возвращает текущий статус зарядки и измеряемые параметры. |
| `ChargingStopResult stop_charging(StopReason reason)` | Штатно или аварийно прекращает передачу энергии. |
| `ChargingCompletionResult verify_charging_completion()` | Проверяет подтверждённое завершение зарядки по данным зарядного модуля и BMS. |
| `ChargingFault classify_charging_fault(const ChargingTelemetry& telemetry)` | Классифицирует отказ зарядки и формирует причину для журнала. |

### src/charging/wireless_power_service.cpp

| Функция | Назначение |
|---|---|
| `PowerTransferResult enable_power_transfer(const PowerTransferProfile& profile)` | Включает передающий модуль по согласованному профилю. |
| `PowerTransferResult disable_power_transfer(StopReason reason)` | Отключает передающий модуль. |
| `WirelessPowerStatus get_wireless_power_status()` | Получает статус силового зарядного оборудования и его защит. |

### src/charging/bms_interface.cpp

| Функция | Назначение |
|---|---|
| `BatteryStatus read_battery_status()` | Получает доступные данные аккумулятора и BMS. |
| `ChargeAuthorization request_charge_authorization()` | Запрашивает у BMS разрешение на зарядку. Не обходит и не заменяет защиту BMS. |

---

## 11. Получение и передача данных

### src/data/data_manager.cpp

| Функция | Назначение |
|---|---|
| `DataReceiveResult receive_mission_materials(const MissionContext& mission)` | Координирует получение материалов с БАС для конкретной миссии. |
| `DataPreparationResult verify_and_store_materials(const TransferJobId& job_id)` | Проверяет полученные материалы и обеспечивает их локальное сохранение. |
| `DataReadinessResult check_data_readiness_for_next_mission(TransferCompletionCriterion criterion)` | Проверяет выполнение выбранного критерия LOCAL_COMPLETE или SERVER_CONFIRMED перед допуском следующей миссии. |
| `MissionBindingResult bind_material_to_mission(const FileManifest& manifest, const MissionContext& mission)` | Связывает полученные материалы с БАС, миссией и маршрутом. Точный контракт этой связи ещё предстоит определить. |

### src/data/drone_storage_manager.cpp

| Функция | Назначение |
|---|---|
| `DroneStorageConnectionResult connect_to_drone_storage()` | Устанавливает связь с файловым сервисом или накопителем БАС. |
| `FileManifestResult get_file_manifest(const MissionId& mission_id)` | Получает перечень файлов и метаданные материалов миссии. |
| `FileChunkResult read_file_chunk(const FileId& file_id, std::uint64_t offset, std::size_t size)` | Получает заданный фрагмент файла с БАС. |
| `DroneStorageStatus get_drone_storage_status()` | Получает состояние бортового накопителя и доступное место, если интерфейс БАС это предоставляет. |

### src/data/file_transfer_manager.cpp

| Функция | Назначение |
|---|---|
| `TransferJob create_transfer_job(const FileManifest& manifest, const MissionContext& mission)` | Создаёт задачу передачи файлов и связывает её с контекстом миссии. |
| `ChunkReceiveResult receive_file_chunk(const TransferJobId& job_id, const FileChunk& chunk)` | Принимает фрагмент файла и проверяет его базовую корректность. |
| `TransferResumeResult resume_transfer(const TransferJobId& job_id)` | Возобновляет незавершённую передачу с подтверждённой позиции. |
| `TransferCompletionResult finalize_file_transfer(const TransferJobId& job_id)` | Завершает передачу после сборки файла и проверки целостности. |

### src/data/data_integrity_manager.cpp

| Функция | Назначение |
|---|---|
| `IntegrityResult verify_chunk_checksum(const FileChunk& chunk)` | Проверяет контрольную сумму полученного фрагмента, если она предусмотрена контрактом. |
| `Checksum calculate_file_checksum(const StoredFile& file)` | Вычисляет контрольную сумму собранного файла. |
| `IntegrityResult verify_completed_file(const StoredFile& file, const FileManifestEntry& manifest_entry)` | Проверяет полноту и целостность итогового файла относительно доступных исходных данных. |
| `IntegrityResult verify_manifest_consistency(const FileManifest& manifest, const TransferResult& result)` | Проверяет соответствие принятых файлов и фрагментов манифесту. |

### src/data/local_storage_manager.cpp

| Функция | Назначение |
|---|---|
| `StorageCapacityResult check_available_capacity(std::uint64_t required_bytes)` | Проверяет, достаточно ли свободного места для следующего приёма. |
| `StorageReservationResult reserve_storage(std::uint64_t required_bytes, const MissionId& mission_id)` | Резервирует место под материалы конкретной миссии. |
| `StorageWriteResult save_file_atomically(const StoredFile& file)` | Сохраняет файл так, чтобы сбой записи не оставлял его ошибочно отмеченным как готовый. |
| `StoredFileResult get_stored_file(const FileId& file_id)` | Находит и предоставляет ранее сохранённый файл. |
| `StorageDeleteResult delete_local_copy(const FileId& file_id, const RetentionAuthorization& authorization)` | Удаляет локальную копию только после подтверждения и по утверждённой политике хранения. |

### src/data/encryption_manager.cpp

| Функция | Назначение |
|---|---|
| `EncryptionResult encrypt_local_file(const FileId& file_id, const KeyReference& key)` | Защищает файл при локальном хранении с использованием выбранной криптографической библиотеки. |
| `DecryptionResult decrypt_local_file(const EncryptedFile& file, const KeyReference& key)` | Расшифровывает локальный файл при наличии необходимых полномочий и ключа. |
| `EncryptionResult encrypt_sensitive_configuration(const Configuration& configuration, const KeyReference& key)` | Защищает чувствительные параметры конфигурации. |
| `KeyStatus verify_key_reference(const KeyReference& key)` | Проверяет доступность и статус ссылки на ключ, не извлекая закрытый ключ в открытом виде. |

### src/data/upload_manager.cpp

| Функция | Назначение |
|---|---|
| `UploadResult upload_file_metadata(const FileManifestEntry& metadata)` | Передаёт серверу метаданные файла. |
| `UploadResult upload_file_chunk(const FileId& file_id, const FileChunk& chunk)` | Загружает фрагмент файла на сервер. |
| `ServerReceiptResult confirm_server_receipt(const FileId& file_id)` | Получает и проверяет подтверждение сервером приёма материалов. |

### src/data/transfer_queue_manager.cpp

| Функция | Назначение |
|---|---|
| `QueueResult enqueue_transfer(const TransferJob& job)` | Добавляет задачу в локальную очередь передачи. |
| `TransferJobResult get_next_pending_transfer()` | Возвращает следующую готовую к отправке задачу. |
| `QueueUpdateResult update_transfer_status(const TransferJobId& job_id, TransferStatus status)` | Сохраняет текущий статус передачи. |
| `RetryDecision schedule_transfer_retry(const TransferJob& job, const TransferError& error)` | Определяет, можно ли повторить передачу и при каких условиях. |
| `QueueRestoreResult restore_pending_transfers()` | Восстанавливает незавершённые задачи передачи после перезапуска. |

---

## 12. Связь и сервер

### src/communications/communication_service.cpp

| Функция | Назначение |
|---|---|
| `ConnectionResult connect_to_server(const ServerConnectionConfig& config)` | Подключается к станции/серверному контуру через серверный маршрут. |
| `ConnectionResult connect_to_local_wifi(const WifiConnectionConfig& config)` | Устанавливает локальное подключение по Wi-Fi. |
| `ConnectionResult connect_to_wired_service(const WiredConnectionConfig& config)` | Устанавливает проводное сервисное подключение. |
| `void disconnect(ConnectionId connection_id)` | Закрывает указанное соединение. |
| `SendResult send_message(ConnectionId connection_id, const Message& message)` | Передаёт сообщение по выбранному защищённому соединению. |
| `ReceiveResult receive_message(ConnectionId connection_id)` | Получает сообщение из соединения. |
| `ConnectionStatus get_connection_status(ConnectionId connection_id)` | Возвращает состояние канала и актуальность связи. |

### src/communications/server_api.cpp

| Функция | Назначение |
|---|---|
| `MissionSyncResult synchronize_missions()` | Получает и синхронизирует разрешённые задания с сервером. |
| `UploadResult upload_material_chunk(const FileChunk& chunk)` | Передаёт серверу очередной фрагмент материала. |
| `ServerReceiptResult request_material_receipt(const FileId& file_id)` | Запрашивает подтверждение приёма материала сервером. |
| `TelemetryPublishResult publish_station_telemetry(const TelemetrySnapshot& snapshot)` | Публикует телеметрию станции. |

---

## 13. Электропитание — PowerManager

### src/power/power_manager.cpp

| Функция | Назначение |
|---|---|
| `PowerSnapshot read_power_status()` | Получает актуальные данные о входном питании и доступных линиях питания. |
| `PowerBudgetResult check_power_budget(const PowerRequest& request)` | Проверяет, хватает ли доступной мощности для запрашиваемой операции. |
| `BackupPowerResult switch_to_backup_power(BackupPowerReason reason)` | Переводит систему на резервный источник по утверждённому сценарию. |
| `PowerRecoveryResult restore_normal_power()` | Проверяет возможность возврата к штатному питанию. |
| `PowerFault classify_power_fault(const PowerSnapshot& snapshot)` | Определяет и описывает выявленный отказ электропитания. |

---

## 14A. Системное журналирование Linux Gateway

### src/platform/logging/logger.cpp

| Функция | Назначение |
|---|---|
| `LoggerResult initialize_logger(const LoggerConfig& config)` | Проверяет sink, уровень и путь (включая NUL); перед переинициализацией проверяет flush/close прежнего файлового sink, затем открывает заданный sink. |
| `std::vector<LogField> redact_sensitive_fields(const std::vector<LogField>& fields)` | Маскирует значения полей, ключи которых указывают на пароль, токен, секрет, авторизацию, приватный ключ, credential или API key; поддерживаются варианты с подчёркиванием, дефисом и пробелом. |
| `LoggerResult write_log_record(LogLevel level, const std::string& component, const std::string& message, const std::vector<LogField>& fields)` | Отклоняет неизвестный уровень, фильтрует записи ниже порога, сбрасывает буфер файлового потока перед проверкой размера, проверяет доступность файла и безопасно сравнивает размер до ротации; затем записывает структурированную JSONL-запись с UTC-временем и полями. |
| `LoggerResult rotate_log_files()` | Выполняет ротацию в пределах настроенного числа архивов; если настроенный файл исчез, возвращает UNAVAILABLE. |
| `LoggerResult flush_log()` | Сбрасывает буферы файлового и stderr sink. |
| `LoggerResult shutdown_logger()` | Сбрасывает буферы, закрывает файловый sink и возвращает явную ошибку при сбое flush/close. |

PlatformLogger обслуживает технические сообщения процесса. Доменный EventLogger и аудит действий пользователей остаются отдельными компонентами; свободный текст сообщения не должен содержать секреты.

---

## 14. Телеметрия и журналирование

### src/telemetry/telemetry_manager.cpp

| Функция | Назначение |
|---|---|
| `TelemetrySnapshot collect_telemetry_snapshot()` | Собирает доступные состояния модулей, датчиков и операций в единый снимок телеметрии. |
| `PublishResult publish_telemetry(const TelemetrySnapshot& snapshot)` | Передаёт телеметрию локальному клиенту или серверу. |
| `TelemetrySnapshot mark_stale_values(const TelemetrySnapshot& snapshot, MonotonicTime now)` | Помечает устаревшие данные, чтобы их нельзя было принять за актуальные. |

### src/telemetry/event_logger.cpp

| Функция | Назначение |
|---|---|
| `LogWriteResult write_event(const EventLogRecord& record)` | Сохраняет структурированную запись о событии, ошибке или действии пользователя. |
| `LogQueryResult query_events(const LogFilter& filter)` | Возвращает записи журнала по фильтру. |
| `LogExportResult export_log(const LogFilter& filter, ExportFormat format)` | Готовит выбранные записи для экспорта оператором. |

---

## 14B. Настройки процесса Linux Gateway

### src/platform/linux/runtime_config.cpp

| Функция / тип | Назначение |
|---|---|
| `RuntimeConfig` | Типизированные настройки процесса: порт локального API и параметры PlatformLogger. |
| `validate_runtime_config(const RuntimeConfig& config)` | Проверяет диапазон порта, уровень и sink журналирования, размер файла и лимит архивов; возвращает первое поле с ошибкой. |
| `initialize_runtime_logger(const RuntimeConfig& config)` | Валидирует настройки до побочного действия; при ошибке возвращает REJECTED и не запускает Logger, при валидной конфигурации инициализирует PlatformLogger и различает READY/UNAVAILABLE. |
| `kGatewayBindAddress` | Фиксированный адрес привязки диагностического Gateway `127.0.0.1`; не является настраиваемым параметром. |
| `kDefaultGatewayPort` | Порт API по умолчанию — 8080. |

Валидатор не читает конфигурационный файл, не открывает сокеты/файлы и не запускает службы. `initialize_runtime_logger()` открывает настроенный файловый sink в рамках инициализации Logger; родительский каталог должен существовать. Формат и загрузчик внешнего файла описаны в §14C. Не путать RuntimeConfig с конфигурацией станции CFG-F00…CFG-F08.

---

## 14C. Загрузка файла настроек процесса Linux Gateway

### src/platform/linux/runtime_config_file.cpp

| Функция / тип | Назначение |
|---|---|
| `RuntimeConfigFileStatus` | Различает LOADED, NOT_FOUND, READ_ERROR и INVALID. |
| `RuntimeConfigFileResult` | Возвращает RuntimeConfig либо причину отказа и номер строки. |
| `parse_runtime_config_text(const std::string& contents)` | Разбирает текстовый формат с ASCII-ключами `key=value`; поддерживает пустые строки, строки-комментарии с `#` и необязательный UTF-8 BOM. |
| `load_runtime_config_file(const std::string& path)` | Читает явно указанный обычный файл размером не более 65536 байт; различает отсутствие, ошибку чтения и некорректное содержимое. |

Поддерживаемые ключи: `api_port`, `log_level`, `log_file`, `log_stderr`, `log_max_file_size_bytes`, `log_max_rotated_files`. Дубликаты и неизвестные ключи запрещены. Inline-комментарии и кавычки не имеют специального значения. Загрузчик не выбирает путь по умолчанию и не запускает Logger. Межполевую проверку выполняют после применения CLI overrides.

---

## 14D. Аргументы запуска Linux Gateway

### src/platform/linux/command_line.cpp

| Функция / тип | Назначение |
|---|---|
| `CommandLineParseStatus` | Различает успешный разбор, запрос справки и некорректные аргументы. |
| `CommandLineParseResult` | Возвращает разобранные CLI-значения, маску явно заданных overrides, путь конфигурационного файла либо причину отказа без побочных действий. |
| `parse_command_line(int argc, const char* const argv[])` | Разбирает поддерживаемые параметры процесса, включая `--config-file`; отклоняет повторы/неизвестные параметры и ошибки значений. Если файл задан, межполевую проверку откладывает до объединения; иначе проверяет CLI-конфигурацию. |
| `apply_command_line_overrides(const CommandLineParseResult& parsed, RuntimeConfig& config)` | Применяет к базовой конфигурации только те CLI-значения, которые пользователь явно указал. |
| `command_line_help(const std::string& executable_name)` | Формирует справку о поддерживаемых аргументах, конфигурационном файле и безопасном loopback bind. |

Поддерживаемые параметры: `--config-file`, `--api-port`, `--log-level`, `--log-file`, `--log-stderr`, `--no-log-stderr`, `--log-max-file-size-bytes`, `--log-max-rotated-files`, отдельный `--help`. Сам парсер не читает файл и не запускает службы. Адрес `127.0.0.1` не настраивается из CLI. Межполевая проверка выполняется Bootstrap после объединения defaults → file → CLI.

---

## 15. Конфигурация станции

### CFG-F00 — типы и логические порты

| Файл | Состав | Назначение |
|---|---|---|
| `src/configuration/configuration_types.hpp` | `ConfigurationLoadStatus`, `ConfigurationIssueSeverity`, `ConfigurationValueType`, `ConfigurationValueView`, `ConfigurationDocumentView`, `ConfigurationCandidate`, `ConfigurationIssue`, `ConfigurationValidationResult`, `ConfigurationSnapshotResult`, `ValidatedConfigurationSnapshot`, `ConfigurationLoadResult` | Базовые типы и нейтральное представление конфигурации CFG-F00. View-типы не владеют памятью; снимок создаётся только через CFG-F06 |
| `src/configuration/configuration_ports.hpp` | `SettingsReadStatus`, `SettingsRepository`, `ConfigurationDecodeStatus`, `ConfigurationDecoderPort` | Read-only границы получения сохранённой записи и её декодирования без выбора хранилища/формата |

### CFG-F01 — проверка версии схемы

| Файл | Состав | Назначение |
|---|---|---|
| `src/configuration/configuration_types.hpp` | `ConfigurationValidationStatus`, `ConfigurationValidationResult`, `ConfigurationSchemaVersionSetView` | Статус проверки, список проблем и явный не владеющий памятью набор поддерживаемых версий |
| `src/configuration/configuration_manager.hpp` | `validate_configuration_schema_version()` | Публичное объявление проверки версии по явно переданному набору |
| `src/configuration/configuration_manager.cpp` | Реализация CFG-F01 | Точное сопоставление идентификаторов и формирование статусов/issues без встроенного production-набора |
| `tests/unit/test_configuration_schema_version.cpp` | 19 host-проверок | Позитивные, негативные и граничные проверки CFG-F01 |

### CFG-F02 — проверка обязательных полей

| Файл | Состав | Назначение |
|---|---|---|
| `src/configuration/configuration_types.hpp` | `ConfigurationRequiredFieldPathView`, `ConfigurationRequiredFieldSetView` | Явная политика обязательных полей как не владеющие памятью сегментированные пути |
| `src/configuration/configuration_manager.hpp/.cpp` | `validate_required_configuration_fields()` | Проверяет наличие обязательных путей и формирует полный список issues в caller-owned buffer |
| `tests/unit/test_configuration_required_fields.cpp` | 25 host-проверок | Наличие и отсутствие полей, вложенные пути, Null/Unknown, неоднозначные ключи, повреждённые views и граничная ёмкость буфера |

### Публичный API ConfigurationManager (объявления в `configuration_manager.hpp`)

| Функция | Реализация | Назначение |
|---|---|---|
| `validate_configuration_schema_version(candidate, supported_versions)` — CFG-F01 | `configuration_manager.cpp` | Точное сопоставление schema_version с явно переданным набором; production-версии не встроены. |
| `validate_required_configuration_fields(candidate, required_fields, issue_buffer, issue_capacity)` — CFG-F02 | `configuration_manager.cpp` | Проверка наличия сегментированных путей; тип и значение проверяются отдельно. |
| `validate_configuration_values(candidate, rules, issue_buffer, issue_capacity)` — CFG-F03 | `configuration_manager.cpp` | Проверка точных типов, скалярных допустимых значений и числовых границ; единицы не преобразуются и не проверяются без метаданных. |
| `validate_configuration_consistency(candidate, rules, issue_buffer, issue_capacity)` — CFG-F04 | `configuration_consistency.cpp` | Проверка прямых отношений между двумя скалярными полями по явной политике. |
| `validate_configuration(candidate, policy, issue_buffer, issue_capacity)` — CFG-F05 | `configuration_validation.cpp` | Оркестрация F01–F04, объединение issues и привязка результата к schema_version/revision кандидата. |
| `build_validated_configuration_snapshot(candidate, validation)` — CFG-F06 | `configuration_snapshot.cpp` | Getter-only snapshot только после VALID и точного совпадения идентификаторов кандидата; дерево не копируется. |
| `load_configuration(repository, decoder, policy, issue_buffer, issue_capacity)` — CFG-F07 | `configuration_load.cpp` | Один read → decode → F05 → F06 проход; нарушения портов и статусов закрываются через INTERNAL_ERROR. |
| `get_configuration_revision(active_configuration)` — CFG-F08 | `configuration_revision.cpp` | Read-only извлечение ревизии из выбранного вызывающим слоем согласованного VALID результата; не хранит активное состояние. |

Функции записи и применения конфигурации остаются будущими задачами и не входят в реализованный read-only срез CFG-F00…F08.
---

## 16. Идентичность станции — StationIdentityManager

### src/identity/station_identity_manager.cpp

| Функция | Назначение |
|---|---|
| `bool validate_station_id_format(std::string_view station_id)` | Проверяет формат Station ID. Конкретный формат ещё должен быть согласован. |
| `StationId generate_station_id(RandomSource& random_source)` | Генерирует новый Station ID в доверенном сервисе регистрации. Предлагаемый UUIDv4 пока не согласован. |
| `IdentityValidationResult validate_station_identity_record(const StationIdentity& record)` | Проверяет согласованность Station ID, серийного номера, ссылки на ключ и статуса регистрации. |
| `RegistrationConflictResult find_registration_conflicts(const StationIdentity& candidate, StationRegistry& registry)` | Проверяет, не заняты ли предлагаемые ID, серийный номер или криптографический отпечаток. |
| `ProvisioningValidationResult validate_provisioning_context(const ProvisioningContext& context)` | Проверяет полномочия регистратора и действительность физически активированного режима регистрации. |
| `CredentialReference create_device_key_reference(KeyStore& key_store)` | Создаёт индивидуальный ключ и возвращает только ссылку на него, не раскрывая закрытый ключ. |
| `EnrollmentRequest build_enrollment_request(const ProvisioningContext& context, const CredentialReference& key_ref)` | Формирует запрос регистрации и одноразовый challenge/запрос сертификата. |
| `EnrollmentValidationResult validate_enrollment_response(const EnrollmentResponse& response, const EnrollmentRequest& request, TrustStore& trust_store)` | Проверяет доверие к ответу регистрации, challenge, Station ID и соответствие публичного ключа. |
| `IdentitySaveResult persist_station_identity(const StationIdentity& record, StationIdentityRepository& repository)` | Атомарно сохраняет подтверждённую идентичность станции. |
| `EnrollmentResult complete_station_enrollment(const EnrollmentRequest& request, const EnrollmentResponse& response, const ProvisioningContext& context)` | Завершает регистрацию только после успешной проверки и устойчивого сохранения результата. |
| `CertificateValidationResult verify_station_certificate(const PeerCertificate& certificate, const StationIdentity& record, TrustStore& trust_store)` | Проверяет сертификат подключившегося устройства и его соответствие записи станции. |
| `CredentialRotationResult begin_credential_rotation(StationId station_id, const AuthorizationContext& authorization)` | Начинает процедуру плановой или аварийной смены учётных данных станции. |
| `CredentialRotationResult apply_credential_rotation(const RotationResponse& response)` | Проверяет и применяет новые учётные данные с сохранением истории. |
| `RevocationResult revoke_station_credential(StationId station_id, CredentialId credential_id, const Reason& reason)` | Инициирует отзыв учётных данных станции. |
| `StationRetirementResult retire_station(StationId station_id, const AuthorizationContext& authorization, const Reason& reason)` | Выводит станцию из эксплуатации, не позволяя повторно использовать её ID. |
| `VoucherValidationResult validate_offline_enrollment_voucher(const EnrollmentVoucher& voucher, const ProvisioningContext& context)` | Проверяет подпись, срок, область действия и однократность офлайн-ваучера. |
| `EnrollmentSyncResult synchronize_pending_enrollment(const StationIdentity& record)` | Синхронизирует ранее выполненную офлайн-регистрацию с центральным реестром. |
| `IdentityRecoveryResult recover_station_identity(const ProvisioningContext& context, const CredentialReference& old_credential)` | Восстанавливает идентичность по сервисной процедуре после замены контроллера или ключа. |

---

## 17. Локальная проверка пользовательских полномочий — UserAccessManager

### src/access/user_access_manager.cpp

Модуль на Repka Pi не является центральным реестром организаций или владельцев. Он проверяет подписанные полномочия, выданные доверенным сервером, для прямого локального доступа и дополнительно проверяет права для каждой операции.

| Функция | Назначение |
|---|---|
| `GrantSignatureResult verify_access_grant_signature(const SignedStationAccessGrant& grant, const TrustedIssuerSet& issuers)` | Проверяет криптографическую подпись локального grant по набору доверенных издателей. |
| `GrantValidationResult validate_station_access_grant(const SignedStationAccessGrant& grant, StationId target_station, UserId actor, ResourceType resource, Permission permission, Timestamp now)` | Проверяет целевую станцию, пользователя, срок, ресурс и разрешение, указанные в подписанном grant. |
| `RevocationResult apply_signed_grant_revocation(const SignedGrantRevocation& revocation)` | Проверяет и применяет полученное от сервера подписанное уведомление об отзыве grant. |
| `ResourceAuthorizationResult authorize_station_resource(const LocalAccessContext& context, StationId station_id, ResourceType resource, Permission permission)` | Разрешает локальный доступ к ресурсу только при наличии действующего и проверенного полномочия; при неопределённости действует запрет по умолчанию. |
| `AuthorizationResult authorize_command(const LocalAccessContext& context, StationId station_id, CommandType command)` | Проверяет локальный grant для команды; не заменяет локальные межблокировки и SafetyManager. |
| `SessionRevocationResult revoke_local_sessions_for_grant(GrantId grant_id)` | Прекращает локальные сессии, основанные на отозванном или истёкшем grant. |

### 17.1. Центральная серверная служба идентичности и авторизации

Следующие функции относятся к центральному серверу DockStation, а не к локальному Linux Gateway/Repka Pi. Конкретный backend, кодовые каталоги, схема БД и API-контракты определяются отдельной серверной спецификацией.

| Функция | Назначение |
|---|---|
| `create_user_account(), authenticate_user(), change_user_password(), set_user_status()` | Создание, аутентификация, изменение и блокировка индивидуальных пользовательских учётных записей. |
| `create_owner_space(), add_owner_space_member(), update_owner_space_membership(), revoke_owner_space_membership()` | Создание OwnerSpace частного владельца/организации и управление членством пользователей в его пределах. |
| `assign_station_owner(), transfer_station_ownership()` | Первичное назначение единственного владельца станции и контролируемая передача станции между OwnerSpace. |
| `list_accessible_stations()` | Формирование списка только тех станций, к которым пользователь имеет действующий доступ. |
| `grant_station_access(), revoke_station_access()` | Выдача, ограничение по функциям и сроку, а также отзыв прав пользователя на конкретные станции. |
| `authorize_station_resource(), authorize_command()` | Проверка принадлежности Station ID, membership/grant и конкретного разрешения при каждом серверном запросе к данным или команде. |
| `issue_signed_station_access_grant(), publish_signed_grant_revocation()` | Выпуск ограниченного локального grant и доставка подписанного отзыва подключённой станции. |
| `record_access_audit_event()` | Запись успешных и отклонённых обращений, административных изменений и передачи владения без секретов. |

Центральный сервер является источником истины для OwnerSpace, пользователей, членства, владения станциями и grants. Локальная станция не может самостоятельно создавать членство или менять владельца в центральном реестре. При потере связи локальная проверка действует только в пределах подписанного grant и утверждённой политики автономной работы.

## 18. Настольное приложение Windows

### operator/connection_manager.cpp

| Функция | Назначение |
|---|---|
| `ConnectionResult connect_via_server(const ServerConnectionConfig& config)` | Подключает приложение к станции через сервер. |
| `ConnectionResult connect_via_wifi(const WifiConnectionConfig& config)` | Подключает приложение к станции по локальному Wi-Fi. |
| `ConnectionResult connect_via_wire(const WiredConnectionConfig& config)` | Подключает приложение через проводной сервисный интерфейс. |
| `void disconnect_active_connection()` | Завершает текущее подключение. |
| `ConnectionStatus get_active_connection_status()` | Показывает текущий канал и доступность соединения. |

### operator/station_api_client.cpp

| Функция | Назначение |
|---|---|
| `StationSnapshot get_station_snapshot(StationId station_id)` | Запрашивает текущее состояние станции. |
| `SubscriptionResult subscribe_to_station_updates(StationId station_id)` | Подписывается на обновления телеметрии и событий. |
| `CommandSubmitResult send_station_command(const CommandRequest& command)` | Отправляет команду через активный канал и возвращает её идентификатор. |
| `CommandResult query_command_result(CommandId command_id)` | Запрашивает фактический результат ранее отправленной команды, если ответ был потерян. Не повторяет команду автоматически. |

### operator/telemetry_store.cpp

| Функция | Назначение |
|---|---|
| `void update_measurement(const SensorMeasurement& measurement)` | Обновляет показание датчика в локальной модели отображения. |
| `void mark_measurement_stale(SensorId sensor_id)` | Помечает показание устаревшим при потере обновлений или связи. |
| `MeasurementHistory get_measurement_history(const MeasurementFilter& filter)` | Возвращает историю измерений для графика или просмотра. |

### operator/command_center.cpp

| Функция | Назначение |
|---|---|
| `CommandRequest build_command_request(CommandType type, const CommandParameters& parameters)` | Формирует запрос команды с адресатом и идентификаторами. |
| `CommandSubmitResult submit_command(const CommandRequest& request)` | Отправляет подтверждённую пользователем команду. |
| `OperationStatus track_operation_status(OperationId operation_id)` | Отображает стадии выполнения операции и её причину отказа. |
| `ConfirmationResult confirm_dangerous_action(const CommandRequest& request)` | Запрашивает явное подтверждение пользователя перед опасным действием. |

### operator/session_manager.cpp

| Функция | Назначение |
|---|---|
| `LoginResult login(const LoginRequest& request)` | Выполняет вход пользователя через доверенный сервис аутентификации. |
| `LogoutResult logout()` | Завершает текущую пользовательскую сессию. |
| `SessionRefreshResult refresh_session()` | Продлевает сессию, если это разрешено политикой сервера. |
| `void clear_local_session()` | Удаляет локальное состояние сессии после выхода или отзыва доступа. |

### operator/camera_viewer.cpp

| Функция | Назначение |
|---|---|
| `CameraOpenResult open_camera_stream(StationId station_id)` | Запрашивает защищённый видеопоток при наличии камеры и соответствующих прав. |
| `void close_camera_stream()` | Закрывает видеопоток. |
| `CameraFrameResult request_camera_frame()` | Запрашивает отдельный кадр, если поток недоступен или не требуется. |

### operator/audit_viewer.cpp

| Функция | Назначение |
|---|---|
| `AuditQueryResult load_audit_events(const AuditFilter& filter)` | Загружает записи аудита. |
| `AuditQueryResult filter_audit_events(const AuditFilter& filter)` | Фильтрует события по станции, пользователю, модулю, времени или типу команды. |
| `AuditExportResult export_audit_events(const AuditFilter& filter, ExportFormat format)` | Экспортирует выбранные записи аудита. |

### operator/mission_and_drone_controls.cpp

| Функция | Назначение |
|---|---|
| `DroneListResult load_registered_drones(StationId station_id)` | Загружает список БАС, зарегистрированных за станцией. |
| `TargetSelectionResult select_target_drone(DroneId drone_id)` | Выбирает БАС для следующего цикла. |
| `QuotaUpdateResult set_local_mission_quota(const LocalMissionQuota& quota)` | Устанавливает локальную квоту заранее разрешённых миссий при недоступности сервера. |
| `TransferCriterionResult set_transfer_completion_criterion(TransferCompletionCriterion criterion)` | Выбирает LOCAL_COMPLETE или SERVER_CONFIRMED для допуска следующей миссии. |
| `CycleLimitResult set_inspection_cycle_limit(unsigned int limit)` | Устанавливает лимит автоматических циклов без физического осмотра, если такая настройка разрешена. |
| `CommandSubmitResult request_module_restart(ModuleId module_id)` | Запрашивает безопасный перезапуск отдельного модуля, если он включён в перечень независимо перезапускаемых. |
| `CommandSubmitResult request_station_restart()` | Запрашивает штатную перезагрузку станции после проверки допустимости. |
| `CommandSubmitResult request_emergency_exit_confirmation()` | Запрашивает процедуру подтверждения выхода из аварийного состояния; не отменяет аппаратные и локальные блокировки. |

---

## 19. Бортовая интеграция БАС

### src/drone/drone_station_bridge.cpp

| Функция | Назначение |
|---|---|
| `DroneIdentity publish_drone_identity()` | Предоставляет станции идентификатор и сведения о конфигурации БАС для проверки подлинности. |
| `ReturnNotice notify_station_of_return(const FlightStatus& status)` | Передаёт станции уведомление о возвращении, если такой интерфейс будет реализован для выбранного БАС. |
| `FileManifest provide_file_manifest(const MissionId& mission_id)` | Предоставляет перечень материалов, созданных в рамках миссии. |
| `FileChunk read_drone_file_chunk(const FileId& file_id, std::uint64_t offset, std::size_t size)` | Возвращает станции запрошенный фрагмент файла из бортового накопителя. |
| `BatteryStatus publish_battery_status()` | Предоставляет актуальное состояние аккумулятора и BMS в пределах доступного интерфейса. |
| `IntegrationStatus publish_drone_integration_status()` | Предоставляет статус связи и готовности интеграционных узлов БАС. |

### src/drone/autopilot_adapter.cpp

| Функция | Назначение |
|---|---|
| `AutopilotStatus read_flight_status()` | Получает статусы штатного автопилота. |
| `MissionApplyResult apply_authorized_mission(const Mission& mission)` | Передаёт разрешённое задание штатному автопилоту через выбранный документированный интерфейс. |
| `GuidanceApplyResult apply_landing_guidance(const LandingGuidance& guidance)` | Передаёт допустимые данные посадочного позиционирования штатному автопилоту. |

---

## 20. Что пока не выделено в отдельные функции

- Драйверы GPIO, ADC, PWM, UART, I2C, SPI, RS-485, Ethernet, Wi-Fi, BLE и USB будут добавляться после выбора конкретных плат и устройств.
- Функции управления нагревателем, вентиляцией и защитой от конденсата появятся после определения назначения и состава климатической подсистемы.
- Функции для выдвижной платформы или автоматической замены батареи не включены в основной перечень: эти механизмы пока не утверждены как функции первой версии.
- Конкретные функции обмена с автопилотом, BMS, сервером и файловым накопителем будут уточнены после выбора БАС и протоколов.

---

## 21. Linux Gateway — системная диагностика платформы

### src/platform/linux/runtime_environment.cpp

| Функция | Назначение |
|---|---|
| `read_os_version()` **(уже есть)** | Читает сведения об ОС из `/etc/os-release`, не исполняя файл как скрипт. |
| `read_kernel_version()` **(уже есть)** | Получает версию ядра через `uname(2)`. |
| `read_hardware_architecture()` **(уже есть)** | Получает машинную архитектуру через `uname(2)`; не определяет точную модель платы. |
| `read_software_build_info()` **(уже есть)** | Возвращает build ID (если задан), компилятор и стандарт C++. |
| `check_runtime_dependencies()` **(уже есть)** | Проверяет доступность базовых системных источников без изменения настроек ОС. Доступность CPU-температуры вычисляется тем же сборщиком и правилами выбора/валидации thermal zone, что и `read_cpu_temperature()`; фиксированная привязка к `thermal_zone0` не используется. |

### src/platform/linux/system_diagnostics.cpp

| Функция | Назначение |
|---|---|
| `UptimeDiagnostic read_uptime()` **(уже есть)** | Читает время с момента запуска Linux из `/proc/uptime`, в секундах. |
| `MemoryDiagnostic read_memory_status()` **(уже есть)** | Читает MemTotal и MemAvailable из `/proc/meminfo`, возвращает общий, доступный и используемый объём в байтах и процент использования. |
| `StorageDiagnostic read_storage_status()` **(уже есть)** | Через `statvfs()` читает объём, свободное и доступное место файловой системы, содержащей `/`. Не утверждает наличие отдельного SSD. |
| `CpuTemperatureDiagnostic read_cpu_temperature()` **(уже есть)** | Ищет в Linux thermal zones датчик с явной меткой CPU, SoC или package; неизвестная зона не выдаётся за температуру процессора. |
| `CpuLoadDiagnostic read_cpu_load()` **(уже есть)** | Читает load average за 1, 5 и 15 минут и число задач из `/proc/loadavg`. Это load average, а не процент загрузки CPU. |
| `SystemDiagnosticsSnapshot collect_system_diagnostics()` **(уже есть)** | Независимо собирает все перечисленные системные показатели; недоступность одного источника не скрывает остальные результаты. |

Проверка нового адаптера: `verification/repka_gateway/test_system_diagnostics.cpp`. Недоступные значения возвращаются со статусом UNAVAILABLE и причиной; их нельзя подменять нулём или последним неизвестным значением.

### src/platform/linux/network_diagnostics.cpp

| Функция | Назначение |
|---|---|
| `read_network_interfaces()` **(реализована; запуск проверки ожидает)** | Получает имена, индексы, флаги UP/RUNNING/LOOPBACK и MTU интерфейсов. Невозможность прочитать флаги/MTU обозначается отдельными признаками доступности. |
| `read_interface_addresses()` **(реализована; запуск проверки ожидает)** | Получает локальные IPv4/IPv6-адреса через `getifaddrs()`; префикс сети отмечается доступным только при корректной маске. |
| `read_default_route()` **(реализована; запуск проверки ожидает)** | Читает IPv4 и IPv6 default routes из Linux procfs; отсутствие маршрута представляется пустым списком при доступных таблицах, ошибка чтения обеих таблиц — UNAVAILABLE. |
| `resolve_dns_name(hostname, family)` **(реализована; запуск проверки ожидает)** | Разрешает явно переданное имя через системный resolver/NSS и возвращает уникальные адреса. Вызов синхронный, отдельный тайм-аут DNS пока не предоставляется. |
| `check_tcp_reachability(host, port, timeout)` **(реализована; запуск проверки ожидает)** | Проверяет TCP-подключение к заданному узлу и порту через non-blocking connect/poll; timeout ограничен 1 мс…60 с после синхронного DNS-разрешения. Статус AVAILABLE означает, что проверка выполнена; `reachable=false` и причина отражают отказ/тайм-аут соединения. |
| `collect_network_diagnostics()` **(реализована; запуск проверки ожидает)** | Собирает только локальные интерфейсы, адреса и default routes. Не инициирует DNS-запросы или внешние TCP-подключения автоматически. |

Проверка: `verification/repka_gateway/test_network_diagnostics.cpp`. Все сетевые пробы в тесте используют только loopback/localhost; внешние узлы не вызываются.

---


---

## 22. Linux Application Bootstrap

### src/application/linux/application_bootstrap.hpp/.cpp

| Функция / тип | Назначение |
|---|---|
| `ApplicationExitCode` | Различает успешный запуск/завершение, некорректные аргументы, ошибку старта, ошибку прикладной службы и ошибку завершения. |
| `ServiceRunner` | Узкий callback-контракт прикладной службы: получает уже проверенный RuntimeConfig и указатель на флаг SIGINT/SIGTERM; служба обязана штатно завершиться после установки флага. |
| `ApplicationRunResult` | Возвращает код завершения, признак выведенной справки и краткое описание результата. |
| `run_application(argc, argv, service_runner)` | Координирует разбор CLI, validate-before-initialize RuntimeConfig/Logger, установку обработчиков завершения, проверку базового окружения, сбор read-only системной/сетевой диагностики, запуск переданной службы и штатное завершение. |
| `is_shutdown_requested(flag)` | Безопасно проверяет флаг `sig_atomic_t`; null указатель трактуется как отсутствие запроса. |

### repka_gateway/main.cpp

| Функция | Назначение |
|---|---|
| `main(argc, argv)` | Формирует безопасный массив аргументов и передаёт управление ApplicationBootstrap; возвращает код ApplicationExitCode процессу ОС. |

Обработчик сигнала только устанавливает `volatile sig_atomic_t`; журналирование, выделение памяти и остановка службы из signal handler не выполняются. Служба получает указатель на флаг и сама отвечает за своевременное завершение. Обработчики SIGINT/SIGTERM восстанавливаются до закрытия Logger.

Текущий entry point запускается в bootstrap-only режиме: callback прикладной службы не зарегистрирован, поэтому процесс выполняет проверки и завершает работу, явно указывая, что HTTP API не запущен. Python Gateway остаётся отдельным временным VM-прототипом. C++ Bootstrap не открывает сокет, не меняет фиксированный loopback bind, не выполняет DNS/TCP-пробы автоматически и не управляет физическим оборудованием.

Проверка: `verification/repka_gateway/test_application_bootstrap.cpp`; регистрация в CTest — `application_bootstrap`. Добавлены сценарии справки, раннего отказа CLI, запуска/завершения, передачи конфигурации службе, SIGTERM и ненулевого кода службы. Сборка и выполнение теста ещё не подтверждены.


## 23. Временный VM-вертикальный срез тестового адаптера

Следующие функции существуют только в Python Gateway-прототипе для проверки маршрута Operator → Gateway → прикладная служба → тестовый адаптер → Operator. Они не являются командами физического управления и не фиксируют производственный HTTP-контракт.

### repka_gateway/dockstation_gateway/application/test_request_service.py

| Функция / тип | Назначение |
|---|---|
| `TestRequestService.handle_test_request(payload)` | Проверяет точный набор полей, версию схемы, ограниченный request_id и единственную разрешённую операцию PING; передаёт запрос тестовому адаптеру и возвращает его квитанцию. |

### repka_gateway/dockstation_gateway/adapters/test_request_adapter.py

| Функция / тип | Назначение |
|---|---|
| `TestRequestAdapter.accept_test_request(request_id, operation)` | Принимает только PING и возвращает TEST_ADAPTER_ACCEPTED с источником TEST_ADAPTER и `physical_action_performed=false`. Не обращается к GPIO, ESP32 или исполнительным устройствам. |

### operator/src/dockstation_operator/api/station_api_client.py

| Функция | Назначение |
|---|---|
| `StationApiClient.submit_test_adapter_request()` | Отправляет единственный тестовый PING по POST через существующее loopback-соединение и проверяет квитанцию, request_id, источник и отсутствие физического действия. |

Маршрут Gateway: `POST /api/v1/test/adapter/request`. По умолчанию отключён; временный Python Gateway включает его только с явным `--enable-test-adapter`. Максимальный размер тела — 4096 байт; неизвестные поля, дубликаты JSON-ключей и любые операции кроме PING отклоняются.


## 24. Логический протокол контроллера Repka—ESP32

### src/hardware/interfaces/controller_protocol.hpp

| Функция / тип | Назначение |
|---|---|
| `ControllerMessageType` | Типизированный перечень HELLO, HELLO_ACK, HEARTBEAT, REQUEST_SNAPSHOT, TELEMETRY_SNAPSHOT и ERROR. |
| `ControllerProtocolVersion` | Пара major/minor логического ControllerLink protocol; текущая версия — 1.1. Точное совпадение major/minor применяется `ControllerLinkSession`, negotiation и downgrade отсутствуют. |
| `ControllerMessageEnvelope` | Невладеющее представление общих метаданных сообщения: тип, версия, controller_id, controller_instance_id, sequence_number и sent_at. |
| `SensorQualityStatus` | Шесть статусов телеметрии из docs/06 §3 и §25.1: OK, STALE, INVALID, UNAVAILABLE, NOT_CONFIGURED и UNKNOWN. ERROR не является качеством измерения. |
| `EmptyControllerMessagePayload` | Пустое тело для HELLO, HELLO_ACK, HEARTBEAT и REQUEST_SNAPSHOT; тип сообщения задаёт смысл операции. |
| `ControllerTelemetryMeasurement` / `ControllerTelemetrySnapshotPayload` | Caller-owned список измерений со sensor_id, variant-значением, единицей, качеством и отдельным временем измерения. |
| `ControllerErrorPayload` / `ControllerErrorSeverity` / `ControllerSubsystemStatus` | Структурированный ERROR с кодом, severity, описанием, причиной и необязательной all-or-nothing парой subsystem_id + subsystem_status (PASS/DEGRADED/FAIL/UNKNOWN). Severity и состояние независимы; одно не выводится из другого. |
| `ControllerMessagePayload` | Типизированный variant логических тел; это не wire-сериализация. |
| `validate_controller_message()` | Fail-closed проверка типа и наличия обязательных полей общего конверта. Не проверяет политику версии/sequence и формат временной метки; версия и строгий порядок sequence применяются в `ControllerLinkSession`. |
| `validate_controller_message_payload()` | Проверяет соответствие типа сообщения телу, обязательные поля измерений/ошибок, известность quality/severity/subsystem_status, all-or-nothing пару subsystem_id/subsystem_status и конечность вещественных значений. |

Проверка: `verification/repka_gateway/test_controller_protocol.cpp`; CTest — `controller_protocol`. Логические тела и валидаторы реализованы.

### src/hardware/interfaces/controller_codec.hpp/.cpp

| Функция / тип | Назначение |
|---|---|
| `ControllerCodecStatus` / `ControllerCodecResult` | Статус кодирования/декодирования и размер полного кадра/смещение ошибки. |
| `is_valid_controller_wire_text()` | Общая проверка wire-строки: корректный UTF-8 без встроенного NUL; пустота разрешена на уровне helper-а, обязательность определяется конкретным полем. |
| `ControllerCodecDecodeStorage` | Caller-owned фиксированное хранилище максимум 16 декодированных измерений. |
| `ControllerCodecDecodedMessage` | Декодированный общий конверт и типизированное тело; строковые представления ссылаются на входной кадр. |
| `encode_controller_message()` | Кодирует logical envelope + payload в полный бинарный кадр v1 в caller-owned буфер без динамического выделения памяти. |
| `decode_controller_message()` | Проверяет и декодирует ровно один полный кадр v1; отклоняет неверный magic/version/type/length/CRC, некорректный UTF-8, недопустимые value tags и превышение лимитов. |

Wire-frame v1: magic `DS`, wire version 1, тип сообщения 1…6, uint16 little-endian body length, общий envelope, typed payload и CRC-32/ISO-HDLC по header + body. Логическая ControllerLink version в общем envelope сейчас 1.1. Для ERROR после reason_text добавлен presence byte; при 1 далее идут subsystem_id (UTF-8) и один из четырёх кодов subsystem_status. Максимальный кадр 2048 байт, максимум 16 измерений. Подробная таблица полей — docs/06 §16.3; версия 1.0 не принимается из-за exact-match policy.

Проверка: `verification/repka_gateway/test_controller_codec.cpp`; CTest — `controller_codec`. Host-тесты проверяют round-trip HELLO/telemetry/ERROR, все типы значений, границы целых чисел, нулевой snapshot, UTF-8, размеры, повреждённую CRC и malformed frames. Кодек декодирует один полный кадр; накопление потока выполняет `ControllerFrameStreamDecoder`. CRC не является аутентификацией/шифрованием. UART-драйвер, защищённый transport/session verifier и ESP-IDF firmware target остаются отдельными задачами.

### src/hardware/interfaces/controller_frame_stream_decoder.hpp/.cpp

| Функция / тип | Назначение |
|---|---|
| `ControllerFrameStreamStatus` / `ControllerFrameStreamResult` | Результат приёма части потока: NeedMoreData, FrameReady, FramePending, NullInput; число потреблённых и отброшенных байтов и размер кадра. |
| `ControllerFrameStreamDecoder` | Фиксированный внутренний буфер максимум 2048 байт; без динамического выделения памяти. |
| `push_bytes()` | Принимает часть входного потока до одного полного кандидата-кадра и сообщает точное количество потреблённых байтов. Хвост входа после FrameReady остаётся ответственностью caller. |
| `frame_data()` / `frame_size()` / `has_frame()` | Предоставляют временное представление текущего candidate frame. |
| `consume_frame()` | Убирает кадр после того, как caller успешно проверил его через `decode_controller_message()`. |
| `reject_frame()` | Отбрасывает первый байт неверного кандидата и повторно ищет сигнатуру, сохраняя возможность синхронизации на следующих данных. |
| `reset()` | Сбрасывает частичный кадр и состояние парсера. |

Парсер проверяет только magic/version/type/ограниченную длину; CRC и структуру тела проверяет ControllerCodec. После FrameReady caller должен декодировать кадр; при успехе вызвать consume_frame(), при ошибке — reject_frame(). Не использовать frame_data() после изменения внутреннего буфера.

Проверка: `verification/repka_gateway/test_controller_frame_stream_decoder.cpp`; CTest — `controller_frame_stream_decoder`. Покрываются неполные куски, split magic, шум/повторная синхронизация, чрезмерная длина, последовательные кадры, CRC rejection и восстановление. Потоковый парсер не открывает UART и не является аппаратной эмуляцией.



### src/platform/linux/controller_psk_tls_client.hpp/.cpp

| Функция / тип | Назначение |
|---|---|
| `ControllerPskCredentials` | Контейнер для 32-байтового PSK и отдельной 16-байтовой бинарной PSK identity; значения поступают от вызывающего слоя, не из обычной JSON-конфигурации. |
| `ControllerPskTlsClient::initialize()` / `close()` | Создаёт/закрывает OpenSSL TLS client и очищает собственную копию credentials. Файл ключа, provisioning и ротацию выполняет отдельный будущий слой. |
| `ControllerPskTlsClient::advance_handshake()` | Выполняет неблокирующий TLS 1.3 PSK handshake и возвращает `NeedRead`/`NeedWrite` до завершения. |
| `feed_encrypted_bytes()` / `take_encrypted_bytes()` | Принимает/выдаёт произвольные части ciphertext через BIO pair; вызывающий physical adapter должен обрабатывать частичные операции. Сам UART не открывается. |
| `write_application_data()` / `read_application_data()` | Обменивается TLS application data только после полного handshake и проверки фиксированного TLS-профиля. |
| `verify_negotiated_profile()` | Пропускает только TLS 1.3, TLS_AES_128_GCM_SHA256, группу P-256 и выбранную external-PSK сессию; сертификатный fallback/PSK-only не являются допустимой заменой. |

Host-проверка: `verification/repka_gateway/test_controller_psk_tls_client.cpp`; CTest — `controller_psk_tls_client`. 13 проверок используют OpenSSL TLS peer и BIO transport pairs. Это проверка TLS state machine на host, а не эмуляция ESP32 и не межстековая проверка с Mbed TLS. TLS client используется через ControllerTransportManager; общая service/Gateway композиция теперь проверяется также end-to-end через POSIX PTY в CTest `controller_link_service` (run 43818913284). Физический UART, Mbed TLS firmware, paired provisioning и аппаратная приёмка остаются открытыми.

### src/platform/linux/controller_uart_port.hpp/.cpp

| Функция / тип | Назначение |
|---|---|
| `ControllerUartStatus` / `ControllerUartIoResult` | Типизированный результат открытия, неблокирующего чтения/записи и hard I/O errors; errno возвращается вызывающему слою без записи в журнал. |
| `ControllerUartPort::open()` / `close()` | Открывает абсолютный путь к символьному tty-устройству с `O_NOCTTY`, `O_NONBLOCK`, `O_CLOEXEC`; настраивает raw 115200 8N1, `CLOCAL|CREAD`, без software/hardware flow control. |
| `ControllerUartPort::read_bytes()` / `write_bytes()` | Переносит только непрозрачные байты, возвращает фактическое число переданных байтов и различает `WouldBlock`, `NotOpen`, `Disconnected` и `Failed`. TLS, буферизация, polling, кадрирование и retry policy остаются уровнем выше. |
| `ControllerUartPort::is_open()` | Позволяет composition/transport manager проверить состояние владения файловым дескриптором. |

Проверка: `verification/repka_gateway/test_controller_uart_port.cpp`; CTest — `controller_uart_port`. 21 host assertion используют POSIX pseudo-terminal: termios profile, raw byte I/O, non-blocking empty read, partial write accounting, invalid paths/buffers/lengths, regular-file rejection, close and hard peer I/O error. PTY не является эмулятором ESP32 и не доказывает электрическую/физическую работу UART. При `VMIN=0/VTIME=0` пустое чтение возвращает `WouldBlock`, поэтому тихий физический обрыв обнаруживается верхним heartbeat/line-monitor, а не этим низкоуровневым классом.

### src/platform/linux/controller_transport_manager.hpp/.cpp

| Функция / тип | Назначение |
|---|---|
| `ControllerTransportStatus` / `ControllerTransportResult` | Отражают состояние старта, продвижения, `WouldBlock`, завершения TLS handshake и фатальных ошибок UART/TLS с кодами errno/OpenSSL. |
| `ControllerTransportManager::start()` / `close()` | Открывает один UART и инициализирует TLS 1.3 PSK client с переданными вызывающим слоем credentials; не читает credential file и очищает внутренние буферы при закрытии. |
| `ControllerTransportManager::poll()` | Выполняет ограниченный неблокирующий цикл UART ciphertext → TLS BIO → TLS progress → UART ciphertext; хранит смещения частичных входных/выходных операций и закрывает обе стороны при фатальной ошибке. |
| `write_application_data()` / `read_application_data()` | Предоставляют application data только через TLS; caller обязан повторно передавать только непринятый остаток, а зашифрованный поток физически переносится следующим `poll()`. |
| `negotiated_tls_version()` / `negotiated_cipher_name()` / `negotiated_group_id()` | Возвращают параметры только после полного PSK-DHE handshake. |

Фиксированные буферы ciphertext — по 4096 байт; одна итерация `poll()` ограничена 12 проходами, чтобы не монополизировать event loop. Проверка: `verification/repka_gateway/test_controller_transport_manager.cpp`, CTest `controller_transport_manager`, 20 assertions. OpenSSL server peer подключён через POSIX PTY; проверяются handshake, plaintext gate, двусторонний application data, 64-KiB поток с backpressure, неверный PSK, недоступное устройство и потеря UART peer. Это host-проверка Linux/PTY/OpenSSL; manager пока не интегрирован с credential loader, Gateway, ControllerCodec или ControllerLinkSession. ESP32, Mbed TLS interop и физическая линия остаются открытыми.

### src/platform/linux/controller_psk_credential_store.hpp/.cpp

| Функция / тип | Назначение |
|---|---|
| `ControllerPskCredentialFileStatus` | Безопасные коды результата загрузки; значения ключей и путь в статус не включаются. |
| `load_controller_psk_credentials()` | Читает `/etc/dockstation/credentials/repka-esp32.psk` с фиксированным именем файла и политикой owner/group/mode. |
| `load_controller_psk_credentials_from_directory()` | Тот же контракт с абсолютным каталогом для host filesystem acceptance tests; filename, root owner, группа `dockstation` и режимы не настраиваются caller. |
| `ControllerPskCredentials::clear()` / destructor | Очищает 32-byte PSK и 16-byte identity через `OPENSSL_cleanse`; loader очищает выходные значения при любой ошибке. |

Формат файла — ровно 48 бинарных байт: PSK[32] затем identity[16]. Проверяются no-follow для всех path components, root ownership, каталог `0750`, файл `0640`, группа `dockstation`, отсутствие ACL, regular file, `nlink=1`, точный размер и стабильность stat metadata во время чтения. Тест `verification/repka_gateway/test_controller_psk_credential_store.sh` использует временные root-owned files в `/run` и запускает binary от service user; он не создаёт production PSK и не реализует provisioning.

### tools/controller_psk_credential_provision.cpp

| Функция / тип | Назначение |
|---|---|
| main() | Принимает только --help или --replace; остальные аргументы отклоняет. Рабочий запуск требует root и системную группу dockstation. |
| provision(false) | Генерирует 32-byte PSK и 16-byte identity через OpenSSL RAND_bytes, создаёт локальный бинарный файл и атомарно устанавливает его без перезаписи существующего файла. |
| provision(true) | Сначала требует, чтобы существующий файл успешно прошёл load_controller_psk_credentials(), затем атомарно заменяет только локальный файл Repka. |
| open_credentials_directory() | Покомпонентно открывает/создаёт /etc/dockstation/credentials без следования symlink; проверяет root ownership, группу dockstation, каталог 0750 и отсутствие ACL. |
| write_random_temp_file() / install_atomically() | Записывает временный файл root-only, fsync, применяет root:dockstation 0640 и выполняет renameat2 RENAME_NOREPLACE либо renameat при явной локальной замене; после rename выполняет fsync каталога. |

Файл имеет фиксированное имя repka-esp32.psk и размер 48 бинарных байт: PSK[32] + identity[16]. Секретные байты не выводятся и не принимаются через аргументы. Обычный запуск не перезаписывает существующий файл. --replace не является coordinated rotation: ESP32 не получает новую пару, поэтому после такой операции TLS связь пары не сможет установиться до отдельного физически контролируемого provisioning ESP32. Утилита не включена в Gateway runtime; она устанавливается как самостоятельная root-only CLI.

Проверка: CTest controller_psk_credential_provision_help и verification/repka_gateway/test_controller_psk_credential_provision.sh. Acceptance script проверяет первый install, loader read от service user, no-overwrite, отказ replace при неверном mode/ACL, атомарную локальную замену и удаление временных файлов.

### tools/dockstation_registry_provision.cpp

| Функция / тип | Назначение |
|---|---|
| `main()` / `parse_options()` | Принимает только `source` или `subsystem`, обязательную revision и для subsystem обязательный ID; `--apply` включает запись, без него выполняется preview. Повторные, неизвестные и небезопасные аргументы отклоняются. |
| `lookup_service_identity()` | Находит системного пользователя и одноимённую группу `dockstation`; root и другие пользователи отклоняются. |
| `safe_config_file()` | Проверяет root-owned `/etc/dockstation/gateway.conf`, каталог root:dockstation 0750, файл 0640, regular-file/no-link/no-ACL и ограниченный размер. |
| `safe_database_file()` | Проверяет приватный StateDirectory dockstation:dockstation 0700 и, если SQLite DB уже существует, regular-file owner/mode 0600/no-link/no-ACL. Отсутствующая DB допускается только внутри уже проверенного private StateDirectory; после open metadata проверяется повторно. |
| `run()` | Читает и валидирует Station ID/source из gateway.conf, загружает локальную pair PSK identity через `ControllerPskCredentialStore`, открывает `GatewayPersistenceContext` и сериализованно вызывает production SQLite registry adapters. |
| source registration | Вызывает `SqliteSourceSessionStore::find_by_peer_identity()` и `create_source_registration()`. Уже зарегистрированная identity, ошибка lookup или DB constraint закрывают операцию; secret PSK в registry не сохраняется. |
| subsystem registration | Требует существующую ACTIVE source registration с тем же Station ID/source, затем точный `find_subsystem()` и `create_subsystem_registration()`. Имплицитное создание source запрещено. |

CLI: `dockstation-registry-provision source --revision r1 [--apply]` и
`dockstation-registry-provision subsystem --subsystem-id weather.temperature --revision r1 [--apply]`.
Сначала используется preview без записи; `--apply` создаёт только отсутствующие записи.
Дубли, конфликт source/identity, несовпадение config/registration и DISABLED/REVOKED
source не изменяются и отклоняются. Утилита не поддерживает ротацию/отзыв, не включает
HTTP CRUD и не выполняет регистрацию при TLS подключении.

Проверка: CTest `registry_provision_help`, `registry_provision_rejects_invalid_revision` и
`registry_provision_rejects_unsafe_subsystem_id`; одноразовый host acceptance
`verification/repka_gateway/ci_registry_provision_acceptance.sh` проверяет root denial,
preview без записи, PSK secret non-disclosure, source-before-subsystem, успешную запись,
duplicate/invalid revision, Station ID mismatch и DB owner/mode. GitHub Actions run
43872537777 SUCCESS: CTest 34/34 на x86_64 и ARM64, 31/31 C++ host-программ и 158
Python/Operator/Gateway integration-тестов; ARM64 bundle/staging и Sigstore provenance
прошли. Это не Mbed TLS или physical Repka acceptance.

### firmware/esp32/components/controller_mbedtls_psk_server

| Функция / тип | Назначение |
|---|---|
| \`ControllerMbedTlsPskCredentials::clear()\` | Очищает локальные копии pair PSK[32] и identity[16] через Mbed TLS zeroize helper. |
| \`ControllerMbedTlsPskServer::initialize()\` | Поднимает TLS server config: TLS 1.3, TLS_AES_128_GCM_SHA256, единственная группа P-256, только PSK-Ephemeral, отключённые early data и TLS 1.3 session tickets; подключает injected byte-stream callbacks. |
| \`ControllerMbedTlsPskServer::select_pair_psk()\` | Принимает только точную 16-байтовую pair identity, сравнивает все байты и устанавливает 32-байтовый PSK только для текущего handshake. Неизвестная identity отвергается. |
| \`ControllerMbedTlsPskServer::advance_handshake()\` | Продвигает Mbed TLS handshake, возвращая NeedRead/NeedWrite; application data разрешены лишь после полного TLS 1.3 PSK-Ephemeral handshake и проверки согласованных версии/cipher. |
| \`ControllerMbedTlsPskServer::read_application_data()\` / \`write_application_data()\` | Передаёт TLS application data после handshake; caller обязан сохранять остаток partial write и повторять неотправленный фрагмент при WANT_READ/WANT_WRITE. |
| \`ControllerMbedTlsPskServer::export_session_proof()\` | Выводит 32-byte TLS exporter с общими Linux/ESP32 label/context. Вызывающий ESP32 session layer обязан дополнительно дождаться HELLO/HELLO_ACK, прежде чем использовать proof для session evidence. |
| \`ControllerMbedTlsPskServer::close()\` | Закрывает TLS state, RNG/config контексты и очищает pair secret/identity; повторное соединение требует нового полного handshake. |

Проверка: \`verification/repka_gateway/test_controller_mbedtls_server_interop.cpp\`, CTest \`controller_mbedtls_server_interop\`; GitHub Actions [run 43904071591](https://github.com/GooseRabbit/dockstation/actions/runs/43904071591) — PASS (1/1); отдельная опция \`DOCKSTATION_BUILD_MBEDTLS_INTEROP_TESTS=ON\` скачивает официальный upstream Mbed TLS 3.6.7 с проверкой SHA-256. GitHub Actions соединяет существующий OpenSSL client и Mbed TLS server через POSIX socket pair: проверяет handshake, exporter parity, application data в обе стороны и отказ при неверной PSK identity/PSK. Это upstream Mbed TLS host interop; Espressif Mbed TLS fork, точная ESP32 плата/SDK, UART, secure NVS/Flash Encryption/Secure Boot и физическая приёмка пока не проверены.

### src/hardware/interfaces/controller_link_peer_session.hpp/.cpp

| Функция / тип | Назначение |
|---|---|
| `ControllerLinkPeerSessionConfig` | Локальные ID/instance ESP32, ожидаемый peer ID Repka и точная версия логического протокола. |
| `ControllerLinkPeerSessionState` / `ControllerLinkPeerSessionStatus` / `ControllerLinkPeerSessionEvent` | Состояния Stopped/AwaitingHello/Established, причины отказов и события SessionEstablished/PeerHeartbeatReceived/SnapshotRequested. |
| `ControllerLinkPeerSession::start()` | Начинает логическую обработку только после того, как вызывающий transport подтвердил аутентификацию канала; TLS не реализует. |
| `ControllerLinkPeerSession::handle_message()` | Принимает HELLO и формирует HELLO_ACK; затем разрешает только HEARTBEAT и REQUEST_SNAPSHOT. Проверяет структуру, точную protocol version, peer ID/instance и строго возрастающий входящий sequence. |
| `ControllerLinkPeerSession::make_telemetry_snapshot()` | Только после принятого REQUEST_SNAPSHOT проверяет типизированный снимок и формирует TELEMETRY_SNAPSHOT с единым возрастающим outbound sequence. Ошибочный снимок не потребляет sequence и не закрывает запрос; после успешного формирования запрос считается обслуженным. Измерения и их строки остаются во владении вызывающей стороны до кодирования кадра. |
| `ControllerLinkPeerSession::stop()` | Заканчивает логическую сессию, сохраняя sequence/replay history на время жизни объекта для защиты при reconnect. |

Идентификаторы хранятся в фиксированных буферах до 128 байт; динамическое выделение памяти не выполняется. Новый peer instance может начать входящую последовательность заново, но повтор старого sequence в пределах прежнего instance отклоняется. Проверка: `verification/repka_gateway/test_controller_link_peer_session.cpp`, CTest `controller_link_peer_session`; host-тест проходит через production ControllerCodec и проверяет HELLO/HELLO_ACK, REQUEST_SNAPSHOT, HEARTBEAT, типизированный snapshot, неверные сообщения и replay после reconnect. Это транспортно-независимая логика, а не Mbed TLS/ESP-IDF/физическая проверка.

### src/hardware/interfaces/controller_link_session.hpp/.cpp

| Функция / тип | Назначение |
|---|---|
| `ControllerLinkSessionConfig` | Локальные ID/instance, ожидаемый peer ID, точная версия протокола и настраиваемые интервалы handshake/heartbeat/timeout/retry. |
| `ControllerLinkSessionState` / `ControllerLinkSessionStatus` / `ControllerLinkSessionEvent` | Состояния Stopped/AwaitingHelloAck/Established/LinkLost, причины отказа и события для вызывающего transport adapter. |
| `ControllerLinkSessionResult` | Возвращает состояние/результат и, для исходящих HELLO/HEARTBEAT/REQUEST_SNAPSHOT, логический envelope; тело для этих сообщений — EmptyControllerMessagePayload. |
| `ControllerLinkSession::start()` | Начинает host-initiated handshake и формирует HELLO с локальными ID, версией и возрастающим sequence. |
| `ControllerLinkSession::request_snapshot()` | После Established формирует пустой REQUEST_SNAPSHOT с тем же возрастающим исходящим sequence; до handshake отклоняет запрос. |
| `ControllerLinkSession::poll()` | Проверяет deadlines, формирует периодический HEARTBEAT, переводит связь в LinkLost и инициирует повторный HELLO. Время передаётся вызывающим слоем в monotonic milliseconds. |
| `ControllerLinkSession::handle_message()` | Принимает уже декодированное сообщение после ControllerCodec; проверяет структуру, точную версию, ожидаемый peer ID/instance, разрешённый тип и строго возрастающий sequence. |
| `ControllerLinkSession::stop()` | Останавливает автомат, не сбрасывая локальную последовательность и последнее наблюдавшееся состояние sequence peer в пределах жизни объекта. |

Идентификаторы хранятся в фиксированных буферах до 128 байт и проверяются общей UTF-8-функцией ControllerCodec; вызывающий слой должен задавать новый local instance ID на каждый запуск, а peer должен менять его после перезапуска. Это namespace sequence, не credential. Сессия не копируется/не перемещается, чтобы не дублировать sequence state и не нарушать lifetime исходящих string_view. Динамическое выделение памяти отсутствует. По умолчанию: handshake timeout 3000 мс, heartbeat interval 1000 мс, peer activity timeout 5000 мс, retry interval 1000 мс. Номер исходящего sequence считается занятым при формировании сообщения, не при успешной физической отправке; пропуски разрешены. Входящие номера должны строго возрастать в пределах одного peer instance, но не обязаны быть непрерывными.

Проверка: `verification/repka_gateway/test_controller_link_session.cpp`; CTest — `controller_link_session`. Host-тесты проверяют codec round-trip HELLO/HELLO_ACK, handshake, peer ID/version/instance, монотонный sequence, heartbeat, потерю связи/retry, переполнение monotonic counter, неверный UTF-8/NUL для ID и `sent_at`, пустой timestamp и compile-time запрет copy/move. Итоговый CI run 43631459004: 16/16 CTest с ASan/UBSan, 31/31 дополнительных C++ host tests, 134 pytest и дополнительные Gateway/QML/shell проверки. Класс не реализует UART, физический обмен, аутентификацию, шифрование, durable anti-replay или команды оборудования. Established не означает authenticated.




### src/platform/linux/controller_link_service.hpp/.cpp

| Функция / тип | Назначение |
|---|---|
| ControllerLinkServiceState / ControllerLinkServiceStatus | Отражают NOT_CONFIGURED, TLS_HANDSHAKE, PROTOCOL_HANDSHAKE, ESTABLISHED, LINK_LOST, ERROR и STOPPED; отдельно показывают TLS authentication и protocol establishment, счётчики принятых telemetry/error кадров и код ошибки без секретов. |
| ControllerLinkService::start() | Принимает optional UART path и локальные/peer controller IDs. При полностью пустой конфигурации остаётся NOT_CONFIGURED; частичная/некорректная конфигурация отклоняется до открытия устройства. Credentials загружаются только из root-controlled credential store. |
| ControllerLinkService::poll() | Ограниченный шаг event loop: продвигает TLS/UART, начинает HELLO только после TLS handshake, принимает HELLO_ACK, отправляет первый REQUEST_SNAPSHOT сразу после установления сессии и повторяет его каждые 2000 мс по monotonic clock, обслуживает heartbeat, принимает/проверяет входные кадры и восстанавливает соединение полным TLS handshake при ошибке транспорта. Интервал ограничивает частоту запросов к ESP32, а не период измерения датчиков. |
| ControllerLinkService::status() / stop() | Возвращает безопасный read-only статус без PSK/identity; завершает transport, сессию и очищает временные буферы. |
| process_plaintext() / process_ready_frame() | Подаёт в ControllerFrameStreamDecoder только TLS application data; затем ControllerCodec и ControllerLinkSession проверяют frame, payload, peer, protocol version и sequence. TELEMETRY_SNAPSHOT копируется в ControllerTelemetrySnapshot отдельного отчётного потока; ERROR передаётся attached handler после session acceptance. Событие SSM появляется только если injected projector возвращает Projected; generic ERROR и telemetry остаются вне Pipeline. |

Optional RuntimeConfig keys: controller_uart_device, controller_local_id, controller_peer_id. Все три должны быть заданы вместе; PSK и identity в .conf не хранятся. Read-only Gateway продолжает запускаться при NOT_CONFIGURED. API: GET /api/v1/controller/link; system diagnostics отмечает controller connected только после успешных TLS и логического handshake. `verification/repka_gateway/test_controller_link_service.cpp`, CTest `controller_link_service`, теперь включает end-to-end сценарий: ephemeral PSK fixture, OpenSSL TLS 1.3 PSK-DHE peer через POSIX PTY, HELLO/HELLO_ACK, начальный и повторный REQUEST_SNAPSHOT после 2,1 секунды, один синтетический TELEMETRY_SNAPSHOT, diagnostics JSON, replay rejection, WARN для сохранённого снимка после потери связи и clear при stop. Отдельный test seam в `ControllerLinkService` собирается только с `BUILD_TESTING`; production/release по-прежнему напрямую вызывает защищённый credential store. GitHub Actions run 43818913284 SUCCESS: 25/25 CTest на x64 и ARM64, 31/31 C++ host-программ, 158 pytest; ARM64 bundle собран, staging installer и Sigstore provenance проверены (67996530). Это не Mbed TLS interop, не физическая Repka/ESP32 и не UART acceptance.


---

## Дополнение: интеграция SourceSessionCoordinator в F13

### src/core/station_event_pipeline.hpp/.cpp

| Функция / тип | Назначение |
|---|---|
| StationEventPipeline | Композиция проверки источника и сессии с общим входным контуром F13. Получает verifier, cursor reader и session repository как зависимости. |
| StationEventPipeline::handle(event, validation_policy, evidence) | Сначала подтверждает сессию через SourceSessionCoordinator; только для Confirmed или AlreadyCurrent вызывает F13 с полученным курсором. Произвольный курсор от вызывающего кода не принимает. |
| StationEventPipelineResult | Сохраняет точный SourceSessionCoordinatorStatus; содержит EventHandlingResult только если F13 был запущен. |

Проверка: verification/regression/test_station_event_pipeline.cpp; CTest — station_event_pipeline. Тесты используют fake-порты и подтверждают блокировку F13 при отказе/ошибке сессии, конфликте регистрации и несовпадении старого source_instance.


### src/platform/persistence/sqlite_database.hpp/.cpp

| Функция / тип | Назначение |
|---|---|
| SqliteDatabaseStatus / SqliteDatabaseResult | Различают Ready, некорректный путь/timeout, ошибку открытия/PRAGMA, чужой application_id, неподдерживаемую или некорректную версию схемы, ошибку миграции и отказ закрытия активного соединения. |
| SqliteDatabase::open() | Принимает абсолютный нормализованный путь и busy timeout; открывает с READWRITE/CREATE/FULLMUTEX/NOFOLLOW; до изменения journal mode проверяет application_id/user_version и неизвестные schema objects; применяет недостающие версионируемые миграции. |
| SqliteDatabase::close() | Возвращает false и сохраняет handle, если SQLite отказывается закрыть соединение из-за незавершённых statements; успешное закрытие подтверждается отдельно. Копирование/move отключены. |
| SqliteDatabase::native_handle() | Доступен только Linux persistence adapters; core/configuration ports не зависят от SQLite headers или типов. |
| Migrations v1–v3 | v1 создаёт schema_migrations/application_id; v2 транзакционно создаёт source_registrations/source_cursors; v3 добавляет subsystem registrations, принятые Event, append-only status history и current-status pointer. |

Соединение при каждом открытии устанавливает/проверяет DELETE rollback journal, synchronous=EXTRA, foreign_keys=ON и trusted_schema=OFF. SQLite утверждён DEC-068 как локальный backend; все обращения к соединению сериализуются одним владельцем. CMake требует SQLite >=3.31 для SQLITE_OPEN_NOFOLLOW. Для subsystem reports подключены SubsystemStatusRepository, schema v3 и Gateway composition root; общие SSM/CFG snapshots, operation state/history, резервное копирование и восстановление пока не входят в этот persistence slice. Проверка systemd StateDirectory на Repka и target filesystem/power-loss acceptance остаются отдельными.

Проверка: verification/repka_gateway/test_sqlite_persistence_probe.cpp, CTest sqlite_persistence_probe. Дополнительно verification/repka_gateway/test_sqlite_source_session_store.cpp проверяет registry/session adapters, binary identity, revision/cursor CAS, uint64 sequence, same/new session и migration upgrade v1→v2. Эти host tests не доказывают внезапное отключение питания и не заменяют физическую Repka acceptance.

### src/platform/persistence/sqlite_subsystem_status_store.hpp/.cpp

| Функция / тип | Назначение |
|---|---|
| `SqliteSubsystemStatusStore::create_subsystem_registration()` | Provisioning-only insert subsystem_id; разрешает только source, зарегистрированный ACTIVE для того же Station ID. |
| `SqliteSubsystemStatusStore::change_subsystem_state()` | Меняет состояние по expected revision; REVOKED terminal, повторная активация требует новой revision. |
| `SqliteSubsystemStatusStore::find_subsystem()` | Реализует SubsystemRegistrationReader и возвращает точную запись station/subsystem/source/revision/state. |
| `SqliteSubsystemStatusStore::find_by_id()` | Реализует EventRepository; возвращает исходный Event с member-owned строковыми буферами до следующего lookup. |
| `SqliteSubsystemStatusStore::commit_subsystem_status()` | В одной BEGIN IMMEDIATE транзакции проверяет duplicate event_id/content, ACTIVE/revision subsystem registration, ACTIVE source registration и полный expected SourceCursor; сохраняет original Event, history report, current pointer и продвигает cursor. Любая ошибка откатывает все связанные записи. |

Адаптер не аутентифицирует transport и не создаёт SourceSessionEvidence; production Gateway вызывает его после SourceSessionCoordinator внутри сериализованного SQLite callback. CTest `sqlite_subsystem_status_store` использует on-disk SQLite и проверяет commit, историю/current pointer, duplicate/conflict, stale cursor, revoked registration, forced rollback и close/reopen. F13-маршрут регистрируется только при передаче реального processor; без него маршрут отсутствует и известный тип возвращает Unsupported.

### src/platform/persistence/sqlite_source_session_store.hpp/.cpp

| Функция / тип | Назначение |
|---|---|
| SqliteSourceSessionStore::create_source_registration() | Provisioning-only insert ACTIVE registration по station_id/source/revision и opaque binary peer identity; дубликаты identity/source отклоняются. HTTP/API endpoint не создаётся. |
| SqliteSourceSessionStore::change_source_state() | Меняет только state/revision по expected revision; DISABLED можно явно реактивировать новой revision, REVOKED terminal. |
| SqliteSourceSessionStore::find_by_peer_identity() | Реализует SourceRegistrationReader; поиск точный по BLOB identity, строки копируются в owned buffers, отсутствующая запись отлична от Error/Unavailable. |
| SqliteSourceSessionStore::read_source_cursor() | Реализует SourceCursorReader; читает source_instance/last_sequence одной записью. Sequence хранится как десятичный TEXT, сохраняя полный uint64 диапазон. |
| SqliteSourceSessionStore::confirm_source_session() | Реализует SourceSessionRepository; в одной BEGIN IMMEDIATE транзакции повторно проверяет ACTIVE/revision регистрации и compare-and-set полного ожидаемого cursor. Новая сессия очищает last_sequence; AlreadyCurrent не меняет sequence. |

Адаптер не выполняет TLS, не создаёт SourceSessionEvidence и не доверяет значениям peer payload. Он получает peer identity только через SourceRegistrationReader вызывающего verifier-а, а session confirmation — только от SourceSessionCoordinator. Все методы должны вызываться сериализованным composition root; registry, expected-cursor и confirmation-result string_view имеют отдельные backing buffers, чтобы coordinator мог сравнить исходный снимок после возврата commit-port. `SqliteSourceSessionStore` подключён к Gateway composition root через `GatewayPersistenceContext`; он реализует только source registration/session/cursor ports. `SubsystemRegistrationReader` и `SubsystemStatusRepository` реализованы отдельным адаптером `SqliteSubsystemStatusStore`.



## 12. Платформенный слой Linux — LinuxPlatformCapabilities

### src/platform/linux/platform_capabilities.hpp / .cpp

| Функция / тип | Назначение |
|---|---|
| LinuxPlatformKind | Профиль GENERIC_LINUX, REPKA_PI5 или UNKNOWN; Repka Pi 5 определяется по model/compatible активного Device Tree. |
| PlatformCapabilityStatus | AVAILABLE — device node доступен текущему процессу для чтения/записи; DETECTED — node найден, но права текущего процесса не подтверждены; NOT_CONFIGURED — sysfs сообщает о контроллере, но подходящий userspace node отсутствует; UNAVAILABLE — доступные источники не показывают интерфейс; UNKNOWN — источники для проверки недоступны. |
| LinuxPlatformProbePaths | Позволяет передать пути /dev, /sys/class, Device Tree и /proc/cmdline; production значения по умолчанию относятся к текущей Linux-системе, fixture paths используются в host-тесте. |
| detect_linux_platform_capabilities() | Пассивно обнаруживает модель/compatible, UART, GPIO, I2C, SPI, PWM и V4L2/media device nodes, доступность текущего процесса, sysfs controller count и конфликты UART с console=. Не открывает устройства, не выполняет I/O, не загружает модули, не меняет pinmux/права/службы и не выбирает порт для Gateway. |
| platform_kind_name() / platform_capability_status_name() | Даёт стабильные строковые названия профиля и статуса для будущей сериализации диагностики. |

Проверка: verification/repka_gateway/test_platform_capabilities.cpp, CTest platform_capabilities. Fixture test использует временную файловую структуру и ссылки на /dev/null как безопасные character-device substitutes; он проверяет только логику обнаружения, а не GPIO/UART/V4L2 hardware. Выявленный кандидатный path — только информация для оператора. Repka WiringRP и repka_gpio.ko не используются; установка/загрузка драйвера и изменение permissions остаются открытыми по DEC-069.


## 13. Диагностическая публикация платформенных возможностей — Gateway / Operator

### src/communications/api_serialization.hpp / .cpp

| Функция / тип | Назначение |
|---|---|
| serialize_interface_capability() | Сериализует статус интерфейса, количество device nodes, доступных текущему процессу nodes, sysfs-контроллеров и конфликтов kernel console; путь устройства публикуется как nullable candidate_device_path, всегда с candidate_only: true. |
| serialize_platform_capabilities() | Публикует профиль Generic Linux/Repka Pi 5, model/compatible Device Tree, доступность источников и шесть аппаратных категорий: UART, GPIO, I2C, SPI, PWM и camera/V4L2. |
| serialize_system_diagnostics() | Включает platform.capabilities в существующий read-only GET /api/v1/system/diagnostics; по умолчанию выполняет пассивное обнаружение текущей системы. Для CTest допускается передача готового снимка без доступа к host hardware. |

### operator/src/dockstation_operator/api/station_api_client.py

_validate_platform_capabilities() проверяет профиль, согласованность Device Tree availability flags, enum статусов, uint32 counters, соотношение accessible/device nodes, candidate path и обязательный candidate_only: true. Раздел является additive extension: старые schema 1.0 snapshots без platform.capabilities остаются допустимыми.

### operator/src/dockstation_operator/ui/qml_bridge.py / ui/qml/Main.qml

hardwarePlatformInfo публикует выбранный платформенный профиль и Device Tree metadata; hardwareInterfaces отображает статусы шести интерфейсов, counts, UART console conflicts и candidate path с явной пометкой «Кандидат (не выбран)». Панель system.hardware-platform расположена на вкладке «Система». Она отображает наблюдаемую доступность, но не подтверждает электрическую исправность, pin mapping или фактический обмен.

**Проверка:** CTest local_api проверяет JSON и сериализацию инъецированного RepkaPi5 snapshot; pytest test_station_api_client.py проверяет валидный раздел, старый snapshot без capabilities и отрицательные случаи; QML tests проверяют bridge mapping и наличие панели. GitHub Actions run 43759675836 SUCCESS для HEAD 6e1272d5bbdd6ece441a44e227133114c544931a: 24/24 CTest на x64 и ARM64, 31/31 C++ host programs, 148 Python/QML/e2e tests, shell/network/target-preflight checks; ARM64 bundle installed in isolated staging and Sigstore provenance was created and verified. Это не аппаратная проверка Repka Pi 5. Решение по WiringRP/repka_gpio.ko остаётся открытым по DEC-069.



## 14. Приём и публикация телеметрии Repka—ESP32 — Gateway / Operator

### src/hardware/interfaces/controller_telemetry_snapshot.hpp / .cpp

| Тип / функция | Назначение |
|---|---|
| `ControllerTelemetrySnapshot::replace(const ControllerCodecDecodedMessage&, uint64_t received_at_unix_ms) noexcept` | Проверяет тип TELEMETRY_SNAPSHOT, envelope и payload; копирует peer ID/instance, sequence и измерения в новый bounded-объект. Публикует его только после успешного копирования всех полей. |
| `ControllerTelemetrySnapshot::measurement(size_t, ControllerTelemetryMeasurementView&) const noexcept` | Возвращает не владеющий view к уже принадлежащей snapshot памяти для одного измерения; view нельзя хранить после замены/очистки snapshot. |
| `ControllerTelemetrySnapshot::clear()` | Очищает последний snapshot при остановке/повторном старте ControllerLinkService. |
 
Буфер текстовых полей ограничен размером одного ControllerCodec frame (2048 байт), число измерений — 16. Это копия одного последнего снимка, без динамического накопления истории и без persistent storage.

### src/platform/linux/controller_psk_tls_client.hpp / .cpp

| Функция | Назначение |
|---|---|
| `ControllerPskTlsClient::export_session_proof(context, output)` | После полного TLS 1.3 PSK-DHE handshake выводит 32 байта `SSL_export_keying_material()` с фиксированной DockStation exporter label и обязательным непустым ограниченным context; очищает output при любой ошибке. Произвольная label не принимается, чтобы сохранять domain separation. |

### src/platform/linux/controller_link_service.cpp

| Функция | Назначение |
|---|---|
| `ControllerLinkService::process_ready_frame()` | После успешного ControllerCodec decode и ControllerLinkSession acceptance передаёт TELEMETRY_SNAPSHOT в `ControllerTelemetrySnapshot::replace()`; для TELEMETRY_SNAPSHOT/ERROR синхронно вызывает attached handler до consume frame. Строковые view живы только в callback; projector NotApplicable не вызывает SSM Pipeline. Ошибка копирования закрывает кадр/транспорт fail-closed. |
| `ControllerLinkService::telemetry_snapshot()` | Возвращает read-only доступ к последнему принятому snapshot для C++ Gateway serializer. |
| `ControllerLinkService::export_authenticated_session_proof()` | Возвращает exporter proof только при установленном TLS и принятом ControllerLink HELLO_ACK; иначе очищает output и возвращает отказ. Не создаёт SSM evidence и не проверяет реестр источников. |

### src/communications/api_serialization.cpp / local_api_server.cpp

| Функция | Назначение |
|---|---|
| `serialize_controller_telemetry()` | Публикует link status, источник/instance, sequence, received_at_utc и типизированные измерения; качество датчика остаётся отдельным полем, JSON value сохраняет исходный тип. |
| `serialize_system_diagnostics()` | Добавляет необязательный раздел `controller_telemetry` к существующему schema 1.0 diagnostics ответу. |
| `dispatch_api_request_impl()` | Передаёт owned snapshot из ControllerLinkService в diagnostics serializer без удержания view на frame decoder buffer. |

### Operator

`_validate_controller_telemetry()` проверяет статус/состояние канала, sequence и identity, UTC received_at, максимум 16 элементов, все пять типов значения, диапазоны int64/uint64, finite NUMBER и quality enum. Старые снимки без раздела остаются допустимыми. `QmlOperatorBridge.controllerTelemetry` формирует generic list без предположений о типе датчика; `controllerTelemetrySummary` отображает sequence, время получения и состояние канала. QML panel `sensors.controller-telemetry` показывает реальные sensor_id/value/unit/quality/measured_at на странице «Датчики».

**Ограничения:** один REQUEST_SNAPSHOT отправляется после логического handshake; периодический запрос, DiagnosticsManager и постоянная история обычного telemetry report stream пока отсутствуют. Production registry/session adapters и Gateway composition root подключены; explicit subsystem status проходит отдельный F13/SQLite путь при точной конфигурации и предварительно созданных registrations. Telemetry и generic ERROR не попадают в SSM. Fixture-телеметрия/ERROR остаются синтетическими и не выдаются за данные физического ESP32.

**Проверка:** GitHub Actions run 43818913284 SUCCESS для точного HEAD `a89b05eae1d3189bdb5306e4830f027421a4827c`: CTest 25/25 на x64 и ARM64, 31/31 C++ host programs и 158 Python/QML/e2e tests. Сквозной `controller_link_service` прошёл на обеих архитектурах, включая TLS peer/PTY и публикацию fixture-телеметрии. ARM64 Gateway-only build без обнаружения SQLite, Release bundle staging/installer, Sigstore provenance creation и verification также прошли. Signed artifact: [14203573374](https://github.com/GooseRabbit/dockstation/actions/runs/43818913284/artifacts/14203573374); attestation: [67996530](https://github.com/GooseRabbit/dockstation/attestations/67996530); archive SHA-256 `ac47e180e04bd721f4f39463a1f4944b021acc38e68a01168e073e3e5e70b7cf`. Physical Repka OS, UART, ESP32 and Mbed TLS interop remain open.


### Host-проверка TLS exporter

`verification/repka_gateway/test_controller_psk_tls_client.cpp` проверяет совпадение 32-байтового exporter результата на TLS client/server, разделение результатов для разных context, отказ до handshake и очистку output после close. `verification/repka_gateway/test_controller_link_service.cpp` проверяет gating на уровне сервиса: proof недоступен до аутентификации и доступен только после TLS + HELLO_ACK. GitHub Actions run 43821980151 SUCCESS: CTest 25/25 x64 и ARM64, 31/31 C++ host programs, 158 Python/QML/e2e; ARM64 Gateway-only build, release bundle staging и Sigstore provenance также прошли. Это host OpenSSL↔OpenSSL, не Mbed TLS interop и не аппаратная приёмка.

## Дополнение: доверенный ControllerLink ingress adapter

### `src/platform/linux/controller_link_service.hpp/.cpp`

| Тип / функция | Назначение |
|---|---|
| `ControllerLinkMessageHandler` | Синхронный callback одного входящего сообщения; сервис вызывает его только для TELEMETRY_SNAPSHOT/ERROR после TLS handshake, HELLO_ACK, peer/version/instance/sequence validation и до consume frame. Decoded views действительны только внутри callback. |
| `ControllerLinkService::attach_message_handler()` / `detach_message_handler()` | Регистрирует один handler до start; lifetime handler должен завершиться до уничтожения service. |
| `authenticated_pair_identity()` | Возвращает локальную 16-байтовую PSK identity, скопированную из credentials, использованных TLS client; доступна только при TLS + ControllerLink Established и очищается на fail/stop. Это не PSK secret и не Station ID. |
| `authenticated_peer_controller_id()` | Возвращает локально ожидаемый peer ID только после Established; входной envelope уже прошёл точную проверку ControllerLinkSession. |

### `src/communications/authenticated_event_transport_adapter.hpp/.cpp`

| Тип / функция | Назначение |
|---|---|
| `ControllerEventProjector::project()` | Внедряемое domain mapping message → event-specific fields. Не задаёт source, instance, sequence, event_id или received_at; backing string_view должны жить до завершения Pipeline. NotApplicable означает, что Event не создаётся и Pipeline не вызывается. |
| `AuthenticatedEventTransportAdapter` | Прикрепляется до ControllerLinkService::start. Проверяет live channel, локальную pair identity и accepted envelope; для Projected создаёт Event + opaque SourceSessionEvidence из одного кадра и текущего exporter proof и синхронно вызывает Pipeline; NotApplicable оставляет сообщение вне SSM. |
| `AuthenticatedEventTransportStatistics` | Счётчики accepted/projected/invoked/rejected/unmapped messages и последние session/event статусы; не содержит PSK/exporter. |

`source` задаётся локальной конфигурацией; `source_instance`/sequence берутся из уже проверенного envelope; `received_at` назначает Gateway как decimal Unix epoch milliseconds. `event_id` включает hex(pair identity), length-prefixed source и instance и sequence, чтобы одинаковые tuples разных PSK-пар не конфликтовали в общей базе. `session_proof` получается в момент callback через ControllerLinkService exporter; proof bytes не журналируются и очищаются RAII после вызова.

**Проверка:** `test_controller_link_service` запускает TLS 1.3 PSK-DHE peer через POSIX PTY, прикрепляет adapter до service start и передаёт один test TELEMETRY_SNAPSHOT. Проверяются отсутствие callback до HELLO_ACK, недоступность identity до Established, точная передача pair identity/exporter/source/instance/sequence, детерминированный event_id и отсутствие второго вызова после replayed sequence. Production ControllerSubsystemStatusEventProjector отображает explicit subsystem ERROR в SUBSYSTEM_STATUS_UPDATED; в SQLite-enabled сборке тот же тест проходит через настоящие RegistrySourceSessionVerifier, SourceSessionCoordinator, StationEventPipeline, SubsystemStatusEventProcessor и SQLite adapters. Проверяются source/subsystem registrations, channel-bound exporter evidence, Event/history/current/cursor commit, следующий PASS report и replay rejection. Это host OpenSSL↔OpenSSL TLS/PTY fixture, не Mbed TLS interop или hardware acceptance.


### src/communications/controller_subsystem_status_event_projector.hpp/.cpp

| Функция / тип | Назначение |
|---|---|
| `ControllerSubsystemStatusEventProjector::project()` | Production mapping для ControllerLink ERROR protocol 1.1. Возвращает NotApplicable для TELEMETRY_SNAPSHOT и generic ERROR без полной пары subsystem_id/subsystem_status. Для явной валидной пары формирует SUBSYSTEM_STATUS_UPDATED с bounded JSON payload; severity сохраняется отдельно и не используется для вывода статуса. |
| `ControllerSubsystemStatusEventProjector::append_json_string()` | Добавляет JSON string в фиксированный буфер с escaping кавычек, обратной косой черты и управляющих символов; не использует динамическую память. |
| `ControllerSubsystemStatusEventProjector::reset()` | Очищает payload storage перед новым projection, чтобы output view не ссылался на прежнее сообщение. |

Проверка: `verification/repka_gateway/test_controller_subsystem_status_event_projector.cpp`; CTest `controller_subsystem_status_event_projector`. Сквозной тест `controller_link_service` использует тот же production projector в TLS/PTY fixture: telemetry остаётся вне SSM, explicit subsystem ERROR доходит до Pipeline. SQLite-enabled сквозной тест подтверждает доменный F13 commit через production projector и профильный processor; SQLite-disabled/no-profile конфигурация остаётся Unsupported. Это не подтверждает обмен с физическим ESP32 или Mbed TLS interop.


## Дополнение: Gateway SQLite persistence composition root

### `src/application/linux/gateway_persistence_context.hpp/.cpp`

| Тип / функция | Назначение |
|---|---|
| `GatewayPersistenceContext` | Единственный владелец `SqliteDatabase`, `SqliteSourceSessionStore` и `SqliteSubsystemStatusStore`. Database объявлена раньше адаптеров и поэтому уничтожается позже них. |
| `open(path)` / `close()` / `is_open()` / `schema_version()` | Выполняют операции жизненного цикла под тем же mutex, который защищает вызовы адаптеров. Production-путь — `/var/lib/dockstation/dockstation.sqlite3`; альтернативный путь предусмотрен для host-тестов. |
| `with_serialized_stores(callback)` | Выдаёт оба адаптера только внутри одного mutex-protected callback; callback обязан возвращать `void`, не удерживать borrowed views после возврата и не вызывать повторно методы самого context. При закрытой БД callback не вызывается. |

Точка входа Gateway открывает context до запуска controller/API service и закрывает после остановки service, до завершения PlatformLogger. Обе systemd-профили используют `StateDirectory=dockstation`, `StateDirectoryMode=0700` и `UMask=0077`. HTTP API остаётся read-only; context не добавляет provisioning/CRUD routes. Сборка Gateway-only с `DOCKSTATION_BUILD_SQLITE_PERSISTENCE=OFF` не подключает SQLite и продолжает предоставлять только диагностический API, явно журналируя `NOT_CONFIGURED`.

`verification/repka_gateway/test_gateway_persistence_context.cpp` проверяет invalid/missing paths, schema startup, наличие обоих адаптеров, сериализацию конкурентных callback, недоступность адаптеров при закрытой БД и close/reopen. Это host-проверка жизненного цикла; она не заменяет target Repka OS и power-loss acceptance. Authenticated ingress + Pipeline end-to-end integration выполнены: профиль `SUBSYSTEM_STATUS_UPDATED` подключается при наличии production processor/configuration, а без injected processor остаётся незарегистрированным и возвращает `Unsupported`.


**Проверка Gateway persistence + authenticated ingress + local registry provisioning:** GitHub Actions [run 43872537777](https://github.com/GooseRabbit/dockstation/actions/runs/43872537777), exact HEAD `6fa6a2e9eda865b0f97c534f6b815f5ed349c339`, SUCCESS. CTest 34/34 на x86_64 и ARM64; `gateway_persistence_context`, `controller_subsystem_status_event_projector`, `controller_link_service` и три registry CLI CTest проходят. Host acceptance проверяет preview/apply и реальные записи source/subsystem в SQLite от UID `dockstation`, primary DB creation, дубликаты, root denial, config mismatch и PSK non-disclosure. Дополнительно: 31/31 C++ host-программ, 158 Python/Operator/Gateway integration-тестов, systemd/non-root startup, SQLite-enabled ARM64 Gateway/registry bundle, staging installer и Sigstore provenance. Это host CI на Ubuntu 24.04 x86_64/ARM64, а не проверка физической Repka OS, ESP32/Mbed TLS или потери питания.


## Дополнение: authenticated subsystem-event ingress → SQLite

### StationEventPipeline

Добавлен точный маршрут SUBSYSTEM_STATUS_UPDATED категории Subsystem. Если профильный EventProcessorPort не передан, F13-маршрут не регистрируется: известный тип события остаётся в каталоге, но маршрутизатор возвращает Unsupported. Это важно, поскольку Unsupported от уже выбранного профильного обработчика трактуется как ошибка его контракта.

### ControllerSubsystemStatusEventProjector

Projector теперь также реализует SubsystemStatusEventDecoderPort. Он сохраняет типизированные поля из уже валидированного ControllerLink codec frame и возвращает их синхронно в Pipeline. Event payload/schema/quality проверяются на точное совпадение с последним projection; повторного JSON parsing нет. Borrowed string_views остаются живы до завершения синхронного callback.

### Gateway composition root

repka_gateway/main.cpp связывает RegistrySourceSessionVerifier, ControllerSubsystemStatusEventProjector, SubsystemStatusEventProcessor, StationEventPipeline и AuthenticatedEventTransportAdapter. Handler прикрепляется до ControllerLinkService::start. Все repository calls идут через один GatewayPersistenceContext::with_serialized_stores callback; этот mutex удерживается на время синхронного controller/API loop и освобождается до close базы. Для API сервера добавлен overload с внешним ControllerLinkService; прежний overload сохранён.

RuntimeConfig получает необязательные station_id и controller_source. Они задаются только вместе с полной ControllerLink config. Без них ingress NOT_CONFIGURED; без SQLite ingress также отключён. Конфигурация не создаёт доверенные записи: заранее должны существовать source registration с точной локальной TLS PSK identity + Station ID + source и subsystem registration с Station ID + subsystem ID + тем же source. HTTP provisioning/API не добавлялся.

### Сквозная проверка

verification/repka_gateway/test_controller_link_service.cpp в SQLite-enabled build использует настоящие SQLite adapters вместо fake repositories. Через POSIX PTY и OpenSSL TLS 1.3 PSK-DHE peer проходит HELLO/HELLO_ACK, TelemetrySnapshot остаётся вне F13, explicit DEGRADED ERROR сохраняет Event/history/current/cursor, второй PASS добавляет историю и двигает cursor; replayed sequence отклоняется до event adapter. SQLite-disabled build оставляет fail-closed Unsupported fixture.
