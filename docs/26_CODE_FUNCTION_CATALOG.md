# Каталог предполагаемых функций DockStation

**Статус:** предварительный рабочий каталог.  
**Дата:** 2026-11-06.  
**Назначение:** показать, на какие отдельные функции может быть разбито программное обеспечение станции и совместно разрабатываемая бортовая часть.

> **Важно:** приведённые функции, их названия, расположение по файлам и их количество могут измениться по мере проектирования, выбора БАС, оборудования, платформы и протоколов. Это предварительный ориентир, а не окончательное техническое задание.

Сигнатуры записаны в условном C++-подобном виде только для наглядности. Язык программирования не выбран. Типы вроде `Result<T>`, `Mission`, `Event`, `Status` и другие названия обозначают данные, структуру которых ещё предстоит определить. Функции со статусом **«уже есть»** присутствуют в текущем коде `main`; остальные — предполагаемые.

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

### src/core/event_processing.cpp

| Функция | Назначение |
|---|---|
| `EventCategory classify_event(const Event& event) noexcept` **(уже есть)** | По точному `event_type` определяет одну из шести категорий из §2.1 документа `06`; неизвестный тип возвращает `Unknown`. Не меняет событие и не выполняет побочных действий. |
| `DuplicateEventResult check_duplicate_event(const Event& event, EventRepository& repository) noexcept` **(уже есть)** | Проверяет сохранённое событие по `event_id`: NEW, DUPLICATE, CONFLICT или REPOSITORY_ERROR. Сравнение точное, кроме поля `received_at`, назначаемого принимающей стороной; репозиторий только читается. Не резервирует ID и не сохраняет результат обработки. |
| `EventFreshnessResult check_event_freshness(const Event& event, const SourceCursor& cursor) noexcept` **(реализована)** | Сравнивает source_sequence в пределах одного источника/сессии; не использует календарное время, не меняет курсор. При новой сессии вызывающий слой предварительно подтверждает её и сбрасывает курсор. |


### src/core/event_handling.cpp

| Функция | Назначение |
|---|---|
| `EventHandlingResult handle_event(const Event& event, const EventValidationPolicy& validation_policy, const SourceCursor& source_cursor, EventRepository& event_repository, EventProcessorPort& processor) noexcept` **(реализована; SSM-F13)** | Выполняет F01–F04 в фиксированном порядке и передаёт прошедшее F01 проверку конверта, известное, новое и упорядоченное событие профильному обработчику. Не разбирает payload, не продвигает курсор и не выполняет физические действия. |

### src/core/global_transition_processor.cpp

| Функция | Назначение |
|---|---|
| `EventProcessorStatus GlobalTransitionProcessor::process(const Event& event, EventCategory category, const SourceCursor& expected_cursor) noexcept` **(реализована; профильная часть SSM-F13)** | Обрабатывает только три согласованные стартовые пары; читает snapshot, получает доверенный контекст, применяет F05 и передаёт план/доказательства в F10 с исходным курсором. Не разбирает payload, не расширяет матрицу переходов и не управляет оборудованием. |

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
| `OperationTimeoutResult handle_operation_timeout(const OperationTimeout& timeout, const SourceCursor& source_cursor, OperationUpdateRepository& repository) noexcept` **(реализована; отдельный harness, интегрированный запуск ожидает проверки)** | По подтверждённому таймерному событию переводит только в FAILED до отправки либо OUTCOME_UNKNOWN после возможной отправки; сохраняет событие и lifecycle атомарно. |


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

## 15. Конфигурация станции

### src/configuration/configuration_manager.cpp

| Функция | Назначение |
|---|---|
| `ConfigurationLoadResult load_configuration()` | Загружает сохранённую конфигурацию станции. |
| `ConfigurationValidationResult validate_configuration(const Configuration& configuration)` | Проверяет версию, обязательные поля, диапазоны и внутреннюю согласованность конфигурации. |
| `ConfigurationSaveResult save_configuration(const Configuration& configuration)` | Сохраняет новую конфигурацию с контролем целостности. |
| `ConfigurationRevision get_configuration_revision()` | Возвращает номер/идентификатор текущей версии конфигурации. |
| `ConfigurationApplyResult apply_configuration_change(const ConfigurationChange& change, const AuthorizationContext& authorization)` | Применяет разрешённое изменение конфигурации и записывает его в аудит. |

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

## 17. Учётные записи и права — UserAccessManager

### src/access/user_access_manager.cpp

| Функция | Назначение |
|---|---|
| `PasswordVerifier create_password_verifier(std::string_view password)` | Создаёт проверочное значение пароля с использованием выбранного алгоритма хранения паролей. Сам алгоритм ещё не утверждён. |
| `bool verify_password(std::string_view password, const PasswordVerifier& verifier)` | Проверяет введённый пароль по сохранённому проверочному значению. |
| `AuthenticationResult authenticate_user(const LoginRequest& request)` | Проверяет учётную запись и создаёт пользовательскую сессию при успехе. |
| `UserResult create_user_account(const UserProfile& profile, const AuthorizationContext& authorization)` | Создаёт индивидуальную учётную запись пользователя. |
| `PasswordChangeResult change_user_password(UserId user_id, const PasswordChangeRequest& request)` | Меняет пароль пользователя по утверждённой процедуре. |
| `UserStatusResult set_user_status(UserId user_id, UserStatus status)` | Блокирует или разблокирует учётную запись. |
| `BindingResult bind_user_to_station(UserId user_id, StationId station_id, const PermissionSet& permissions)` | Создаёт связь пользователя с конкретной станцией и назначает полномочия. |
| `PermissionUpdateResult update_user_station_permissions(UserId user_id, StationId station_id, const PermissionSet& permissions)` | Изменяет права пользователя в пределах выбранной станции. |
| `BindingResult revoke_user_station_binding(UserId user_id, StationId station_id)` | Отвязывает пользователя от станции и прекращает доступ к ней. |
| `SessionRevocationResult revoke_active_sessions(UserId user_id, StationId station_id)` | Отзывает активные сессии и токены после блокировки, смены прав или отвязки. |
| `AuthorizationResult authorize_command(const UserContext& user, StationId station_id, CommandType command)` | Проверяет право пользователя выполнить конкретную команду для конкретной станции. |

---

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
