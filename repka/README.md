# Диагностика Linux и DockStation Gateway

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

## Расширенный сбор диагностики Linux-платформы

Для инвентаризации фактического окружения предусмотрен отдельный read-only сборщик:

```bash
bash repka/collect_platform_diagnostics.sh
```

По умолчанию в текущем каталоге создаются каталог отчёта, архив `.tar.gz` и файл контрольной суммы `.sha256`. Можно указать собственный путь:

```bash
bash repka/collect_platform_diagnostics.sh ./repka-platform-report
```

Запускается от обычного пользователя, без `sudo`. Утилита не устанавливает пакеты, не загружает драйверы, не изменяет загрузку, Device Tree, pinmux, права устройств или службы. Отказ в доступе фиксируется как недоступный источник, а не обходится повышением привилегий.

Отчёт включает выбранные сведения об ОС/ABI, процессоре и памяти, ядре и загруженных модулях, доступной конфигурации ядра, активном Device Tree и aliases, allowlist параметров загрузки, перечень загрузочных файлов, UART/GPIO/I2C/SPI/PWM/V4L2/media устройств, thermal zones, выбранные версии пакетов/библиотек, наличие диагностических утилит и доступные мультимедийные сведения. Для GPIO/I2C выполняются только безопасные операции перечисления: `gpioinfo` не запрашивает значения линий, `i2cdetect -l` показывает только адаптеры и не сканирует шину.

Конфиденциальность: полный `/proc/cmdline`, сетевые адреса, hostname, machine-id, MAC, серийные номера, ключи, credentials, переменные окружения и журналы служб не собираются. В отчёт попадают только явно разрешённые параметры загрузки и выбранные несекретные сведения. Каталог и файлы отчёта создаются с ограниченными правами доступа.

**Ограничения интерпретации:** наличие device node, модуля или Device Tree-узла не доказывает, что контакт физического разъёма настроен правильно или электрически работает. Наличие GStreamer/FFmpeg плагина не подтверждает работу аппаратного кодека; список V4L2-устройств не подтверждает полноценный видеопоток.

Запуск на существующей Ubuntu VM даст профиль именно этой Ubuntu — он полезен как базовая точка сравнения, но не является снимком Repka OS. Для фактических сведений Repka Pi 5 тот же сборщик запускается позднее непосредственно на плате. Отдельная виртуальная машина с Repka OS для этого этапа не требуется.

## C++ LinuxPlatformCapabilities

В C++ runtime добавлен компонент LinuxPlatformCapabilities (src/platform/linux/platform_capabilities.hpp/.cpp). Он пассивно распознаёт Repka Pi 5 по Device Tree или выбирает общий Generic Linux профиль, а также учитывает UART, GPIO, I2C, SPI, PWM и V4L2/media nodes и соответствующие sysfs controllers.

Статусы: AVAILABLE — узел доступен текущему процессу для чтения/записи; DETECTED — узел существует, но права доступа текущего процесса не подтверждены; NOT_CONFIGURED — sysfs показывает контроллер, но userspace node не найден; UNAVAILABLE — прочитанные источники не показывают интерфейс; UNKNOWN — источники недоступны. Для UART отдельно фиксируется совпадение с kernel console=.

Компонент только собирает сведения. Он не открывает устройство для обмена, не назначает контакты, не выбирает UART автоматически, не загружает драйверы и не меняет права. Выявленный путь устройства — диагностический кандидат, а не готовая настройка. Решение по WiringRP, модулю repka_gpio.ko, автозагрузке и минимальным правам службы остаётся открытым (DEC-069); установщик и Gateway не должны менять эти параметры до отдельного согласования.

Тест: verification/repka_gateway/test_platform_capabilities.cpp, CTest platform_capabilities. Fixture проверки не являются аппаратным тестом.

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

### Готовый ARM64 bundle из CI

После успешного ARM64 CI run workflow публикует готовый архив
`dockstation-repka-pi5-arm64.tar.gz` и файл его контрольной суммы. Архив собирается
на нативном ARM64 runner из Release Gateway-only build без SQLite, содержит два
исполняемых файла, два systemd unit-файла, пример конфигурации, manifest и список
фактических динамических зависимостей. Перед публикацией CI проверяет AArch64 ELF,
SHA-256 файлов и разрешение runtime-зависимостей на ARM64 runner.

