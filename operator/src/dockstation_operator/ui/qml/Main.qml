import QtQuick
import QtQuick.Controls
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: root
    width: 1440
    height: 900
    minimumWidth: 1024
    minimumHeight: 700
    visible: true
    visibility: Window.Maximized
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
        { label: "Камера", symbol: "▣" },
        { label: "Система", symbol: "⌁" }
    ]

    function sectionTitle(text) {
        return text
    }

    component Panel: Rectangle {
        radius: 12
        color: root.surface
        border.color: root.borderColor
        border.width: 1
    }

    component PageScrollView: ScrollView {
        clip: true
        padding: 28
        contentWidth: availableWidth
        ScrollBar.vertical.policy: ScrollBar.AsNeeded
    }

    component CameraPreviewSurface: Rectangle {
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

    Window {
        id: cameraFullscreenWindow
        objectName: "camera.fullscreen-window"
        visible: false
        color: "#0B1117"
        title: "DockStation Operator — камера"
        CameraPreviewSurface {
            width: Math.min(parent.width, parent.height * 16 / 9)
            height: width * 9 / 16
            anchors.centerIn: parent
            radius: 0
            border.width: 0
        }
        Shortcut {
            sequence: "Escape"
            onActivated: cameraFullscreenWindow.hide()
        }
    }

    component ActionButton: Button {
        id: actionButton
        implicitHeight: 38
        contentItem: Text {
            text: actionButton.text
            color: actionButton.enabled ? root.primaryText : root.muted
            font.pixelSize: 12
            font.bold: actionButton.highlighted
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 6
            color: actionButton.highlighted ? "#23423F" : (actionButton.enabled ? root.elevated : root.surface)
            border.width: 1
            border.color: actionButton.highlighted ? root.accent : root.borderColor
            opacity: actionButton.enabled ? 1.0 : 0.65
        }
    }

    component DarkTextField: TextField {
        id: darkField
        implicitHeight: 38
        color: root.primaryText
        selectionColor: root.accent
        selectedTextColor: "#101820"
        verticalAlignment: TextInput.AlignVCenter
        background: Rectangle {
            radius: 6
            color: root.elevated
            border.width: 1
            border.color: darkField.activeFocus ? root.accent : root.borderColor
        }
    }

    component MetricGauge: Item {
        id: gauge
        property string caption: ""
        property string value: "—"
        property string unit: ""
        property string note: "Нет данных"
        property bool available: false
        property real progress: -1
        implicitWidth: 148
        implicitHeight: 176

        ColumnLayout {
            anchors.fill: parent
            spacing: 7
            Item {
                Layout.alignment: Qt.AlignHCenter
                width: 124
                height: 124
                Canvas {
                    id: dialCanvas
                    anchors.fill: parent
                    onPaint: {
                        var context = getContext("2d")
                        context.clearRect(0, 0, width, height)
                        var startAngle = Math.PI * 0.75
                        var endAngle = Math.PI * 2.25
                        context.lineWidth = 8
                        context.lineCap = "round"
                        context.strokeStyle = "#46535E"
                        context.beginPath()
                        context.arc(width / 2, height / 2, 50, startAngle, endAngle, false)
                        context.stroke()
                        if (gauge.available && gauge.progress >= 0) {
                            var boundedProgress = Math.min(1, Math.max(0, gauge.progress))
                            context.strokeStyle = root.accent
                            context.beginPath()
                            context.arc(width / 2, height / 2, 50, startAngle,
                                        startAngle + (endAngle - startAngle) * boundedProgress, false)
                            context.stroke()
                        }
                    }
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
        onAvailableChanged: dialCanvas.requestPaint()
        onProgressChanged: dialCanvas.requestPaint()
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
                    Text {
                        Layout.fillWidth: true
                        text: "ЛОКАЛЬНЫЕ ПРОФИЛИ"
                        color: root.muted
                        font.pixelSize: 10
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                    }
                    ScrollView {
                        objectName: "stations.local-profile-list"
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(180, stationProfileItems.implicitHeight)
                        clip: true
                        ScrollBar.vertical.policy: ScrollBar.AsNeeded
                        ColumnLayout {
                            id: stationProfileItems
                            width: parent.width
                            spacing: 3
                            Repeater {
                                model: bridge.stationProfiles
                                delegate: Button {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 44
                                    onClicked: bridge.selectStationProfile(modelData.profile_id)
                                    background: Rectangle {
                                        radius: 7
                                        color: modelData.selected ? "#23423F" : "transparent"
                                        border.color: modelData.selected ? root.accent : "transparent"
                                    }
                                    contentItem: RowLayout {
                                        spacing: 7
                                        Text {
                                            text: modelData.alias
                                            color: modelData.selected ? root.primaryText : root.secondaryText
                                            font.pixelSize: 11
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                        Rectangle {
                                            width: 8
                                            height: 8
                                            radius: 4
                                            color: modelData.status_color === "success" ? root.success
                                                 : modelData.status_color === "danger" ? root.danger : root.warning
                                        }
                                        Text {
                                            text: modelData.status_label
                                            color: modelData.status_color === "success" ? root.success
                                                 : modelData.status_color === "danger" ? root.danger : root.warning
                                            font.pixelSize: 9
                                        }
                                    }
                                }
                            }
                        }
                    }
                    ActionButton {
                        Layout.fillWidth: true
                        text: "+ Добавить профиль"
                        onClicked: bridge.addStationProfile("Новая станция")
                    }
                    Rectangle { Layout.fillWidth: true; height: 1; color: root.borderColor }
                    Repeater {
                        model: root.navigation
                        delegate: ActionButton {
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
                PageScrollView {
                    objectName: "page.connection"
                    ColumnLayout {
                        width: parent.width
                        spacing: 20

                        Text {
                            text: "Подключение к станции"
                            color: root.primaryText
                            font.pixelSize: 24
                            font.bold: true
                        }
                        Text {
                            text: "Лабораторный SSH-профиль. Локальные имена и подключения хранятся на этом компьютере; это ещё не каталог станций аккаунта."
                            color: root.secondaryText
                            font.pixelSize: 13
                            wrapMode: Text.WordWrap
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
                                    Label { text: "Имя профиля"; color: root.secondaryText }
                                    DarkTextField {
                                        objectName: "connection.station-alias-field"
                                        Layout.fillWidth: true
                                        text: bridge.stationAlias
                                        placeholderText: "Например, Дом"
                                        onTextEdited: bridge.setStationAlias(text)
                                    }
                                    Label { text: "Адрес Linux VM"; color: root.secondaryText }
                                    DarkTextField {
                                        Layout.fillWidth: true
                                        text: bridge.host
                                        placeholderText: "192.168.56.10"
                                        onTextEdited: bridge.setHost(text)
                                    }
                                    Label { text: "Linux-пользователь"; color: root.secondaryText }
                                    DarkTextField {
                                        Layout.fillWidth: true
                                        text: bridge.username
                                        placeholderText: "operator"
                                        onTextEdited: bridge.setUsername(text)
                                    }
                                    Label { text: "SSH-порт"; color: root.secondaryText }
                                    DarkTextField {
                                        objectName: "connection.ssh-port-field"
                                        Layout.fillWidth: true
                                        text: bridge.sshPort
                                        placeholderText: "22"
                                        placeholderTextColor: root.muted
                                        inputMethodHints: Qt.ImhDigitsOnly
                                        onTextEdited: bridge.setSshPort(text)
                                    }
                                    Label { text: "Локальный порт туннеля"; color: root.secondaryText }
                                    DarkTextField {
                                        objectName: "connection.local-port-field"
                                        Layout.fillWidth: true
                                        text: bridge.localPort
                                        placeholderText: "18080"
                                        placeholderTextColor: root.muted
                                        inputMethodHints: Qt.ImhDigitsOnly
                                        onTextEdited: bridge.setLocalPort(text)
                                    }
                                    Label { text: "Приватный SSH-ключ"; color: root.secondaryText }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        DarkTextField {
                                            Layout.fillWidth: true
                                            text: bridge.privateKeyPath
                                            placeholderText: "Путь к приватному ключу"
                                            onTextEdited: bridge.setPrivateKeyPath(text)
                                        }
                                        ActionButton { text: "Обзор…"; onClicked: privateKeyDialog.open() }
                                    }
                                    Label { text: "Файл known_hosts"; color: root.secondaryText }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        DarkTextField {
                                            Layout.fillWidth: true
                                            text: bridge.knownHostsPath
                                            placeholderText: "Путь к known_hosts"
                                            onTextEdited: bridge.setKnownHostsPath(text)
                                        }
                                        ActionButton { text: "Обзор…"; onClicked: knownHostsDialog.open() }
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    ActionButton {
                                        text: bridge.busy ? "Подключение…" : "Подключить"
                                        enabled: !bridge.busy && !bridge.connected
                                        highlighted: true
                                        onClicked: bridge.connectToStation()
                                    }
                                    ActionButton {
                                        text: "Отключить"
                                        enabled: !bridge.busy && bridge.connected
                                        onClicked: bridge.disconnectFromStation()
                                    }
                                    ActionButton {
                                        text: "Сохранить профиль"
                                        enabled: !bridge.busy
                                        onClicked: bridge.saveProfile()
                                    }
                                    ActionButton {
                                        text: "Удалить профиль"
                                        enabled: !bridge.busy && bridge.selectedProfileId.length > 0
                                        onClicked: bridge.removeSelectedStation()
                                    }
                                    Item { Layout.fillWidth: true }
                                }
                            }
                        }
                        Panel {
                            Layout.fillWidth: true
                            implicitHeight: statusInfo.implicitHeight + 40
                            ColumnLayout {
                                id: statusInfo
                                anchors.fill: parent
                                anchors.margins: 20
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
                PageScrollView {
                    objectName: "page.climate"
                    ColumnLayout {
                        width: parent.width
                        spacing: 20
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
                                        anchors.margins: 20
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
                PageScrollView {
                    objectName: "page.sensors"
                    ColumnLayout {
                        width: parent.width
                        spacing: 20
                        Text {
                            text: "Состояние датчиков"
                            color: root.primaryText
                            font.pixelSize: 24
                            font.bold: true
                        }
                        Panel {
                            Layout.fillWidth: true
                            implicitHeight: sensorList.implicitHeight + 40
                            ColumnLayout {
                                id: sensorList
                                anchors.fill: parent
                                anchors.margins: 20
                                spacing: 12
                                Text {
                                    text: "Подключение оборудования"
                                    color: root.primaryText
                                    font.pixelSize: 16
                                    font.bold: true
                                }
                                Text {
                                    text: "Для каждого устройства отображается подтверждённое состояние подключения."
                                    color: root.secondaryText
                                    wrapMode: Text.WordWrap
                                }
                                Repeater {
                                    model: [
                                        { name: "DHT22 №1", role: "Температура и влажность" },
                                        { name: "DHT22 №2", role: "Температура и влажность" },
                                        { name: "Концевик открытия крышки", role: "Концевик положения" },
                                        { name: "Концевик закрытия крышки", role: "Концевик положения" },
                                        { name: "USB-камера", role: "Видеоустройство" }
                                    ]
                                    delegate: RowLayout {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        spacing: 16
                                        Text {
                                            text: modelData.name
                                            color: root.primaryText
                                            font.pixelSize: 14
                                            Layout.preferredWidth: 270
                                        }
                                        Text {
                                            text: modelData.role
                                            color: root.secondaryText
                                            Layout.fillWidth: true
                                        }
                                        Rectangle {
                                            radius: 7
                                            color: "#303A43"
                                            border.color: root.borderColor
                                            implicitWidth: 132
                                            implicitHeight: 30
                                            Text {
                                                anchors.centerIn: parent
                                                text: "Неизвестно"
                                                color: root.muted
                                                font.pixelSize: 12
                                                font.bold: true
                                            }
                                        }
                                    }
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: "Состояния подключения (например, «Подключён» и «Отключён») будут показаны после публикации достоверных результатов диагностики. Сейчас API их не предоставляет."
                                    color: root.muted
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: 12
                                }
                            }
                        }
                        Panel {
                            objectName: "sensor.cover-status"
                            Layout.fillWidth: true
                            implicitHeight: coverStatus.implicitHeight + 40
                            ColumnLayout {
                                id: coverStatus
                                anchors.fill: parent
                                anchors.margins: 20
                                spacing: 12
                                Text {
                                    text: "Положение крышки"
                                    color: root.primaryText
                                    font.pixelSize: 16
                                    font.bold: true
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 16
                                    Text {
                                        text: "Текущее состояние"
                                        color: root.secondaryText
                                        font.pixelSize: 14
                                        Layout.fillWidth: true
                                    }
                                    Rectangle {
                                        radius: 7
                                        color: "#3A3325"
                                        border.color: root.warning
                                        implicitWidth: 160
                                        implicitHeight: 34
                                        Text {
                                            anchors.centerIn: parent
                                            text: "НЕИЗВЕСТНО"
                                            color: root.warning
                                            font.pixelSize: 12
                                            font.bold: true
                                        }
                                    }
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: "Возможные состояния: ОТКРЫТА · ЗАКРЫТА · НЕИЗВЕСТНО · В ПРОЦЕССЕ · ОШИБКА"
                                    color: root.secondaryText
                                    font.pixelSize: 13
                                    wrapMode: Text.WordWrap
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: "Текущий API не публикует сигналы концевиков и обратную связь привода. Поэтому положение крышки пока не подтверждено."
                                    color: root.muted
                                    font.pixelSize: 12
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }
                    }
                }

                // 4. Network
                PageScrollView {
                    objectName: "page.network"
                    ColumnLayout {
                        width: parent.width
                        spacing: 20
                        Text { text: "Сеть"; color: root.primaryText; font.pixelSize: 24; font.bold: true }
                        Text { text: bridge.freshnessText; color: root.secondaryText; wrapMode: Text.WordWrap }
                        Panel {
                            Layout.fillWidth: true
                            implicitHeight: interfacesColumn.implicitHeight + 40
                            ColumnLayout {
                                id: interfacesColumn
                                anchors.fill: parent
                                anchors.margins: 20
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
                            implicitHeight: routesColumn.implicitHeight + 40
                            ColumnLayout {
                                id: routesColumn
                                anchors.fill: parent
                                anchors.margins: 20
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
                            implicitHeight: networkStatus.implicitHeight + 40
                            ColumnLayout {
                                id: networkStatus
                                anchors.fill: parent
                                anchors.margins: 20
                                spacing: 8
                                Text { text: bridge.dnsSummary; color: root.primaryText; wrapMode: Text.WordWrap }
                                Text { text: bridge.sshSummary; color: root.primaryText; wrapMode: Text.WordWrap }
                                Text { text: bridge.platformSummary; color: root.secondaryText; wrapMode: Text.WordWrap }
                            }
                        }
                    }
                }

                // 5. Logs
                PageScrollView {
                    objectName: "page.logs"
                    ColumnLayout {
                        width: parent.width
                        spacing: 20
                        Text { text: "Логи"; color: root.primaryText; font.pixelSize: 24; font.bold: true }
                        Panel {
                            Layout.fillWidth: true
                            implicitHeight: clientLogs.implicitHeight + 40
                            ColumnLayout {
                                id: clientLogs
                                anchors.fill: parent
                                anchors.margins: 20
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
                            implicitHeight: recordings.implicitHeight + 40
                            ColumnLayout {
                                id: recordings
                                anchors.fill: parent
                                anchors.margins: 20
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

                // 6. Camera preview placeholder. The viewport is fixed at 960x540.
                PageScrollView {
                    objectName: "page.camera"
                    ColumnLayout {
                        width: parent.width
                        spacing: 20
                        Text { text: "Камера"; color: root.primaryText; font.pixelSize: 24; font.bold: true }
                        Panel {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: 996
                            Layout.maximumWidth: 996
                            implicitHeight: cameraPanelContent.implicitHeight + 36
                            ColumnLayout {
                                id: cameraPanelContent
                                anchors.fill: parent
                                anchors.margins: 18
                                spacing: 12
                                CameraPreviewSurface {
                                    objectName: "camera.preview"
                                    Layout.alignment: Qt.AlignHCenter
                                    Layout.preferredWidth: 960
                                    Layout.minimumWidth: 960
                                    Layout.maximumWidth: 960
                                    Layout.preferredHeight: 540
                                    Layout.minimumHeight: 540
                                    Layout.maximumHeight: 540
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text { text: "Live View: недоступно"; color: root.muted }
                                    Item { Layout.fillWidth: true }
                                    ActionButton { text: "На весь экран"; onClicked: cameraFullscreenWindow.showFullScreen() }
                                    ActionButton { text: "Начать запись"; enabled: false }
                                    ActionButton { text: "Остановить"; enabled: false }
                                }
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: "Область предпросмотра — 960×540 (16:9). При подключении камеры изображение будет центрироваться на чёрном фоне без растяжения и обрезки; исходное соотношение сторон сохраняется."
                            color: root.muted
                            wrapMode: Text.WordWrap
                            font.pixelSize: 11
                        }
                    }
                }

                // 7. Linux host system monitoring. Unsupported API fields stay unavailable.
                PageScrollView {
                    objectName: "page.system"
                    ColumnLayout {
                        width: parent.width
                        spacing: 20
                        Text { text: "Система"; color: root.primaryText; font.pixelSize: 24; font.bold: true }
                        Text {
                            text: "Мониторинг бортового компьютера Linux Gateway — отдельно от климатических датчиков станции."
                            color: root.secondaryText
                            wrapMode: Text.WordWrap
                        }
                        GridLayout {
                            Layout.fillWidth: true
                            columns: width >= 900 ? 4 : 2
                            rowSpacing: 16
                            columnSpacing: 16
                            Panel {
                                Layout.fillWidth: true
                                implicitHeight: 190
                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 14
                                    spacing: 8
                                    Text { text: "Температура CPU / SoC"; color: root.primaryText; font.pixelSize: 14; font.bold: true; wrapMode: Text.WordWrap }
                                    Text { text: "— °C"; color: root.muted; font.pixelSize: 30; font.bold: true }
                                    Text { text: "Gateway API пока не публикует показатель"; color: root.muted; font.pixelSize: 11; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                                }
                            }
                            Panel {
                                Layout.fillWidth: true
                                implicitHeight: 190
                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 14
                                    spacing: 8
                                    Text { text: "Оперативная память"; color: root.primaryText; font.pixelSize: 14; font.bold: true }
                                    Text { text: "— %"; color: root.muted; font.pixelSize: 30; font.bold: true }
                                    Text { text: "Использовано / доступно — пока не передаётся API"; color: root.muted; font.pixelSize: 11; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                                }
                            }
                            Panel {
                                Layout.fillWidth: true
                                implicitHeight: 190
                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 14
                                    spacing: 8
                                    Text { text: "Корневая файловая система"; color: root.primaryText; font.pixelSize: 14; font.bold: true; wrapMode: Text.WordWrap }
                                    Text { text: "— %"; color: root.muted; font.pixelSize: 30; font.bold: true }
                                    Text { text: "Заполнение диска / пока не передаётся API"; color: root.muted; font.pixelSize: 11; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                                }
                            }
                            Panel {
                                Layout.fillWidth: true
                                implicitHeight: 190
                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 14
                                    spacing: 8
                                    Text { text: "Средняя системная нагрузка"; color: root.primaryText; font.pixelSize: 14; font.bold: true; wrapMode: Text.WordWrap }
                                    Text { text: "— / — / —"; color: root.muted; font.pixelSize: 25; font.bold: true }
                                    Text { text: "Load average за 1 / 5 / 15 минут; это не процент загрузки CPU"; color: root.muted; font.pixelSize: 11; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                                }
                            }
                        }
                        Panel {
                            Layout.fillWidth: true
                            implicitHeight: systemInfo.implicitHeight + 40
                            ColumnLayout {
                                id: systemInfo
                                anchors.fill: parent
                                anchors.margins: 20
                                spacing: 10
                                Text { text: "Сведения о Linux Gateway"; color: root.primaryText; font.pixelSize: 16; font.bold: true }
                                Text { text: bridge.platformSummary; color: root.secondaryText; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                                Text {
                                    text: "Время работы, ОС, версия ядра и архитектура берутся из текущего диагностического снимка. Температура CPU/SoC, RAM, файловая система и load average пока не входят в ответ Python Gateway API. Их сборщики в Linux C++-части существуют, но ещё не подключены к этому API."
                                    color: root.muted
                                    wrapMode: Text.WordWrap
                                    Layout.fillWidth: true
                                }
                            }
                        }
                        Text { text: bridge.freshnessText; color: root.secondaryText; wrapMode: Text.WordWrap }
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
