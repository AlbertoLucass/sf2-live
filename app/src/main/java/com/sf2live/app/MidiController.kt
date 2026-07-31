package com.sf2live.app

import android.content.Context
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbManager
import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiManager
import android.media.midi.MidiOutputPort
import android.media.midi.MidiReceiver
import android.os.Build
import android.os.Handler
import android.os.Looper
import java.io.IOException
import java.util.Locale
import java.util.concurrent.Executor
import java.util.concurrent.atomic.AtomicLong

/**
 * Recebe MIDI de dispositivos físicos e entrega os eventos ao sintetizador nativo.
 *
 * A versão anterior abria automaticamente o primeiro dispositivo e a porta 0. Isso
 * funcionava em configurações simples, mas podia escolher a porta errada quando um
 * hub USB continha teclado, interface de áudio e outros dispositivos ao mesmo tempo.
 */
class MidiController(
    context: Context,
    private val listener: Listener,
) {
    data class PortOption(
        val number: Int,
        val name: String,
    ) {
        val label: String
            get() = if (name.isBlank()) "Porta ${number + 1}" else "${number + 1} · $name"
    }

    data class DeviceOption(
        val id: Int,
        val name: String,
        val manufacturer: String,
        val product: String,
        val type: Int,
        val ports: List<PortOption>,
    ) {
        val stableKey: String
            get() = listOf(manufacturer, product, name, type.toString())
                .joinToString("|")
                .lowercase(Locale.ROOT)

        val typeLabel: String
            get() = when (type) {
                MidiDeviceInfo.TYPE_USB -> "USB"
                MidiDeviceInfo.TYPE_BLUETOOTH -> "Bluetooth"
                MidiDeviceInfo.TYPE_VIRTUAL -> "Virtual"
                else -> "MIDI"
            }

        val displayLabel: String
            get() = buildString {
                append(name)
                append(" · ")
                append(typeLabel)
                append(" · ")
                append(ports.size)
                append(if (ports.size == 1) " porta" else " portas")
            }
    }

    data class UsbDiagnostic(
        val label: String,
        val vendorId: Int,
        val productId: Int,
        val hasMidiInterface: Boolean,
        val hasAudioInterface: Boolean,
        val descriptorSummary: String,
    )

    data class Activity(
        val description: String,
        val rawHex: String,
        val note: Int? = null,
        val velocity: Int? = null,
        val channel: Int? = null,
    )

    interface Listener {
        fun onMidiDevicesChanged(devices: List<DeviceOption>, connectedDeviceId: Int?, connectedPort: Int?)
        fun onUsbDiagnosticsChanged(devices: List<UsbDiagnostic>)
        fun onMidiStatus(text: String, connected: Boolean)
        fun onMidiActivity(activity: Activity)
    }

    private val appContext = context.applicationContext
    private val midiManager = appContext.getSystemService(MidiManager::class.java)
    private val usbManager = appContext.getSystemService(UsbManager::class.java)
    private val mainHandler = Handler(Looper.getMainLooper())
    private val mainExecutor = Executor { command -> mainHandler.post(command) }

    private var knownDevices: List<DeviceOption> = emptyList()
    private var openedDevice: MidiDevice? = null
    private var outputPort: MidiOutputPort? = null
    private var connectedDeviceId: Int? = null
    private var connectedPortNumber: Int? = null
    private var usingNativeMidi = false
    private var connectionGeneration = 0L
    private val parser = MidiByteParser(::handleMessage)
    private val lastUiUpdate = AtomicLong(0L)

    private val receiver = object : MidiReceiver() {
        override fun onSend(data: ByteArray, offset: Int, count: Int, timestamp: Long) {
            parser.consume(data, offset, count)
        }
    }

    private val callback = object : MidiManager.DeviceCallback() {
        override fun onDeviceAdded(info: MidiDeviceInfo) {
            // O Android pode levar alguns milissegundos para publicar todas as portas.
            // Ao aparecer uma nova entrada física, seleciona-a automaticamente. Um
            // teclado conhecido (HA-500) também pode substituir uma interface MIDI
            // genérica já aberta, mas um dispositivo de menor prioridade não derruba
            // um teclado melhor que já esteja funcionando.
            mainHandler.postDelayed({
                knownDevices = availableDevices()
                listener.onMidiDevicesChanged(knownDevices, connectedDeviceId, connectedPortNumber)
                listener.onUsbDiagnosticsChanged(readUsbDiagnostics())

                val added = knownDevices.firstOrNull { it.id == info.id } ?: return@postDelayed
                val current = knownDevices.firstOrNull { it.id == connectedDeviceId }
                if (current == null || devicePriority(added) <= devicePriority(current)) {
                    val port = added.ports.firstOrNull()?.number ?: return@postDelayed
                    connect(added.id, port)
                }
            }, 180)
        }

        override fun onDeviceRemoved(info: MidiDeviceInfo) {
            if (connectedDeviceId == info.id) {
                closeCurrent(panic = true)
                listener.onMidiStatus("Teclado MIDI desconectado", false)
            }
            mainHandler.postDelayed({ refreshDevices(reconnectIfNeeded = true) }, 300)
        }
    }

    fun start() {
        val manager = midiManager
        if (manager == null) {
            listener.onMidiStatus("Este Android não disponibilizou o serviço MIDI", false)
            return
        }
        if (Build.VERSION.SDK_INT >= 33) {
            manager.registerDeviceCallback(
                MidiManager.TRANSPORT_MIDI_BYTE_STREAM,
                mainExecutor,
                callback,
            )
        } else {
            @Suppress("DEPRECATION")
            manager.registerDeviceCallback(callback, mainHandler)
        }
        refreshDevices(reconnectIfNeeded = true)
    }

    fun stop() {
        try {
            midiManager?.unregisterDeviceCallback(callback)
        } catch (_: Exception) {
        }
        closeCurrent(panic = true)
    }

    fun refreshDevices(reconnectIfNeeded: Boolean = false) {
        knownDevices = availableDevices()
        listener.onMidiDevicesChanged(knownDevices, connectedDeviceId, connectedPortNumber)
        listener.onUsbDiagnosticsChanged(readUsbDiagnostics())

        if (knownDevices.isEmpty()) {
            if (connectedDeviceId == null) {
                listener.onMidiStatus("Nenhum dispositivo MIDI encontrado", false)
            }
            return
        }

        if (reconnectIfNeeded && connectedDeviceId == null) {
            // Prioriza USB físico. A escolha final continua disponível na interface.
            val candidate = knownDevices.minByOrNull(::devicePriority) ?: return
            connect(candidate.id, candidate.ports.firstOrNull()?.number ?: 0)
        }
    }

    fun connect(deviceId: Int, portNumber: Int) {
        val option = knownDevices.firstOrNull { it.id == deviceId }
        if (option == null) {
            listener.onMidiStatus("O dispositivo MIDI selecionado não está mais disponível", false)
            refreshDevices(reconnectIfNeeded = false)
            return
        }
        if (option.ports.none { it.number == portNumber }) {
            listener.onMidiStatus("A porta MIDI selecionada não está disponível", false)
            return
        }
        if (connectedDeviceId == deviceId && connectedPortNumber == portNumber && (usingNativeMidi || outputPort != null)) {
            val path = if (usingNativeMidi) "AMidi nativo" else "MIDI Android"
            listener.onMidiStatus("MIDI conectado: ${option.name} · porta ${portNumber + 1} · $path", true)
            return
        }

        val manager = midiManager ?: return
        closeCurrent(panic = true)
        val generation = ++connectionGeneration
        listener.onMidiStatus("Abrindo ${option.name} · porta ${portNumber + 1}…", false)

        val info = availableDeviceInfos().firstOrNull { it.id == deviceId }
        if (info == null) {
            listener.onMidiStatus("Não foi possível localizar ${option.name}", false)
            return
        }

        manager.openDevice(info, { device ->
            if (generation != connectionGeneration) {
                try {
                    device?.close()
                } catch (_: IOException) {
                }
                return@openDevice
            }
            if (device == null) {
                listener.onMidiStatus("O Android não conseguiu abrir ${option.name}", false)
                return@openDevice
            }

            // Caminho preferencial: o MidiDevice é entregue uma única vez ao C++.
            // A partir daí, AMidi é consultado diretamente dentro do callback nativo Oboe/AAudio,
            // removendo MidiReceiver/Kotlin e uma chamada JNI por nota.
            val nativeConnected = runCatching {
                NativeSynth.nativeConnectMidi(device, portNumber)
            }.getOrDefault(false)

            if (nativeConnected) {
                openedDevice = device
                outputPort = null
                usingNativeMidi = true
                connectedDeviceId = deviceId
                connectedPortNumber = portNumber
                parser.reset()
                listener.onMidiStatus(
                    "MIDI conectado: ${option.name} · porta ${portNumber + 1} · AMidi nativo",
                    true,
                )
                listener.onMidiDevicesChanged(knownDevices, connectedDeviceId, connectedPortNumber)
                return@openDevice
            }

            // Fallback compatível para aparelhos em que AMidi não abrir a porta.
            NativeSynth.nativeDisconnectMidi()
            val port = device.openOutputPort(portNumber)
            if (port == null) {
                try {
                    device.close()
                } catch (_: IOException) {
                }
                listener.onMidiStatus(
                    "${option.name} não abriu a porta ${portNumber + 1}. Tente outra porta.",
                    false,
                )
                return@openDevice
            }

            openedDevice = device
            outputPort = port
            usingNativeMidi = false
            connectedDeviceId = deviceId
            connectedPortNumber = portNumber
            parser.reset()
            port.connect(receiver)
            listener.onMidiStatus(
                "MIDI conectado: ${option.name} · porta ${portNumber + 1} · fallback Android",
                true,
            )
            listener.onMidiDevicesChanged(knownDevices, connectedDeviceId, connectedPortNumber)
        }, mainHandler)
    }

    fun disconnect() {
        closeCurrent(panic = true)
        listener.onMidiStatus("MIDI desconectado pelo usuário", false)
        listener.onMidiDevicesChanged(knownDevices, null, null)
    }

    fun devices(): List<DeviceOption> = knownDevices

    private fun closeCurrent(panic: Boolean) {
        ++connectionGeneration
        if (usingNativeMidi) {
            runCatching { NativeSynth.nativeDisconnectMidi() }
        }
        usingNativeMidi = false
        try {
            outputPort?.disconnect(receiver)
        } catch (_: Exception) {
        }
        try {
            outputPort?.close()
        } catch (_: IOException) {
        }
        try {
            openedDevice?.close()
        } catch (_: IOException) {
        }
        outputPort = null
        openedDevice = null
        connectedDeviceId = null
        connectedPortNumber = null
        parser.reset()
        if (panic) NativeSynth.nativePanic()
    }

    @Suppress("DEPRECATION")
    private fun availableDeviceInfos(): List<MidiDeviceInfo> {
        val manager = midiManager ?: return emptyList()
        return if (Build.VERSION.SDK_INT >= 33) {
            manager.getDevicesForTransport(MidiManager.TRANSPORT_MIDI_BYTE_STREAM).toList()
        } else {
            manager.devices.toList()
        }
    }

    private fun availableDevices(): List<DeviceOption> {
        return availableDeviceInfos()
            .mapNotNull(::toOption)
            .sortedWith(compareBy<DeviceOption> { devicePriority(it) }.thenBy { it.name.lowercase() })
    }

    private fun toOption(info: MidiDeviceInfo): DeviceOption? {
        val ports = info.ports
            .filter { it.type == MidiDeviceInfo.PortInfo.TYPE_OUTPUT }
            .map { PortOption(it.portNumber, it.name.orEmpty()) }
            .sortedBy { it.number }
        if (ports.isEmpty()) return null

        val properties = info.properties
        val manufacturer = properties.getString(MidiDeviceInfo.PROPERTY_MANUFACTURER).orEmpty().trim()
        val product = properties.getString(MidiDeviceInfo.PROPERTY_PRODUCT).orEmpty().trim()
        val propertyName = properties.getString(MidiDeviceInfo.PROPERTY_NAME).orEmpty().trim()
        val name = propertyName.ifBlank { product.ifBlank { manufacturer.ifBlank { "Dispositivo MIDI ${info.id}" } } }

        return DeviceOption(
            id = info.id,
            name = name,
            manufacturer = manufacturer,
            product = product,
            type = info.type,
            ports = ports,
        )
    }

    private fun devicePriority(device: DeviceOption): Int {
        val searchable = "${device.manufacturer} ${device.product} ${device.name}".lowercase(Locale.ROOT)
        return when {
            "harmonics" in searchable || "ha-500" in searchable -> 0
            device.type == MidiDeviceInfo.TYPE_USB -> 1
            device.type == MidiDeviceInfo.TYPE_BLUETOOTH -> 2
            else -> 3
        }
    }

    private fun readUsbDiagnostics(): List<UsbDiagnostic> {
        val manager = usbManager ?: return emptyList()
        return manager.deviceList.values.map { device ->
            var hasMidi = false
            var hasAudio = false
            val descriptorLines = mutableListOf<String>()
            for (index in 0 until device.interfaceCount) {
                val usbInterface = device.getInterface(index)
                if (usbInterface.interfaceClass == UsbConstants.USB_CLASS_AUDIO) {
                    val subclass = usbInterface.interfaceSubclass
                    // USB MIDI streaming usa a subclasse 3 da classe USB Audio.
                    if (subclass == 3) hasMidi = true else hasAudio = true
                }
                val endpoints = (0 until usbInterface.endpointCount).joinToString(", ") { endpointIndex ->
                    val endpoint = usbInterface.getEndpoint(endpointIndex)
                    val direction = if (endpoint.direction == UsbConstants.USB_DIR_OUT) "OUT" else "IN"
                    val transfer = when (endpoint.type) {
                        UsbConstants.USB_ENDPOINT_XFER_ISOC -> "ISO"
                        UsbConstants.USB_ENDPOINT_XFER_BULK -> "BULK"
                        UsbConstants.USB_ENDPOINT_XFER_INT -> "INT"
                        else -> "CTRL"
                    }
                    "0x${endpoint.address.toString(16)} $direction/$transfer max=${endpoint.maxPacketSize} int=${endpoint.interval}"
                }.ifBlank { "sem endpoint" }
                descriptorLines += "IF${usbInterface.id} alt=${usbInterface.alternateSetting} " +
                    "class=${usbInterface.interfaceClass}/${usbInterface.interfaceSubclass} · $endpoints"
            }
            val label = runCatching {
                device.productName?.takeIf { it.isNotBlank() }
                    ?: device.manufacturerName?.takeIf { it.isNotBlank() }
            }.getOrNull() ?: "USB ${device.vendorId.toString(16)}:${device.productId.toString(16)}"
            UsbDiagnostic(
                label = label,
                vendorId = device.vendorId,
                productId = device.productId,
                hasMidiInterface = hasMidi,
                hasAudioInterface = hasAudio,
                descriptorSummary = descriptorLines.joinToString("\n"),
            )
        }.sortedBy { it.label.lowercase() }
    }

    private fun handleMessage(status: Int, data1: Int, data2: Int) {
        val command = status and 0xF0
        val channel = status and 0x0F
        val raw = when (command) {
            0xC0, 0xD0 -> "%02X %02X".format(status, data1)
            else -> "%02X %02X %02X".format(status, data1, data2)
        }

        val activity = when (command) {
            0x80 -> {
                NativeSynth.nativeNoteOff(channel, data1)
                Activity("Note Off · nota $data1 · canal ${channel + 1}", raw, data1, 0, channel)
            }
            0x90 -> {
                if (data2 == 0) {
                    NativeSynth.nativeNoteOff(channel, data1)
                    Activity("Note Off · nota $data1 · canal ${channel + 1}", raw, data1, 0, channel)
                } else {
                    NativeSynth.nativeNoteOn(channel, data1, data2)
                    Activity(
                        "Note On · nota $data1 · velocidade $data2 · canal ${channel + 1}",
                        raw,
                        data1,
                        data2,
                        channel,
                    )
                }
            }
            0xB0 -> {
                NativeSynth.nativeControlChange(channel, data1, data2)
                val controllerName = when (data1) {
                    1 -> "Modulação"
                    7 -> "Volume"
                    11 -> "Expressão"
                    64 -> "Pedal sustain"
                    120 -> "All Sound Off"
                    123 -> "All Notes Off"
                    else -> "CC$data1"
                }
                Activity("$controllerName · valor $data2 · canal ${channel + 1}", raw, channel = channel)
            }
            0xC0 -> {
                NativeSynth.nativeProgramChange(channel, data1)
                Activity("Program Change · programa $data1 · canal ${channel + 1}", raw, channel = channel)
            }
            0xE0 -> {
                val value = (data2 shl 7) or data1
                NativeSynth.nativePitchBend(channel, value)
                Activity("Pitch bend · $value · canal ${channel + 1}", raw, channel = channel)
            }
            else -> return
        }
        notifyActivity(activity)
    }

    private fun notifyActivity(activity: Activity) {
        val now = System.currentTimeMillis()
        val previous = lastUiUpdate.get()
        if (now - previous < 25 || !lastUiUpdate.compareAndSet(previous, now)) return
        mainHandler.post { listener.onMidiActivity(activity) }
    }
}