Артефакт доступен на странице соответствующего GitHub Actions run в разделе
Artifacts (ссылка также попадает в Summary). Срок хранения CI artifact — 30 дней;
это не постоянный релиз. Рядом лежит sidecar `.sha256`; после скачивания проверьте
его командой:

```sh
sha256sum -c dockstation-repka-pi5-arm64.tar.gz.sha256
```

Проверку состава, целостности и AArch64-зависимостей архива можно повторить в
Linux окружении с `tar`, `sha256sum`, `file` и `ldd`:

```sh
bash repka/verify_arm64_bundle.sh dockstation-repka-pi5-arm64.tar.gz
```

Важно: bundle собирается на Ubuntu 24.04 ARM64, а не на официальной Repka OS.
Успешный `ldd` на runner не гарантирует совпадение runtime-библиотек на конкретной
версии Repka OS 5. На самой плате перед установкой потребуется проверить
совместимость runtime libraries; bundle не включает системные библиотеки и не
пытается их обновлять.

### Установка готового ARM64 bundle без компилятора

Скачайте из GitHub Actions artifact архив `dockstation-repka-pi5-arm64.tar.gz`
и sidecar `.sha256` в один каталог. Из корня checkout DockStation выполните:

```sh
sudo bash repka/install_gateway.sh --bundle ./dockstation-repka-pi5-arm64.tar.gz
```

В этом режиме используется runtime-only preflight: требуются 64-битная ARM64,
systemd, `ldd`, `tar`, `sha256sum` и стандартные инструменты системной установки.
CMake, C++ compiler, pkg-config и OpenSSL development headers на плате не нужны.
Установщик проверяет внешнюю sidecar SHA-256, затем внутренние SHA-256, manifest,
допустимый состав и пути архива, отсутствие symlink/hardlink и фактические runtime
библиотеки через `ldd` непосредственно на целевой ОС. Если обязательной runtime
библиотеки не хватает, установка останавливается до копирования файлов. На CI
дополнительно проверяются максимальные версии ELF-символов: `GLIBC_2.34`,
`GLIBCXX_3.4.32`, `CXXABI_1.3.9` и `OPENSSL_3.0.0`; будущий bundle с более новыми
ABI-требованиями не пройдёт publication gate без осознанного пересмотра целевой
платформы. На самой Repka `readelf` необязателен, но проверка фактических библиотек
через `ldd` обязательна.

ARM64 CI запускает режим с реальным Release bundle в изолированном staging:
проверяются runtime preflight, sidecar и internal SHA, ELF architecture/symbol
versions, `ldd`, копирование файлов и повторный запуск с сохранением операторской
конфигурации. В staging не вызывается systemd manager; фактический
`systemd-analyze verify` выполняется на целевой системе после установки в её
настоящий `/usr/local`.

Bundle не включает системные библиотеки и не обновляет ОС. Установщик не запускает
`apt`, `apt upgrade` или `do-release-upgrade`, не меняет boot, Device Tree,
pinmux или права устройств. Если отсутствует, например, `libssl.so.3`, сначала
нужно подтвердить у производителя поддерживаемый способ получения библиотеки;
автоматическая установка или обновление пакетов намеренно не выполняется.



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

