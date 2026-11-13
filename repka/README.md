# Диагностика Linux VM и DockStation Gateway

Этот каталог содержит read-only диагностику Linux, пример конфигурации C++ Gateway
и шаблон его systemd-службы. Сетевые проверки находятся в `check_network.sh`,
вспомогательные проверки ОС и архитектуры — в `network_helpers.sh`.

## Что проверяется

- обязательные команды `ip` и `uname`;
- название и версия ОС из `/etc/os-release` (файл читается как данные, не исполняется);
- архитектура CPU и разрядность процесса;
- наличие Ethernet-интерфейса и физический carrier;
- IPv4/IPv6-адреса и маршруты по умолчанию;
- системное разрешение контрольного DNS-имени, если оно задано;
- состояние `ssh.service`/`sshd.service` либо `ssh.socket`/`sshd.socket`.

Скрипт не устанавливает пакеты, не меняет адреса, маршруты, firewall или состояние
SSH-служб. Значение `DOCKSTATION_DNS_CHECK_NAME` необязательно: без него DNS-проверка
помечается как не настроенная и влияет на итог как предупреждение. `getent` использует
NSS, поэтому имя может разрешиться через `/etc/hosts`, а не через внешний DNS.

## C++ Gateway и systemd

Текущий C++ Gateway — отдельная исполняемая цель `dockstation_linux_bootstrap`.
Она предоставляет только read-only маршруты:

- `GET http://127.0.0.1:8080/api/v1/health`;
- `GET http://127.0.0.1:8080/api/v1/system/diagnostics`.

Адрес привязки фиксирован в коде как `127.0.0.1`; изменить его параметрами
командной строки или файла конфигурации нельзя. Для Windows-клиента используется
проверенный SSH local-forward к Linux `127.0.0.1:8080`. Не открывайте порт 8080
на LAN или в Интернет.

### Установка в Ubuntu VM

Команды ниже предназначены для ручной VM-проверки на Ubuntu. Сборка выполняется
внутри Linux VM. Стандартный install prefix намеренно задан как `/usr/local`,
поскольку systemd unit содержит сформированный CMake абсолютный путь к бинарному файлу.

Перед установкой остановите ранее запущенный вручную C++ Gateway или временный
Python Gateway, если они ещё занимают порт 8080. В одном порту одновременно может
работать только один сервер.

Из корня репозитория:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DDOCKSTATION_ENABLE_SANITIZERS=OFF \
  -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build --parallel
# Подготовить relink-артефакт, используемый CMake install для этой цели
cmake --build build --target preinstall --parallel
```

Создайте отдельную системную группу и непривилегированную учётную запись без
возможности интерактивного входа. Команды безопасно пропускают создание,
если соответствующая запись уже существует:

```sh
getent group dockstation >/dev/null || sudo groupadd --system dockstation
getent passwd dockstation >/dev/null || sudo useradd --system --gid dockstation --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin dockstation
```

Установите бинарный файл, unit и пример конфигурации:

```sh
sudo cmake --install build
sudo install -d -o root -g dockstation -m 0750 /etc/dockstation
sudo install -o root -g dockstation -m 0640 \
  /usr/local/share/dockstation/gateway.conf.example \
  /etc/dockstation/gateway.conf
