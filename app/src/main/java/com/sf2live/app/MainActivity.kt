package com.sf2live.app

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.OpenableColumns
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.WindowInsets
import android.view.WindowManager
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.FrameLayout
import android.widget.GridLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.text.DecimalFormat
import java.util.Locale
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicLong

class MainActivity : Activity(), MidiController.Listener, AudioDeviceMonitor.Listener {
    companion object {
        private const val REQUEST_SF2 = 1001
        private const val PREFS = "sf2_live_preferences"
        private const val KEY_PATH = "soundfont_path"
        private const val KEY_PRESET = "preset_index"
        private const val KEY_RELEASE = "release_ms"
        private const val KEY_RELEASE_ENABLED = "release_enabled"
        private const val KEY_VOLUME = "master_volume"
        private const val KEY_LATENCY = "latency_profile"
        private const val KEY_MIDI_DEVICE = "midi_device_key"
        private const val KEY_MIDI_PORT = "midi_port"
        private const val KEY_AUDIO_DEVICE = "audio_device_id"
        private const val KEY_AUDIO_DEVICE_NAME = "audio_device_name"
        private const val KEY_AUDIO_DEVICE_TYPE = "audio_device_type"

        private val BACKGROUND = Color.rgb(16, 20, 24)
        private val PANEL = Color.rgb(27, 33, 39)
        private val PANEL_ALT = Color.rgb(34, 41, 48)
        private val YELLOW = Color.rgb(255, 193, 7)
        private val TEXT = Color.rgb(244, 246, 248)
        private val MUTED = Color.rgb(174, 186, 195)
        private val GREEN = Color.rgb(82, 190, 128)
        private val RED = Color.rgb(239, 83, 80)
    }

