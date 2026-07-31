#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

GRADLE_VERSION="8.13"
GRADLE_SHA256="20f1b1176237254a6fc204d8434196fa11a4cfb387567519c61556e8710aed78"
ANDROID_PLATFORM="android-36"
BUILD_TOOLS_VERSION="35.0.0"
NDK_VERSION="27.0.12077973"
CMAKE_VERSION="3.22.1"
APK_SOURCE="$ROOT/app/build/outputs/apk/debug/app-debug.apk"
APK_FINAL="$ROOT/SF2Live-v0.10.0-debug.apk"
LOG_DIR="$ROOT/.build-logs"
INSTALL_AFTER_BUILD=false
USE_RTK=true

for argument in "$@"; do
  case "$argument" in
    --instalar|-i)
      INSTALL_AFTER_BUILD=true
      ;;
    --sem-rtk)
      USE_RTK=false
      ;;
    --ajuda|-h|--help)
      cat <<'HELP'
Uso:
  ./gerar-apk-facil.sh             Gera o APK usando RTK, quando disponível.
  ./gerar-apk-facil.sh --instalar  Gera e instala no celular conectado por USB.
  ./gerar-apk-facil.sh --sem-rtk   Usa a saída normal do Gradle.
HELP
      exit 0
      ;;
    *)
      printf 'Argumento desconhecido: %s\n' "$argument" >&2
      exit 2
      ;;
  esac
done

mkdir -p "$LOG_DIR"

info() { printf '\n==> %s\n' "$*"; }
ok() { printf 'OK: %s\n' "$*"; }
fail() { printf '\nERRO: %s\n' "$*" >&2; exit 1; }

show_log() {
  local file="$1"
  if [[ "$USE_RTK" == true ]] && command -v rtk >/dev/null 2>&1; then
    rtk log "$file" || tail -n 100 "$file"
  else
    tail -n 100 "$file"
  fi
}

run_logged() {
  local label="$1"
  local log_file="$2"
  shift 2
  info "$label"
  if "$@" >"$log_file" 2>&1; then
    ok "$label"
  else
    printf '\nA etapa falhou. Saída relevante:\n' >&2
    show_log "$log_file" >&2
    return 1
  fi
}

run_gradle() {
  if [[ "$USE_RTK" == true ]] && command -v rtk >/dev/null 2>&1; then
    rtk err ./gradlew --console=plain "$@"
  else
    ./gradlew --console=plain "$@"
  fi
}

java_major_version() {
  "$1/bin/java" -version 2>&1 | awk -F '[".]' '/version/ { if ($2 == "1") print $3; else print $2; exit }'
}

