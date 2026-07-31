<div align="center">
  <img src="docs/sf2-live-icon.svg" width="120" height="120" alt="SF2 Live" />

  # SF2 Live

  **Instrumentos SF2 · MIDI USB · Áudio em tempo real**

  Um app Android simples para carregar arquivos `.sf2`, escolher presets, ajustar o release/sustain e tocar com um teclado MIDI USB.
</div>

---

## Sobre o projeto

**SF2 Live** é um player de SoundFont para Android feito para quem quer tocar instrumentos `.sf2` com baixa latência usando um teclado MIDI USB.

A ideia é ser direto: importar um SoundFont, escolher o preset, conectar o teclado, selecionar a saída de áudio e tocar. Ele foi pensado para estudos, testes de timbres, ensaios simples e uso com controladores MIDI externos.

O foco do projeto é manter o caminho de áudio o mais leve possível, usando MIDI nativo, síntese em C++ e Oboe/AAudio para áudio de baixa latência.

## Funcionalidades principais

- Importação de arquivos **`.sf2`**.
- Biblioteca local de SoundFonts.
- Seleção de **presets/timbres** dentro do SF2.
- Entrada por **teclado MIDI USB**.
- Seleção automática do MIDI conectado quando possível.
- Seleção da saída de áudio, incluindo interfaces USB.
- Controle de **release/sustain** para deixar o som mais seco ou mais prolongado.
- Suporte a pedal de sustain MIDI (**CC64**).
- Teclado virtual para teste rápido.
- Botão **Panic** para cortar notas presas.
- Motor nativo otimizado para acordes, retrigger rápido e baixa latência.
- Diagnóstico de áudio, MIDI, vozes, buffer e xruns.

## Tecnologias

- **Kotlin** para a interface Android.
- **C++17** para o motor de áudio e sintetizador SF2.
- **JNI** para comunicação entre Kotlin e C++.
- **AMidi / NDK MIDI** para leitura MIDI nativa.
- **Oboe 1.10.0** sobre AAudio para áudio de baixa latência.
- **CMake** para o build nativo.
- **Gradle / Android Gradle Plugin** para o build Android.
- **mmap** para carregar SoundFonts grandes sem duplicar o arquivo inteiro na RAM.

## Requisitos para compilar

Testado inicialmente no **Ubuntu**.

Você precisa ter:

- Android Studio ou Android SDK instalado.
- Java 17 ou superior.
- Android SDK Command-line Tools.
- `sdkmanager` disponível dentro do Android SDK.
- `curl` ou `wget`.
- `unzip`.
- Um celular Android conectado por USB, se quiser instalar direto.

O script prepara o restante quando possível, incluindo:

- Gradle Wrapper.
- Android platform usada pelo projeto.
- Build Tools.
- NDK.
- CMake.

## Como compilar

Na raiz do projeto, rode:

```bash
chmod +x gerar-apk-facil.sh
./gerar-apk-facil.sh
```

O APK será gerado na pasta do projeto com um nome parecido com:

```text
SF2Live-v0.10.0-debug.apk
```

## Como compilar e instalar no celular

Ative a **Depuração USB** no Android, conecte o celular ao computador e autorize a conexão quando o sistema pedir.

Depois rode:

```bash
chmod +x gerar-apk-facil.sh
./gerar-apk-facil.sh --instalar
```

Permissões e acessos usados pelo app:

- Acesso ao arquivo `.sf2` escolhido pelo usuário.
- Acesso ao dispositivo MIDI USB quando o Android solicitar permissão.
- Permissão para ajustar configurações de áudio.
- Wake lock para manter o processamento ativo durante o uso.

## Baixar APK pronto

Se você não quiser instalar Android Studio, SDK, NDK ou qualquer dependência de build, basta acessar a **[página de Releases](https://github.com/AlbertoLucass/sf2-live/releases)** do projeto no GitHub.

Abra a versão publicada mais recente, vá até a seção **Assets**, baixe o arquivo `.apk` e instale-o no seu dispositivo Android.

## Uso básico

1. Abra o app.
2. Importe um arquivo `.sf2`.
3. Escolha o preset desejado.
4. Conecte o teclado MIDI USB.
5. Selecione a saída de áudio, se necessário.
6. Ajuste o release/sustain.
7. Toque.

Para menor latência, use uma interface de áudio USB e mantenha o perfil de menor buffer estável para o seu aparelho.

## Observação sobre latência

O SF2 Live usa o melhor caminho geral disponível pelas APIs públicas do Android: MIDI nativo, sintetizador em C++ e Oboe/AAudio em baixa latência.

Apps comerciais como o Audio Evolution podem usar drivers USB próprios, contornando parte do sistema de áudio do Android. Por isso, em alguns aparelhos e interfaces, eles ainda podem ter latência menor.

## Licença

Este projeto está licenciado sob a licença **MIT**. Veja o arquivo [`LICENSE`](LICENSE).