private class MidiByteParser(
    private val onMessage: (status: Int, data1: Int, data2: Int) -> Unit,
) {
    private var runningStatus = 0
    private var pendingStatus = 0
    private var expectedData = 0
    private var dataCount = 0
    private var firstData = 0
    private var inSysEx = false

    fun reset() {
        runningStatus = 0
        pendingStatus = 0
        expectedData = 0
        dataCount = 0
        firstData = 0
        inSysEx = false
    }

    fun consume(bytes: ByteArray, offset: Int, count: Int) {
        val end = (offset + count).coerceAtMost(bytes.size)
        for (index in offset until end) consumeByte(bytes[index].toInt() and 0xFF)
    }

    private fun consumeByte(value: Int) {
        if (value >= 0xF8) return
        if (inSysEx) {
            if (value == 0xF7) inSysEx = false
            return
        }
        if (value and 0x80 != 0) {
            when {
                value == 0xF0 -> {
                    inSysEx = true
                    runningStatus = 0
                    clearPending()
                }
                value >= 0xF0 -> {
                    runningStatus = 0
                    configureSystemMessage(value)
                }
                else -> {
                    runningStatus = value
                    configureChannelMessage(value)
                }
            }
            return
        }
        if (pendingStatus == 0 && runningStatus != 0) configureChannelMessage(runningStatus)
        if (pendingStatus == 0 || expectedData == 0) return
        if (dataCount == 0) {
            firstData = value
            dataCount = 1
            if (expectedData == 1) finishMessage(firstData, 0)
        } else {
            finishMessage(firstData, value)
        }
    }

    private fun configureChannelMessage(status: Int) {
        pendingStatus = status
        dataCount = 0
        expectedData = when (status and 0xF0) {
            0xC0, 0xD0 -> 1
            0x80, 0x90, 0xA0, 0xB0, 0xE0 -> 2
            else -> 0
        }
    }

    private fun configureSystemMessage(status: Int) {
        pendingStatus = status
        dataCount = 0
        expectedData = when (status) {
            0xF1, 0xF3 -> 1
            0xF2 -> 2
            else -> 0
        }
        if (expectedData == 0) clearPending()
    }

    private fun finishMessage(data1: Int, data2: Int) {
        val status = pendingStatus
        if (status in 0x80..0xEF) onMessage(status, data1, data2)
        val keepRunning = status in 0x80..0xEF
        clearPending()
        if (keepRunning && runningStatus != 0) configureChannelMessage(runningStatus)
    }

    private fun clearPending() {
        pendingStatus = 0
        expectedData = 0
        dataCount = 0
        firstData = 0
    }
}
