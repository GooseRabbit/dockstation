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
  -DDOCKSTATION_BUILD_SQLITE_PERSISTENCE=OFF \
  -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build --parallel
# Подготовить relink-артефакт, используемый CMake install для этой цели
cmake --build build --target preinstall --parallel
```

SQLite persistence foundation пока остаётся отдельным экспериментальным модулем:
Gateway runtime его не использует. Для целевой сборки Gateway параметр
`DOCKSTATION_BUILD_SQLITE_PERSISTENCE=OFF` исключает сам target и проверку
`find_package(SQLite3)`, поэтому Repka не требуется устанавливать SQLite
development headers только ради сборки read-only Gateway. Полный CI и отдельные
проверки кандидата SQLite собираются с параметром `ON` и продолжают проверять
SQLite. Отключение модуля не означает отказ от будущего постоянного хранения.

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
Этот шаг формирует relink-артефакт, на который ссылается \`cmake_install.cmake\`.

### Установка целевого C++ Gateway одной командой

После checkout репозитория и read-only preflight установку на целевую ARM64 Repka OS
можно выполнить так:

\`\`\`sh
sudo bash repka/install_gateway.sh
\`\`\`

Сценарий собирает Release Gateway без SQLite development dependency, устанавливает
бинарные файлы и оба systemd unit-файла, создаёт системную группу и непривилегированного
пользователя \`dockstation\`, сохраняет существующую конфигурацию, проверяет unit-файлы
и запускает только loopback read-only Gateway. Если уже работает UART instance, он
обновляется и перезапускается без смены аппаратного режима. Если в конфигурации задан
\`controller_uart_device\`, а UART unit не активен, базовый профиль не запускается.

Установщик не вызывает \`apt\`, не скачивает или обновляет пакеты, не меняет загрузочную
конфигурацию, Device Tree, pinmux, права UART и не создаёт PSK. CMake, C++17 и OpenSSL 3
development headers должны быть доступны заранее. При их отсутствии сценарий
остановится на preflight/build, не меняя пакетную конфигурацию ОС. GPIO, камера и UART
не включаются автоматически.

Для CI/package staging предусмотрен \`DOCKSTATION_INSTALL_ROOT=/путь\`. В этом режиме
файлы устанавливаются в staging-каталог, а системные учётные записи и systemd manager
хоста не затрагиваются; на самой плате этот режим не используется.

### Отдельный профиль для одного UART-устройства

Обычный \`dockstation-gateway.service\` намеренно использует \`PrivateDevices=true\`:
он остаётся подходящим для read-only диагностики и не получает физические устройства.
Для будущего соединения Repka—ESP32 устанавливается отдельный шаблон
\`dockstation-gateway-uart@.service\`. Он не включается автоматически и требует
явного выбора одного реального character device в имени экземпляра службы.

Перед включением профиля на целевой Repka OS сначала установите фактический узел
UART и проверьте, что это именно character device. Не считайте \`/dev/serial0\`,
\`/dev/ttyS0\` или номер UART гарантированным до проверки конкретного образа:

\`\`\`sh
ls -l /dev/serial0 /dev/serial/by-id/* /dev/serial/by-path/* \\
  /dev/ttyS* /dev/ttyAMA* /dev/ttyFIQ* /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || true
readlink -f /dev/serial0 2>/dev/null || true
test -c /dev/ttyS0 && stat -c '%F %A %U:%G %n' /dev/ttyS0
\`\`\`

В командах ниже \`ttyS0\` — только пример. Замените его на basename реального
устройства, подтверждённого предыдущей проверкой. Если \`/dev/serial0\` является
символьной ссылкой, используйте конечный узел, на который она указывает, а не
предполагаемое имя ссылки.

Укажите в \`/etc/dockstation/gateway.conf\` тот же абсолютный путь:

\`\`\`ini
controller_uart_device=/dev/ttyS0
\`\`\`

Проверьте владельца, группу и режим доступа к этому узлу:

\`\`\`sh
stat -c '%F %A %U:%G %n' /dev/ttyS0
\`\`\`

Пользователь \`dockstation\` должен иметь обычное право доступа к устройству через
его фактическую группу или отдельное узкое правило udev. Не добавляйте его в
группу \`root\` и не меняйте права устройства на \`0666\`. Если группа устройства
предназначена для доступа к UART, добавьте \`dockstation\` именно в эту группу
(например, \`dialout\`, только если \`stat\` действительно показал \`dialout\`):

\`\`\`sh
sudo usermod -aG dialout dockstation
\`\`\`

После подтверждения пути, прав и настроек остановите базовый unit и явно
включите экземпляр UART-профиля:

\`\`\`sh
sudo systemctl disable --now dockstation-gateway.service
sudo systemctl daemon-reload
sudo systemd-analyze verify /usr/local/lib/systemd/system/dockstation-gateway-uart@.service
sudo systemctl enable --now dockstation-gateway-uart@ttyS0.service
\`\`\`

Замените \`ttyS0\` в последней команде на выбранный basename. \`DevicePolicy=closed\`
совместно с \`DeviceAllow=/dev/%i rw\` добавляет адресное разрешение чтения и
записи выбранного узла сверх штатных исключений systemd; wildcard-доступ к
остальным классам устройств не добавляется. \`PrivateDevices=false\` здесь нужен
только для видимости физического \`/dev\`, а не для снятия device-фильтра.
Остальные ограничения
(\`NoNewPrivileges\`, пустые capabilities, \`ProtectSystem=strict\`, защита \`/home\`,
kernel и сетевых семейств) сохранены. Профиль не добавляет явных разрешений на GPIO,
камеру, общие блочные устройства или другие UART.

Проверьте состояние и журнал:

\`\`\`sh
systemctl show dockstation-gateway-uart@ttyS0.service -p User -p Group -p ActiveState -p SubState
sudo journalctl -u dockstation-gateway-uart@ttyS0.service -n 50 --no-pager
curl -i http://127.0.0.1:8080/api/v1/health
curl -i http://127.0.0.1:8080/api/v1/controller/link
\`\`\`

Этот профиль подтверждает только конфигурацию доступа на уровне systemd. Он не
подтверждает, что выбранный UART не занят системной консолью, соответствует
нужному физическому разъёму и корректно связан с ESP32. Проверка console/DT/pinmux,
прав и фактического обмена должна быть выполнена на точной версии Repka OS 5.
Профили камеры и GPIO будут добавляться отдельно после появления соответствующих
адаптеров и проверки минимальных device permissions.

### Read-only preflight целевой Repka OS

Перед установкой и особенно перед включением UART-профиля выполните из корня
checkout репозитория:

```sh
bash repka/check_target_platform.sh
```

Скрипт только читает `/etc/os-release`, модель платы, архитектуру и ядро,
проверяет CMake, C++17, заголовки OpenSSL 3.x, наличие systemd и установленные
Gateway unit-файлы, если они уже есть. Он перечисляет UART character devices,
сопоставляет их имена с параметрами `console=` в `/proc/cmdline` и сообщает
владельца/группу обнаруженных узлов. SQLite отмечается как необязательная
возможность: для Gateway-only сборки её development headers не требуются.

Скрипт не запускает `apt`, не устанавливает/обновляет пакеты, не отключает
системную консоль, не меняет Device Tree/pinmux, права устройств или службы.
Код завершения: `0` — обязательные проверки пройдены без предупреждений,
`1` — есть предупреждения, `2` — обязательное требование не выполнено.
Предупреждение о непроверенных pinmux и физическом UART ожидаемо до аппаратной
приёмки; его нельзя трактовать как разрешение включить порт.

Проверка не подтверждает физическую распиновку, электрические уровни, свободность
линии на разъёме или обмен с ESP32. Для включения UART всё равно требуется
проверить точный device node, kernel console и его группу на конкретном образе.

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

Нативные ARM64 GitHub Actions runs 43730599989, 43730790585, 43731200878 и
43731476237 завершились SUCCESS. Последние прогоны подтверждают CMake build,
23/23 CTest на x86_64 и ARM64; дополнительно выполнена сборка Gateway-only без
поиска SQLite development package, установка и проверка обоих systemd unit-файлов,
а также host-проверка read-only Repka preflight. Run 43731476237 включает
регрессионную проверку согласованности RuntimeEnvironment/SystemDiagnostics.
Даже успешная ARM64-сборка на Ubuntu не подтверждает ABI и package set точной
официальной Repka OS 5, настройки её ядра, физический UART и подключение ESP32 —
целевая плата требует отдельной приёмки.