**Проверка установщика:** GitHub Actions [run 43743089682](https://github.com/GooseRabbit/dockstation/actions/runs/43743089682) завершился SUCCESS на x64 и ARM64. На обеих архитектурах прошли 23/23 CTest; на x64 также прошли 31 host-программа, 141 Python-тест, Operator/Python Gateway/e2e, shell syntax check и полный Repka shell-test loop, включая новые hardening-тесты. Нативный ARM64 job собрал Release bundle, проверил SHA-256, AArch64 ELF, ABI symbol ceilings и runtime dependencies, затем установил и повторно установил архив в изолированном staging с `DOCKSTATION_CMAKE_COMMAND=/bin/false`, сохранив пользовательскую конфигурацию. Artifact ID: `14173500998` (срок хранения GitHub Actions — 30 дней). Физический install и systemd lifecycle на официальной Repka OS не выполнялись.

### Дополнительные проверки безопасности установщика

Перед установкой bundle проверяется строгая привязка sidecar SHA-256 к имени
переданного архива; sidecar должен содержать ровно одну запись. Архив копируется
в приватный временный каталог, и проверка с последующей распаковкой используют
один и тот же снимок. Пути назначения проверяются до записи файлов, symlink и
необычные типы файлов назначения запрещаются.

На реальной установке перед копированием проверяются уже существующие учётные
записи `dockstation`; допустимы только непривилегированные UID/GID, ожидаемая
основная группа, каталог `/nonexistent` и оболочка `nologin`. Проверка
вынесена в read-only `repka/check_service_identity.sh`; в staging-режиме
учётные записи хоста не читаются и не изменяются.

`repka/verify_arm64_bundle.sh` отклоняет повторяющиеся пути и любые типы
архивных записей, кроме ожидаемых обычных файлов и каталогов. После распаковки
каждый бинарник, unit и пример конфигурации повторно проверяется как обычный файл,
а не symlink. Внутренний `SHA256SUMS` должен включать ровно семь ожидаемых файлов,
без пропусков, дубликатов и путей за пределами пакета; имена и формат строк
проверяются до вызова `sha256sum --check`. Эти ограничения покрываются отдельными скриптами `test_install_gateway_hardening.sh` и
`test_service_identity_preflight.sh`. Оба файла автоматически входят в обязательный
shell test loop текущего Linux CI workflow; новые `test_*.sh` в этой директории
также попадают в общий цикл без отдельного списка в workflow.

### Аудит состава и происхождения ARM64 bundle

Release job проверяет, что checkout HEAD точно совпадает с `github.sha`, и записывает
40-символьный source commit и его commit timestamp в manifest. Manifest также
фиксирует Release-only профиль, отключённую SQLite persistence и ABI ceilings
(`GLIBC_2.34`, `GLIBCXX_3.4.32`, `CXXABI_1.3.9`, `OPENSSL_3.0.0`).

`RUNTIME-DEPENDENCIES.txt` содержит отсортированный SONAME inventory для каждого
бинарника. Он не сохраняет ASLR-адреса или абсолютные пути build runner. При сборке
CI требует точного совпадения этого inventory с `ldd`; при установке verifier
проверяет, что каждый записанный SONAME разрешается на целевой системе. Это
позволяет целевой ОС иметь дополнительные транзитивные зависимости, не теряя
контроль над обязательными библиотеками из build environment.

Архив создаётся с фиксированными owner/group, порядком имён, commit timestamp и
gzip без timestamp/original filename. CI повторно упаковывает тот же payload и
сравнивает архивы побайтно. Это подтверждает детерминированность упаковки одного
готового дерева; отдельная проверка воспроизводимости компиляции из двух чистых
build directories пока не заявляется.

**Результат аудита (GitHub Actions run 43744455212):** SUCCESS на x64 и ARM64.
На каждой архитектуре прошли 23/23 CTest; x64 также прошёл 31 host-программу,
141 Python-тест, QML/e2e и полный Repka shell-test loop. ARM64 job подтвердил
manifest/source commit, exact build SONAME inventory, ABI ceilings, ldd,
побайтно идентичное повторное создание архива и два запуска bundle installer
в isolated staging с CMake=/bin/false. Новый artifact ID: 14173668321;
срок хранения — 30 дней. Это проверка Ubuntu 24.04 ARM64, не физической Repka OS.

### Проверка криптографической подписи и происхождения ARM64 bundle

Для обычной публикации в рабочую ветку CI выпускает GitHub Artifact Attestation
для точного файла `dockstation-repka-pi5-arm64.tar.gz`. Подпись формируется
Sigstore с краткоживущим сертификатом GitHub OIDC и связывает SHA-256 архива
с репозиторием, workflow и конкретным исходным commit.

Signing выполняется отдельной job только для push или ручного запуска на
`chore/flatten-repka-directories`; Pull Request не получает signing job.
CI проверяет аттестацию через GitHub API и повторно по локально скачанному JSONL
bundle. Отдельные negative checks должны отклонять изменённый архив, чужой
workflow и неверный source commit. Итоговый Actions artifact хранит архив,
его `.sha256` и `dockstation-repka-pi5-arm64.tar.gz.attestation.jsonl`.
Срок хранения — 30 дней; ссылка на artifact и запись attestation попадают в Summary.

**Проверка на Windows до передачи архива на Repka (online).** Установите
актуальный GitHub CLI (`gh`), распакуйте Actions artifact ZIP и выполните
в PowerShell из каталога с архивом:

```powershell
$archive = '.\dockstation-repka-pi5-arm64.tar.gz'
$manifest = tar -xOf $archive ./BUNDLE-MANIFEST.txt
$sourceCommit = ($manifest | Select-String '^Source commit: ').Line -replace '^Source commit:\s*', ''
if ($sourceCommit -notmatch '^[0-9a-f]{40}$') {
  throw 'Invalid or missing Source commit in bundle manifest.'
}
gh attestation verify $archive `
  --repo GooseRabbit/dockstation `
  --signer-workflow GooseRabbit/dockstation/.github/workflows/linux-cmake.yml `
  --source-ref refs/heads/chore/flatten-repka-directories `
  --source-digest $sourceCommit `
  --deny-self-hosted-runners
```

Проверка обращается к GitHub и подтверждает подпись, digest архива, ожидаемые
репозиторий и workflow, ветку и ровно тот commit, который записан в manifest.
При изменении байтов архива его digest перестанет совпадать с подписанным subject.

**Проверка без GitHub API (offline).** На доверенной машине с Интернетом
получите Sigstore trusted roots:

```powershell
gh attestation trusted-root > .\trusted_root.jsonl
```

Перенесите на изолированную машину GitHub CLI, архив, файл
`dockstation-repka-pi5-arm64.tar.gz.attestation.jsonl` и `trusted_root.jsonl`.
На изолированной машине заново задайте путь к архиву и извлеките commit
из его manifest:

```powershell
$archive = '.\dockstation-repka-pi5-arm64.tar.gz'
$manifest = tar -xOf $archive ./BUNDLE-MANIFEST.txt
$sourceCommit = ($manifest | Select-String '^Source commit: ').Line -replace '^Source commit:\s*', ''
if ($sourceCommit -notmatch '^[0-9a-f]{40}
  --repo GooseRabbit/dockstation `
  --bundle .\dockstation-repka-pi5-arm64.tar.gz.attestation.jsonl `
  --custom-trusted-root .\trusted_root.jsonl `
  --signer-workflow GooseRabbit/dockstation/.github/workflows/linux-cmake.yml `
  --source-ref refs/heads/chore/flatten-repka-directories `
  --source-digest $sourceCommit
```

Для offline режима дополнительно сверьте неизменяемый numeric repository ID.
На online-машине получите его командой `gh api repos/GooseRabbit/dockstation --jq .id`.
На offline-машине извлеките ID из проверенного сертификата:

```powershell
gh attestation verify $archive `
  --repo GooseRabbit/dockstation `
  --bundle .\dockstation-repka-pi5-arm64.tar.gz.attestation.jsonl `
  --custom-trusted-root .\trusted_root.jsonl `
  --signer-workflow GooseRabbit/dockstation/.github/workflows/linux-cmake.yml `
  --source-ref refs/heads/chore/flatten-repka-directories `
  --source-digest $sourceCommit `
  --format json `
  --jq '.[].verificationResult.signature.certificate.sourceRepositoryIdentifier'
```

Полученное число должно совпасть с online repository ID. Это дополнительная
проверка нужна потому, что локальная аттестация не может сама запросить текущее
состояние GitHub и проверить, не было ли имя репозитория переиспользовано.
Подробности: [offline verification](https://docs.github.com/en/actions/how-tos/secure-your-work/use-artifact-attestations/verify-attestations-offline).


**Фактическая проверка механизма (2026-11-15).** GitHub Actions run
[43746375584](https://github.com/GooseRabbit/dockstation/actions/runs/43746375584)
завершился `SUCCESS` на x64, ARM64 и отдельной signing job. Пройдены 23/23 CTest
на каждой архитектуре, 31 host test program, 141 Python-тест, Operator/QML/e2e,
shell syntax и полный Repka shell-test loop. ARM64 Release bundle прошёл manifest,
ABI, SONAME и installer staging проверки; пакет дважды установлен в staging без CMake.

Итоговый подписанный artifact (30 дней):
[dockstation-repka-pi5-arm64-6fb22bf79fd665ffed4ae50debd16b0105b86f71](https://github.com/GooseRabbit/dockstation/actions/runs/43746375584/artifacts/14174691384),
artifact ID `14174691384`. Состав — TAR.GZ, `.sha256` и detached Sigstore JSONL.
SHA-256 TAR.GZ: `11c5024b765bc3f8311ca031d46288038bbfc269fd7235ccd6fce91cf68550a5`.
GitHub attestation: [67847564](https://github.com/GooseRabbit/dockstation/attestations/67847564);
запись в публичном Rekor transparency log: [log index 3907782864](https://search.sigstore.dev?logIndex=3907782864).
CI проверил GitHub API и скачанный JSONL bundle; отрицательные проверки подмены
архива, signer workflow и source commit завершились ожидаемым отказом.

Эта проверка относится к source commit `6fb22bf79fd665ffed4ae50debd16b0105b86f71`
в рабочей ветке. Она не означает физическую приёмку на Repka OS и не изменяет `main`.

**Важно:** `repka/install_gateway.sh --bundle` не вызывает GitHub CLI и не
проверяет attestation автоматически. Криптографическую проверку следует
выполнить на доверенной управляющей машине до передачи архива на Repka.
Установщик продолжает проверять sidecar SHA-256, manifest, состав архива и
runtime dependencies; это не заменяет проверку происхождения.

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

## Риск жизненного цикла и сопровождения ОС

В официальном changelog Repka OS 5 Basic 1.0.6 указана база Ubuntu 25.10 и собственное
ядро `6.1.115.1-repka-pi5` (образ датирован 9 ноября 2026 года). Стандартный срок
поддержки Ubuntu 25.10 у Canonical закончился 9 июля 2026 года. Это не доказывает,
что Repka прекратила выпускать исправления: производитель может сопровождать свой
репозиторий и backport-ить security fixes через `repka-os-update`. Но срок, охват
и политика этих обновлений пока не подтверждены. До production нужно получить от
производителя подтверждение сопровождения kernel, glibc, OpenSSL, systemd и OpenSSH:
срок поддержки, источники security updates и процедура перехода на следующий образ.

Пока это не подтверждено, не выполнять `apt upgrade` или `do-release-upgrade` на
Repka. Использовать только рекомендованный производителем механизм обновления и
сохранять проверенный образ/резервную копию перед изменениями.

## Границы текущей проверки

Данные относятся к ОС и сетевому стеку виртуальной машины. Они не являются
телеметрией ESP32, датчиков, приводов или физической док-станции. Состояние
systemd unit не подтверждает успешный вход с Windows по SSH-ключу, а DNS-результат
не подтверждает доступность конкретного внешнего сервера.

Ubuntu VM x86_64 не является целевой Repka Pi 5: ARM64-сборка, Repka OS,
systemd-развёртывание на целевой плате, ESP32 и физические узлы требуют отдельной
проверки. Для инструкции по архитектуре и результатам ручных VM-испытаний
см. `docs/30_REPKA_NETWORK_IMPLEMENTATION_PLAN.md` и мастер-план `docs/31`.

GitHub Actions run 43736420005 завершился SUCCESS на x64 и ARM64. На каждой
архитектуре прошли 23/23 CTest; x64 также прошёл 31 host-программу и 141 Python-
тест. Нативный ARM64 job создал архив и установил его через реальный
`install_gateway.sh --bundle` в isolated staging; повторная установка сохранила
операторскую настройку. CI дополнительно закрепляет ABI symbol ceilings и
отклоняет bundle с требованием `GLIBCXX_3.4.33`. Это подтверждает сборку,
упаковку, checksum/ELF/ABI/ldd gates и installer copy path на Ubuntu 24.04 ARM64,
но не заменяет запуск на официальной Repka OS 5. Точный пакетный набор Repka,
systemd manager lifecycle на плате, UART/console/pinmux, электрические уровни и
обмен с ESP32 остаются открытыми.
) {
  throw 'Invalid or missing Source commit in bundle manifest.'
}
```

Затем выполните:

```powershell
gh attestation verify $archive `
  --repo GooseRabbit/dockstation `
  --bundle .\dockstation-repka-pi5-arm64.tar.gz.attestation.jsonl `
  --custom-trusted-root .\trusted_root.jsonl `
  --signer-workflow GooseRabbit/dockstation/.github/workflows/linux-cmake.yml `
  --source-ref refs/heads/chore/flatten-repka-directories `
  --source-digest $sourceCommit
```

Для offline режима дополнительно сверьте неизменяемый numeric repository ID.
На online-машине получите его командой `gh api repos/GooseRabbit/dockstation --jq .id`.
На offline-машине извлеките ID из проверенного сертификата:

```powershell
gh attestation verify $archive `
  --repo GooseRabbit/dockstation `
  --bundle .\dockstation-repka-pi5-arm64.tar.gz.attestation.jsonl `
  --custom-trusted-root .\trusted_root.jsonl `
  --signer-workflow GooseRabbit/dockstation/.github/workflows/linux-cmake.yml `
  --source-ref refs/heads/chore/flatten-repka-directories `
  --source-digest $sourceCommit `
  --format json `
  --jq '.[].verificationResult.signature.certificate.sourceRepositoryIdentifier'
```

Полученное число должно совпасть с online repository ID. Это дополнительная
проверка нужна потому, что локальная аттестация не может сама запросить текущее
состояние GitHub и проверить, не было ли имя репозитория переиспользовано.
Подробности: [offline verification](https://docs.github.com/en/actions/how-tos/secure-your-work/use-artifact-attestations/verify-attestations-offline).

**Важно:** `repka/install_gateway.sh --bundle` не вызывает GitHub CLI и не
проверяет attestation автоматически. Криптографическую проверку следует
выполнить на доверенной управляющей машине до передачи архива на Repka.
Установщик продолжает проверять sidecar SHA-256, manifest, состав архива и
runtime dependencies; это не заменяет проверку происхождения.

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

## Риск жизненного цикла и сопровождения ОС

В официальном changelog Repka OS 5 Basic 1.0.6 указана база Ubuntu 25.10 и собственное
ядро `6.1.115.1-repka-pi5` (образ датирован 9 ноября 2026 года). Стандартный срок
поддержки Ubuntu 25.10 у Canonical закончился 9 июля 2026 года. Это не доказывает,
что Repka прекратила выпускать исправления: производитель может сопровождать свой
репозиторий и backport-ить security fixes через `repka-os-update`. Но срок, охват
и политика этих обновлений пока не подтверждены. До production нужно получить от
производителя подтверждение сопровождения kernel, glibc, OpenSSL, systemd и OpenSSH:
срок поддержки, источники security updates и процедура перехода на следующий образ.

Пока это не подтверждено, не выполнять `apt upgrade` или `do-release-upgrade` на
Repka. Использовать только рекомендованный производителем механизм обновления и
сохранять проверенный образ/резервную копию перед изменениями.

## Границы текущей проверки

Данные относятся к ОС и сетевому стеку виртуальной машины. Они не являются
телеметрией ESP32, датчиков, приводов или физической док-станции. Состояние
systemd unit не подтверждает успешный вход с Windows по SSH-ключу, а DNS-результат
не подтверждает доступность конкретного внешнего сервера.

Ubuntu VM x86_64 не является целевой Repka Pi 5: ARM64-сборка, Repka OS,
systemd-развёртывание на целевой плате, ESP32 и физические узлы требуют отдельной
проверки. Для инструкции по архитектуре и результатам ручных VM-испытаний
см. `docs/30_REPKA_NETWORK_IMPLEMENTATION_PLAN.md` и мастер-план `docs/31`.

GitHub Actions run 43736420005 завершился SUCCESS на x64 и ARM64. На каждой
архитектуре прошли 23/23 CTest; x64 также прошёл 31 host-программу и 141 Python-
тест. Нативный ARM64 job создал архив и установил его через реальный
`install_gateway.sh --bundle` в isolated staging; повторная установка сохранила
операторскую настройку. CI дополнительно закрепляет ABI symbol ceilings и
отклоняет bundle с требованием `GLIBCXX_3.4.33`. Это подтверждает сборку,
упаковку, checksum/ELF/ABI/ldd gates и installer copy path на Ubuntu 24.04 ARM64,
но не заменяет запуск на официальной Repka OS 5. Точный пакетный набор Repka,
systemd manager lifecycle на плате, UART/console/pinmux, электрические уровни и
обмен с ESP32 остаются открытыми.