find_java_home() {
  local candidates=()
  local java_path=""
  [[ -n "${JAVA_HOME:-}" ]] && candidates+=("$JAVA_HOME")
  if command -v java >/dev/null 2>&1; then
    java_path="$(readlink -f "$(command -v java)" 2>/dev/null || command -v java)"
    candidates+=("$(cd "$(dirname "$java_path")/.." 2>/dev/null && pwd || true)")
  fi
  candidates+=(
    "/opt/android-studio/jbr"
    "/usr/local/android-studio/jbr"
    "/snap/android-studio/current/jbr"
    "$HOME/android-studio/jbr"
    "$HOME/Applications/Android Studio.app/Contents/jbr/Contents/Home"
    "/Applications/Android Studio.app/Contents/jbr/Contents/Home"
  )
  if [[ -d "$HOME/.local/share/JetBrains/Toolbox/apps/AndroidStudio" ]]; then
    while IFS= read -r candidate; do candidates+=("$candidate"); done < <(
      find "$HOME/.local/share/JetBrains/Toolbox/apps/AndroidStudio" -type d -name jbr 2>/dev/null | sort -Vr
    )
  fi
  local candidate major
  for candidate in "${candidates[@]}"; do
    [[ -n "$candidate" && -x "$candidate/bin/java" ]] || continue
    major="$(java_major_version "$candidate" || true)"
    if [[ "$major" =~ ^[0-9]+$ ]] && (( major >= 17 )); then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

find_android_sdk() {
  local candidates=("${ANDROID_SDK_ROOT:-}" "${ANDROID_HOME:-}" "$HOME/Android/Sdk" "$HOME/Library/Android/sdk")
  local candidate
  for candidate in "${candidates[@]}"; do
    [[ -n "$candidate" && -d "$candidate" ]] || continue
    printf '%s\n' "$candidate"
    return 0
  done
  return 1
}

find_sdkmanager() {
  local sdk="$1"
  local candidates=("$sdk/cmdline-tools/latest/bin/sdkmanager" "$sdk/tools/bin/sdkmanager")
  local candidate
  for candidate in "${candidates[@]}"; do
    if [[ -x "$candidate" ]]; then printf '%s\n' "$candidate"; return 0; fi
  done
  if [[ -d "$sdk/cmdline-tools" ]]; then
    candidate="$(find "$sdk/cmdline-tools" -mindepth 2 -maxdepth 3 -type f -name sdkmanager 2>/dev/null | sort -V | tail -n 1)"
    if [[ -n "$candidate" && -x "$candidate" ]]; then printf '%s\n' "$candidate"; return 0; fi
  fi
  return 1
}

write_local_properties() {
  local sdk="$1"
  local escaped_sdk="${sdk// /\\ }"
  if [[ -f local.properties ]]; then
    local temp_file
    temp_file="$(mktemp)"
    awk -v value="sdk.dir=$escaped_sdk" '
      BEGIN { replaced = 0 }
      /^sdk\.dir=/ && replaced == 0 { print value; replaced = 1; next }
      { print }
      END { if (replaced == 0) print value }
    ' local.properties > "$temp_file"
    mv "$temp_file" local.properties
  else
    printf 'sdk.dir=%s\n' "$escaped_sdk" > local.properties
  fi
}

verify_sha256() {
  local file="$1" expected="$2" actual=""
  if command -v sha256sum >/dev/null 2>&1; then
    actual="$(sha256sum "$file" | awk '{print $1}')"
  elif command -v shasum >/dev/null 2>&1; then
    actual="$(shasum -a 256 "$file" | awk '{print $1}')"
  else
    fail "Não encontrei sha256sum nem shasum."
  fi
  [[ "$actual" == "$expected" ]]
}

download_file() {
  local url="$1" destination="$2"
  if command -v curl >/dev/null 2>&1; then
    curl --fail --location --retry 3 --silent --show-error --output "$destination" "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget --quiet --output-document="$destination" "$url"
  else
    fail "Instale curl ou wget para baixar o Gradle."
  fi
}

prepare_gradle_wrapper() {
  if [[ -x ./gradlew && -f gradle/wrapper/gradle-wrapper.jar ]]; then
    ok "Gradle Wrapper já está pronto"
    return
  fi
  command -v unzip >/dev/null 2>&1 || fail "Instale o pacote unzip e execute novamente."
  local cache_root="${XDG_CACHE_HOME:-$HOME/.cache}/sf2live"
  local zip_file="$cache_root/gradle-${GRADLE_VERSION}-bin.zip"
  local gradle_home="$cache_root/gradle-${GRADLE_VERSION}"
  mkdir -p "$cache_root"

  if [[ -f "$zip_file" ]] && ! verify_sha256 "$zip_file" "$GRADLE_SHA256"; then rm -f "$zip_file"; fi
  if [[ ! -f "$zip_file" ]]; then
    info "Baixando Gradle $GRADLE_VERSION"
    download_file "https://services.gradle.org/distributions/gradle-${GRADLE_VERSION}-bin.zip" "$zip_file.part"
    mv "$zip_file.part" "$zip_file"
    ok "Gradle baixado"
  fi
  verify_sha256 "$zip_file" "$GRADLE_SHA256" || fail "O Gradle falhou na verificação SHA-256."
  if [[ ! -x "$gradle_home/bin/gradle" ]]; then
    rm -rf "$gradle_home"
    unzip -q "$zip_file" -d "$cache_root"
  fi
  run_logged "Preparando o Gradle Wrapper" "$LOG_DIR/wrapper.log" \
    "$gradle_home/bin/gradle" --quiet --console=plain wrapper \
      --gradle-version "$GRADLE_VERSION" --distribution-type bin
  chmod +x ./gradlew
  local wrapper_properties="gradle/wrapper/gradle-wrapper.properties"
  if [[ -f "$wrapper_properties" ]] && ! grep -q '^distributionSha256Sum=' "$wrapper_properties"; then
    printf 'distributionSha256Sum=%s\n' "$GRADLE_SHA256" >> "$wrapper_properties"
  fi
}

install_android_components() {
  local sdkmanager="$1" sdk="$2"
  info "Aceitando licenças do Android SDK"
  set +o pipefail
  yes | "$sdkmanager" --sdk_root="$sdk" --licenses >/dev/null 2>&1
  set -o pipefail
  ok "Licenças verificadas"

  run_logged "Verificando Android SDK, NDK e CMake" "$LOG_DIR/sdkmanager.log" \
    "$sdkmanager" --sdk_root="$sdk" \
      "platform-tools" "platforms;$ANDROID_PLATFORM" "build-tools;$BUILD_TOOLS_VERSION" \
      "ndk;$NDK_VERSION" "cmake;$CMAKE_VERSION"
}

install_apk() {
  local sdk="$1" adb="$sdk/platform-tools/adb"
  [[ -x "$adb" ]] || fail "Não encontrei o ADB em $adb."
  "$adb" start-server >/dev/null
  local device_state
  device_state="$($adb devices | awk 'NR > 1 && $2 != "" {print $2; exit}')"
  case "$device_state" in
    device)
      info "Instalando o SF2 Live no celular"
      if [[ "$USE_RTK" == true ]] && command -v rtk >/dev/null 2>&1; then
        rtk err "$adb" install -r "$APK_FINAL"
      else
        "$adb" install -r "$APK_FINAL"
      fi
      "$adb" shell monkey -p com.sf2live.app 1 >/dev/null 2>&1 || true
      ok "Aplicativo instalado e aberto"
      ;;
    unauthorized)
      fail "Autorize a depuração USB no celular e execute novamente com --instalar."
      ;;
    *)
      fail "Nenhum celular autorizado. O APK já foi gerado em: $APK_FINAL"
      ;;
  esac
}

