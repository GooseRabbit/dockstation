import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: root
    width: 1440
    height: 900
    minimumWidth: 1024
    minimumHeight: 700
    visible: true
    title: "DockStation Operator"
    color: "#101820"

    property int currentTab: 0
    property color surface: "#18232D"
    property color elevated: "#202D38"
    property color borderColor: "#31414E"
    property color primaryText: "#E8EEF3"
    property color secondaryText: "#A8B5C0"
    property color accent: "#58B8A5"
    property color muted: "#8796A4"
    property color success: "#46A982"
    property color warning: "#D6A34A"
    property color danger: "#D96570"

    readonly property var navigation: [
        { label: "Подключение", symbol: "⇄" },
        { label: "Климат", symbol: "◉" },
        { label: "Датчики", symbol: "⌁" },
        { label: "Сеть", symbol: "⌘" },
        { label: "Логи", symbol: "≡" },
        { label: "Камера", symbol: "▣" }
    ]

    function sectionTitle(text) {
        return text
    }

    component Panel: Rectangle {
        radius: 10
        color: root.surface
        border.color: root.borderColor
        border.width: 1
    }

    component MetricGauge: Item {
        id: gauge
        property string caption: ""
        property string value: "—"
        property string unit: ""
        property string note: "Нет данных"
        property bool available: false
        implicitWidth: 148
        implicitHeight: 176

        ColumnLayout {
            anchors.fill: parent
            spacing: 7
            Item {
                Layout.alignment: Qt.AlignHCenter
                width: 124
                height: 124
                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color: "transparent"
                    border.width: 8
                    border.color: gauge.available ? root.accent : "#46535E"
                }
                Column {
                    anchors.centerIn: parent
                    spacing: 1
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: gauge.value
                        color: gauge.available ? root.primaryText : root.muted
                        font.pixelSize: 27
                        font.bold: true
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: gauge.unit
                        color: root.secondaryText
                        font.pixelSize: 12
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                text: gauge.caption
                color: root.primaryText
                font.pixelSize: 13
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                Layout.fillWidth: true
                text: gauge.note
                color: root.muted
                font.pixelSize: 11
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
        }
    }

    FileDialog {
        id: privateKeyDialog
        title: "Выбери приватный SSH-ключ"
        fileMode: FileDialog.OpenFile
        onAccepted: bridge.setPrivateKeyPath(selectedFile.toLocalFile())
    }

    FileDialog {
        id: knownHostsDialog
        title: "Выбери файл known_hosts"
        fileMode: FileDialog.OpenFile
        onAccepted: bridge.setKnownHostsPath(selectedFile.toLocalFile())
    }

    Timer {
        interval: 5000
        running: true
        repeat: true
        onTriggered: {
            bridge.poll()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 66
            color: "#151F28"
            border.color: root.borderColor
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 22
                anchors.rightMargin: 22
                spacing: 16
                Text {
                    text: "DockStation"
                    color: root.primaryText
                    font.pixelSize: 21
                    font.bold: true
                }
                Rectangle {
                    width: 1
                    height: 28
                    color: root.borderColor
                }
                Text {
                    text: "OPERATOR"
                    color: root.accent
                    font.pixelSize: 12
                    font.bold: true
                    font.letterSpacing: 1.4
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: bridge.apiStatus
                    color: bridge.connected ? root.success : root.muted
                    font.pixelSize: 12
                }
                Rectangle {
                    radius: 6
                    color: bridge.connected ? "#1D3A35" : "#303A43"
                    border.color: bridge.connected ? root.success : root.borderColor
                    implicitWidth: statusText.implicitWidth + 22
                    implicitHeight: 30
                    Text {
                        id: statusText
                        anchors.centerIn: parent
                        text: bridge.connectionState
                        color: root.primaryText
                        font.pixelSize: 11
                        font.bold: true
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Rectangle {
                Layout.preferredWidth: 218
                Layout.fillHeight: true
                color: "#131D26"
                border.color: root.borderColor
                ColumnLayout {
                    anchors.fill: parent
                    anchors.topMargin: 18
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 6
                    Repeater {
                        model: root.navigation
                        delegate: Button {
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            Layout.preferredHeight: 48
                            flat: true
                            highlighted: root.currentTab === index
                            background: Rectangle {
                                radius: 7
                                color: root.currentTab === index ? "#23423F" : "transparent"
                                border.color: root.currentTab === index ? root.accent : "transparent"
                            }
                            contentItem: RowLayout {
                                spacing: 12
                                Text {
                                    text: modelData.symbol
                                    color: root.currentTab === index ? root.accent : root.secondaryText
                                    font.pixelSize: 19
                                    Layout.preferredWidth: 24
                                    horizontalAlignment: Text.AlignHCenter
                                }
                                Text {
                                    text: modelData.label
                                    color: root.currentTab === index ? root.primaryText : root.secondaryText
                                    font.pixelSize: 13
                                    font.bold: root.currentTab === index
                                    Layout.fillWidth: true
                                }
                            }
                            onClicked: root.currentTab = index
                        }
                    }
                    Item { Layout.fillHeight: true }
                    Text {
                        Layout.fillWidth: true
                        text: "ЛАБОРАТОРНЫЙ ПРОТОТИП\nТолько чтение · без команд механизмам"
                        color: root.muted
                        font.pixelSize: 10
                        lineHeight: 1.35
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                    }
                    Item { Layout.preferredHeight: 10 }
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: root.currentTab

                // 1. Connection
                ScrollView {
                    clip: true
                    ColumnLayout {
                        width: parent.width
                        spacing: 16
                        anchors.margins: 24

                        Text {
                            text: "Подключение к станции"
                            color: root.primaryText
                            font.pixelSize: 24
                            font.bold: true
                        }
                        Text {
                            text: "Лабораторное SSH-подключение к Linux Gateway"
                            color: root.secondaryText
                            font.pixelSize: 13
                        }
                        Panel {
                            Layout.fillWidth: true
                            implicitHeight: connectionForm.implicitHeight + 36
                            ColumnLayout {
                                id: connectionForm
                                anchors.fill: parent
                                anchors.margins: 18
                                spacing: 12
                                GridLayout {
                                    Layout.fillWidth: true
                                    columns: 2
                                    columnSpacing: 16
                                    rowSpacing: 10
                                    Label { text: "Адрес Linux VM"; color: root.secondaryText }
                                    TextField {
                                        Layout.fillWidth: true
                                        text: bridge.host
                                        placeholderText: "192.168.56.10"
                                        onTextEdited: bridge.setHost(text)
                                    }
                                    Label { text: "Linux-пользователь"; color: root.secondaryText }
                                    TextField {
                                        Layout.fillWidth: true
                                        text: bridge.username
                                        placeholderText: "operator"
                                        onTextEdited: bridge.setUsername(text)
                                    }
                                    Label { text: "SSH-порт"; color: root.secondaryText }
                                    TextField {
                                        Layout.fillWidth: true
                                        text: bridge.sshPort
                                        onTextEdited: bridge.setSshPort(text)
                                    }
                                    Label { text: "Локальный порт туннеля"; color: root.secondaryText }
                                    TextField {
                                        Layout.fillWidth: true
                                        text: bridge.localPort
                                        onTextEdited: bridge.setLocalPort(text)
                                    }
                                    Label { text: "Приватный SSH-ключ"; color: root.secondaryText }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        TextField {
                                            Layout.fillWidth: true
                                            text: bridge.privateKeyPath
                                            placeholderText: "Путь к приватному ключу"
                                            onTextEdited: bridge.setPrivateKeyPath(text)
                                        }
                                        Button { text: "Обзор…"; onClicked: privateKeyDialog.open() }
                                    }
                                    Label { text: "Файл known_hosts"; color: root.secondaryText }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        TextField {
                                            Layout.fillWidth: true
                                            text: bridge.knownHostsPath
                                            placeholderText: "Путь к known_hosts"
                                            onTextEdited: bridge.setKnownHostsPath(text)
                                        }
                                        Button { text: "Обзор…"; onClicked: knownHostsDialog.open() }
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Button {
                                        text: bridge.busy ? "Подключение…" : "Подключить"
                                        enabled: !bridge.busy && !bridge.connected
                                        highlighted: true
                                        onClicked: bridge.connectToStation()
                                    }
                                    Button {
                                        text: "Отключить"
                                        enabled: !bridge.busy && bridge.connected
                                        onClicked: bridge.disconnectFromStation()
                                    }
                                    Button {
                                        text: "Сохранить профиль"
                                        enabled: !bridge.busy
                                        onClicked: bridge.saveProfile()
                                    }
                                    Item { Layout.fillWidth: true }
                                }
                            }
                        }
                        Panel {
                            Layout.fillWidth: true
                            implicitHeight: statusInfo.implicitHeight + 32
                            ColumnLayout {
                                id: statusInfo
                                anchors.fill: parent
                                anchors.margins: 16
                                spacing: 8
                                Text { text: "Состояние соединения"; color: root.primaryText; font.bold: true; font.pixelSize: 15 }
                                Text { text: bridge.connectionState; color: bridge.connected ? root.success : root.warning; font.pixelSize: 14 }
                                Text { text: bridge.noticeText; color: root.secondaryText; wrapMode: Text.WordWrap }
                                Text { text: bridge.freshnessText; color: root.secondaryText; wrapMode: Text.WordWrap }
                                Text { text: bridge.lastUpdateText; color: root.muted; font.pixelSize: 11 }
                                Text {
                                    Layout.fillWidth: true
                                    text: "SSH-туннель, ответ API и актуальность диагностических данных — разные признаки. Проверка host key обязательна; парольный fallback отключён."
                                    color: root.muted
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: 11
                                }
                            }
                        }
                    }
                }

                // 2. Climate
                ScrollView {
                    clip: true
                    ColumnLayout {
                        width: parent.width
                        anchors.margins: 24
                        spacing: 16
                        Text { text: "Климат"; color: root.primaryText; font.pixelSize: 24; font.bold: true }
                        Text {
                            text: "Два независимых датчика DHT22 · температура и относительная влажность"
                            color: root.secondaryText
                        }
                        GridLayout {
                            Layout.fillWidth: true
                            columns: width > 900 ? 2 : 1
                            columnSpacing: 16
                            rowSpacing: 16
                            Repeater {
                                model: ["Датчик №1 · DHT22", "Датчик №2 · DHT22"]
                                delegate: Panel {
                                    required property string modelData
                                    Layout.fillWidth: true
                                    implicitHeight: 254
                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 16
                                        spacing: 8
                                        Text { text: modelData; color: root.primaryText; font.pixelSize: 16; font.bold: true }
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.alignment: Qt.AlignHCenter
                                            MetricGauge { caption: "Температура"; value: "—"; unit: "°C"; note: "Нет данных от API"; available: false }
                                            MetricGauge { caption: "Влажность"; value: "—"; unit: "%"; note: "Нет данных от API"; available: false }
                                        }
                                    }
                                }
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: "Текущий Gateway API не публикует показания физических датчиков. Значения не подменяются демонстрационными данными."
                            color: root.muted
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                // 3. Sensors and cover
                ScrollView {
                    clip: true
                    ColumnLayout {
                        width: parent.width
                        anchors.margins: 24
                        spacing: 16
                        Text { text: "Состояние датчиков"; color: root.primaryText; font.pixelSize: 24; font.bold: true }
                        Panel {
                            Layout.fillWidth: true
                            implicitHeight: coverStatus.implicitHeight + 32
                            ColumnLayout {
                                id: coverStatus
                                anchors.fill: parent
                                anchors.margins: 16
                                spacing: 8
                                Text { text: "Крышка станции"; color: root.primaryText; font.pixelSize: 16; font.bold: true }
                                Text { text: "Положение: НЕИЗВЕСТНО"; color: root.warning; font.pixelSize: 20; font.bold: true }
                                Text {
                                    text: "Текущий API не публикует сигналы концевиков. Положение крышки не подтверждено."
                                    color: root.muted
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }
                        Panel {
                            Layout.fillWidth: true
                            implicitHeight: sensorList.implicitHeight + 32
                            ColumnLayout {
                                id: sensorList
                                anchors.fill: parent
                                anchors.margins: 16
                                spacing: 10
                                Text { text: "Ожидаемое оборудование прототипа"; color: root.primaryText; font.pixelSize: 16; font.bold: true }
                                Repeater {
                                    model: [
                                        { name: "DHT22 №1", role: "Температура и влажность" },
                                        { name: "DHT22 №2", role: "Температура и влажность" },
                                        { name: "Концевик открытия крышки", role: "Положение крышки" },
                                        { name: "Концевик закрытия крышки", role: "Положение крышки" },
                                        { name: "USB-камера", role: "Видеопоток" }
                                    ]
                                    delegate: RowLayout {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        Text { text: modelData.name; color: root.primaryText; Layout.fillWidth: true }
                                        Text { text: modelData.role; color: root.secondaryText }
                                        Rectangle {
                                            radius: 5
                                            color: "#303A43"
                                            implicitWidth: 112
                                            implicitHeight: 25
                                            Text {
                                                anchors.centerIn: parent
                                                text: "Нет данных"
                                                color: root.muted
                                                font.pixelSize: 11
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        Text {
                            text: "«Нет данных» не означает «не обнаружен»: текущий API пока не позволяет достоверно установить состояние этих устройств."
                            color: root.muted
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                // 4. Network
                ScrollView {
                    clip: true
                    ColumnLayout {
                        width: parent.width
                        anchors.margins: 24
                        spacing: 16
                        Text { text: "Сеть"; color: root.primaryText; font.pixelSize: 24; font.bold: true }
                        Text { text: bridge.freshnessText; color: root.secondaryText; wrapMode: Text.WordWrap }
                        Panel {
                            Layout.fillWidth: true
                            implicitHeight: interfacesColumn.implicitHeight + 32
                            ColumnLayout {
                                id: interfacesColumn
                                anchors.fill: parent
                                anchors.margins: 16
                                spacing: 10
                                Text { text: "Сетевые интерфейсы"; color: root.primaryText; font.pixelSize: 16; font.bold: true }
                                Repeater {
                                    model: bridge.networkInterfaces
                                    delegate: RowLayout {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        Text { text: modelData.name; color: root.primaryText; Layout.preferredWidth: 90 }
                                        Text { text: modelData.kind + " · " + modelData.state; color: root.secondaryText; Layout.preferredWidth: 150 }
                                        Text { text: modelData.addresses; color: root.primaryText; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
                                        Text { text: modelData.status; color: root.secondaryText }
                                    }
                                }
                                Text {
                                    visible: bridge.networkInterfaces.length === 0
                                    text: "Нет опубликованных данных об интерфейсах."
                                    color: root.muted
                                }
                            }
                        }
                        Panel {
                            Layout.fillWidth: true
                            implicitHeight: routesColumn.implicitHeight + 32
                            ColumnLayout {
                                id: routesColumn
                                anchors.fill: parent
                                anchors.margins: 16
                                spacing: 10
                                Text { text: "Маршруты по умолчанию"; color: root.primaryText; font.pixelSize: 16; font.bold: true }
                                Repeater {
                                    model: bridge.defaultRoutes
                                    delegate: RowLayout {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        Text { text: modelData.family; color: root.primaryText; Layout.preferredWidth: 70 }
                                        Text { text: modelData.gateway; color: root.secondaryText; Layout.preferredWidth: 170 }
                                        Text { text: modelData.interface; color: root.primaryText; Layout.fillWidth: true }
                                        Text { text: modelData.metric; color: root.secondaryText }
                                    }
                                }
                                Text { visible: bridge.defaultRoutes.length === 0; text: "Нет опубликованных маршрутов."; color: root.muted }
                            }
                        }
                        Panel {
                            Layout.fillWidth: true
                            implicitHeight: networkStatus.implicitHeight + 32
                            ColumnLayout {
                                id: networkStatus
                                anchors.fill: parent
                                anchors.margins: 16
                                spacing: 8
                                Text { text: bridge.dnsSummary; color: root.primaryText; wrapMode: Text.WordWrap }
                                Text { text: bridge.sshSummary; color: root.primaryText; wrapMode: Text.WordWrap }
                                Text { text: bridge.platformSummary; color: root.secondaryText; wrapMode: Text.WordWrap }
                            }
                        }
                    }
                }

                // 5. Logs
                ScrollView {
                    clip: true
                    ColumnLayout {
                        width: parent.width
                        anchors.margins: 24
                        spacing: 16
                        Text { text: "Логи"; color: root.primaryText; font.pixelSize: 24; font.bold: true }
                        Panel {
                            Layout.fillWidth: true
                            implicitHeight: clientLogs.implicitHeight + 32
                            ColumnLayout {
                                id: clientLogs
                                anchors.fill: parent
                                anchors.margins: 16
                                spacing: 10
                                Text { text: "События Operator"; color: root.primaryText; font.pixelSize: 16; font.bold: true }
                                Repeater {
                                    model: bridge.events
                                    delegate: Text {
                                        required property string modelData
                                        Layout.fillWidth: true
                                        text: modelData
                                        color: root.secondaryText
                                        font.family: "Consolas"
                                        font.pixelSize: 11
                                        wrapMode: Text.WrapAnywhere
                                    }
                                }
                                Text {
                                    visible: bridge.events.length === 0
                                    text: "Событий пока нет."
                                    color: root.muted
                                }
                            }
                        }
                        Panel {
                            Layout.fillWidth: true
                            implicitHeight: recordings.implicitHeight + 32
                            ColumnLayout {
                                id: recordings
                                anchors.fill: parent
                                anchors.margins: 16
                                spacing: 8
                                Text { text: "Записи камеры"; color: root.primaryText; font.pixelSize: 16; font.bold: true }
                                Text {
                                    text: "Просмотр сохранённых видеозаписей будет добавлен отдельным подразделом после появления API каталога файлов и механизма чтения записей."
                                    color: root.muted
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }
                    }
                }

                // 6. Camera placeholder
                ScrollView {
                    clip: true
                    ColumnLayout {
                        width: parent.width
                        anchors.margins: 24
                        spacing: 16
                        Text { text: "Камера"; color: root.primaryText; font.pixelSize: 24; font.bold: true }
                        Panel {
                            Layout.fillWidth: true
                            implicitHeight: 430
                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 18
                                spacing: 12
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    color: "#0B1117"
                                    radius: 8
                                    border.color: root.borderColor
                                    Column {
                                        anchors.centerIn: parent
                                        spacing: 10
                                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: "▣"; color: root.muted; font.pixelSize: 48 }
                                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Видеопоток пока недоступен"; color: root.primaryText; font.pixelSize: 17; font.bold: true }
                                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: "USB-камера и медиатранспорт ещё не подключены"; color: root.muted; font.pixelSize: 12 }
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text { text: "Live View: недоступно"; color: root.muted }
                                    Item { Layout.fillWidth: true }
                                    Button { text: "Начать запись"; enabled: false }
                                    Button { text: "Остановить"; enabled: false }
                                }
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            color: "#151F28"
            border.color: root.borderColor
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                Text { text: bridge.freshnessText; color: root.muted; font.pixelSize: 10; Layout.fillWidth: true; elide: Text.ElideRight }
                Text { text: bridge.lastUpdateText; color: root.muted; font.pixelSize: 10 }
            }
        }
    }
}