sudo systemd-analyze verify /usr/local/lib/systemd/system/dockstation-gateway.service
sudo systemctl daemon-reload
sudo systemctl enable --now dockstation-gateway.service
```

Конфигурация остаётся доступной службе только для чтения: владелец — root,
группа — dockstation, режим — 0640; каталог — root:dockstation, режим — 0750.
В текущем профиле журналирование направлено в stderr и далее в systemd journal.
Файловый log sink не включён: служба работает с `ProtectSystem=strict` и не
получает каталогов для записи.

### Проверка службы

```sh
systemctl show dockstation-gateway.service -p User -p Group -p ActiveState -p SubState
curl -i http://127.0.0.1:8080/api/v1/health
curl -i http://127.0.0.1:8080/api/v1/system/diagnostics
ss -ltnp | grep ':8080'
sudo journalctl -u dockstation-gateway.service -n 50 --no-pager
```

В `systemctl show` ожидаются `User=dockstation`, `Group=dockstation` и
`ActiveState=active`. В `ss` должен отображаться только локальный listener
`127.0.0.1:8080`, не wildcard и не Ethernet-адрес VM. Оба GET-запроса должны
вернуть HTTP 200. Общий статус диагностики может оставаться `WARN`, если источник
DNS или статус SSH-службы для C++ сборщика не настроены; это не означает отказ
самого HTTP Gateway.

Проверьте управляемый перезапуск и остановку:

```sh
sudo systemctl restart dockstation-gateway.service
sudo systemctl stop dockstation-gateway.service
sudo systemctl start dockstation-gateway.service
```

После остановки API должен перестать отвечать; после старта — снова вернуть
health и diagnostics. `Restart=on-failure` перезапускает процесс при аварийном
завершении, но не отменяет явную команду `systemctl stop`. Для проверки запуска
после перезагрузки VM отдельно убедитесь, что служба включена (`enabled`), затем
перезагрузите VM и повторите проверки статуса и API.

Unit ограничивает права процесса: отдельные User/Group, пустые Linux capabilities,
`NoNewPrivileges`, `ProtectSystem=strict`, `ProtectHome`, изоляция временного
каталога и устройств, read-only защита kernel tunables/modules/control groups,
ограничение семейств сокетов. Эти ограничения соответствуют текущему
read-only Gateway и не дают ему права выполнять действия с GPIO, UART или
исполнительными механизмами. При будущем добавлении драйверов устройств,
постоянного хранилища или файлового журналирования разрешённый доступ нужно
согласовать и добавить адресно, а не отключать hardening целиком.

**Статус проверки:** установочная раскладка и systemd unit проходят автоматическую
проверку CI. Дополнительно владелец вручную проверил в Ubuntu 24.04.5 LTS x86_64 VM
установку и `systemctl enable/start/stop/restart`, пользователя `dockstation`,
HTTP health/diagnostics, loopback-only listener, journal и автозапуск после reboot.
После перезагрузки служба имела `enabled` и `active`, а journal текущей загрузки
подтвердил запуск unit; Windows Operator снова отображал Connected.
CI не заменяет ручное испытание systemd manager. Протокол — в
`docs/30_REPKA_NETWORK_IMPLEMENTATION_PLAN.md`.

Примечание по CMake: в проверенной конфигурации перед `sudo cmake --install build`
потребовалось явно выполнить `cmake --build build --target preinstall --parallel`.
Этот шаг формирует relink-артефакт, на который ссылается `cmake_install.cmake`.

## Временный Python Gateway

Временный read-only Gateway находится в `repka_gateway/`. Он использует только
стандартную библиотеку Python 3.11+ и привязывается исключительно к `127.0.0.1`.
Он сохранён для ранних VM-тестов и test-only сценария PING; это не текущая служба
systemd и не production API.

Запуск из корня репозитория:

```sh
python3 -m repka_gateway.dockstation_gateway.main --port 8080
```

Python и C++ Gateway используют один и тот же порт 8080 — не запускайте их
одновременно.

## Границы текущей проверки

Данные относятся к ОС и сетевому стеку виртуальной машины. Они не являются
телеметрией ESP32, датчиков, приводов или физической док-станции. Состояние
systemd unit не подтверждает успешный вход с Windows по SSH-ключу, а DNS-результат
не подтверждает доступность конкретного внешнего сервера.

Ubuntu VM x86_64 не является целевой Repka Pi 5: ARM64-сборка, Repka OS,
systemd-развёртывание на целевой плате, ESP32 и физические узлы требуют отдельной
проверки. Для инструкции по архитектуре и результатам ручных VM-испытаний
см. `docs/30_REPKA_NETWORK_IMPLEMENTATION_PLAN.md` и мастер-план `docs/31`.