info "Preparando o SF2 Live v0.10.0"
if [[ "$USE_RTK" == true ]] && command -v rtk >/dev/null 2>&1; then
  ok "RTK encontrado ($(rtk --version 2>/dev/null || printf desconhecido)): erros do Gradle serão compactados"
else
  ok "RTK não encontrado: usando saída simples do Gradle"
fi

JAVA_HOME_FOUND="$(find_java_home || true)"
[[ -n "$JAVA_HOME_FOUND" ]] || fail "Não encontrei Java 17 ou superior. Instale o Android Studio."
export JAVA_HOME="$JAVA_HOME_FOUND"
export PATH="$JAVA_HOME/bin:$PATH"
ok "Java $(java_major_version "$JAVA_HOME") encontrado"

ANDROID_SDK_FOUND="$(find_android_sdk || true)"
[[ -n "$ANDROID_SDK_FOUND" ]] || fail "Não encontrei o Android SDK. Abra o Android Studio uma vez e conclua a instalação inicial."
export ANDROID_SDK_ROOT="$ANDROID_SDK_FOUND"
export ANDROID_HOME="$ANDROID_SDK_FOUND"
write_local_properties "$ANDROID_SDK_FOUND"
ok "Android SDK encontrado em $ANDROID_SDK_FOUND"

SDKMANAGER="$(find_sdkmanager "$ANDROID_SDK_FOUND" || true)"
if [[ -z "$SDKMANAGER" ]]; then
  fail 'Instale "Android SDK Command-line Tools (latest)" em Tools > SDK Manager > SDK Tools.'
fi

install_android_components "$SDKMANAGER" "$ANDROID_SDK_FOUND"
prepare_gradle_wrapper

info "Compilando o aplicativo"
run_gradle --no-daemon assembleDebug

[[ -f "$APK_SOURCE" ]] || fail "O APK não foi encontrado em $APK_SOURCE"
cp -f "$APK_SOURCE" "$APK_FINAL"
ok "APK gerado em: $APK_FINAL"

if [[ "$INSTALL_AFTER_BUILD" == true ]]; then
  install_apk "$ANDROID_SDK_FOUND"
else
  printf '\nPara instalar no celular depois:\n  ./gerar-apk-facil.sh --instalar\n'
fi
