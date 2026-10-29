; Skullburglar setup for Windows (NSIS 3), built by tools/ci/package.sh:
;   makensis -DVERSION=<x.y.z> -DSRC=<staged dist dir> -DOUT=<setup.exe> installer.nsi
; Per-user, no admin prompt: installs to C:\Games\Skullburglar by default (normal users may create folders at the
; root of C:, and the game writes logs\ next to bots.exe, which Program Files wouldn't allow). Running a newer setup
; finds the existing install and replaces its program files in place. Game files, saves and settings live outside
; the install folder (%LOCALAPPDATA%\bots-recomp, Documents\bots), so upgrades and uninstalls never touch them.

!ifndef VERSION | SRC | OUT
  !error "define VERSION, SRC and OUT"
!endif

Unicode true
SetCompressor /SOLID lzma
RequestExecutionLevel user
ManifestDPIAware true

!define APP "Skullburglar"
!define REGKEY "Software\${APP}"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}"

Name "${APP} ${VERSION}"
OutFile "${OUT}"
InstallDir "C:\Games\${APP}"
InstallDirRegKey HKCU "${REGKEY}" "InstallDir"
BrandingText "${APP} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APP} setup"
VIAddVersionKey "LegalCopyright" ""

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!insertmacro GetParameters
!insertmacro GetOptions

!define MUI_ICON "skullburglar.ico"
!define MUI_UNICON "skullburglar.ico"
!define MUI_ABORTWARNING

!define MUI_WELCOMEPAGE_TEXT "This installs ${APP} ${VERSION}, a native port of 50 Cent: Blood on the Sand.$\r$\n$\r$\nYou need your own copy of the game: on first start the launcher asks for your disc image or extracted disc.$\r$\n$\r$\nIf ${APP} is already installed, this updates it. Your game files, saves and settings are kept."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\bots.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APP}"
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\README.txt"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Show the README"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; The game must not be running while its files are replaced: a running exe can't be opened for writing.
; Silent (/S, as the launcher's "Update now" runs it): no message box; the launcher is quitting as this starts, so wait
; for it up to half a minute, then give up with an error code (2).
!macro WaitForGameClosed
  ${If} ${FileExists} "$INSTDIR\bots.exe"
    StrCpy $2 0
    retry:
    ClearErrors
    FileOpen $0 "$INSTDIR\bots.exe" a
    ${If} ${Errors}
      ${If} ${Silent}
        IntOp $2 $2 + 1
        ${If} $2 > 60
          SetErrorLevel 2
          Abort
        ${EndIf}
        Sleep 500
        Goto retry
      ${EndIf}
      MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "${APP} is running. Close it, then choose Retry." IDRETRY retry
      Abort
    ${EndIf}
    FileClose $0
  ${EndIf}
!macroend

!macro RemoveProgramFiles
  ; Only what a release ships; logs\ and anything the player added stay.
  Delete "$INSTDIR\bots.exe"
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\THIRD-PARTY-NOTICES.txt"
  Delete "$INSTDIR\skullburglar.ico"
  RMDir /r "$INSTDIR\pipelines"
!macroend

; The launcher's "Update now" runs this setup with /UPDATE over the folder it runs from. Only a folder this setup made
; (it has uninstall.exe) is accepted: an unzipped copy, or any other folder, must never have its program files
; deleted by an update. Error level 3.
!macro RequireOwnInstallForUpdate
  ${GetParameters} $R0
  ${GetOptions} $R0 "/UPDATE" $R1
  ${IfNot} ${Errors}
    ${IfNot} ${FileExists} "$INSTDIR\uninstall.exe"
      SetErrorLevel 3
      Abort
    ${EndIf}
  ${EndIf}
!macroend

Section "Install"
  SetShellVarContext current
  ; Before SetOutPath, which creates the folder: a bad /D= must leave no empty folder behind.
  !insertmacro RequireOwnInstallForUpdate
  SetOutPath "$INSTDIR"
  !insertmacro WaitForGameClosed
  !insertmacro RemoveProgramFiles
  File /r "${SRC}\*"
  File "skullburglar.ico"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  CreateShortcut "$SMPROGRAMS\${APP}.lnk" "$INSTDIR\bots.exe" "" "$INSTDIR\skullburglar.ico"

  WriteRegStr HKCU "${REGKEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${REGKEY}" "Version" "${VERSION}"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayName" "${APP}"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTKEY}" "Publisher" "${APP}"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\skullburglar.ico"
  WriteRegStr HKCU "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTKEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  WriteRegDWORD HKCU "${UNINSTKEY}" "EstimatedSize" $0

  ; "Update now" in the launcher runs this setup silently with /UPDATE: the finish page (which would start the game)
  ; doesn't show then, so start it here.
  ${GetParameters} $R0
  ${GetOptions} $R0 "/UPDATE" $R1
  ${IfNot} ${Errors}
    ; The launcher leaves this note before it starts the setup, and a note still there at its next start means the update
    ; didn't finish: remove it now that it did.
    Delete "$LOCALAPPDATA\bots-recomp\update-pending.txt"
    Exec '"$INSTDIR\bots.exe"'
  ${EndIf}
SectionEnd

; A silent update that stops (the game wouldn't close in half a minute, no room, an error): the launcher already quit, so
; start the old version again if its files are still there. The note stays, and the launcher says at its next start that
; the update didn't finish. Not when bots.exe is still running (it can't be opened for writing): that is the failure
; where the game never closed, and starting it again would run a second copy.
Function .onInstFailed
  ${GetParameters} $R0
  ${GetOptions} $R0 "/UPDATE" $R1
  ${IfNot} ${Errors}
  ${AndIf} ${FileExists} "$INSTDIR\bots.exe"
    ClearErrors
    FileOpen $0 "$INSTDIR\bots.exe" a
    ${IfNot} ${Errors}
      FileClose $0
      Exec '"$INSTDIR\bots.exe"'
    ${EndIf}
  ${EndIf}
FunctionEnd

Section "Uninstall"
  SetShellVarContext current
  !insertmacro WaitForGameClosed
  !insertmacro RemoveProgramFiles
  Delete "$INSTDIR\uninstall.exe"
  Delete "$SMPROGRAMS\${APP}.lnk"
  DeleteRegKey HKCU "${UNINSTKEY}"
  DeleteRegKey HKCU "${REGKEY}"
  ; Removed only if empty: logs\ stays when there is one.
  RMDir "$INSTDIR"
SectionEnd
