# Проверки отказов обмена ControllerLink (Repka ↔ ESP32)

**Статус:** матрица host-level verification; физическая приёмка не выполнена.  
**Дата:** 2026-11-18.

## 1. Назначение

Документ фиксирует программные проверки отказных сценариев, расположение тестов и границы доказательности. Host-тесты не заменяют проверку реальных UART-линий, питания, GPIO и ESP-IDF driver queue на физических платах.

## 2. Матрица проверок

| Сценарий | Проверка | Ожидаемое поведение |
|---|---|---|
| ESP32 отключился в середине передачи | controller_link_service (PTY + OpenSSL TLS) | неполный кадр не публикуется; сессия теряется; сохраняется последний полный snapshot |
| Повторное подключение | controller_link_service, controller_link_peer_session | новый TLS transport и HELLO/HELLO_ACK; replay history сохраняется для того же boot instance |
| Неполные сообщения | controller_frame_stream_decoder, controller_link_service | фрагментированная доставка ожидает остаток кадра; данные не передаются выше до полной границы кадра |
| Повреждённые сообщения | controller_codec, controller_frame_stream_decoder | ошибочная длина/CRC отклоняются; scanner восстанавливается на следующем корректном кадре |
| Задержка или отсутствие ответа | controller_link_session, controller_transport_manager, controller_link_reconnect_backoff, controller_link_service | handshake/heartbeat deadline приводит к потере сессии и контролируемому reconnect |
| Переполнение очереди событий UART | controller_uart_event_queue_policy | не более 8 событий за poll; остаток откладывается; FIFO overflow, RX buffer full, framing/parity/BREAK являются фатальными ошибками потока |
| Повторная отправка команд | controller_link_peer_session, controller_link_peer_service | повтор с тем же sequence отвергается; вторая команда не принимается, пока первая pending; disconnect очищает временный pending state |

## 3. Семантика повторной отправки команд

- Repka не выполняет автоматическую повторную отправку COMMAND_REQUEST.
- Повтор кадра с тем же sequence_number защищён от replay в рамках сохранённой ControllerLink session.
- Вторая команда с новым sequence при уже pending-команде отклоняется peer session.
- ACCEPTED означает принятие/ожидание, а не физическое завершение.
- Если транспорт потерян после возможного принятия команды, физический исход считается UNKNOWN. Нельзя автоматически повторять её с новым sequence.
- В текущем протоколе нет durable command ID и гарантии exactly-once через перезапуск контроллера. Для безопасной автоматической доставки потребуется отдельное решение по идентификаторам команд, хранению результата и восстановлению после reboot.

## 4. Что доказано host-тестами

Host CTest использует production codec/session/service классы, POSIX PTY/socket pair и TLS реализации на тестовых концах. Проверяется программная обработка байтов, тайм-аутов, sequence/replay и очередей согласно матрице.

UART event queue policy используется ESP-IDF transport и отдельно проверяется на host. Это доказывает bounded drain и классификацию событий, но не поведение физического FreeRTOS queue или UART peripheral.

## 5. Что остаётся аппаратной приёмкой

На физической паре Repka Pi + ESP32 необходимо отдельно проверить:
1. UART 115200, 8N1, уровни 3,3 В, перекрёст TX/RX и общую землю;
2. отключение/перезапуск ESP32 во время фактической передачи;
3. восстановление после потери питания и отсоединения проводов;
4. реальные FIFO/RX buffer/framing/parity события ESP-IDF;
5. задержки, шум линии и длительную нагрузку;
6. RAM/stack во время TLS handshake, обмена и reconnect.

До такой проверки физический транспорт имеет статус NOT_CONFIGURED / NOT VERIFIED. Успешный CI означает только прохождение перечисленных host/build tests.

## 6. Восстановление телеметрии после потери канала и рестарта Gateway

Расширен сквозной host-тест `verification/repka_gateway/test_controller_link_service.cpp`:

1. При потере peer посреди telemetry frame неполный снимок не публикуется; Gateway сохраняет последний полный снимок только как диагностически устаревший.
2. После нового TLS 1.3 PSK-DHE и HELLO/HELLO_ACK Gateway отправляет REQUEST_SNAPSHOT. Новый снимок заменяет прежний и проверяется по всем четырём DHT22-каналам: два числовых измерения первого датчика и две явные записи NOT_CONFIGURED второго.
3. Повтор ранее принятого sequence отклоняется; последний полный снимок сохраняется, но общий статус телеметрии становится WARN из-за потерянного authenticated link.
4. Явная остановка ControllerLinkService очищает process-local snapshot. Повторный start создаёт новый per-process instance ID и не публикует прежние значения.
5. После нового TLS/HELLO/HELLO_ACK тот же peer продолжает собственную последовательность сообщений, получает REQUEST_SNAPSHOT и передаёт новый DHT22 snapshot. Gateway публикует его в diagnostics со статусом PASS и проверенными новыми числовыми значениями.
6. Повторная остановка снова очищает process-local snapshot.

Тест использует production `ControllerLinkService`, POSIX PTY и OpenSSL TLS 1.3 PSK-DHE peer. Это проверка lifecycle/recovery на Linux host. Она моделирует stop/start сервиса внутри тестового процесса; не является испытанием фактического systemd process restart, ESP-IDF FreeRTOS task, Mbed TLS peer на физической ESP32 или UART-проводов. Отдельные systemd startup/restart checks остаются в CI, а физическое отключение питания/кабеля и восстановление на паре плат — аппаратной приёмкой.

**Результат CI:** run [44485093597](https://github.com/GooseRabbit/dockstation/actions/runs/44485093597) — SUCCESS для exact HEAD `88e8fc4f7cd38c8ba70abb4b50e47905f1455650`. CTest прошёл 50/50 на Linux x86_64, Linux ARM64 и OpenSSL 3.5; дополнительно прошли 31 C++ host program, 165 Python/Operator/Gateway tests, Mbed TLS/OpenSSL interoperability и подписание ARM64 bundle. Физическая приёмка остаётся отдельной задачей.


## 7. Восстановление ESP32 peer service после потери транспорта

Расширен `verification/repka_gateway/test_controller_link_peer_service.cpp`: после завершения полного TLS/ControllerLink сценария клиент закрывает первое соединение. Тот же экземпляр `ControllerLinkPeerService` обнаруживает закрытие, очищает pending-команду и переинициализирует TLS transport через `restart_transport()` на новом POSIX socket pair.

Затем тест выполняет новый TLS 1.3 PSK-DHE handshake и отправляет HELLO от того же peer boot instance с sequence 8 (после ранее принятых sequence 1–7). Проверяется HELLO_ACK с прежним локальным controller instance ID и продолжающимся исходящим sequence 8. Тем самым на host проверяется сохранение объекта peer-session, replay history и счётчиков последовательности между transport reconnect, а также очистка временного pending command state.

Это host integration test production Mbed TLS peer service на POSIX socket pair. Он не проверяет UART driver, FreeRTOS scheduling, реальную потерю питания/проводов или физическую ESP32. Отдельная аппаратная приёмка остаётся обязательной.