    private val mainHandler = Handler(Looper.getMainLooper())
    private val prefs by lazy { getSharedPreferences(PREFS, MODE_PRIVATE) }
    private val decimal = DecimalFormat("0.0")
    private val soundFontLibrary by lazy { SoundFontLibrary(this) }
    private val soundFontOperationId = AtomicLong(0)
    private val soundFontExecutor = Executors.newSingleThreadExecutor { task ->
        Thread({
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND)
            task.run()
        }, "SF2Live-SoundFont").apply { isDaemon = true }
    }

    private lateinit var midiController: MidiController
    private lateinit var audioDeviceMonitor: AudioDeviceMonitor
    private lateinit var realtimeSession: RealtimePerformanceSession

    private lateinit var summarySf2: TextView
    private lateinit var summaryMidi: TextView
    private lateinit var summaryAudio: TextView

    private lateinit var soundFontSpinner: Spinner
    private lateinit var soundFontDetails: TextView
    private lateinit var presetSpinner: Spinner
    private lateinit var importButton: Button
    private lateinit var removeSoundFontButton: Button

    private lateinit var midiDeviceSpinner: Spinner
    private lateinit var midiPortSpinner: Spinner
    private lateinit var midiStatus: TextView
    private lateinit var midiActivity: TextView
    private lateinit var usbDiagnostics: TextView

    private lateinit var audioOutputSpinner: Spinner
    private lateinit var audioStatus: TextView
    private lateinit var audioDetected: TextView

    private lateinit var releaseSwitch: Switch
    private lateinit var releaseSeek: SeekBar
    private lateinit var releaseLabel: TextView
    private lateinit var volumeLabel: TextView

    private var soundFonts: List<SoundFontLibrary.Entry> = emptyList()
    private var midiDevices: List<MidiController.DeviceOption> = emptyList()
    private var audioDevices: List<AudioDeviceMonitor.DeviceOption> = emptyList()
    private var loadedSoundFontPath: String? = null
    @Volatile private var nativeLoadedSoundFontPath: String? = null
    @Volatile private var activityDestroyed = false
    private var presetItemCount = 0

    private var suppressSoundFontSelection = false
    private var suppressPresetSelection = false
    private var pendingProgrammaticSoundFontPath: String? = null
    private var pendingProgrammaticPresetPosition: Int? = null
    private var suppressMidiDeviceSelection = false
    private var suppressMidiPortSelection = false
    private var suppressAudioSelection = false
    private val pages = mutableListOf<View>()
    private val navigationButtons = mutableListOf<Button>()
    private var lastNativeMidiCount = -1L
    private var currentLatencyProfile = 1

    private data class SoundFontLoadResult(
        val entry: SoundFontLibrary.Entry,
        val presets: List<String>,
    )

    private val statusUpdater = object : Runnable {
        override fun run() {
            updateAudioDiagnostics()
            mainHandler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.statusBarColor = BACKGROUND
        window.navigationBarColor = BACKGROUND

        buildInterface()

        realtimeSession = RealtimePerformanceSession(this).also { it.start() }

        // Configure volume, release e perfil antes de abrir o stream. Isso evita
        // abrir o Oboe com os padrões e reiniciá-lo imediatamente na inicialização.
        applySavedControls()
        val initialized = NativeSynth.nativeInitialize()
        summaryAudio.text = if (initialized) "● Áudio ativo" else "● Falha no áudio"
        summaryAudio.setTextColor(if (initialized) GREEN else RED)

        midiController = MidiController(this, this)
        audioDeviceMonitor = AudioDeviceMonitor(this, this)
        midiController.start()
        audioDeviceMonitor.start()

        refreshSoundFontLibrary(restoreSaved = true)
        mainHandler.post(statusUpdater)
    }

    override fun onDestroy() {
        activityDestroyed = true
        soundFontOperationId.incrementAndGet()
        soundFontExecutor.shutdownNow()
        mainHandler.removeCallbacks(statusUpdater)
        if (::midiController.isInitialized) midiController.stop()
        if (::audioDeviceMonitor.isInitialized) audioDeviceMonitor.stop()
        NativeSynth.nativeShutdown()
        if (::realtimeSession.isInitialized) realtimeSession.close()
        super.onDestroy()
    }

    private fun buildInterface() {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(BACKGROUND)
        }

        val header = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(18), dp(22), dp(18), dp(10))
        }
        root.setOnApplyWindowInsetsListener { _, insets ->
            val statusBarInset = if (Build.VERSION.SDK_INT >= 30) {
                insets.getInsets(WindowInsets.Type.statusBars()).top
            } else {
                @Suppress("DEPRECATION")
                insets.systemWindowInsetTop
            }
            header.setPadding(dp(18), statusBarInset + dp(18), dp(18), dp(10))
            insets
        }

        header.addView(TextView(this).apply {
            text = "SF2 Live"
            textSize = 29f
            setTextColor(YELLOW)
            setTypeface(typeface, Typeface.BOLD)
        })
        header.addView(TextView(this).apply {
            text = "Instrumentos SF2 · MIDI USB · Áudio em tempo real"
            textSize = 14f
            setTextColor(MUTED)
            setPadding(0, dp(1), 0, dp(12))
        })

        val summary = roundedPanel(PANEL_ALT, 16)
        summary.addView(TextView(this).apply {
            text = "SESSÃO"
            textSize = 11f
            letterSpacing = 0.12f
            setTextColor(YELLOW)
            setTypeface(typeface, Typeface.BOLD)
        })
        summarySf2 = statusLine("● Nenhum timbre", RED)
        summaryMidi = statusLine("● MIDI desconectado", RED)
        summaryAudio = statusLine("● Inicializando áudio", MUTED)
        summary.addView(summarySf2, matchWrap(top = 9))
        summary.addView(summaryMidi, matchWrap(top = 5))
        summary.addView(summaryAudio, matchWrap(top = 5))
        header.addView(summary)
        root.addView(header)

        val navRow = horizontalRow().apply {
            setPadding(dp(12), dp(4), dp(12), dp(10))
        }
        listOf("Tocar", "Timbres", "MIDI", "Áudio").forEachIndexed { index, label ->
            val button = Button(this).apply {
                text = label
                textSize = 13f
                minHeight = 0
                minimumHeight = 0
                setPadding(dp(18), dp(9), dp(18), dp(9))
                setOnClickListener { showPage(index) }
            }
            navigationButtons += button
            navRow.addView(button, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { marginEnd = dp(8) })
        }
        root.addView(HorizontalScrollView(this).apply {
            isHorizontalScrollBarEnabled = false
            addView(navRow)
        })

        val pageHost = FrameLayout(this)
        root.addView(pageHost, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            0,
            1f,
        ))

        fun addPage(builder: (LinearLayout) -> Unit) {
            val content = LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(18), dp(4), dp(18), dp(28))
            }
            builder(content)
            val scroll = ScrollView(this).apply {
                isFillViewport = true
                addView(content)
                visibility = View.GONE
            }
            pages += scroll
            pageHost.addView(scroll, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
            ))
        }

        addPage { page ->
            page.addView(diagnosticBox(
                "Modo ao vivo: mantenha esta tela aberta. O teclado físico usa AMidi diretamente dentro do callback de áudio quando disponível."
            ), matchWrap(bottom = 13))
            buildSustainPanel(page)
            buildTestPanel(page)
        }
        addPage(::buildSoundFontPanel)
        addPage(::buildMidiPanel)
        addPage(::buildAudioPanel)

        setContentView(root)
        root.post { root.requestApplyInsets() }
        showPage(0)
    }

    private fun showPage(index: Int) {
        pages.forEachIndexed { pageIndex, page ->
            page.visibility = if (pageIndex == index) View.VISIBLE else View.GONE
        }
        navigationButtons.forEachIndexed { buttonIndex, button ->
            val selected = buttonIndex == index
            button.setTextColor(if (selected) BACKGROUND else TEXT)
            button.setTypeface(button.typeface, if (selected) Typeface.BOLD else Typeface.NORMAL)
            button.background = roundedDrawable(if (selected) YELLOW else PANEL_ALT, 100)
        }
    }

    private fun buildSoundFontPanel(root: LinearLayout) {
        val panel = sectionPanel("♪", "Timbres", "Escolha o arquivo SF2 e o instrumento")
        panel.addView(fieldLabel("SoundFont ativo"))
        soundFontSpinner = styledSpinner().apply {
            onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                    if (position !in soundFonts.indices) return
                    val entry = soundFonts[position]
                    val pendingPath = pendingProgrammaticSoundFontPath
                    if (pendingPath != null) {
                        if (entry.stablePath == pendingPath) pendingProgrammaticSoundFontPath = null
                        // A troca de adapter pode emitir primeiro a posição zero e
                        // depois a posição solicitada. Nenhuma das duas é escolha do usuário.
                        return
                    }
                    if (suppressSoundFontSelection) return
                    if (entry.stablePath != loadedSoundFontPath) loadSoundFont(entry)
                }
                override fun onNothingSelected(parent: AdapterView<*>?) = Unit
            }
        }
        panel.addView(soundFontSpinner, matchWrap(top = 5))
        soundFontDetails = secondaryText("Nenhum arquivo importado")
        panel.addView(soundFontDetails, matchWrap(top = 8))

        val buttons = horizontalRow()
        importButton = primaryButton("Importar .sf2") { chooseSoundFont() }
        removeSoundFontButton = outlineButton("Remover") { confirmRemoveCurrentSoundFont() }
        buttons.addView(importButton, weightedButton(end = 6))
        buttons.addView(removeSoundFontButton, weightedButton(start = 6))
        panel.addView(buttons, matchWrap(top = 12))

        panel.addView(fieldLabel("Preset", top = 16))
        presetSpinner = styledSpinner().apply {
            isEnabled = false
            onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                    val pendingPosition = pendingProgrammaticPresetPosition
                    if (pendingPosition != null) {
                        if (position == pendingPosition) pendingProgrammaticPresetPosition = null
                        return
                    }
                    if (suppressPresetSelection || !isEnabled) return
                    NativeSynth.nativeSelectPreset(position)
                    prefs.edit().putInt(KEY_PRESET, position).apply()
                    updateSoundFontDetails()
                }
                override fun onNothingSelected(parent: AdapterView<*>?) = Unit
            }
        }
        panel.addView(presetSpinner, matchWrap(top = 5))
        root.addView(panel, matchWrap(bottom = 14))
    }

    private fun buildMidiPanel(root: LinearLayout) {
        val panel = sectionPanel("M", "Teclado MIDI", "Conexão automática ao detectar uma nova entrada")

        panel.addView(fieldLabel("Dispositivo"))
        midiDeviceSpinner = styledSpinner().apply {
            onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                    if (suppressMidiDeviceSelection || position !in midiDevices.indices) return
                    val device = midiDevices[position]
                    prefs.edit().putString(KEY_MIDI_DEVICE, device.stableKey).apply()
                    populateMidiPorts(device, connect = true)
                }
                override fun onNothingSelected(parent: AdapterView<*>?) = Unit
            }
        }
        panel.addView(midiDeviceSpinner, matchWrap(top = 5))

        panel.addView(fieldLabel("Porta de saída do teclado", top = 12))
        midiPortSpinner = styledSpinner().apply {
            onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                    if (suppressMidiPortSelection) return
                    val device = midiDevices.getOrNull(midiDeviceSpinner.selectedItemPosition) ?: return
                    val port = device.ports.getOrNull(position) ?: return
                    prefs.edit().putInt(KEY_MIDI_PORT, port.number).apply()
                    midiController.connect(device.id, port.number)
                }
                override fun onNothingSelected(parent: AdapterView<*>?) = Unit
            }
        }
        panel.addView(midiPortSpinner, matchWrap(top = 5))

        midiStatus = secondaryText("Procurando dispositivos MIDI…")
        panel.addView(midiStatus, matchWrap(top = 10))
        midiActivity = diagnosticBox("Monitor MIDI: nenhuma mensagem recebida")
        panel.addView(midiActivity, matchWrap(top = 10))

        val actions = horizontalRow()
        actions.addView(primaryButton("Reconectar") {
            val device = midiDevices.getOrNull(midiDeviceSpinner.selectedItemPosition)
            val port = device?.ports?.getOrNull(midiPortSpinner.selectedItemPosition)
            if (device != null && port != null) midiController.connect(device.id, port.number)
            else midiController.refreshDevices()
        }, weightedButton(end = 6))
        actions.addView(outlineButton("Atualizar lista") {
            midiController.refreshDevices()
        }, weightedButton(start = 6))
        panel.addView(actions, matchWrap(top = 10))

        usbDiagnostics = secondaryText("USB: aguardando diagnóstico…")
        panel.addView(usbDiagnostics, matchWrap(top = 12))
        root.addView(panel, matchWrap(bottom = 14))
    }

    private fun buildAudioPanel(root: LinearLayout) {
        val panel = sectionPanel("A", "Saída de áudio", "Escolha a interface USB ou a saída desejada")
        panel.addView(fieldLabel("Dispositivo de saída"))
        audioOutputSpinner = styledSpinner().apply {
            onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                    if (suppressAudioSelection) return
                    val device = if (position == 0) null else audioDevices.getOrNull(position - 1)
                    val editor = prefs.edit().putInt(KEY_AUDIO_DEVICE, device?.id ?: -1)
                    if (device == null) {
                        editor.remove(KEY_AUDIO_DEVICE_NAME).remove(KEY_AUDIO_DEVICE_TYPE)
                    } else {
                        editor.putString(KEY_AUDIO_DEVICE_NAME, device.name)
                            .putInt(KEY_AUDIO_DEVICE_TYPE, device.type)
                    }
                    editor.apply()
                    applyAudioConfiguration(currentLatencyProfile, device?.id ?: -1)
                    mainHandler.postDelayed({ updateAudioDiagnostics() }, 350)
                }
                override fun onNothingSelected(parent: AdapterView<*>?) = Unit
            }
        }
        panel.addView(audioOutputSpinner, matchWrap(top = 5))
        audioDetected = secondaryText("Procurando interfaces de áudio…")
        panel.addView(audioDetected, matchWrap(top = 9))

        volumeLabel = secondaryText("")
        panel.addView(volumeLabel, matchWrap(top = 13))
        val volumeSeek = SeekBar(this).apply {
            max = 120
            progress = prefs.getInt(KEY_VOLUME, 80).coerceIn(0, 120)
            progressTintList = ColorStateList.valueOf(YELLOW)
            thumbTintList = ColorStateList.valueOf(YELLOW)
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                    volumeLabel.text = "Volume master: $progress%"
                    NativeSynth.nativeSetMasterVolume(progress / 100f)
                    if (fromUser) prefs.edit().putInt(KEY_VOLUME, progress).apply()
                }
                override fun onStartTrackingTouch(seekBar: SeekBar?) = Unit
                override fun onStopTrackingTouch(seekBar: SeekBar?) = Unit
            })
        }
        volumeLabel.text = "Volume master: ${volumeSeek.progress}%"
        panel.addView(volumeSeek)

        panel.addView(fieldLabel("Perfil do buffer", top = 12))
        val latencyGroup = RadioGroup(this).apply { orientation = RadioGroup.VERTICAL }
        val labels = listOf(
            "Máxima resposta — 1 burst + proteção automática para acordes",
            "Baixa latência — 1 burst + polifonia equilibrada",
            "Estável — 2 bursts + maior margem de processamento",
        )
        val selected = prefs.getInt(KEY_LATENCY, 1).coerceIn(0, 2)
        currentLatencyProfile = selected
        labels.forEachIndexed { index, label ->
            latencyGroup.addView(RadioButton(this).apply {
                id = 5000 + index
                text = label
                textSize = 15f
                setTextColor(TEXT)
                buttonTintList = ColorStateList.valueOf(YELLOW)
                isChecked = index == selected
            })
        }
        latencyGroup.setOnCheckedChangeListener { _, checkedId ->
            val profile = (checkedId - 5000).coerceIn(0, 2)
            currentLatencyProfile = profile
            prefs.edit().putInt(KEY_LATENCY, profile).apply()
            applyAudioConfiguration(profile)
        }
        panel.addView(latencyGroup)

        audioStatus = diagnosticBox("Inicializando diagnóstico de áudio…")
        panel.addView(audioStatus, matchWrap(top = 10))
        root.addView(panel, matchWrap(bottom = 14))
    }

    private fun buildSustainPanel(root: LinearLayout) {
        val panel = sectionPanel("S", "Sustentação", "Faça a nota desaparecer de forma suave")
        releaseSwitch = Switch(this).apply {
            text = "Cauda suave após soltar a tecla"
            textSize = 16f
            setTextColor(TEXT)
            thumbTintList = switchThumbColors()
            trackTintList = switchTrackColors()
            setOnCheckedChangeListener { _, enabled ->
                releaseSeek.isEnabled = enabled
                NativeSynth.nativeSetReleaseMilliseconds(if (enabled) releaseSeek.progress.toFloat() else 0f)
                prefs.edit().putBoolean(KEY_RELEASE_ENABLED, enabled).apply()
                updateReleaseLabel()
            }
        }
        panel.addView(releaseSwitch)
        releaseLabel = secondaryText("")
        panel.addView(releaseLabel, matchWrap(top = 8))
        releaseSeek = SeekBar(this).apply {
            min = 50
            max = 5000
            progressTintList = ColorStateList.valueOf(YELLOW)
            thumbTintList = ColorStateList.valueOf(YELLOW)
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                    updateReleaseLabel()
                    if (fromUser && releaseSwitch.isChecked) {
                        NativeSynth.nativeSetReleaseMilliseconds(progress.toFloat())
                        prefs.edit().putInt(KEY_RELEASE, progress).apply()
                    }
                }
                override fun onStartTrackingTouch(seekBar: SeekBar?) = Unit
                override fun onStopTrackingTouch(seekBar: SeekBar?) = Unit
            })
        }
        panel.addView(releaseSeek)
        panel.addView(secondaryText("O pedal físico continua chegando separadamente como MIDI CC64."), matchWrap(top = 7))
        root.addView(panel, matchWrap(bottom = 14))
    }

    private fun buildTestPanel(root: LinearLayout) {
        val panel = sectionPanel("T", "Teste", "Isole rapidamente áudio, SF2 e MIDI")
        val actions = horizontalRow()
        actions.addView(primaryButton("Tom de saída") { NativeSynth.nativeTestTone() }, weightedButton(end = 6))
        actions.addView(Button(this).apply {
            text = "PANIC"
            textSize = 15f
            setTextColor(Color.WHITE)
            background = roundedDrawable(Color.rgb(183, 28, 28), 12)
            setOnClickListener {
                NativeSynth.nativePanic()
                Toast.makeText(this@MainActivity, "Todas as notas foram interrompidas", Toast.LENGTH_SHORT).show()
            }
        }, weightedButton(start = 6))
        panel.addView(actions)
        panel.addView(fieldLabel("Teclado virtual de diagnóstico", top = 15))
        panel.addView(buildDiagnosticKeyboard(), matchWrap(top = 6))
        root.addView(panel)
    }

    private fun applySavedControls() {
        val releaseEnabled = prefs.getBoolean(KEY_RELEASE_ENABLED, true)
        val release = prefs.getInt(KEY_RELEASE, 800).coerceIn(50, 5000)
        releaseSeek.progress = release
        releaseSwitch.isChecked = releaseEnabled
        releaseSeek.isEnabled = releaseEnabled
        NativeSynth.nativeSetReleaseMilliseconds(if (releaseEnabled) release.toFloat() else 0f)
        NativeSynth.nativeSetMasterVolume(prefs.getInt(KEY_VOLUME, 80).coerceIn(0, 120) / 100f)
        currentLatencyProfile = prefs.getInt(KEY_LATENCY, 1).coerceIn(0, 2)
        NativeSynth.nativeSetLatencyProfile(currentLatencyProfile)
        NativeSynth.nativeSetOutputDevice(prefs.getInt(KEY_AUDIO_DEVICE, -1))
        updateReleaseLabel()
    }

    private fun refreshSoundFontLibrary(restoreSaved: Boolean = false) {
        val entries = soundFontLibrary.entries()
        val preferredPath = prefs.getString(KEY_PATH, null) ?: loadedSoundFontPath
        val selectedEntry = entries.firstOrNull { it.stablePath == preferredPath }
            ?: entries.firstOrNull()
        updateSoundFontList(entries, selectedEntry?.stablePath)

        if (entries.isEmpty()) {
            showNoSoundFontLoaded()
        } else if (restoreSaved || loadedSoundFontPath == null) {
            selectedEntry?.let(::loadSoundFont)
        }
    }

    private fun updateSoundFontList(
        entries: List<SoundFontLibrary.Entry>,
        selectedPath: String?,
    ) {
        soundFonts = entries
        suppressSoundFontSelection = true
        val selectedIndex = entries.indexOfFirst { it.stablePath == selectedPath }
            .takeIf { it >= 0 }
            ?: 0
        val programmaticPath = entries.getOrNull(selectedIndex)?.stablePath
        pendingProgrammaticSoundFontPath = programmaticPath
        soundFontSpinner.adapter = spinnerAdapter(
            entries.map { "${it.displayName} · ${humanFileSize(it.sizeBytes)}" }
                .ifEmpty { listOf("Nenhum SoundFont importado") },
        )
        soundFontSpinner.isEnabled = entries.isNotEmpty()
        removeSoundFontButton.isEnabled = entries.isNotEmpty()
        if (entries.isNotEmpty()) soundFontSpinner.setSelection(selectedIndex, false)
        else pendingProgrammaticSoundFontPath = null
        suppressSoundFontSelection = false
        mainHandler.postDelayed({
            if (pendingProgrammaticSoundFontPath == programmaticPath) {
                pendingProgrammaticSoundFontPath = null
            }
        }, 300)
    }

    private fun showNoSoundFontLoaded() {
        loadedSoundFontPath = null
        nativeLoadedSoundFontPath = null
        prefs.edit().remove(KEY_PATH).remove(KEY_PRESET).apply()
        soundFontDetails.text = "Importe um arquivo .sf2 para começar"
        summarySf2.text = "● Nenhum timbre"
        summarySf2.setTextColor(RED)
        fillPresets(emptyList())
        setSoundFontLoading(false)
    }

    private fun chooseSoundFont() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        startActivityForResult(intent, REQUEST_SF2)
    }

    @Deprecated("Mantido sem AndroidX para simplificar o projeto")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_SF2 || resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        try {
            contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        } catch (_: Exception) {
        }
        importSoundFont(uri)
    }

    private fun importSoundFont(uri: Uri) {
        val displayName = queryDisplayName(uri)
        if (!displayName.lowercase(Locale.ROOT).endsWith(".sf2")) {
            Toast.makeText(this, "Selecione um arquivo com extensão .sf2", Toast.LENGTH_LONG).show()
            return
        }

        val operationId = soundFontOperationId.incrementAndGet()
        setSoundFontLoading(true, "Copiando $displayName…")
        soundFontExecutor.execute {
            var destination: File? = null
            try {
                if (!isSoundFontOperationCurrent(operationId)) return@execute
                val target = soundFontLibrary.uniqueDestination(displayName)
                destination = target
                contentResolver.openInputStream(uri).use { input ->
                    requireNotNull(input) { "Não foi possível abrir o arquivo selecionado." }
                    FileOutputStream(target).use { output ->
                        val buffer = ByteArray(256 * 1024)
                        var total = 0L
                        var nextUiUpdate = 128L * 1024L * 1024L
                        while (true) {
                            if (Thread.currentThread().isInterrupted) {
                                throw InterruptedException("Importação cancelada")
                            }
                            val count = input.read(buffer)
                            if (count < 0) break
                            total += count
                            output.write(buffer, 0, count)
                            if (total >= nextUiUpdate) {
                                val copied = total
                                runOnUiThreadIfCurrent(operationId) {
                                    setSoundFontLoading(
                                        true,
                                        "Copiando $displayName… ${humanFileSize(copied)}",
                                    )
                                }
                                nextUiUpdate += 128L * 1024L * 1024L
                            }
                        }
                        output.fd.sync()
                    }
                }

                if (!isSoundFontOperationCurrent(operationId)) return@execute
                val importedEntry = soundFontLibrary.entries()
                    .firstOrNull { it.file.absolutePath == target.absolutePath }
                    ?: error("O arquivo importado não apareceu na biblioteca.")
                val loaded = loadSoundFontNative(importedEntry)
                val entries = soundFontLibrary.entries()
                runOnUiThreadIfCurrent(operationId) {
                    updateSoundFontList(entries, importedEntry.stablePath)
                    applyLoadedSoundFont(loaded, resetPreset = true)
                }
            } catch (cancelled: InterruptedException) {
                destination?.delete()
                Thread.currentThread().interrupt()
            } catch (error: Exception) {
                destination?.delete()
                runOnUiThreadIfCurrent(operationId) {
                    setSoundFontLoading(false)
                    showSoundFontError(error.message ?: "Não foi possível importar o SF2.")
                }
            }
        }
    }

    private fun loadSoundFont(entry: SoundFontLibrary.Entry) {
        val operationId = soundFontOperationId.incrementAndGet()
        setSoundFontLoading(true, "Carregando ${entry.displayName}…")
        soundFontExecutor.execute {
            if (!isSoundFontOperationCurrent(operationId)) return@execute
            try {
                val loaded = loadSoundFontNative(entry)
                val entries = soundFontLibrary.entries()
                runOnUiThreadIfCurrent(operationId) {
                    updateSoundFontList(entries, entry.stablePath)
                    applyLoadedSoundFont(loaded, resetPreset = false)
                }
            } catch (error: Exception) {
                runOnUiThreadIfCurrent(operationId) {
                    setSoundFontLoading(false)
                    showSoundFontError(error.message ?: "Não foi possível carregar o SF2.")
                }
            }
        }
    }

    private fun loadSoundFontNative(entry: SoundFontLibrary.Entry): SoundFontLoadResult {
        val response = NativeSynth.nativeLoadSoundFont(entry.file.absolutePath)
        if (!response.startsWith("OK|")) {
            throw IllegalStateException(
                response.substringAfter("ERR|", "Não foi possível carregar o SF2."),
            )
        }
        nativeLoadedSoundFontPath = entry.stablePath
        return SoundFontLoadResult(entry, NativeSynth.nativeGetPresetNames().toList())
    }

    private fun applyLoadedSoundFont(
        result: SoundFontLoadResult,
        resetPreset: Boolean,
    ) {
        loadedSoundFontPath = result.entry.stablePath
        prefs.edit().putString(KEY_PATH, result.entry.stablePath).apply()
        val preferredPreset = if (resetPreset) 0 else prefs.getInt(KEY_PRESET, 0)
        val selectedPreset = preferredPreset.coerceIn(
            0,
            (result.presets.size - 1).coerceAtLeast(0),
        )
        fillPresets(result.presets, selectedPreset)
        if (result.presets.isNotEmpty()) {
            NativeSynth.nativeSelectPreset(selectedPreset)
            prefs.edit().putInt(KEY_PRESET, selectedPreset).apply()
        }
        summarySf2.text = "● Timbre pronto"
        summarySf2.setTextColor(GREEN)
        setSoundFontLoading(false)
        updateSoundFontDetails()
    }

    private fun confirmRemoveCurrentSoundFont() {
        val selectedIndex = soundFontSpinner.selectedItemPosition
        val entry = soundFonts.getOrNull(selectedIndex) ?: return
        AlertDialog.Builder(this)
            .setTitle("Remover SoundFont?")
            .setMessage("${entry.displayName} será removido apenas do aplicativo.")
            .setNegativeButton("Cancelar", null)
            .setPositiveButton("Remover") { _, _ ->
                removeSoundFont(entry, selectedIndex)
            }
            .show()
    }

    private fun removeSoundFont(entry: SoundFontLibrary.Entry, selectedIndex: Int) {
        val operationId = soundFontOperationId.incrementAndGet()
        setSoundFontLoading(true, "Removendo ${entry.displayName}…")
        soundFontExecutor.execute {
            val previouslyLoadedPath = nativeLoadedSoundFontPath
            val removingLoadedSoundFont = previouslyLoadedPath == entry.stablePath
            try {
                if (!isSoundFontOperationCurrent(operationId)) return@execute

                // Nunca apague um arquivo ainda mapeado pelo sintetizador. O unload
                // para o stream, limpa as vozes e libera o mmap antes da exclusão.
                if (removingLoadedSoundFont) {
                    // Não reabra o Oboe entre o unload e o carregamento do próximo
                    // SF2. Uma única reabertura ao final reduz trocas de rota e evita
                    // callbacks antigos concorrendo com a operação de remoção.
                    NativeSynth.nativeUnloadSoundFont(false)
                    nativeLoadedSoundFontPath = null
                    if (!isSoundFontOperationCurrent(operationId)) return@execute
                }

                if (!soundFontLibrary.remove(entry)) {
                    if (removingLoadedSoundFont && entry.file.exists()) {
                        loadSoundFontNative(entry)
                    }
                    error("Não foi possível remover o arquivo do armazenamento do aplicativo.")
                }

                val entries = soundFontLibrary.entries()
                if (!removingLoadedSoundFont) {
                    // Remover um item que não está ativo não deve trocar o timbre.
                    val active = entries.firstOrNull { it.stablePath == previouslyLoadedPath }
                    runOnUiThreadIfCurrent(operationId) {
                        updateSoundFontList(entries, active?.stablePath)
                        setSoundFontLoading(false)
                        updateSoundFontDetails()
                        Toast.makeText(
                            this,
                            "${entry.displayName} removido. O timbre atual foi mantido.",
                            Toast.LENGTH_SHORT,
                        ).show()
                    }
                    return@execute
                }

                val nearestIndex = selectedIndex.coerceAtMost((entries.size - 1).coerceAtLeast(0))
                val candidates = buildList {
                    entries.getOrNull(nearestIndex)?.let(::add)
                    entries.forEach { candidate ->
                        if (none { it.stablePath == candidate.stablePath }) add(candidate)
                    }
                }
                var loadedFallback: SoundFontLoadResult? = null
                var lastLoadError: Exception? = null
                for (candidate in candidates) {
                    try {
                        loadedFallback = loadSoundFontNative(candidate)
                        break
                    } catch (error: Exception) {
                        lastLoadError = error
                    }
                }

                val fallbackResult = loadedFallback
                val fallbackError = lastLoadError
                if (fallbackResult == null) {
                    // Se a biblioteca ficou vazia, não haverá nativeLoadSoundFont()
                    // para reabrir o stream que foi pausado pela transação.
                    NativeSynth.nativeInitialize()
                }
                runOnUiThreadIfCurrent(operationId) {
                    prefs.edit().remove(KEY_PRESET).apply()
                    updateSoundFontList(entries, fallbackResult?.entry?.stablePath)
                    if (fallbackResult != null) {
                        applyLoadedSoundFont(fallbackResult, resetPreset = true)
                        Toast.makeText(
                            this,
                            "${entry.displayName} removido. ${fallbackResult.entry.displayName} foi ativado.",
                            Toast.LENGTH_SHORT,
                        ).show()
                    } else {
                        showNoSoundFontLoaded()
                        Toast.makeText(this, "${entry.displayName} removido.", Toast.LENGTH_SHORT).show()
                        if (entries.isNotEmpty() && fallbackError != null) {
                            showSoundFontError(
                                fallbackError.message
                                    ?: "Os demais SoundFonts não puderam ser carregados.",
                            )
                        }
                    }
                }
            } catch (error: Exception) {
                runOnUiThreadIfCurrent(operationId) {
                    val entries = soundFontLibrary.entries()
                    updateSoundFontList(entries, nativeLoadedSoundFontPath)
                    setSoundFontLoading(false)
                    showSoundFontError(error.message ?: "Não foi possível remover o SoundFont.")
                }
            }
        }
    }

    private fun isSoundFontOperationCurrent(operationId: Long): Boolean {
        return !activityDestroyed && soundFontOperationId.get() == operationId
    }

    private fun runOnUiThreadIfCurrent(operationId: Long, action: () -> Unit) {
        runOnUiThread {
            if (isSoundFontOperationCurrent(operationId)) action()
        }
    }

    private fun fillPresets(items: List<String>, selectedPosition: Int = 0) {
        presetItemCount = items.size
        suppressPresetSelection = true
        val selected = selectedPosition.coerceIn(0, (items.size - 1).coerceAtLeast(0))
        pendingProgrammaticPresetPosition = if (items.isNotEmpty()) selected else null
        presetSpinner.adapter = spinnerAdapter(items.ifEmpty { listOf("Nenhum preset compatível") })
        presetSpinner.isEnabled = items.isNotEmpty()
        if (items.isNotEmpty()) presetSpinner.setSelection(selected, false)
        suppressPresetSelection = false
        mainHandler.postDelayed({
            if (pendingProgrammaticPresetPosition == selected) {
                pendingProgrammaticPresetPosition = null
            }
        }, 300)
    }

    private fun updateSoundFontDetails() {
        val entry = soundFonts.firstOrNull { it.stablePath == loadedSoundFontPath }
        if (entry == null) {
            soundFontDetails.text = "Nenhum timbre carregado"
            return
        }
        val preset = if (presetItemCount > 0) presetSpinner.selectedItem?.toString().orEmpty() else "Sem preset"
        soundFontDetails.text = "${entry.displayName}\n${humanFileSize(entry.sizeBytes)} · $preset"
    }

    override fun onMidiDevicesChanged(
        devices: List<MidiController.DeviceOption>,
        connectedDeviceId: Int?,
        connectedPort: Int?,
    ) {
        midiDevices = devices
        suppressMidiDeviceSelection = true
        midiDeviceSpinner.adapter = spinnerAdapter(
            devices.map { it.displayLabel }.ifEmpty { listOf("Nenhum dispositivo MIDI") },
        )
        midiDeviceSpinner.isEnabled = devices.isNotEmpty()

        if (devices.isEmpty()) {
            suppressMidiDeviceSelection = false
            suppressMidiPortSelection = true
            midiPortSpinner.adapter = spinnerAdapter(listOf("Nenhuma porta"))
            midiPortSpinner.isEnabled = false
            suppressMidiPortSelection = false
            summaryMidi.text = "● MIDI não encontrado"
            summaryMidi.setTextColor(RED)
            return
        }

        val savedKey = prefs.getString(KEY_MIDI_DEVICE, null)
        val selectedIndex = devices.indexOfFirst { it.id == connectedDeviceId }
            .takeIf { it >= 0 }
            ?: devices.indexOfFirst { it.stableKey == savedKey }.takeIf { it >= 0 }
            ?: devices.indexOfFirst {
                val text = "${it.name} ${it.product} ${it.manufacturer}".lowercase(Locale.ROOT)
                "harmonics" in text || "ha-500" in text
            }.takeIf { it >= 0 }
            ?: 0

        midiDeviceSpinner.setSelection(selectedIndex)
        suppressMidiDeviceSelection = false
        if (connectedDeviceId != null) {
            prefs.edit()
                .putString(KEY_MIDI_DEVICE, devices[selectedIndex].stableKey)
                .putInt(KEY_MIDI_PORT, connectedPort ?: devices[selectedIndex].ports.firstOrNull()?.number ?: 0)
                .apply()
        }
        populateMidiPorts(devices[selectedIndex], connect = false, preferredPort = connectedPort)
    }

    private fun populateMidiPorts(
        device: MidiController.DeviceOption,
        connect: Boolean,
        preferredPort: Int? = null,
    ) {
        suppressMidiPortSelection = true
        midiPortSpinner.adapter = spinnerAdapter(device.ports.map { it.label }.ifEmpty { listOf("Nenhuma porta") })
        midiPortSpinner.isEnabled = device.ports.isNotEmpty()
        val saved = preferredPort ?: prefs.getInt(KEY_MIDI_PORT, device.ports.firstOrNull()?.number ?: 0)
        val portIndex = device.ports.indexOfFirst { it.number == saved }.takeIf { it >= 0 } ?: 0
        if (device.ports.isNotEmpty()) midiPortSpinner.setSelection(portIndex)
        suppressMidiPortSelection = false
        if (connect && device.ports.isNotEmpty()) midiController.connect(device.id, device.ports[portIndex].number)
    }

    override fun onUsbDiagnosticsChanged(devices: List<MidiController.UsbDiagnostic>) {
        usbDiagnostics.text = if (devices.isEmpty()) {
            "USB: nenhum dispositivo visível para o Android"
        } else {
            buildString {
                append("USB detectado:\n")
                devices.forEachIndexed { index, device ->
                    if (index > 0) append('\n')
                    append("• ${device.label} [${device.vendorId.toString(16)}:${device.productId.toString(16)}]")
                    val capabilities = mutableListOf<String>()
                    if (device.hasMidiInterface) capabilities += "MIDI"
                    if (device.hasAudioInterface) capabilities += "áudio"
                    if (capabilities.isNotEmpty()) append(" · ${capabilities.joinToString(" + ")}")
                    if (device.hasAudioInterface && device.descriptorSummary.isNotBlank()) {
                        append("\n  ${device.descriptorSummary.replace("\n", "\n  ")}")
                    }
                }
            }
        }
    }

    override fun onMidiStatus(text: String, connected: Boolean) {
        midiStatus.text = text
        summaryMidi.text = if (connected) "● Teclado MIDI conectado" else "● MIDI desconectado"
        summaryMidi.setTextColor(if (connected) GREEN else RED)
    }

    override fun onMidiActivity(activity: MidiController.Activity) {
        midiActivity.text = "Monitor MIDI\n${activity.description}\nHEX: ${activity.rawHex}"
    }

    override fun onAudioDevicesChanged(devices: List<AudioDeviceMonitor.DeviceOption>) {
        audioDevices = devices
        suppressAudioSelection = true
        val labels = mutableListOf("Automática — escolhida pelo Android")
        labels += devices.map { it.displayLabel }
        audioOutputSpinner.adapter = spinnerAdapter(labels)

        val savedId = prefs.getInt(KEY_AUDIO_DEVICE, -1)
        val savedName = prefs.getString(KEY_AUDIO_DEVICE_NAME, null)
        val savedType = prefs.getInt(KEY_AUDIO_DEVICE_TYPE, Int.MIN_VALUE)
        val exactIndex = devices.indexOfFirst { it.id == savedId }
        val identityIndex = if (savedName != null && savedType != Int.MIN_VALUE) {
            devices.indexOfFirst { it.name == savedName && it.type == savedType }
        } else {
            -1
        }
        val automaticUsbIndex = if (savedId < 0 && savedName == null) {
            devices.indexOfFirst {
                it.type == android.media.AudioDeviceInfo.TYPE_USB_DEVICE ||
                    it.type == android.media.AudioDeviceInfo.TYPE_USB_HEADSET
            }
        } else {
            -1
        }
        val deviceIndex = when {
            exactIndex >= 0 -> exactIndex
            identityIndex >= 0 -> identityIndex
            automaticUsbIndex >= 0 -> automaticUsbIndex
            else -> -1
        }
        val selectedPosition = deviceIndex + 1
        audioOutputSpinner.setSelection(selectedPosition.coerceAtLeast(0))
        suppressAudioSelection = false

        val usb = devices.filter {
            it.type == android.media.AudioDeviceInfo.TYPE_USB_DEVICE ||
                it.type == android.media.AudioDeviceInfo.TYPE_USB_HEADSET
        }
        audioDetected.text = when {
            usb.isEmpty() -> "Nenhuma interface USB de áudio detectada"
            usb.size == 1 -> "Interface USB detectada: ${usb.first().name}"
            else -> "Interfaces USB detectadas: ${usb.joinToString { it.name }}"
        }

        val selectedDevice = devices.getOrNull(deviceIndex)
        if (selectedDevice != null) {
            // O ID do Android pode mudar após desconectar e reconectar a mesma
            // interface. Preserve a identidade nome+tipo e atualize apenas o ID.
            prefs.edit()
                .putInt(KEY_AUDIO_DEVICE, selectedDevice.id)
                .putString(KEY_AUDIO_DEVICE_NAME, selectedDevice.name)
                .putInt(KEY_AUDIO_DEVICE_TYPE, selectedDevice.type)
                .apply()
        }
        // Enquanto uma interface preferida está temporariamente ausente, use a
        // rota automática sem apagar a preferência. Na reconexão ela será retomada.
        applyAudioConfiguration(currentLatencyProfile, selectedDevice?.id ?: -1)
    }

    private fun selectedAudioDeviceId(): Int {
        val position = audioOutputSpinner.selectedItemPosition
        return if (position > 0) audioDevices.getOrNull(position - 1)?.id ?: -1 else -1
    }

    private fun applyAudioConfiguration(
        profile: Int = currentLatencyProfile,
        deviceId: Int = selectedAudioDeviceId(),
    ) {
        NativeSynth.nativeConfigureOutput(
            deviceId = deviceId,
            profile = profile.coerceIn(0, 2),
            optimized = false,
            sampleRate = 0,
            dataFormat = 0,
        )
    }

    private fun updateAudioDiagnostics() {
        try {
            val status = JSONObject(NativeSynth.nativeGetStatusJson())
            val rate = status.optInt("sampleRate")
            val burst = status.optInt("framesPerBurst")
            val callbackFrames = status.optInt("framesPerCallback")
            val buffer = status.optInt("bufferFrames")
            val capacity = status.optInt("bufferCapacityFrames")
            val xruns = status.optInt("xruns")
            val voices = status.optInt("activeVoices")
            val voiceLimit = status.optInt("voiceLimit")
            val callbackLoad = status.optInt("callbackLoadPermille") / 10.0
            val dropped = status.optInt("droppedCommands")
            val droppedMidi = status.optInt("droppedMidiEvents")
            val running = status.optBoolean("running")
            val exclusive = status.optInt("sharingMode") == 0
            val lowLatencyMode = status.optBoolean("lowLatencyMode")
            val audioApi = when (status.optInt("audioApi")) {
                2 -> "AAudio"
                1 -> "OpenSL ES"
                else -> "API automática"
            }
            val mmapUsed = status.optBoolean("mmapUsed")
            val routeFallback = status.optBoolean("routeFallbackUsed")
            val restartAttempts = status.optInt("restartAttempts")
            val restartFailures = status.optInt("restartFailures")
            val performanceHint = status.optBoolean("performanceHintEnabled")
            val workloadReported = status.optInt("workloadReported")
            val dataFormat = when (status.optInt("dataFormat")) {
                1 -> "PCM 16-bit nativo"
                2 -> "Float 32-bit nativo"
                else -> "formato automático"
            }
            val activeDeviceId = status.optInt("activeDeviceId", -1)
            val activeDevice = audioDevices.firstOrNull { it.id == activeDeviceId }
            val bufferMs = if (rate > 0) buffer * 1000.0 / rate else 0.0

            audioStatus.text = buildString {
                append(if (running) "Oboe ativo · $audioApi" else "Oboe parado")
                append(if (lowLatencyMode) " · baixa latência" else " · modo comum")
                append(if (exclusive) " · exclusivo" else " · compartilhado")
                append("\nSaída real: ${activeDevice?.displayLabel ?: "automática / id $activeDeviceId"}")
                if (rate > 0) {
                    append("\n${rate} Hz · $dataFormat")
                    append("\nBurst $burst · callback $callbackFrames")
                    append("\nBuffer: $buffer/$capacity frames · ${decimal.format(bufferMs)} ms")
                }
                append("\nFast path: ${if (mmapUsed) "MMAP" else "não confirmado"}")
                append(" · CPU adaptativa: ${if (performanceHint) "ativa" else "indisponível"}")
                append("\nVozes: $voices/$voiceLimit · carga prevista: $workloadReported")
                append(" · callback: ${decimal.format(callbackLoad)}%")
                append("\nxruns: $xruns · comandos descartados: $dropped")
                if (droppedMidi > 0) append(" · MIDI descartado: $droppedMidi")
                if (routeFallback && running) {
                    append("\nRota temporária: a interface preferida não estava disponível.")
                }
                if (restartAttempts > 0) {
                    append("\nRecuperações Oboe: $restartAttempts")
                    if (restartFailures > 0) append(" · falhas: $restartFailures")
                }
                if (!exclusive && running) {
                    append("\nAtenção: a saída recusou o modo exclusivo; há uma camada extra de mixagem.")
                }
            }

            summaryAudio.text = when {
                !running -> "● Áudio parado"
                exclusive && lowLatencyMode -> "● Áudio exclusivo · ${decimal.format(bufferMs)} ms de buffer"
                else -> "● Áudio compartilhado · ${decimal.format(bufferMs)} ms de buffer"
            }
            summaryAudio.setTextColor(if (running) GREEN else RED)

            val nativeMidi = status.optBoolean("nativeMidiConnected")
            val midiCount = status.optLong("midiMessageCount")
            if (nativeMidi) {
                summaryMidi.text = "● MIDI nativo ativo"
                summaryMidi.setTextColor(GREEN)
                if (midiCount != lastNativeMidiCount) {
                    lastNativeMidiCount = midiCount
                    if (midiCount > 0) {
                        val midiStatusByte = status.optInt("lastMidiStatus")
                        val data1 = status.optInt("lastMidiData1")
                        val data2 = status.optInt("lastMidiData2")
                        val midiToAudioMicros = status.optLong("lastMidiToAudioMicros", -1L)
                        midiActivity.text = formatNativeMidiActivity(
                            midiStatusByte,
                            data1,
                            data2,
                            midiToAudioMicros,
                        )
                    }
                }
            }
        } catch (_: Exception) {
            audioStatus.text = "Diagnóstico de áudio indisponível"
            summaryAudio.text = "● Diagnóstico indisponível"
            summaryAudio.setTextColor(RED)
        }
    }

    private fun formatNativeMidiActivity(
        status: Int,
        data1: Int,
        data2: Int,
        midiToAudioMicros: Long,
    ): String {
        val command = status and 0xF0
        val channel = (status and 0x0F) + 1
        val description = when (command) {
            0x80 -> "Note Off · nota $data1 · canal $channel"
            0x90 -> if (data2 == 0) {
                "Note Off · nota $data1 · canal $channel"
            } else {
                "Note On · nota $data1 · velocidade $data2 · canal $channel"
            }
            0xB0 -> {
                val name = when (data1) {
                    1 -> "Modulação"
                    7 -> "Volume"
                    11 -> "Expressão"
                    64 -> "Pedal sustain"
                    120 -> "All Sound Off"
                    123 -> "All Notes Off"
                    else -> "CC$data1"
                }
                "$name · valor $data2 · canal $channel"
            }
            0xC0 -> "Program Change · programa $data1 · canal $channel"
            0xE0 -> "Pitch bend · ${(data2 shl 7) or data1} · canal $channel"
            else -> "Mensagem MIDI nativa"
        }
        val bytes = if (command == 0xC0 || command == 0xD0) {
            "%02X %02X".format(status, data1)
        } else {
            "%02X %02X %02X".format(status, data1, data2)
        }
        val timing = if (midiToAudioMicros >= 0) {
            "\nMIDI → callback: ${decimal.format(midiToAudioMicros / 1000.0)} ms"
        } else {
            ""
        }
        return "Monitor MIDI · AMidi nativo\n$description\nHEX: $bytes$timing"
    }

    private fun buildDiagnosticKeyboard(): View {
        val grid = GridLayout(this).apply {
            columnCount = 4
            rowCount = 2
            useDefaultMargins = true
        }
        val notes = listOf(
            60 to "C", 62 to "D", 64 to "E", 65 to "F",
            67 to "G", 69 to "A", 71 to "B", 72 to "C+",
        )
        notes.forEach { (note, name) ->
            grid.addView(Button(this).apply {
                text = name
                textSize = 15f
                setTextColor(BACKGROUND)
                background = roundedDrawable(Color.rgb(238, 241, 243), 10)
                setOnTouchListener { _, event ->
                    when (event.actionMasked) {
                        MotionEvent.ACTION_DOWN -> {
                            NativeSynth.nativeNoteOn(0, note, 105)
                            true
                        }
                        MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                            NativeSynth.nativeNoteOff(0, note)
                            true
                        }
                        else -> false
                    }
                }
            }, GridLayout.LayoutParams().apply {
                width = 0
                height = dp(54)
                columnSpec = GridLayout.spec(GridLayout.UNDEFINED, 1f)
            })
        }
        return grid
    }

    private fun updateReleaseLabel() {
        releaseLabel.text = if (releaseSwitch.isChecked) {
            "Tempo da cauda: ${decimal.format(releaseSeek.progress / 1000.0)} s"
        } else {
            "Cauda adicional desligada; será usado o envelope natural do SF2."
        }
    }

    private fun setSoundFontLoading(loading: Boolean, message: String? = null) {
        importButton.isEnabled = !loading
        soundFontSpinner.isEnabled = !loading && soundFonts.isNotEmpty()
        presetSpinner.isEnabled = !loading && presetItemCount > 0
        removeSoundFontButton.isEnabled = !loading && soundFonts.isNotEmpty()
        if (message != null) soundFontDetails.text = message
    }

    private fun showSoundFontError(message: String) {
        AlertDialog.Builder(this)
            .setTitle("Não foi possível carregar o SoundFont")
            .setMessage(message)
            .setPositiveButton("OK", null)
            .show()
    }

    private fun queryDisplayName(uri: Uri): String {
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) {
                val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (index >= 0) return cursor.getString(index)
            }
        }
        return uri.lastPathSegment?.substringAfterLast('/') ?: "soundfont.sf2"
    }

    private fun humanFileSize(bytes: Long): String {
        val mb = bytes / (1024.0 * 1024.0)
        return if (mb >= 1.0) "${decimal.format(mb)} MB" else "${decimal.format(bytes / 1024.0)} KB"
    }

    private fun sectionPanel(number: String, title: String, subtitle: String): LinearLayout {
        return roundedPanel(PANEL, 17).apply {
            val header = horizontalRow().apply { gravity = Gravity.CENTER_VERTICAL }
            header.addView(TextView(this@MainActivity).apply {
                text = number
                gravity = Gravity.CENTER
                textSize = 15f
                setTextColor(BACKGROUND)
                setTypeface(typeface, Typeface.BOLD)
                background = roundedDrawable(YELLOW, 100)
            }, LinearLayout.LayoutParams(dp(34), dp(34)).apply { marginEnd = dp(11) })
            header.addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.VERTICAL
                addView(TextView(this@MainActivity).apply {
                    text = title
                    textSize = 20f
                    setTextColor(TEXT)
                    setTypeface(typeface, Typeface.BOLD)
                })
                addView(TextView(this@MainActivity).apply {
                    text = subtitle
                    textSize = 13f
                    setTextColor(MUTED)
                })
            }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(header, matchWrap(bottom = 17))
        }
    }

    private fun roundedPanel(color: Int, radius: Int): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(dp(16), dp(16), dp(16), dp(16))
        background = roundedDrawable(color, radius)
    }

    private fun statusLine(value: String, color: Int) = TextView(this).apply {
        text = value
        textSize = 15f
        setTextColor(color)
        setTypeface(typeface, Typeface.BOLD)
    }

    private fun fieldLabel(value: String, top: Int = 0) = TextView(this).apply {
        text = value
        textSize = 13f
        setTextColor(MUTED)
        setTypeface(typeface, Typeface.BOLD)
        if (top > 0) setPadding(0, dp(top), 0, 0)
    }

    private fun secondaryText(value: String) = TextView(this).apply {
        text = value
        textSize = 14f
        setTextColor(MUTED)
        setLineSpacing(0f, 1.12f)
    }

    private fun diagnosticBox(value: String) = TextView(this).apply {
        text = value
        textSize = 13f
        setTextColor(TEXT)
        setLineSpacing(0f, 1.15f)
        setPadding(dp(12), dp(11), dp(12), dp(11))
        background = roundedDrawable(PANEL_ALT, 11)
    }

    private fun styledSpinner() = Spinner(this).apply {
        backgroundTintList = ColorStateList.valueOf(YELLOW)
    }

    private fun spinnerAdapter(items: List<String>) = ArrayAdapter(
        this,
        android.R.layout.simple_spinner_dropdown_item,
        items,
    )

    private fun primaryButton(value: String, action: () -> Unit) = Button(this).apply {
        text = value
        textSize = 14f
        setTextColor(BACKGROUND)
        setTypeface(typeface, Typeface.BOLD)
        background = roundedDrawable(YELLOW, 12)
        setOnClickListener { action() }
    }

    private fun outlineButton(value: String, action: () -> Unit) = Button(this).apply {
        text = value
        textSize = 14f
        setTextColor(YELLOW)
        setTypeface(typeface, Typeface.BOLD)
        background = GradientDrawable().apply {
            shape = GradientDrawable.RECTANGLE
            cornerRadius = dp(12).toFloat()
            setColor(Color.TRANSPARENT)
            setStroke(dp(1), YELLOW)
        }
        setOnClickListener { action() }
    }

    private fun roundedDrawable(color: Int, radius: Int) = GradientDrawable().apply {
        shape = GradientDrawable.RECTANGLE
        cornerRadius = dp(radius).toFloat()
        setColor(color)
    }

    private fun horizontalRow() = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
    }

    private fun weightedButton(start: Int = 0, end: Int = 0) =
        LinearLayout.LayoutParams(0, dp(48), 1f).apply {
            marginStart = dp(start)
            marginEnd = dp(end)
        }

    private fun matchWrap(top: Int = 0, bottom: Int = 0) =
        LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT,
        ).apply {
            topMargin = dp(top)
            bottomMargin = dp(bottom)
        }

    private fun switchThumbColors() = ColorStateList(
        arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()),
        intArrayOf(YELLOW, Color.rgb(130, 140, 148)),
    )

    private fun switchTrackColors() = ColorStateList(
        arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()),
        intArrayOf(Color.rgb(117, 91, 16), Color.rgb(63, 72, 78)),
    )

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()
}
