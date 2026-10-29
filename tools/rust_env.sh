# Sourced by the build scripts (tools/ci/build.sh, tools/ios/build.sh, tools/android/build.sh): makes cargo available
# for n1 (n1/, Rust), which the CMake build compiles, and adds what a cross build needs to the pinned toolchain.
#   . tools/rust_env.sh; rust_env [<rust target>...]     (from the repository root)
# cargo comes from PATH, rustup's usual places, or, when there is none (a fresh Mac runner), a user-local rustup
# installed without touching shell profiles (rustup-init into ~/.cargo). n1/rust-toolchain.toml selects the version,
# which rustup fetches on first use. Each target is added to that version (`rustup target add`), except a tier-3 one
# with no prebuilt standard library (aarch64-apple-tvos*): it gets the rust-src component instead, from which cargo
# builds the standard library (-Zbuild-std, CMakeLists.txt). Without rustup (a toolchain from elsewhere) the targets
# are left to whoever installed it.
rust_env() {
  if ! command -v cargo >/dev/null; then
    for d in "$HOME/.cargo/bin" /opt/homebrew/opt/rustup/bin /usr/local/opt/rustup/bin; do
      [ ! -x "$d/cargo" ] || { PATH="$d:$PATH"; break; }
    done
    if ! command -v cargo >/dev/null; then
      echo "== rustup (user-local install)"
      curl -fsSL https://sh.rustup.rs | sh -s -- -y --no-modify-path --profile minimal --default-toolchain none
      PATH="$HOME/.cargo/bin:$PATH"
    fi
    export PATH
  fi
  command -v rustup >/dev/null || return 0
  [ $# -gt 0 ] || return 0
  _rust_pin=$(sed -n 's/^channel *= *"\(.*\)"/\1/p' n1/rust-toolchain.toml)
  rustup toolchain install --profile minimal --no-self-update "$_rust_pin" >/dev/null
  for _t; do
    case $_t in
    *-apple-tvos*) rustup component add --toolchain "$_rust_pin" rust-src ;;
    *) rustup target add --toolchain "$_rust_pin" "$_t" ;;
    esac
  done
}
